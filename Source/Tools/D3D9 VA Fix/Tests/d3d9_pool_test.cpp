// x86: how much 32-bit address space does a D3D9 texture cost per pool, and
// does the d3d9_memlog VA fix keep textures intact?
//   managed : CreateTexture(MANAGED), Lock/fill/Unlock every mip, PreLoad
//   default : CreateTexture(DEFAULT) + SYSTEMMEM staging, fill, UpdateTexture,
//             staging released
//   dynamic : CreateTexture(DEFAULT, DYNAMIC), locked directly
// Run: d3d9_pool_test.exe <d3d9.dll> <managed|default|dynamic> [count] [size]
//      [words...]
// Words (any order): argb (default DXT5), surface (fill through
// GetSurfaceLevel + LockRect, as D3DX does), relock (read every top level back,
// as a streaming mip reallocation does), verify (ARGB: copy levels 0 and 3 to a
// render target on the GPU, read back, compare), churn=N (N more rounds of
// release-all + recreate). Scenario words (run before the main flow):
// doublelock (a second lock of a locked level must fail; another level may be
// locked), getdesc (pool reported by GetLevelDesc and surface GetDesc),
// surfacelast (textures released last through a level surface, then new
// surfaces at reused addresses must not be served from a dead shadow),
// threads=N (N threads create/fill/relock/check/release small textures),
// redevice (a second device after the first is released).
#include <atomic>
#include <cstdio>
#include <cstring>
#include <d3d9.h>
#include <psapi.h>
#include <thread>
#include <vector>
#include <windows.h>

#include "crash_report.h"

static void Report(const char* step)
{
   size_t free_total = 0, largest = 0, committed = 0;
   MEMORY_BASIC_INFORMATION mbi;
   for (char* p = nullptr; VirtualQuery(p, &mbi, sizeof(mbi));
      p = (char*)mbi.BaseAddress + mbi.RegionSize)
   {
      if (mbi.State == MEM_FREE)
      {
         free_total += mbi.RegionSize;
         largest = max(largest, mbi.RegionSize);
      }
      else if (mbi.State == MEM_COMMIT)
      {
         committed += mbi.RegionSize;
      }
      if ((uintptr_t)mbi.BaseAddress + mbi.RegionSize <
          (uintptr_t)mbi.BaseAddress)
         break;
   }
   PROCESS_MEMORY_COUNTERS_EX pmc = {sizeof(pmc)};
   GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc,
      sizeof(pmc));
   printf("%-40s free %7.1f MB  largest %7.1f MB  committed %7.1f MB  private "
          "%7.1f MB\n",
      step, free_total / 1048576.0, largest / 1048576.0,
      committed / 1048576.0, pmc.PrivateUsage / 1048576.0);
}

// Deterministic non-constant texels so no layer can dedupe or zero-page them;
// verify recomputes them.
static uint32_t Pattern(UINT x, UINT y, UINT seed)
{
   return (x * 2654435761u) ^ (y * 40503u) ^ seed;
}

static UINT Seed(size_t texture, UINT level)
{
   return UINT(texture) * 131 + level;
}

static void Fill(const D3DLOCKED_RECT& lr, UINT rows, UINT row_bytes,
   UINT seed)
{
   for (UINT y = 0; y < rows; y++)
   {
      auto* row = (uint32_t*)((char*)lr.pBits + y * lr.Pitch);
      for (UINT x = 0; x < row_bytes / 4; x++)
         row[x] = Pattern(x, y, seed);
   }
}

static IDirect3DTexture9* CreateFilledTexture(IDirect3DDevice9* dev, UINT size,
   UINT seed)
{
   IDirect3DTexture9* tex = nullptr;
   if (FAILED(dev->CreateTexture(size, size, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
          &tex, nullptr)))
      return nullptr;
   for (UINT level = 0; level < tex->GetLevelCount(); level++)
   {
      const UINT s = max(size >> level, 1u);
      if (D3DLOCKED_RECT lr; SUCCEEDED(tex->LockRect(level, &lr, nullptr, 0)))
      {
         Fill(lr, s, s * 4, seed + level);
         tex->UnlockRect(level);
      }
   }
   return tex;
}

// Mismatching texels of level 0 read back through a READONLY lock.
static int CheckLevel0(IDirect3DTexture9* tex, UINT size, UINT seed)
{
   D3DLOCKED_RECT lr;
   if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
      return -1;
   int mismatches = 0;
   for (UINT y = 0; y < size; y++)
   {
      auto* row = (uint32_t*)((char*)lr.pBits + y * lr.Pitch);
      for (UINT x = 0; x < size; x++)
         mismatches += row[x] != Pattern(x, y, seed);
   }
   tex->UnlockRect(0);
   return mismatches;
}

static void DoubleLock(IDirect3DDevice9* dev)
{
   IDirect3DTexture9* tex = CreateFilledTexture(dev, 256, 7);
   IDirect3DSurface9* surface = nullptr;
   tex->GetSurfaceLevel(0, &surface);
   D3DLOCKED_RECT a, b, c, d;
   const HRESULT first = tex->LockRect(0, &a, nullptr, 0);
   const HRESULT second = tex->LockRect(0, &b, nullptr, 0);
   const HRESULT via_surface = surface->LockRect(&c, nullptr, 0);
   const HRESULT other_level = tex->LockRect(1, &d, nullptr, 0);
   printf("doublelock: first 0x%08lX, second 0x%08lX, via surface 0x%08lX, other "
          "level 0x%08lX\n",
      first, second, via_surface, other_level);
   if (SUCCEEDED(other_level))
      tex->UnlockRect(1);
   if (SUCCEEDED(via_surface))
      surface->UnlockRect();
   if (SUCCEEDED(second))
      tex->UnlockRect(0);
   if (SUCCEEDED(first))
      tex->UnlockRect(0);
   printf("doublelock: level 0 after unlocks, %d mismatching texels\n",
      CheckLevel0(tex, 256, 7));
   surface->Release();
   tex->Release();
}

static void GetDescCheck(IDirect3DDevice9* dev)
{
   IDirect3DTexture9* tex = CreateFilledTexture(dev, 256, 9);
   IDirect3DSurface9* surface = nullptr;
   tex->GetSurfaceLevel(0, &surface);
   D3DSURFACE_DESC level_desc = {}, surface_desc = {};
   tex->GetLevelDesc(0, &level_desc);
   surface->GetDesc(&surface_desc);
   printf("getdesc: GetLevelDesc pool %d, surface GetDesc pool %d (1 = MANAGED)\n",
      int(level_desc.Pool), int(surface_desc.Pool));
   surface->Release();
   tex->Release();
}

static void SurfaceLast(IDirect3DDevice9* dev)
{
   constexpr int COUNT = 64;
   for (int i = 0; i < COUNT; i++)
   {
      IDirect3DTexture9* tex = CreateFilledTexture(dev, 256, Seed(i, 0));
      IDirect3DSurface9* surface = nullptr;
      tex->GetSurfaceLevel(0, &surface);
      tex->Release();
      surface->Release(); // the last reference goes through the surface
   }
   int surfaces_ok = 0, textures_ok = 0;
   for (int i = 0; i < COUNT; i++)
   {
      IDirect3DSurface9* surface = nullptr;
      if (FAILED(dev->CreateOffscreenPlainSurface(256, 256, D3DFMT_A8R8G8B8,
             D3DPOOL_SYSTEMMEM, &surface, nullptr)))
         continue;
      D3DLOCKED_RECT lr;
      if (SUCCEEDED(surface->LockRect(&lr, nullptr, 0)))
      {
         Fill(lr, 256, 256 * 4, Seed(1000 + i, 0));
         surface->UnlockRect();
      }
      if (SUCCEEDED(surface->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
      {
         int mismatches = 0;
         for (UINT y = 0; y < 256; y++)
         {
            auto* row = (uint32_t*)((char*)lr.pBits + y * lr.Pitch);
            for (UINT x = 0; x < 256; x++)
               mismatches += row[x] != Pattern(x, y, Seed(1000 + i, 0));
         }
         surface->UnlockRect();
         surfaces_ok += mismatches == 0;
      }
      surface->Release();
      // New textures at reused addresses must get fresh entries.
      IDirect3DTexture9* tex = CreateFilledTexture(dev, 256, Seed(2000 + i, 0));
      textures_ok += CheckLevel0(tex, 256, Seed(2000 + i, 0)) == 0;
      tex->Release();
   }
   printf("surfacelast: %d/%d offscreen surfaces and %d/%d new textures read back "
          "correctly\n",
      surfaces_ok, COUNT, textures_ok, COUNT);
}

static void Threads(IDirect3DDevice9* dev, int thread_count)
{
   constexpr int PER_THREAD = 250;
   std::atomic<int> errors{0};
   std::vector<std::thread> threads;
   for (int t = 0; t < thread_count; t++)
   {
      threads.emplace_back([&, t]
         {
      for (int i = 0; i < PER_THREAD; i++) {
        const UINT seed = Seed(t * PER_THREAD + i, 0);
        IDirect3DTexture9 *tex = CreateFilledTexture(dev, 64, seed);
        if (!tex || CheckLevel0(tex, 64, seed) != 0)
          errors++;
        if (tex)
          tex->Release();
      } });
   }
   for (std::thread& thread : threads)
      thread.join();
   printf("threads: %d threads x %d textures, %d errors\n", thread_count, PER_THREAD,
      errors.load());
}

int main(int argc, char** argv)
{
   SetUnhandledExceptionFilter(&CrashReport);
   setvbuf(stdout, nullptr, _IONBF, 0); // a crash loses buffered output
   if (argc < 3)
      return printf(
                "usage: d3d9_pool_test <d3d9.dll> <managed|default|dynamic> "
                "[count] [size] [argb] [surface] [relock] [verify] [churn=N]\n"),
             2;
   auto has = [&](const char* word)
   {
      for (int i = 3; i < argc; i++)
      {
         if (strcmp(argv[i], word) == 0)
            return true;
      }
      return false;
   };
   int churn_rounds = 0, thread_count = 0;
   for (int i = 3; i < argc; i++)
   {
      if (strncmp(argv[i], "churn=", 6) == 0)
         churn_rounds = atoi(argv[i] + 6);
      if (strncmp(argv[i], "threads=", 8) == 0)
         thread_count = atoi(argv[i] + 8);
   }
   const bool managed = strcmp(argv[2], "managed") == 0;
   const bool dynamic = strcmp(argv[2], "dynamic") == 0;
   const int count = argc > 3 && atoi(argv[3]) > 0 ? atoi(argv[3]) : 64;
   const UINT size = argc > 4 && atoi(argv[4]) > 0 ? atoi(argv[4]) : 2048;
   const bool dxt = !has("argb");
   const bool via_surface = has("surface");
   // "mixed": texture i is (size - 4 * (i % 97)) square, so the staging pool sees
   // many sizes and has to evict (and the arena to reuse pages).
   const bool mixed = has("mixed");
   auto texture_size = [&](size_t i)
   {
      return mixed ? size - UINT(i % 97) * 4 : size;
   };
   const D3DFORMAT format = dxt ? D3DFMT_DXT5 : D3DFMT_A8R8G8B8;

   Report("start");
   HMODULE d3d9 = LoadLibraryA(argv[1]);
   if (!d3d9)
      return printf("LoadLibrary failed %lu\n", GetLastError()), 1;
   auto create9 =
      (IDirect3D9 * (WINAPI*)(UINT)) GetProcAddress(d3d9, "Direct3DCreate9");
   IDirect3D9* d3d = create9 ? create9(D3D_SDK_VERSION) : nullptr;
   if (!d3d)
      return printf("Direct3DCreate9 failed\n"), 1;

   WNDCLASSA wc = {.lpfnWndProc = DefWindowProcA,
      .hInstance = GetModuleHandleA(nullptr),
      .lpszClassName = "d3d9pool"};
   RegisterClassA(&wc);
   HWND hwnd = CreateWindowA("d3d9pool", "d3d9pool", WS_OVERLAPPEDWINDOW, 0, 0,
      256, 256, nullptr, nullptr, wc.hInstance, nullptr);
   D3DPRESENT_PARAMETERS pp = {.BackBufferWidth = 256,
      .BackBufferHeight = 256,
      .BackBufferFormat = D3DFMT_X8R8G8B8,
      .BackBufferCount = 1,
      .SwapEffect = D3DSWAPEFFECT_DISCARD,
      .hDeviceWindow = hwnd,
      .Windowed = TRUE};
   IDirect3DDevice9* dev = nullptr;
   HRESULT hr =
      d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
         D3DCREATE_HARDWARE_VERTEXPROCESSING |
            D3DCREATE_MULTITHREADED,
         &pp, &dev);
   if (FAILED(hr))
      return printf("CreateDevice failed 0x%08lX\n", hr), 1;
   Report("device");
   if (has("doublelock"))
      DoubleLock(dev);
   if (has("getdesc"))
      GetDescCheck(dev);
   if (has("surfacelast"))
      SurfaceLast(dev);
   if (thread_count > 0)
      Threads(dev, thread_count);

   double bytes = 0;
   std::vector<IDirect3DTexture9*> textures;
   for (int round = 0; round <= churn_rounds; round++)
   {
      for (auto* tex : textures)
         tex->Release();
      textures.clear();
      bytes = 0;
      for (int i = 0; i < count; i++)
      {
         IDirect3DTexture9* tex = nullptr;
         if (FAILED(dev->CreateTexture(
                texture_size(i), texture_size(i), 0, dynamic ? D3DUSAGE_DYNAMIC : 0,
                format,
                managed ? D3DPOOL_MANAGED : D3DPOOL_DEFAULT, &tex, nullptr)))
         {
            printf("CreateTexture %d failed\n", i);
            break;
         }
         const UINT levels = tex->GetLevelCount();
         IDirect3DTexture9* upload = tex;
         if (!managed && !dynamic &&
             FAILED(dev->CreateTexture(texture_size(i), texture_size(i), levels, 0,
                format, D3DPOOL_SYSTEMMEM, &upload,
                nullptr)))
         {
            printf("staging %d failed\n", i);
            tex->Release();
            break;
         }
         for (UINT level = 0; level < levels; level++)
         {
            const UINT s = max(texture_size(i) >> level, 1u);
            const UINT rows = dxt ? max((s + 3) / 4, 1u) : s;
            const UINT row_bytes = dxt ? max((s + 3) / 4, 1u) * 16 : s * 4;
            D3DLOCKED_RECT lr;
            if (via_surface)
            {
               IDirect3DSurface9* surface = nullptr;
               if (SUCCEEDED(upload->GetSurfaceLevel(level, &surface)) &&
                   SUCCEEDED(surface->LockRect(&lr, nullptr, 0)))
               {
                  Fill(lr, rows, row_bytes, Seed(i, level));
                  surface->UnlockRect();
               }
               if (surface)
                  surface->Release();
            }
            else if (SUCCEEDED(upload->LockRect(level, &lr, nullptr, 0)))
            {
               Fill(lr, rows, row_bytes, Seed(i, level));
               upload->UnlockRect(level);
            }
            bytes += double(rows) * row_bytes;
         }
         if (managed)
         {
            tex->PreLoad();
         }
         else if (!dynamic)
         {
            dev->UpdateTexture(upload, tex);
            upload->Release();
         }
         textures.push_back(tex);
      }
      if (round > 0 && (round == churn_rounds || round % 4 == 0))
      {
         char churn_label[64];
         sprintf_s(churn_label, "churn round %d", round);
         Report(churn_label);
      }
   }

   if (has("relock"))
   {
      for (auto* tex : textures)
      {
         if (D3DLOCKED_RECT lr;
            SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
            tex->UnlockRect(0);
      }
   }

   // Needs a pool the runtime can StretchRect from, i.e. not MANAGED as the
   // runtime sees it (the VA fix makes it DEFAULT).
   if (has("verify") && !dxt)
   {
      int checked = 0, bad = 0;
      for (size_t i = 0; i < textures.size(); i++)
      {
         for (UINT level : {0u, 3u})
         {
            const UINT s = max(texture_size(i) >> level, 1u);
            IDirect3DSurface9 *src = nullptr, *rt = nullptr, *sys = nullptr;
            textures[i]->GetSurfaceLevel(level, &src);
            dev->CreateRenderTarget(s, s, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
               FALSE, &rt, nullptr);
            dev->CreateOffscreenPlainSurface(s, s, D3DFMT_A8R8G8B8,
               D3DPOOL_SYSTEMMEM, &sys, nullptr);
            HRESULT hr =
               (src && rt && sys)
                  ? dev->StretchRect(src, nullptr, rt, nullptr, D3DTEXF_POINT)
                  : E_FAIL;
            if (SUCCEEDED(hr))
               hr = dev->GetRenderTargetData(rt, sys);
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(hr) &&
                SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
            {
               int mismatches = 0;
               for (UINT y = 0; y < s; y++)
               {
                  auto* row = (uint32_t*)((char*)lr.pBits + y * lr.Pitch);
                  for (UINT x = 0; x < s; x++)
                     mismatches += row[x] != Pattern(x, y, Seed(i, level));
               }
               sys->UnlockRect();
               checked++;
               bad += mismatches != 0;
               if (mismatches)
                  printf("verify: texture %zu level %u: %d mismatching texels\n", i,
                     level, mismatches);
            }
            else
            {
               printf("verify: texture %zu level %u: readback failed 0x%08lX\n", i,
                  level, hr);
               bad++;
            }
            for (IDirect3DSurface9* surface : {src, rt, sys})
            {
               if (surface)
                  surface->Release();
            }
         }
      }
      printf("verify: %d levels checked, %d bad\n", checked, bad);
   }

   // Make the driver actually consume the uploads before measuring.
   IDirect3DQuery9* query = nullptr;
   dev->CreateQuery(D3DQUERYTYPE_EVENT, &query);
   if (query)
   {
      query->Issue(D3DISSUE_END);
      while (query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
         Sleep(1);
   }
   Sleep(500);
   const char* pool_name =
      managed ? "MANAGED" : (dynamic ? "DYNAMIC" : "DEFAULT");
   char label[96];
   sprintf_s(label, "%zu x %u %s %s (%.0f MB)", textures.size(), size,
      dxt ? "DXT5" : "ARGB", pool_name, bytes / 1048576);
   Report(label);

   for (auto* tex : textures)
      tex->Release();
   if (query)
      query->Release();
   Sleep(500);
   Report("textures released");
   printf("device Release -> %lu\n", dev->Release());
   Report("device released");
   if (has("redevice"))
   {
      IDirect3DDevice9* second = nullptr;
      if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
             D3DCREATE_HARDWARE_VERTEXPROCESSING |
                D3DCREATE_MULTITHREADED,
             &pp, &second)))
      {
         IDirect3DTexture9* tex = CreateFilledTexture(second, 256, 11);
         printf("redevice: second device texture %d mismatching texels\n",
            tex ? CheckLevel0(tex, 256, 11) : -1);
         if (tex)
            tex->Release();
         printf("redevice: second device Release -> %lu\n", second->Release());
      }
   }
   d3d->Release();
   return 0;
}
