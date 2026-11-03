// d3d9 logging proxy (x86): how much of a 32-bit D3D9 game's address space is
// texture shadow copies, and does the game touch MANAGED resources after their
// first upload (relocks, READONLY locks)? Install: rename the real D3D9.dll
// (dgVoodoo or a copy of the system one) to d3d9_chain.dll, put this as
// d3d9.dll next to the exe. With d3d9_memlog.on next to this dll, every 5 s
// (D3D9_MEMLOG_MS) it appends a line to d3d9_memlog.log there: live bytes per
// pool, the process's VA, lock stats. Without it nothing is logged.
//
// VA fix, on when d3d9_vafix.on exists next to this dll:
// - the device is created as D3D9Ex, so DEFAULT resources survive Reset;
// - MANAGED 2D and cube textures become DEFAULT; their CPU copy lives in a
//   pagefile-backed section (outside the address space), and a level is mapped
//   only while it is locked (DXVK's idea);
// - on the last unlock of a written level it is uploaded through a pooled
//   SYSTEMMEM staging texture (LRU, 64 MB) and UpdateSurface. When the runtime
//   takes user memory (Windows d3d9; dgVoodoo doesn't), staging memory comes
//   from one 64 MB arena reserved against the address space ceiling;
// - level views are mapped top-down, so the fix stays away from the middle of
//   the address space where the game's big allocations need room;
// - MANAGED volumes become DEFAULT DYNAMIC, MANAGED buffers DEFAULT (small in
//   UE3 games).
// DEFAULT resources count against dgVoodoo's VRAM setting (MANAGED ones don't),
// so with the fix dgVoodoo needs VRAM = 4096; the log warns near the limit.
//
// d3d9_nomt.on next to this dll (experiment): the device is created without
// D3DCREATE_MULTITHREADED, so dgVoodoo skips its per-call critical section.
// Only safe while one thread makes every D3D9 call.
//
// d3d9_csmt.on next to this dll: D3D9 calls run on a worker thread (csmt.h),
// which also makes dropping D3DCREATE_MULTITHREADED safe. With d3d9_memlog.on
// the report adds the layer's per-frame counters.
//
// dgVoodoo workarounds, on whenever d3d9_chain.dll is dgVoodoo (d3d9_dgvfix.off
// turns them off): see "dgVoodoo workarounds" below.
#include "csmt.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <intrin.h>
#include <cstdio>
#include <d3d9.h>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <psapi.h>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <windows.h>

// Everything but Direct3DCreate9 and Direct3DCreate9Ex goes straight to the real
// D3D9 (the loader resolves d3d9_chain.dll from the exe's folder).
#pragma comment(linker, "/EXPORT:Direct3DCreate9=_ProxyDirect3DCreate9@4")
#pragma comment(linker, "/EXPORT:Direct3DCreate9Ex=_ProxyDirect3DCreate9Ex@8")
#pragma comment(linker, "/EXPORT:LumaGetVertexConstants=_LumaGetVertexConstants@0")
#pragma comment(linker, \
   "/EXPORT:D3DPERF_BeginEvent=d3d9_chain.D3DPERF_BeginEvent")
#pragma comment(linker, "/EXPORT:D3DPERF_EndEvent=d3d9_chain.D3DPERF_EndEvent")
#pragma comment(linker, \
   "/EXPORT:D3DPERF_GetStatus=d3d9_chain.D3DPERF_GetStatus")
#pragma comment( \
   linker,       \
   "/EXPORT:D3DPERF_QueryRepeatFrame=d3d9_chain.D3DPERF_QueryRepeatFrame")
#pragma comment(linker, \
   "/EXPORT:D3DPERF_SetMarker=d3d9_chain.D3DPERF_SetMarker")
#pragma comment(linker, \
   "/EXPORT:D3DPERF_SetOptions=d3d9_chain.D3DPERF_SetOptions")
#pragma comment(linker, \
   "/EXPORT:D3DPERF_SetRegion=d3d9_chain.D3DPERF_SetRegion")
#pragma comment(linker, "/EXPORT:DebugSetMute=d3d9_chain.DebugSetMute")
#pragma comment( \
   linker,       \
   "/EXPORT:Direct3DShaderValidatorCreate9=d3d9_chain.Direct3DShaderValidatorCreate9")

namespace
{
   constexpr double MB = 1048576.0;

   // vtable slots, shared by Patch and Original.
   constexpr int SLOT_RELEASE = 2;  // IUnknown
   constexpr int SLOT_SET_LOD = 11; // IDirect3DBaseTexture9
   constexpr int SLOT_GET_LOD = 12;
   constexpr int SLOT_SURFACE_GET_DESC = 12; // IDirect3DSurface9
   constexpr int SLOT_SURFACE_LOCK_RECT = 13;
   constexpr int SLOT_SURFACE_UNLOCK_RECT = 14;
   constexpr int SLOT_GET_LEVEL_DESC =
      17;                        // IDirect3DTexture9, IDirect3DCubeTexture9
   constexpr int SLOT_LOCK = 19; // LockRect (2D, cube), LockBox (volume)
   constexpr int SLOT_UNLOCK = 20;
   constexpr int SLOT_CREATE_DEVICE = 16;    // IDirect3D9
   constexpr int SLOT_CREATE_DEVICE_EX = 20; // IDirect3D9Ex
   constexpr int SLOT_RESET = 16;            // IDirect3DDevice9
   constexpr int SLOT_CREATE_TEXTURE = 23;
   constexpr int SLOT_CREATE_VOLUME_TEXTURE = 24;
   constexpr int SLOT_CREATE_CUBE_TEXTURE = 25;
   constexpr int SLOT_CREATE_VERTEX_BUFFER = 26;
   constexpr int SLOT_CREATE_INDEX_BUFFER = 27;
   constexpr int SLOT_UPDATE_SURFACE = 30;
   constexpr int SLOT_UPDATE_TEXTURE = 31;

   wchar_t g_dir[MAX_PATH] = {};
   FILE* g_log = nullptr;              // "NUL" without d3d9_memlog.on
   bool g_log_enabled = false;         // d3d9_memlog.on present
   bool g_fix_requested = false;       // d3d9_vafix.on present
   bool g_strip_multithreaded = false; // d3d9_nomt.on present
   bool g_csmt_requested = false;      // d3d9_csmt.on present
   bool g_fix_active = false;          // the device really is D3D9Ex (one device assumed)
   bool g_user_memory = false;         // the runtime takes user-memory SYSTEMMEM textures
   UINT g_available_texture_mb =
      0; // what the runtime reports (dgVoodoo: its VRAM cap)

   HMODULE Chain()
   {
      static HMODULE chain = []
      {
         wchar_t path[MAX_PATH];
         swprintf_s(path, L"%sd3d9_chain.dll", g_dir);
         HMODULE module = LoadLibraryW(path);
         fprintf(g_log, "chain %ls -> %p (error %lu)\n", path, module,
            module ? 0 : GetLastError());
         fflush(g_log);
         return module;
      }();
      return chain;
   }
   // The product name in the chained runtime's version resource.
   bool ChainIsDgVoodoo()
   {
      static const bool dgvoodoo = []
      {
         HRSRC info = FindResourceW(Chain(), MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
         const auto* data = (const wchar_t*)(info ? LockResource(LoadResource(Chain(), info)) : nullptr);
         const std::wstring_view block(data, data ? SizeofResource(Chain(), info) / sizeof(wchar_t) : 0);
         return block.find(L"dgVoodoo") != std::wstring_view::npos;
      }();
      return dgvoodoo;
   }

   // Return addresses: the runtime validates calls such as UpdateSurface and
   // StretchRect by calling GetDesc through the vtable (Windows d3d9), and
   // reporting MANAGED to it makes it reject our own uploads, so only callers
   // outside the runtime get the game's view.
   bool InChainImage(const void* address)
   {
      static const auto [begin, end] = []
      {
         MODULEINFO info = {};
         GetModuleInformation(GetCurrentProcess(), Chain(), &info, sizeof(info));
         char* base = (char*)info.lpBaseOfDll;
         return std::pair{base, base + info.SizeOfImage};
      }();
      return address >= begin && address < end;
   }

   // ---- vtable hooks ----------------------------------------------------------

   // Originals per patched slot (dgVoodoo may use several classes per
   // interface). Append-only: readers scan without a lock, Patch publishes the
   // original before it swaps the slot.
   struct HookedSlot
   {
      void** slot;
      void* original;
   };
   constexpr int MAX_HOOKED_SLOTS = 128;
   HookedSlot g_hooked[MAX_HOOKED_SLOTS];
   std::atomic<int> g_hooked_count{0};
   std::mutex g_patch_mutex;

   // Windows d3d9 keeps the device's vtable inside the device, right after a
   // pointer to the static table it came from, and copies that table over it
   // again in BeginStateBlock and for every later device. Patch hooks the tables
   // it validated (append-only, like g_hooked).
   constexpr int MAX_SOURCE_TABLES = 4;
   void** g_source_tables[MAX_SOURCE_TABLES];
   std::atomic<int> g_source_table_count{0};

   // The hooked static table a vtable was copied from, or nullptr.
   void** HookedSourceTable(void** vtable)
   {
      const int count = g_source_table_count.load(std::memory_order_acquire);
      if (!count || InChainImage(vtable))
         return nullptr;
      void** source = (void**)vtable[-1];
      for (int i = 0; i < count; i++)
      {
         if (g_source_tables[i] == source)
            return source;
      }
      return nullptr;
   }

   template <typename T>
   T Original(void* object, int index)
   {
      void** vtable = *(void***)object;
      void** slot = &vtable[index];
      const int count = g_hooked_count.load(std::memory_order_acquire);
      for (int i = 0; i < count; i++)
      {
         if (g_hooked[i].slot == slot)
            return reinterpret_cast<T>(g_hooked[i].original);
      }
      // A later device's vtable, copied from a table we hooked: its slot isn't
      // recorded and may hold another module's hook that calls ours (apphelp.dll's
      // COM router on an exe's first run), the table's slot is.
      if (void** source = HookedSourceTable(vtable))
      {
         for (int i = 0; i < count; i++)
         {
            if (g_hooked[i].slot == &source[index])
               return reinterpret_cast<T>(g_hooked[i].original);
         }
      }
      // Never patched (GetDesc with the fix off, read by the LockRect hook): the
      // slot still holds the runtime's method. A Patch racing this read can
      // return the hook, which then finds its recorded original.
      return reinterpret_cast<T>(*slot);
   }

   void WriteSlot(void** slot, void* value)
   {
      DWORD old_protect;
      VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect);
      *slot = value;
      VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
   }

   void Patch(void* object, int index, void* hook)
   {
      void** vtable = *(void***)object;
      void** slot = &vtable[index];
      if (*slot == hook) // unlocked read; a stale value just takes the locked path
         return;
      std::lock_guard lock(g_patch_mutex);
      const int count = g_hooked_count.load(std::memory_order_relaxed);
      if (*slot == hook || count + 2 > MAX_HOOKED_SLOTS)
         return;
      for (int i = 0; i < count; i++)
      {
         if (g_hooked[i].slot == slot)
            return; // patched before; the CSMT layer may sit above our hook now
      }
      if (void** source = HookedSourceTable(vtable); source && source[index] == hook)
         return; // copied from a table we hooked: our hook is in this slot's chain
      // A slot the CSMT layer owns: our hook goes below it, so it runs in the
      // layer's order (on the worker for deferred calls).
      if (void* below = Csmt::InsertBelow(object, index, hook))
      {
         g_hooked[count] = {.slot = slot, .original = below};
         g_hooked_count.store(count + 1, std::memory_order_release);
         return;
      }
      g_hooked[count] = {.slot = slot, .original = *slot};
      g_hooked_count.store(count + 1, std::memory_order_release);
      WriteSlot(slot, hook);
      // Hook the static table of a Windows d3d9 device vtable too, or its copies
      // drop our hooks (MANAGED creates then reach D3D9Ex, which rejects them)
      // while other threads may already call in. Taken only if the pointer is a
      // table in the runtime's image that matches most of this vtable: the copy
      // differs in our hooks and in others' (apphelp.dll shims Release and
      // CreateAdditionalSwapChain on an exe's first run, overlays hook Present).
      constexpr int DEVICE9_METHODS = 119; // what BeginStateBlock copies
      if (InChainImage(vtable))
         return;
      void** source = (void**)vtable[-1];
      if (!InChainImage(source) || !InChainImage(source + DEVICE9_METHODS) || source[index] == hook)
         return;
      int same = 0;
      for (int i = 0; i < DEVICE9_METHODS; i++)
      {
         same += (vtable[i] == source[i]);
      }
      if (same < 90)
         return;
      g_hooked[count + 1] = {.slot = &source[index], .original = source[index]};
      g_hooked_count.store(count + 2, std::memory_order_release);
      // Known before any copy can carry the hook.
      const int tables = g_source_table_count.load(std::memory_order_relaxed);
      if (!HookedSourceTable(vtable) && tables < MAX_SOURCE_TABLES)
      {
         g_source_tables[tables] = source;
         g_source_table_count.store(tables + 1, std::memory_order_release);
      }
      WriteSlot(&source[index], hook);
   }

   // ---- counters and report
   // -----------------------------------------------------

   // Reset by every report. Atomic so neither the hooks nor the fix take a lock
   // just to count.
   enum Counter
   {
      MANAGED_LOCKS,    // locks of MANAGED textures (any kind)
      MANAGED_RELOCKS,  // a MANAGED texture locked more times than it has levels
      MANAGED_READONLY, // D3DLOCK_READONLY on MANAGED: the game reads a copy back
      MANAGED_PARTIAL,  // non-null rect/box
      SURFACE_MANAGED,  // IDirect3DSurface9::LockRect on a MANAGED surface (D3DX)
      SURFACE_READONLY,
      OTHER_LOCKS, // DEFAULT/SYSTEMMEM texture locks
      UPLOADS,
      UPLOAD_BYTES,
      UPLOAD_FAILURES,
      POOL_HITS,        // staging texture reused
      POOL_MISSES,      // staging texture created
      POOL_BUSY,        // pooled staging still read by the GPU
      POOL_EVICTIONS,   // over budget, released
      TOP_DOWN_VIEWS,   // level views mapped top-down
      OS_VIEWS,         // no MapViewOfFile3: placed by the OS
      ARENA_STAGING,    // staging memory from the arena
      TOP_DOWN_STAGING, // arena full: our staging memory top-down outside it
      RUNTIME_STAGING,  // allocated by the runtime (no user memory, tiny DXT chains)
      AUTOGEN,          // GenerateMipSubLevels after a level 0 upload
      SURFACE_SERVED,   // surface locks served from a shadow
      STALE_SURFACES,   // surface entries of a texture that died through a surface
      LOCK_FAILURES,
      SET_LOD_REPLACED,      // SetLOD on a replaced texture (DEFAULT ignores it)
      UPDATES_INTO_REPLACED, // game UpdateSurface/UpdateTexture into a replaced
                             // texture: its shadow goes stale
      COUNTER_COUNT,
   };
   std::atomic<uint64_t> g_counters[COUNTER_COUNT];

   void Count(Counter counter, uint64_t amount = 1)
   {
      g_counters[counter].fetch_add(amount, std::memory_order_relaxed);
   }

   // Live totals, kept current by create/release/map/unmap so a report never has
   // to walk the resources. Pool index = D3DPOOL (DEFAULT, MANAGED, SYSTEMMEM,
   // SCRATCH), as the game asked for it.
   struct Totals
   {
      std::atomic<int64_t> texture_bytes[4], texture_count[4], buffer_bytes[4];
      std::atomic<int64_t> rendertarget_bytes, shadow_bytes, shadow_count;
      std::atomic<int64_t> mapped_bytes, mapped_count, arena_bytes;
      std::atomic<uint64_t> resets, creates_failed, converted_volumes,
         converted_buffers, section_failures, shadow_fallbacks;
   };
   Totals g_totals;

   void Report(const char* event)
   {
      size_t free_total = 0, largest_free = 0, committed = 0;
      MEMORY_BASIC_INFORMATION mbi;
      for (char* p = nullptr; VirtualQuery(p, &mbi, sizeof(mbi));
         p = (char*)mbi.BaseAddress + mbi.RegionSize)
      {
         if (mbi.State == MEM_FREE)
         {
            free_total += mbi.RegionSize;
            largest_free = max(largest_free, mbi.RegionSize);
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
      uint64_t c[COUNTER_COUNT];
      for (int i = 0; i < COUNTER_COUNT; i++)
         c[i] = g_counters[i].exchange(0, std::memory_order_relaxed);
      const Totals& t = g_totals;
      SYSTEMTIME now;
      GetLocalTime(&now);
      fprintf(
         g_log,
         "%02u:%02u:%02u %-13s | VA commit %6.0f private %6.0f free %6.0f "
         "largest %5.0f | tex MANAGED %5lld %6.0f MB  DEFAULT %5lld %6.0f MB "
         "(RT/DS %5.0f)  SYSMEM %5.0f  SCRATCH %4.0f | buf MANAGED %5.0f "
         "DEFAULT %5.0f SYSMEM %4.0f | locks M %llu relock %llu RO %llu partial "
         "%llu surfM %llu surfRO %llu other %llu | resets %llu fails %llu\n",
         now.wHour, now.wMinute, now.wSecond, event, committed / MB,
         pmc.PrivateUsage / MB, free_total / MB, largest_free / MB,
         t.texture_count[1].load(), t.texture_bytes[1] / MB,
         t.texture_count[0].load(), t.texture_bytes[0] / MB,
         t.rendertarget_bytes / MB, t.texture_bytes[2] / MB,
         t.texture_bytes[3] / MB, t.buffer_bytes[1] / MB, t.buffer_bytes[0] / MB,
         t.buffer_bytes[2] / MB, c[MANAGED_LOCKS], c[MANAGED_RELOCKS],
         c[MANAGED_READONLY], c[MANAGED_PARTIAL], c[SURFACE_MANAGED],
         c[SURFACE_READONLY], c[OTHER_LOCKS], t.resets.load(),
         t.creates_failed.load());
      if (g_fix_active)
      {
         // With the fix every MANAGED texture and buffer is DEFAULT to the runtime.
         const double default_mb = (t.texture_bytes[0] + t.texture_bytes[1] +
                                      t.buffer_bytes[0] + t.buffer_bytes[1]) /
                                   MB;
         fprintf(
            g_log,
            "         fix | shadows %5lld %6.0f MB (outside VA), mapped levels now "
            "%lld %5.1f MB | uploads %llu %6.1f MB, failed %llu, autogen %llu | "
            "staging pool hit %llu miss %llu busy %llu evicted %llu | views "
            "top-down %llu os %llu, staging arena %llu top-down %llu runtime %llu, "
            "arena used %.1f MB | DEFAULT %.0f of %u MB | surface served %llu, "
            "stale surfaces %llu, lock failures %llu, set LOD %llu, updates into "
            "replaced %llu | converted volumes %llu buffers %llu, section "
            "failures %llu, shadow fallbacks %llu\n",
            t.shadow_count.load(), t.shadow_bytes / MB, t.mapped_count.load(),
            t.mapped_bytes / MB, c[UPLOADS], c[UPLOAD_BYTES] / MB,
            c[UPLOAD_FAILURES], c[AUTOGEN], c[POOL_HITS], c[POOL_MISSES],
            c[POOL_BUSY], c[POOL_EVICTIONS], c[TOP_DOWN_VIEWS], c[OS_VIEWS],
            c[ARENA_STAGING], c[TOP_DOWN_STAGING], c[RUNTIME_STAGING],
            t.arena_bytes / MB, default_mb, g_available_texture_mb,
            c[SURFACE_SERVED], c[STALE_SURFACES], c[LOCK_FAILURES],
            c[SET_LOD_REPLACED], c[UPDATES_INTO_REPLACED],
            t.converted_volumes.load(), t.converted_buffers.load(),
            t.section_failures.load(), t.shadow_fallbacks.load());
         // Below ~4 GB the reported value is a real cap (dgVoodoo: VRAM - 5 MB);
         // at 4095+ it is clamped and says nothing.
         static bool warned = false;
         if (!warned && g_available_texture_mb < 4000 &&
             default_mb > 0.9 * g_available_texture_mb)
         {
            warned = true;
            fprintf(g_log,
               "VA fix: WARNING DEFAULT resources at %.0f of %u MB: raise the "
               "runtime's video memory (dgVoodoo VRAM = 4096) or creates will "
               "fail\n",
               default_mb, g_available_texture_mb);
         }
      }
      fflush(g_log);
   }

   // Logged at once, with a full report: a game that can't handle the failure
   // (UE3 aborts) dies before the next tick.
   HRESULT CreateFailed(HRESULT hr, const char* kind, UINT width, UINT height,
      DWORD format, D3DPOOL asked, bool replaced, DWORD usage)
   {
      if (g_totals.creates_failed++ < 100)
      {
         fprintf(
            g_log,
            "Create%s failed 0x%08lX%s: %s %u%s%u format/fvf %lu usage 0x%lX pool "
            "%d%s\n",
            kind, hr, (hr == D3DERR_OUTOFVIDEOMEMORY) ? " (OUTOFVIDEOMEMORY)" : "",
            height ? "size" : "length", width, height ? "x" : "", height, format,
            usage, int(asked), replaced ? " -> DEFAULT (VA fix)" : "");
         Report("create failed");
      }
      return hr;
   }

   // ---- formats ---------------------------------------------------------------

   struct FormatInfo
   {
      UINT bytes; // per pixel, or per 4x4 block
      bool block;
   };

   FormatInfo GetFormatInfo(D3DFORMAT format)
   {
      switch (format)
      {
      case D3DFMT_DXT1:
      case MAKEFOURCC('A', 'T', 'I', '1'):
         return {.bytes = 8, .block = true};
      case D3DFMT_DXT2:
      case D3DFMT_DXT3:
      case D3DFMT_DXT4:
      case D3DFMT_DXT5:
      case MAKEFOURCC('A', 'T', 'I', '2'):
         return {.bytes = 16, .block = true};
      case D3DFMT_L8:
      case D3DFMT_A8:
      case D3DFMT_P8:
         return {.bytes = 1};
      case D3DFMT_R5G6B5:
      case D3DFMT_X1R5G5B5:
      case D3DFMT_A1R5G5B5:
      case D3DFMT_A4R4G4B4:
      case D3DFMT_A8L8:
      case D3DFMT_V8U8:
      case D3DFMT_L16:
      case D3DFMT_R16F:
      case D3DFMT_D16:
         return {.bytes = 2};
      case D3DFMT_A16B16G16R16:
      case D3DFMT_A16B16G16R16F:
      case D3DFMT_G32R32F:
         return {.bytes = 8};
      case D3DFMT_A32B32G32R32F:
         return {.bytes = 16};
      default:
         return {.bytes = 4};
      }
   }

   // One level, tightly packed (pitch = one row of pixels or of 4x4 blocks).
   struct LevelSize
   {
      UINT width, height, pitch, rows;
      size_t bytes;
   };

   LevelSize GetLevelSize(D3DFORMAT format, UINT width, UINT height, UINT level)
   {
      const FormatInfo info = GetFormatInfo(format);
      const UINT w = max(width >> level, 1u);
      const UINT h = max(height >> level, 1u);
      const UINT pitch =
         (info.block ? max((w + 3) / 4, 1u) * info.bytes : w * info.bytes);
      const UINT rows = (info.block ? max((h + 3) / 4, 1u) : h);
      return {.width = w,
         .height = h,
         .pitch = pitch,
         .rows = rows,
         .bytes = size_t(pitch) * rows};
   }

   uint64_t ChainBytes(D3DFORMAT format, UINT width, UINT height, UINT depth,
      UINT levels)
   {
      uint64_t total = 0;
      for (UINT level = 0; level < levels; level++)
      {
         total += uint64_t(GetLevelSize(format, width, height, level).bytes) *
                  max(depth >> level, 1u);
      }
      return total;
   }

   // ---- placement
   // ---------------------------------------------------------------

   using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T,
      ULONG, ULONG, MEM_EXTENDED_PARAMETER*,
      ULONG);

   // Top-down keeps the fix's views against the address space ceiling, away from
   // the middle where the game's heaps grow. MapViewOfFile has no top-down flag;
   // MapViewOfFile3 (Windows 10 1803+) takes MEM_TOP_DOWN.
   char* MapTopDown(HANDLE section, size_t offset, size_t size)
   {
      static const auto map_view3 = (MapViewOfFile3Fn)GetProcAddress(
         GetModuleHandleW(L"kernelbase.dll"), "MapViewOfFile3");
      if (map_view3)
      {
         if (void* view =
                map_view3(section, GetCurrentProcess(), nullptr, offset, size,
                   MEM_TOP_DOWN, PAGE_READWRITE, nullptr, 0))
         {
            Count(TOP_DOWN_VIEWS);
            return (char*)view;
         }
      }
      Count(OS_VIEWS);
      return (char*)MapViewOfFile(section, FILE_MAP_ALL_ACCESS,
         DWORD(uint64_t(offset) >> 32), DWORD(offset),
         size);
   }

   // The staging pool is the fix's only long-lived allocation, and pool entries
   // come and go, so allocated one by one they would leave holes near the
   // ceiling. They come from one reservation made at device creation instead:
   // pages are committed per block and decommitted on free, blocks are first fit
   // with neighbours merged. Callers know each block's size.
   class StagingArena
   {
   public:
      static constexpr size_t SIZE = 64 << 20; // = the pool budget
      static constexpr size_t PAGE = 4096;

      // Once per process: a second device reuses it.
      void Reserve()
      {
         std::lock_guard lock(mutex_);
         if (base_)
            return;
         base_ = (char*)VirtualAlloc(nullptr, SIZE, MEM_RESERVE | MEM_TOP_DOWN,
            PAGE_READWRITE);
         if (base_)
         {
            free_[0] = SIZE;
         }
      }

      char* Base()
      {
         std::lock_guard lock(mutex_);
         return base_;
      }

      void* Allocate(size_t size)
      {
         size = Round(size);
         std::lock_guard lock(mutex_);
         for (auto it = free_.begin(); it != free_.end(); ++it)
         {
            auto [offset, block] = *it;
            if (block < size)
               continue;
            if (!VirtualAlloc(base_ + offset, size, MEM_COMMIT, PAGE_READWRITE))
               return nullptr;
            free_.erase(it);
            if (block > size)
            {
               free_[offset + size] = block - size;
            }
            g_totals.arena_bytes += size;
            return base_ + offset;
         }
         return nullptr;
      }

      // False if the memory isn't from the arena.
      bool Free(void* memory, size_t size)
      {
         size = Round(size);
         std::lock_guard lock(mutex_);
         if (!base_ || memory < base_ || memory >= base_ + SIZE)
            return false;
         VirtualFree(memory, size, MEM_DECOMMIT);
         g_totals.arena_bytes -= size;
         size_t start = (char*)memory - base_;
         if (auto next = free_.find(start + size); next != free_.end())
         {
            size += next->second;
            free_.erase(next);
         }
         if (auto next = free_.lower_bound(start); next != free_.begin())
         {
            if (auto previous = std::prev(next);
               previous->first + previous->second == start)
            {
               start = previous->first;
               size += previous->second;
               free_.erase(previous);
            }
         }
         free_[start] = size;
         return true;
      }

   private:
      static size_t Round(size_t size)
      {
         return (size + PAGE - 1) & ~(PAGE - 1);
      }

      std::mutex mutex_;
      char* base_ = nullptr;
      std::map<size_t, size_t> free_; // offset -> size
   };
   StagingArena g_staging_arena;

   // Committed staging memory: the arena, else top-down outside it.
   void* AllocStagingMemory(size_t size)
   {
      if (void* memory = g_staging_arena.Allocate(size))
      {
         Count(ARENA_STAGING);
         return memory;
      }
      void* memory = VirtualAlloc(
         nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
      if (memory)
      {
         Count(TOP_DOWN_STAGING);
      }
      return memory;
   }

   void FreeStagingMemory(void* memory, size_t size)
   {
      if (!g_staging_arena.Free(memory, size))
      {
         VirtualFree(memory, 0, MEM_RELEASE);
      }
   }

   // ---- tracked resources -----------------------------------------------------

   // VA fix: CPU copy of a replaced texture. Levels (x faces) are packed with
   // tight pitch in one section; a level is mapped only while it is locked.
   struct Shadow
   {
      struct Level
      {
         size_t offset; // in the section
         LevelSize size;
         IDirect3DSurface9* surface; // the DEFAULT level, lives as long as the texture
         char* base = nullptr;       // view (granularity aligned), while locked
         char* bits = nullptr;       // the level inside the view
         // Locked from the first Lock until Unlock has uploaded and unmapped; a
         // second lock meanwhile fails, as the runtime's own would.
         bool locked = false;
         bool writable = false; // the lock was not READONLY: upload on unlock

         void Map(char* view, size_t level_offset_in_view)
         {
            base = view;
            bits = view + level_offset_in_view;
            g_totals.mapped_bytes += size.bytes;
            g_totals.mapped_count++;
         }

         // Returns the view to unmap, or nullptr.
         char* Unmap()
         {
            char* view = base;
            base = bits = nullptr;
            if (view)
            {
               g_totals.mapped_bytes -= size.bytes;
               g_totals.mapped_count--;
            }
            return view;
         }
      };
      HANDLE section = nullptr;
      size_t bytes = 0;
      D3DFORMAT format = D3DFMT_UNKNOWN;
      bool autogen = false;
      IDirect3DDevice9* device = nullptr; // outlives its resources
      UINT level_count = 0;
      std::vector<Level> levels; // face * level_count + level

      Level* Find(UINT face, UINT level)
      {
         const UINT sub = face * level_count + level;
         return (level < level_count && sub < levels.size()) ? &levels[sub]
                                                             : nullptr;
      }

      ~Shadow()
      {
         for (Level& level : levels)
         {
            if (char* view = level.Unmap())
            {
               UnmapViewOfFile(view);
            }
         }
         if (section)
         {
            CloseHandle(section);
         }
      }
   };

   struct Resource
   {
      bool buffer;
      D3DPOOL pool; // as the game asked for it
      DWORD usage;
      uint64_t bytes;
      UINT subresources; // levels x faces, 1 for buffers
      UINT locks = 0;
      std::unique_ptr<Shadow> shadow; // VA fix
      // Set by Track; a Release erases the entry only if it still has the value it
      // saw, since a concurrent Create may reuse the address once the object dies.
      uint64_t generation = 0;
   };

   struct SurfaceRef
   {
      void* texture;
      UINT face, level;
   };

   // mutex is a leaf lock: never held across a call into the D3D runtime (a
   // runtime may call our hooked methods while holding its own lock).
   struct State
   {
      std::mutex mutex;
      std::unordered_map<void*, Resource> resources;
      std::unordered_map<void*, SurfaceRef>
         surfaces; // levels of replaced textures
      uint64_t last_generation = 0;
   };
   State g_state;

   // Caller holds g_state.mutex.
   Resource* FindResource(void* object)
   {
      auto it = g_state.resources.find(object);
      return (it == g_state.resources.end()) ? nullptr : &it->second;
   }

   void AddTotals(const Resource& resource, int64_t sign)
   {
      const int pool = min(int(resource.pool), 3);
      const int64_t bytes = sign * int64_t(resource.bytes);
      if (resource.buffer)
      {
         g_totals.buffer_bytes[pool] += bytes;
         return;
      }
      g_totals.texture_bytes[pool] += bytes;
      g_totals.texture_count[pool] += sign;
      if (resource.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
      {
         g_totals.rendertarget_bytes += bytes;
      }
      if (resource.shadow)
      {
         g_totals.shadow_bytes += sign * int64_t(resource.shadow->bytes);
         g_totals.shadow_count += sign;
      }
   }

   // Caller holds g_state.mutex.
   void SetSurfaces(void* texture, const Shadow& shadow, bool add)
   {
      for (UINT sub = 0; sub < shadow.levels.size(); sub++)
      {
         IDirect3DSurface9* surface = shadow.levels[sub].surface;
         if (add)
         {
            g_state.surfaces[surface] = {.texture = texture,
               .face = sub / shadow.level_count,
               .level = sub % shadow.level_count};
         }
         else
         {
            g_state.surfaces.erase(surface);
         }
      }
   }

   // Caller holds g_state.mutex and erases the entry; the returned shadow is
   // destroyed (unmapped, closed) after the lock is released.
   std::unique_ptr<Shadow> Untrack(void* object, Resource* resource)
   {
      AddTotals(*resource, -1);
      if (resource->shadow)
      {
         SetSurfaces(object, *resource->shadow, false);
      }
      return std::move(resource->shadow);
   }

   ULONG STDMETHODCALLTYPE HookRelease(IUnknown* self)
   {
      uint64_t generation = 0; // 0 = untracked (our staging textures)
      {
         std::lock_guard lock(g_state.mutex);
         if (const Resource* resource = FindResource(self))
         {
            generation = resource->generation;
         }
      }
      const ULONG refs = Original<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>(
         self, SLOT_RELEASE)(self);
      if (refs != 0 || generation == 0)
         return refs;
      std::unique_ptr<Shadow> shadow; // destroyed after the lock
      std::lock_guard lock(g_state.mutex);
      if (auto it = g_state.resources.find(self);
         it != g_state.resources.end() && it->second.generation == generation)
      {
         shadow = Untrack(self, &it->second);
         g_state.resources.erase(it);
      }
      return refs;
   }

   // Common tail of every Create* hook.
   void Track(void* object, Resource resource)
   {
      Patch(object, SLOT_RELEASE, (void*)&HookRelease);
      std::unique_ptr<Shadow> stale; // destroyed after the lock
      std::lock_guard lock(g_state.mutex);
      resource.generation = ++g_state.last_generation;
      AddTotals(resource, +1);
      Resource* entry = FindResource(object);
      if (entry)
      {
         // An entry whose last reference went through a level surface's Release,
         // which isn't hooked.
         stale = Untrack(object, entry);
         *entry = std::move(resource);
      }
      else
      {
         entry =
            &g_state.resources.emplace(object, std::move(resource)).first->second;
      }
      if (entry->shadow)
      {
         SetSurfaces(object, *entry->shadow, true);
      }
   }

   // Drops a stale entry at the address of an object we create ourselves.
   void ForgetStale(void* object)
   {
      std::unique_ptr<Shadow> stale; // destroyed after the lock
      std::lock_guard lock(g_state.mutex);
      if (auto it = g_state.resources.find(object); it != g_state.resources.end())
      {
         stale = Untrack(object, &it->second);
         g_state.resources.erase(it);
      }
   }

   // Section and level table for a replaced texture; nullptr if the section can't
   // be created.
   std::unique_ptr<Shadow> CreateShadow(IDirect3DBaseTexture9* texture,
      IDirect3DDevice9* device, D3DFORMAT format,
      UINT width, UINT height, UINT faces,
      DWORD usage)
   {
      auto shadow = std::make_unique<Shadow>();
      shadow->format = format;
      shadow->autogen = usage & D3DUSAGE_AUTOGENMIPMAP;
      shadow->device = device;
      shadow->level_count = texture->GetLevelCount();
      for (UINT face = 0; face < faces; face++)
      {
         for (UINT level = 0; level < shadow->level_count; level++)
         {
            IDirect3DSurface9* surface = nullptr;
            if (faces == 1)
            {
               ((IDirect3DTexture9*)texture)->GetSurfaceLevel(level, &surface);
            }
            else
            {
               ((IDirect3DCubeTexture9*)texture)
                  ->GetCubeMapSurface(D3DCUBEMAP_FACES(face), level, &surface);
            }
            if (surface)
            {
               surface->Release(); // the texture keeps it alive
            }
            const LevelSize size = GetLevelSize(format, width, height, level);
            shadow->levels.push_back(
               {.offset = shadow->bytes, .size = size, .surface = surface});
            shadow->bytes += size.bytes;
         }
      }
      shadow->section =
         CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            DWORD(shadow->bytes), nullptr);
      if (!shadow->section)
      {
         g_totals.section_failures++;
         return nullptr;
      }
      return shadow;
   }

   // ---- staging pool ----------------------------------------------------------

   struct StagingKey
   {
      IDirect3DDevice9* device;
      D3DFORMAT format;
      UINT width, height, levels;
      bool operator==(const StagingKey&) const = default;
   };

   // SYSTEMMEM staging textures, reused across uploads. They live in the address
   // space, so the pool is capped; the least recently used go first. Entries with
   // the same key are interchangeable and the pool is small, so a list is enough.
   class StagingPool
   {
   public:
      static constexpr size_t BUDGET = StagingArena::SIZE;
      static constexpr size_t MAX_ENTRIES = 256; // tiny levels still reserve 64 KB

      struct Entry
      {
         StagingKey key;
         IDirect3DTexture9* texture;
         size_t bytes;
         void* memory; // ours (user-memory SYSTEMMEM texture, bytes long) or nullptr
      };

      std::optional<Entry> Acquire(const StagingKey& key)
      {
         std::lock_guard lock(mutex_);
         auto it = std::find_if(lru_.begin(), lru_.end(), [&](const Entry& entry)
            { return entry.key == key; });
         if (it == lru_.end())
            return std::nullopt;
         Entry entry = *it;
         bytes_ -= entry.bytes;
         lru_.erase(it);
         return entry;
      }

      void Put(const Entry& entry)
      {
         std::vector<Entry> evicted;
         {
            std::lock_guard lock(mutex_);
            lru_.push_front(entry);
            bytes_ += entry.bytes;
            while (bytes_ > BUDGET || lru_.size() > MAX_ENTRIES)
            {
               evicted.push_back(lru_.back());
               bytes_ -= lru_.back().bytes;
               lru_.pop_back();
            }
         }
         for (const Entry& old : evicted)
            Destroy(old);
         Count(POOL_EVICTIONS, evicted.size());
      }

      size_t Size()
      {
         std::lock_guard lock(mutex_);
         return lru_.size();
      }

      void Flush()
      {
         std::list<Entry> entries;
         {
            std::lock_guard lock(mutex_);
            entries.swap(lru_);
            bytes_ = 0;
         }
         for (const Entry& entry : entries)
            Destroy(entry);
      }

      static void Destroy(const Entry& entry)
      {
         if (entry.memory)
         {
            // Pending UpdateSurface reads of our memory must finish before its pages
            // are decommitted or reused; a blocking write lock waits for them (a
            // READONLY one needn't).
            if (D3DLOCKED_RECT lr;
               SUCCEEDED(entry.texture->LockRect(0, &lr, nullptr, 0)))
            {
               entry.texture->UnlockRect(0);
            }
         }
         entry.texture->Release();
         if (entry.memory)
         {
            FreeStagingMemory(entry.memory, entry.bytes);
         }
      }

   private:
      std::mutex mutex_;
      std::list<Entry> lru_; // front = most recently returned
      size_t bytes_ = 0;
   };
   StagingPool g_staging_pool;

   using CreateTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT,
      UINT, UINT, DWORD,
      D3DFORMAT, D3DPOOL,
      IDirect3DTexture9**,
      HANDLE*);
   using CreateCubeTextureFn = HRESULT(STDMETHODCALLTYPE*)(
      IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
      IDirect3DCubeTexture9**, HANDLE*);
   using UpdateSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,
      IDirect3DSurface9*,
      const RECT*,
      IDirect3DSurface9*,
      const POINT*);

   // A new staging texture. D3D9Ex takes a single-level SYSTEMMEM texture over
   // our memory (passed through pSharedHandle), so it can live in the arena.
   std::optional<StagingPool::Entry> CreateStaging(const StagingKey& key)
   {
      auto create = Original<CreateTextureFn>(key.device, SLOT_CREATE_TEXTURE);
      const size_t bytes =
         size_t(ChainBytes(key.format, key.width, key.height, 1, key.levels));
      IDirect3DTexture9* texture = nullptr;
      if (key.levels == 1 && g_user_memory)
      {
         if (void* memory = AllocStagingMemory(bytes))
         {
            HANDLE user_memory = memory;
            if (SUCCEEDED(create(key.device, key.width, key.height, 1, 0, key.format,
                   D3DPOOL_SYSTEMMEM, &texture, &user_memory)))
            {
               ForgetStale(texture);
               return StagingPool::Entry{
                  .key = key, .texture = texture, .bytes = bytes, .memory = memory};
            }
            FreeStagingMemory(memory, bytes);
         }
      }
      if (FAILED(create(key.device, key.width, key.height, key.levels, 0,
             key.format, D3DPOOL_SYSTEMMEM, &texture, nullptr)))
         return std::nullopt;
      ForgetStale(texture);
      Count(RUNTIME_STAGING);
      return StagingPool::Entry{
         .key = key, .texture = texture, .bytes = bytes, .memory = nullptr};
   }

   // Copies one shadow level into the DEFAULT texture's surface.
   bool Upload(IDirect3DDevice9* device, IDirect3DSurface9* destination,
      D3DFORMAT format, const LevelSize& size, const char* bits)
   {
      // DXT levels under 4x4 can't be a top level: stage the smallest chain whose
      // top is at least 4x4 and use its matching level.
      UINT staging_level = 0;
      while (GetFormatInfo(format).block && ((size.width << staging_level) < 4 ||
                                               (size.height << staging_level) < 4))
         staging_level++;
      const StagingKey key = {.device = device,
         .format = format,
         .width = size.width << staging_level,
         .height = size.height << staging_level,
         .levels = staging_level + 1};
      D3DLOCKED_RECT lr = {};
      HRESULT hr = E_FAIL;
      std::optional<StagingPool::Entry> staging = g_staging_pool.Acquire(key);
      if (staging)
      {
         hr = staging->texture->LockRect(staging_level, &lr, nullptr,
            D3DLOCK_DONOTWAIT);
         if (SUCCEEDED(hr))
         {
            Count(POOL_HITS);
         }
         else
         {
            // Still read by the GPU: back to the pool. Any other failure: unusable.
            if (hr == D3DERR_WASSTILLDRAWING)
            {
               Count(POOL_BUSY);
               g_staging_pool.Put(*staging);
            }
            else
            {
               StagingPool::Destroy(*staging);
            }
            staging.reset();
         }
      }
      if (!staging)
      {
         Count(POOL_MISSES);
         staging = CreateStaging(key);
         if (!staging)
            return false;
         hr = staging->texture->LockRect(staging_level, &lr, nullptr, 0);
      }
      bool ok = false;
      if (SUCCEEDED(hr))
      {
         if (UINT(lr.Pitch) == size.pitch)
         {
            memcpy(lr.pBits, bits, size.bytes);
         }
         else
         {
            for (UINT row = 0; row < size.rows; row++)
            {
               memcpy((char*)lr.pBits + size_t(row) * lr.Pitch,
                  bits + size_t(row) * size.pitch, size.pitch);
            }
         }
         staging->texture->UnlockRect(staging_level);
         if (IDirect3DSurface9* surface = nullptr;
            SUCCEEDED(staging->texture->GetSurfaceLevel(staging_level, &surface)))
         {
            ok = SUCCEEDED(Original<UpdateSurfaceFn>(device, SLOT_UPDATE_SURFACE)(
               device, surface, nullptr, destination, nullptr));
            surface->Release();
         }
      }
      g_staging_pool.Put(*staging);
      return ok;
   }

   // ---- locks -----------------------------------------------------------------

   // Set while a texture lock is forwarded to the runtime: the Windows runtime
   // locks the level's surface inside, and that nested call needs no lookup.
   thread_local bool t_in_texture_call = false;

   struct TextureCallScope
   {
      TextureCallScope()
      {
         t_in_texture_call = true;
      }
      ~TextureCallScope()
      {
         t_in_texture_call = false;
      }
   };

   // Caller holds g_state.mutex. nullptr = untracked (our staging textures).
   void CountTextureLock(Resource* resource, DWORD flags, bool partial)
   {
      if (!resource || resource->pool != D3DPOOL_MANAGED)
      {
         Count(OTHER_LOCKS);
         return;
      }
      Count(MANAGED_LOCKS);
      if (resource->locks++ >= resource->subresources)
      {
         Count(MANAGED_RELOCKS);
      }
      if (flags & D3DLOCK_READONLY)
      {
         Count(MANAGED_READONLY);
      }
      if (partial)
      {
         Count(MANAGED_PARTIAL);
      }
   }

   void FillLocked(const Shadow& shadow, const Shadow::Level& level,
      const RECT* rect, D3DLOCKED_RECT* locked)
   {
      size_t offset = 0;
      if (rect)
      {
         const FormatInfo info = GetFormatInfo(shadow.format);
         offset = (info.block ? size_t(rect->top / 4) * level.size.pitch +
                                   size_t(rect->left / 4) * info.bytes
                              : size_t(rect->top) * level.size.pitch +
                                   size_t(rect->left) * info.bytes);
      }
      locked->pBits = level.bits + offset;
      locked->Pitch = int(level.size.pitch);
   }

   // Counts a texture lock (count_lock) and, for a replaced texture, serves it
   // from the shadow. S_FALSE = not replaced: the caller forwards it.
   HRESULT LockSubresource(void* texture, UINT face, UINT level,
      D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags,
      bool count_lock)
   {
      static const DWORD granularity = []
      {
         SYSTEM_INFO info;
         GetSystemInfo(&info);
         return info.dwAllocationGranularity;
      }();
      Shadow* shadow = nullptr;
      Shadow::Level* target = nullptr;
      {
         std::lock_guard lock(g_state.mutex);
         Resource* resource = FindResource(texture);
         if (count_lock)
         {
            CountTextureLock(resource, flags, rect != nullptr);
         }
         shadow = (resource ? resource->shadow.get() : nullptr);
         if (!shadow)
            return S_FALSE;
         target = shadow->Find(face, level);
         if (!target || !locked || target->locked)
         {
            Count(LOCK_FAILURES);
            return D3DERR_INVALIDCALL;
         }
         target->locked = true;
      }
      // Map without holding the global lock; the level is ours until Unlock.
      const size_t aligned = target->offset - target->offset % granularity;
      char* view = MapTopDown(shadow->section, aligned,
         target->offset - aligned + target->size.bytes);
      std::lock_guard lock(g_state.mutex);
      if (!view)
      {
         target->locked = false;
         Count(LOCK_FAILURES);
         return D3DERR_INVALIDCALL;
      }
      target->Map(view, target->offset - aligned);
      if (!(flags & D3DLOCK_READONLY))
      {
         target->writable = true;
      }
      FillLocked(*shadow, *target, rect, locked);
      return S_OK;
   }

   // Uploads the level if the lock wrote it, then unmaps it and unlocks.
   // S_FALSE = not replaced.
   HRESULT UnlockSubresource(IDirect3DBaseTexture9* texture, UINT face,
      UINT level)
   {
      Shadow* shadow = nullptr;
      Shadow::Level* target = nullptr;
      bool upload = false;
      {
         std::lock_guard lock(g_state.mutex);
         Resource* resource = FindResource(texture);
         shadow = (resource ? resource->shadow.get() : nullptr);
         if (!shadow)
            return S_FALSE;
         target = shadow->Find(face, level);
         if (!target || !target->locked)
            return D3DERR_INVALIDCALL;
         upload = target->writable;
         target->writable = false;
      }
      bool retry = false;
      if (upload)
      {
         if (Upload(shadow->device, target->surface, shadow->format, target->size,
                target->bits))
         {
            Count(UPLOADS);
            Count(UPLOAD_BYTES, target->size.bytes);
            if (level == 0 && shadow->autogen)
            {
               texture->GenerateMipSubLevels();
               Count(AUTOGEN);
            }
         }
         else
         {
            Count(UPLOAD_FAILURES);
            retry = true; // the next unlock of this level tries again
         }
      }
      char* view = nullptr;
      {
         std::lock_guard lock(g_state.mutex);
         if (retry)
         {
            target->writable = true;
         }
         view = target->Unmap();
         target->locked = false;
      }
      UnmapViewOfFile(view);
      return S_OK;
   }

   // The replaced level a surface is, checked against the surface's container: an
   // entry can outlive its texture when the last reference went through a level
   // surface's Release, and a new surface can then get its address.
   std::optional<SurfaceRef> ReplacedSurface(IDirect3DSurface9* surface)
   {
      std::optional<SurfaceRef> ref;
      {
         std::lock_guard lock(g_state.mutex);
         if (auto it = g_state.surfaces.find(surface);
            it != g_state.surfaces.end())
         {
            ref = it->second;
         }
      }
      if (!ref)
         return std::nullopt;
      IDirect3DBaseTexture9* container = nullptr;
      surface->GetContainer(IID_IDirect3DBaseTexture9, (void**)&container);
      if (container)
      {
         container->Release(); // still referenced by the surface
      }
      if (container == ref->texture)
         return ref;
      Count(STALE_SURFACES);
      std::lock_guard lock(g_state.mutex);
      if (auto it = g_state.surfaces.find(surface);
         it != g_state.surfaces.end() && it->second.texture == ref->texture)
      {
         g_state.surfaces.erase(it);
      }
      return std::nullopt;
   }

   HRESULT STDMETHODCALLTYPE HookSurfaceGetDesc(IDirect3DSurface9* self,
      D3DSURFACE_DESC* desc)
   {
      const HRESULT hr = Original<decltype(&HookSurfaceGetDesc)>(
         self, SLOT_SURFACE_GET_DESC)(self, desc);
      if (SUCCEEDED(hr) && !InChainImage(_ReturnAddress()))
      {
         std::lock_guard lock(g_state.mutex);
         if (g_state.surfaces.contains(self))
         {
            desc->Pool = D3DPOOL_MANAGED; // as the game created it (D3DX then locks)
         }
      }
      return hr;
   }

   HRESULT STDMETHODCALLTYPE HookSurfaceLockRect(IDirect3DSurface9* self,
      D3DLOCKED_RECT* locked,
      const RECT* rect, DWORD flags)
   {
      auto original =
         Original<decltype(&HookSurfaceLockRect)>(self, SLOT_SURFACE_LOCK_RECT);
      if (t_in_texture_call)
         return original(self, locked, rect, flags);
      const std::optional<SurfaceRef> ref = ReplacedSurface(self);
      if (D3DSURFACE_DESC desc;
         ref || (SUCCEEDED(Original<decltype(&HookSurfaceGetDesc)>(
                    self, SLOT_SURFACE_GET_DESC)(self, &desc)) &&
                   desc.Pool == D3DPOOL_MANAGED))
      {
         Count(SURFACE_MANAGED);
         if (flags & D3DLOCK_READONLY)
         {
            Count(SURFACE_READONLY);
         }
      }
      if (!ref)
         return original(self, locked, rect, flags);
      Count(SURFACE_SERVED);
      return LockSubresource(ref->texture, ref->face, ref->level, locked, rect,
         flags, false);
   }

   HRESULT STDMETHODCALLTYPE HookSurfaceUnlockRect(IDirect3DSurface9* self)
   {
      auto original = Original<decltype(&HookSurfaceUnlockRect)>(
         self, SLOT_SURFACE_UNLOCK_RECT);
      if (t_in_texture_call)
         return original(self);
      if (std::optional<SurfaceRef> ref = ReplacedSurface(self))
         return UnlockSubresource((IDirect3DBaseTexture9*)ref->texture, ref->face,
            ref->level);
      return original(self);
   }

   HRESULT STDMETHODCALLTYPE HookTextureLockRect(IDirect3DTexture9* self,
      UINT level,
      D3DLOCKED_RECT* locked,
      const RECT* rect, DWORD flags)
   {
      if (const HRESULT hr =
             LockSubresource(self, 0, level, locked, rect, flags, true);
         hr != S_FALSE)
         return hr;
      TextureCallScope scope;
      return Original<decltype(&HookTextureLockRect)>(self, SLOT_LOCK)(
         self, level, locked, rect, flags);
   }

   HRESULT STDMETHODCALLTYPE HookTextureUnlockRect(IDirect3DTexture9* self,
      UINT level)
   {
      if (const HRESULT hr = UnlockSubresource(self, 0, level); hr != S_FALSE)
         return hr;
      TextureCallScope scope;
      return Original<decltype(&HookTextureUnlockRect)>(self, SLOT_UNLOCK)(self,
         level);
   }

   HRESULT STDMETHODCALLTYPE HookCubeLockRect(IDirect3DCubeTexture9* self,
      D3DCUBEMAP_FACES face, UINT level,
      D3DLOCKED_RECT* locked,
      const RECT* rect, DWORD flags)
   {
      if (const HRESULT hr =
             LockSubresource(self, UINT(face), level, locked, rect, flags, true);
         hr != S_FALSE)
         return hr;
      TextureCallScope scope;
      return Original<decltype(&HookCubeLockRect)>(self, SLOT_LOCK)(
         self, face, level, locked, rect, flags);
   }

   HRESULT STDMETHODCALLTYPE HookCubeUnlockRect(IDirect3DCubeTexture9* self,
      D3DCUBEMAP_FACES face,
      UINT level)
   {
      if (const HRESULT hr = UnlockSubresource(self, UINT(face), level);
         hr != S_FALSE)
         return hr;
      TextureCallScope scope;
      return Original<decltype(&HookCubeUnlockRect)>(self, SLOT_UNLOCK)(self, face,
         level);
   }

   HRESULT STDMETHODCALLTYPE HookVolumeLockBox(IDirect3DVolumeTexture9* self,
      UINT level, D3DLOCKED_BOX* locked,
      const D3DBOX* box, DWORD flags)
   {
      {
         std::lock_guard lock(g_state.mutex);
         CountTextureLock(FindResource(self), flags, box != nullptr);
      }
      return Original<decltype(&HookVolumeLockBox)>(self, SLOT_LOCK)(
         self, level, locked, box, flags);
   }

   // Replaced textures still report MANAGED, as the game created them. Same slot
   // and signature for 2D and cube textures.
   HRESULT STDMETHODCALLTYPE HookGetLevelDesc(IDirect3DBaseTexture9* self,
      UINT level, D3DSURFACE_DESC* desc)
   {
      const HRESULT hr = Original<decltype(&HookGetLevelDesc)>(
         self, SLOT_GET_LEVEL_DESC)(self, level, desc);
      if (SUCCEEDED(hr) && !InChainImage(_ReturnAddress()))
      {
         std::lock_guard lock(g_state.mutex);
         if (Resource* resource = FindResource(self); resource && resource->shadow)
         {
            desc->Pool = D3DPOOL_MANAGED;
         }
      }
      return hr;
   }

   // Counted only: SetLOD caps the mips of a MANAGED texture and is ignored on
   // DEFAULT, so a game that relies on it would see full detail.
   DWORD STDMETHODCALLTYPE HookSetLOD(IDirect3DBaseTexture9* self, DWORD lod)
   {
      {
         std::lock_guard lock(g_state.mutex);
         if (Resource* resource = FindResource(self); resource && resource->shadow)
         {
            Count(SET_LOD_REPLACED);
         }
      }
      return Original<decltype(&HookSetLOD)>(self, SLOT_SET_LOD)(self, lod);
   }

   // Takes the reference GetSurfaceLevel/GetCubeMapSurface returned.
   void PatchSurfaces(IDirect3DSurface9* surface)
   {
      if (!surface)
         return;
      Patch(surface, SLOT_SURFACE_LOCK_RECT, (void*)&HookSurfaceLockRect);
      Patch(surface, SLOT_SURFACE_UNLOCK_RECT, (void*)&HookSurfaceUnlockRect);
      if (g_fix_active)
      {
         Patch(surface, SLOT_SURFACE_GET_DESC, (void*)&HookSurfaceGetDesc);
      }
      surface->Release();
   }

   // ---- dgVoodoo workarounds ---------------------------------------------------
   // D3D9 behaviour dgVoodoo 2.87.6 gets wrong (docs/dgVoodoo-RE.md "Agent conformance run", repros in
   // _tools/dgv_conformance). On whenever the chained runtime is dgVoodoo, off with d3d9_dgvfix.off next to this dll.
   // Each is temporary: drop it once a dgVoodoo release fixes the bug. The hooks sit below the CSMT layer, so they run on
   // its worker; calls back into the device go straight to dgVoodoo's methods ("DgvMethods"), never through the vtable
   // top that the layer owns.

   constexpr int SLOT_GET_DISPLAY_MODE = 8; // IDirect3DDevice9
   constexpr int SLOT_GET_SWAP_CHAIN = 14;
   constexpr int SLOT_SET_GAMMA_RAMP = 21;
   constexpr int SLOT_GET_GAMMA_RAMP = 22;
   constexpr int SLOT_SET_RENDER_TARGET = 37;
   constexpr int SLOT_SET_RENDER_STATE = 57;
   constexpr int SLOT_CREATE_STATE_BLOCK = 59;
   constexpr int SLOT_BEGIN_STATE_BLOCK = 60;
   constexpr int SLOT_END_STATE_BLOCK = 61;
   constexpr int SLOT_GET_TEXTURE = 64;
   constexpr int SLOT_SET_TEXTURE = 65;
   constexpr int SLOT_GET_TEXTURE_STAGE_STATE = 66;
   constexpr int SLOT_SET_TEXTURE_STAGE_STATE = 67;
   constexpr int SLOT_GET_SAMPLER_STATE = 68;
   constexpr int SLOT_SET_SAMPLER_STATE = 69;
   constexpr int SLOT_DRAW_PRIMITIVE = 81; // then DrawIndexedPrimitive, DrawPrimitiveUP, DrawIndexedPrimitiveUP
   constexpr int SLOT_CREATE_VERTEX_SHADER = 91;
   constexpr int SLOT_GET_PRESENT_PARAMETERS = 9; // IDirect3DSwapChain9
   constexpr int SLOT_STATE_BLOCK_CAPTURE = 4;    // IDirect3DStateBlock9
   constexpr int SLOT_STATE_BLOCK_APPLY = 5;

   constexpr DWORD PIXEL_SAMPLERS = 16;
   constexpr DWORD VERTEX_SAMPLERS = 4; // D3DVERTEXTEXTURESAMPLER0..3
   constexpr DWORD SAMPLER_STATES = 14; // D3DSAMP_ADDRESSU (1) .. D3DSAMP_DMAPOFFSET (13), index 0 unused
   // What a fresh device reports per state (Windows d3d9): wrap, border 0, point min/mag, no mip, aniso 1
   constexpr DWORD DEFAULT_SAMPLER_STATES[SAMPLER_STATES] = {0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0};

   bool g_dgv_fixes_disabled = false; // d3d9_dgvfix.off present
   bool g_dgv_fixes = false;          // on for this device (chain is dgVoodoo)

   // dgVoodoo's own device methods, taken before the CSMT layer patches the vtable.
   struct DgvMethods
   {
      HRESULT(STDMETHODCALLTYPE* set_render_state)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD) = nullptr;
      HRESULT(STDMETHODCALLTYPE* get_texture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9**) = nullptr;
      HRESULT(STDMETHODCALLTYPE* set_texture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*) = nullptr;
      HRESULT(STDMETHODCALLTYPE* get_texture_stage_state)(IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD*) = nullptr;
      HRESULT(STDMETHODCALLTYPE* set_texture_stage_state)(IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) = nullptr;
      HRESULT(STDMETHODCALLTYPE* set_sampler_state)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD) = nullptr;
   };

   // Texture stages a D3DSBT_ALL block restores: the pixel samplers, then the vertex samplers.
   constexpr DWORD STATE_BLOCK_STAGES = PIXEL_SAMPLERS + VERTEX_SAMPLERS;
   DWORD StateBlockStage(DWORD i)
   {
      return i < PIXEL_SAMPLERS ? i : D3DVERTEXTEXTURESAMPLER0 + (i - PIXEL_SAMPLERS);
   }

   struct StateBlockFix
   {
      D3DSTATEBLOCKTYPE type = D3DSBT_ALL;
      IDirect3DBaseTexture9* textures[STATE_BLOCK_STAGES] = {}; // D3DSBT_ALL: referenced, as the runtime's block would
   };

   // What the game asked for in CreateDevice / Reset, as GetPresentParameters must report it.
   struct AppPresentation
   {
      IDirect3DDevice9* device = nullptr;
      UINT interval = D3DPRESENT_INTERVAL_DEFAULT;
      bool unknown_format = false;
      D3DFORMAT display_format = D3DFMT_UNKNOWN;
   };

   struct DgvState
   {
      std::shared_mutex mutex;
      IDirect3DDevice9* device = nullptr; // one device assumed, as for the rest of this proxy
      DgvMethods methods;
      std::unordered_map<IDirect3DSwapChain9*, AppPresentation> swap_chains; // each device's implicit swap chain
      IDirect3DSurface9* bound_targets[4] = {};                              // RT1..3 as the game bound them (not referenced: the device holds them)
      std::vector<IDirect3DSurface9*> unbound_targets;                       // referenced until the next draw
      DWORD vertex_samplers[VERTEX_SAMPLERS][SAMPLER_STATES] = {};
      std::unordered_map<IDirect3DStateBlock9*, StateBlockFix> state_blocks;
      std::unordered_map<UINT, D3DGAMMARAMP> gamma_ramps;             // per swap chain index, as the game set them
      std::unordered_map<IDirect3DBaseTexture9*, DWORD> texture_lods; // SetLOD of MANAGED 2D and cube textures
      // Tracked once a texture LOD was set (g_dgv_texture_lod_set), per STATE_BLOCK_STAGES index:
      IDirect3DBaseTexture9* stage_textures[STATE_BLOCK_STAGES] = {}; // as bound (not referenced)
      DWORD max_mip_levels[STATE_BLOCK_STAGES] = {};                  // the game's MAXMIPLEVEL
      DWORD applied_max_mip_levels[STATE_BLOCK_STAGES] = {};          // what dgVoodoo holds
   };
   DgvState g_dgv;
   std::atomic<bool> g_dgv_unbound_pending{false};
   std::atomic<bool> g_dgv_texture_lod_set{false};
   std::atomic<uint32_t> g_dgv_max_mip_level_stages{0}; // STATE_BLOCK_STAGES bits a draw must recompute
   constexpr uint32_t ALL_STAGES = (1u << STATE_BLOCK_STAGES) - 1;
   std::atomic<bool> g_dgv_recording{false}; // between BeginStateBlock and EndStateBlock: sets are recorded, not applied

   // Index into the STATE_BLOCK_STAGES arrays, or -1.
   int StageIndex(DWORD sampler)
   {
      if (sampler < PIXEL_SAMPLERS)
         return int(sampler);
      if (sampler >= D3DVERTEXTEXTURESAMPLER0 && sampler < D3DVERTEXTEXTURESAMPLER0 + VERTEX_SAMPLERS)
         return int(PIXEL_SAMPLERS + sampler - D3DVERTEXTEXTURESAMPLER0);
      return -1;
   }
   std::atomic<bool> g_dgv_premultiplied_blend_set{false}; // a stage op was ever set to BLENDTEXTUREALPHAPM

   void ResetVertexSamplers()
   {
      for (auto& sampler : g_dgv.vertex_samplers)
      {
         std::copy(std::begin(DEFAULT_SAMPLER_STATES), std::end(DEFAULT_SAMPLER_STATES), sampler);
      }
   }

   // A fresh or Reset dgVoodoo device starts POINTSIZE_MIN at 0 and POINTSCALE_B/C at 1 (documented: 1, 0, 0), which
   // shrinks scaled points that rely on the defaults.
   void SetDocumentedPointDefaults(IDirect3DDevice9* device)
   {
      const float one = 1.0f, zero = 0.0f;
      g_dgv.methods.set_render_state(device, D3DRS_POINTSIZE_MIN, *(const DWORD*)&one);
      g_dgv.methods.set_render_state(device, D3DRS_POINTSCALE_B, *(const DWORD*)&zero);
      g_dgv.methods.set_render_state(device, D3DRS_POINTSCALE_C, *(const DWORD*)&zero);
   }

   // Releasing a surface right after unbinding it from RT1..3, with no draw in between, crashes inside dgVoodoo (its
   // cached render-target set still points at it until the next draw). The unbound surface is kept alive until then.
   HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface)
   {
      const HRESULT hr = Original<decltype(&HookSetRenderTarget)>(self, SLOT_SET_RENDER_TARGET)(self, index, surface);
      if (FAILED(hr) || index == 0 || index >= std::size(g_dgv.bound_targets))
         return hr;
      std::unique_lock lock(g_dgv.mutex);
      IDirect3DSurface9*& bound = g_dgv.bound_targets[index];
      if (bound && bound != surface)
      {
         bound->AddRef();
         g_dgv.unbound_targets.push_back(bound);
         g_dgv_unbound_pending.store(true, std::memory_order_release);
      }
      bound = surface;
      return hr;
   }

   void ReleaseUnboundTargets()
   {
      if (!g_dgv_unbound_pending.exchange(false, std::memory_order_acq_rel))
         return;
      std::vector<IDirect3DSurface9*> surfaces;
      {
         std::unique_lock lock(g_dgv.mutex);
         surfaces.swap(g_dgv.unbound_targets);
      }
      for (IDirect3DSurface9* surface : surfaces)
      {
         surface->Release();
      }
   }

   // vs_2_0+ "sgn dst, src0, scratch1, scratch2" returns 1 for every input (verify_sh_ops). Rewritten as
   // "slt scratch1, -src0, src0; slt scratch2, src0, -src0; add dst, scratch1, -scratch2". Left as sgn when predicated
   // or when a scratch register is src0 itself. Returns empty when the shader has no sgn to rewrite.
   std::vector<DWORD> ReplaceSgn(const DWORD* function)
   {
      if (!function || D3DSHADER_VERSION_MAJOR(function[0]) < 2)
         return {};
      constexpr DWORD REGISTER_MASK = D3DSP_REGNUM_MASK | D3DSP_REGTYPE_MASK | D3DSP_REGTYPE_MASK2;
      auto operand_length = [](const DWORD* token)
      { return (*token & D3DSHADER_ADDRESSMODE_MASK) == D3DSHADER_ADDRMODE_RELATIVE ? 2u : 1u; };
      std::vector<DWORD> rewritten = {function[0]};
      bool changed = false;
      const DWORD* token = function + 1;
      while (*token != D3DVS_END())
      {
         const DWORD opcode = *token & D3DSI_OPCODE_MASK;
         const DWORD length = opcode == D3DSIO_COMMENT ? (*token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT
                                                       : (*token & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
         if (opcode == D3DSIO_SGN && !(*token & D3DSHADER_INSTRUCTION_PREDICATED))
         {
            const DWORD* dst = token + 1;
            const DWORD dst_length = operand_length(dst);
            const DWORD* src = dst + dst_length;
            const DWORD src_length = operand_length(src);
            const DWORD scratch[2] = {src[src_length] & REGISTER_MASK, src[src_length + 1] & REGISTER_MASK};
            DWORD negated = *src & ~D3DSP_SRCMOD_MASK;
            switch (*src & D3DSP_SRCMOD_MASK)
            {
            case D3DSPSM_NONE:
               negated |= D3DSPSM_NEG;
               break;
            case D3DSPSM_NEG:
               negated |= D3DSPSM_NONE;
               break;
            case D3DSPSM_ABS:
               negated |= D3DSPSM_ABSNEG;
               break;
            case D3DSPSM_ABSNEG:
               negated |= D3DSPSM_ABS;
               break;
            default:
               negated = 0;
               break; // not valid on a vertex shader source
            }
            const DWORD source_register = *src & REGISTER_MASK;
            if (dst_length + src_length + 2 == length && negated && scratch[0] != source_register && scratch[1] != source_register)
            {
               const DWORD write_mask = *dst & D3DSP_WRITEMASK_ALL;
               const DWORD slt = D3DSIO_SLT | ((1 + 2 * src_length) << D3DSI_INSTLENGTH_SHIFT);
               auto append_source = [&](DWORD first)
               {
                  rewritten.push_back(first);
                  rewritten.insert(rewritten.end(), src + 1, src + src_length);
               };
               rewritten.insert(rewritten.end(), {slt, 0x80000000 | scratch[0] | write_mask});
               append_source(negated);
               append_source(*src);
               rewritten.insert(rewritten.end(), {slt, 0x80000000 | scratch[1] | write_mask});
               append_source(*src);
               append_source(negated);
               rewritten.push_back(D3DSIO_ADD | ((dst_length + 2) << D3DSI_INSTLENGTH_SHIFT));
               rewritten.insert(rewritten.end(), dst, dst + dst_length);
               rewritten.insert(rewritten.end(), {0x80000000 | scratch[0] | D3DSP_NOSWIZZLE, 0x80000000 | scratch[1] | D3DSP_NOSWIZZLE | D3DSPSM_NEG});
               changed = true;
               token += 1 + length;
               continue;
            }
         }
         rewritten.insert(rewritten.end(), token, token + 1 + length);
         token += 1 + length;
      }
      if (!changed)
         return {};
      rewritten.push_back(D3DVS_END());
      return rewritten;
   }

   // The game's GetFunction then returns the rewritten code.
   HRESULT STDMETHODCALLTYPE HookCreateVertexShader(IDirect3DDevice9* self, const DWORD* function, IDirect3DVertexShader9** shader)
   {
      const std::vector<DWORD> rewritten = ReplaceSgn(function);
      return Original<decltype(&HookCreateVertexShader)>(self, SLOT_CREATE_VERTEX_SHADER)(self, rewritten.empty() ? function : rewritten.data(), shader);
   }

   // GetGammaRamp returns the ramp divided by 257 (8-bit values): answered from what was set.
   void STDMETHODCALLTYPE HookSetGammaRamp(IDirect3DDevice9* self, UINT swap_chain, DWORD flags, const D3DGAMMARAMP* ramp)
   {
      Original<decltype(&HookSetGammaRamp)>(self, SLOT_SET_GAMMA_RAMP)(self, swap_chain, flags, ramp);
      if (!ramp)
         return;
      std::unique_lock lock(g_dgv.mutex);
      g_dgv.gamma_ramps[swap_chain] = *ramp;
   }

   void STDMETHODCALLTYPE HookGetGammaRamp(IDirect3DDevice9* self, UINT swap_chain, D3DGAMMARAMP* ramp)
   {
      Original<decltype(&HookGetGammaRamp)>(self, SLOT_GET_GAMMA_RAMP)(self, swap_chain, ramp);
      std::shared_lock lock(g_dgv.mutex);
      if (const auto it = g_dgv.gamma_ramps.find(swap_chain); ramp && it != g_dgv.gamma_ramps.end())
      {
         *ramp = it->second;
      }
   }

   // dgVoodoo rejects GetSamplerState on D3DVERTEXTEXTURESAMPLER0..3 while accepting SetSamplerState: the getter answers
   // from what was set.
   bool IsVertexSampler(DWORD sampler)
   {
      return sampler >= D3DVERTEXTEXTURESAMPLER0 && sampler < D3DVERTEXTEXTURESAMPLER0 + VERTEX_SAMPLERS;
   }

   HRESULT STDMETHODCALLTYPE HookSetSamplerState(IDirect3DDevice9* self, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
   {
      const HRESULT hr = Original<decltype(&HookSetSamplerState)>(self, SLOT_SET_SAMPLER_STATE)(self, sampler, type, value);
      if (SUCCEEDED(hr) && IsVertexSampler(sampler) && type >= D3DSAMP_ADDRESSU && type <= D3DSAMP_DMAPOFFSET)
      {
         std::unique_lock lock(g_dgv.mutex);
         g_dgv.vertex_samplers[sampler - D3DVERTEXTEXTURESAMPLER0][type] = value;
      }
      if (const int index = StageIndex(sampler); SUCCEEDED(hr) && type == D3DSAMP_MAXMIPLEVEL && index >= 0 && g_dgv_texture_lod_set.load(std::memory_order_relaxed) && !g_dgv_recording.load(std::memory_order_relaxed))
      {
         std::unique_lock lock(g_dgv.mutex);
         g_dgv.max_mip_levels[index] = value;
         g_dgv.applied_max_mip_levels[index] = value;
         g_dgv_max_mip_level_stages.fetch_or(1u << index, std::memory_order_relaxed);
      }
      return hr;
   }

   HRESULT STDMETHODCALLTYPE HookGetSamplerState(IDirect3DDevice9* self, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value)
   {
      // dgVoodoo may hold a MAXMIPLEVEL raised to a texture's LOD.
      if (const int index = StageIndex(sampler); type == D3DSAMP_MAXMIPLEVEL && index >= 0 && value && g_dgv_texture_lod_set.load(std::memory_order_relaxed))
      {
         std::shared_lock lock(g_dgv.mutex);
         *value = g_dgv.max_mip_levels[index];
         return D3D_OK;
      }
      const HRESULT hr = Original<decltype(&HookGetSamplerState)>(self, SLOT_GET_SAMPLER_STATE)(self, sampler, type, value);
      if (SUCCEEDED(hr) || !value || !IsVertexSampler(sampler) || type < D3DSAMP_ADDRESSU || type > D3DSAMP_DMAPOFFSET)
         return hr;
      std::shared_lock lock(g_dgv.mutex);
      *value = g_dgv.vertex_samplers[sampler - D3DVERTEXTEXTURESAMPLER0][type];
      return D3D_OK;
   }

   // BLENDTEXTUREALPHAPM computes arg1 + arg2 * (1 + Ta) instead of arg1 + arg2 * (1 - Ta). MULTIPLYADD
   // arg0 + arg1 * arg2 with arg2 = inverted texture alpha gives the documented result (verify_ff). Swapped in for the
   // draw only, so getters and state blocks keep seeing the game's states.
   struct StageOp
   {
      D3DTEXTURESTAGESTATETYPE op, arg0, arg1, arg2;
   };
   constexpr StageOp STAGE_OPS[] = {{D3DTSS_COLOROP, D3DTSS_COLORARG0, D3DTSS_COLORARG1, D3DTSS_COLORARG2},
      {D3DTSS_ALPHAOP, D3DTSS_ALPHAARG0, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2}};
   constexpr DWORD TEXTURE_STAGES = 8;

   struct ReplacedBlend
   {
      DWORD stage = 0;
      const StageOp* op = nullptr;
      DWORD args[3] = {}; // the game's arg0..arg2
   };

   // Returns how many entries of "replaced" (TEXTURE_STAGES * 2) it filled.
   int ReplacePremultipliedBlends(IDirect3DDevice9* device, ReplacedBlend* replaced)
   {
      auto get = g_dgv.methods.get_texture_stage_state;
      auto set = g_dgv.methods.set_texture_stage_state;
      int count = 0;
      for (DWORD stage = 0; stage < TEXTURE_STAGES; stage++)
      {
         DWORD color_op = D3DTOP_DISABLE;
         get(device, stage, D3DTSS_COLOROP, &color_op);
         if (color_op == D3DTOP_DISABLE)
            break;
         for (const StageOp& op : STAGE_OPS)
         {
            DWORD value = D3DTOP_DISABLE;
            get(device, stage, op.op, &value);
            if (value != D3DTOP_BLENDTEXTUREALPHAPM)
               continue;
            ReplacedBlend& entry = replaced[count++];
            entry = {.stage = stage, .op = &op};
            get(device, stage, op.arg0, &entry.args[0]);
            get(device, stage, op.arg1, &entry.args[1]);
            get(device, stage, op.arg2, &entry.args[2]);
            set(device, stage, op.op, D3DTOP_MULTIPLYADD);
            set(device, stage, op.arg0, entry.args[1]);
            set(device, stage, op.arg1, entry.args[2]);
            set(device, stage, op.arg2, D3DTA_TEXTURE | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE);
         }
      }
      return count;
   }

   HRESULT STDMETHODCALLTYPE HookSetTextureStageState(IDirect3DDevice9* self, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
   {
      if (value == D3DTOP_BLENDTEXTUREALPHAPM && (type == D3DTSS_COLOROP || type == D3DTSS_ALPHAOP))
      {
         g_dgv_premultiplied_blend_set.store(true, std::memory_order_relaxed);
      }
      return Original<decltype(&HookSetTextureStageState)>(self, SLOT_SET_TEXTURE_STAGE_STATE)(self, stage, type, value);
   }

   // dgVoodoo's SetLOD and GetLOD are stubs returning 0. The LOD is kept here and applied by raising the bound stage's
   // MAXMIPLEVEL to it (dgVoodoo's sampler MinLOD is max(MAXMIPLEVEL, its own texture LOD, always 0)). Windows only
   // honors it on MANAGED textures and clamps it to the last level. Volumes are not covered. Once a LOD is set, the
   // bindings and the game's MAXMIPLEVEL are tracked and dgVoodoo only gets a change when a draw needs one; state blocks
   // see the game's values (restored before Capture and Apply, read back after Apply).

   DWORD STDMETHODCALLTYPE HookDgvGetLOD(IDirect3DBaseTexture9* self)
   {
      std::shared_lock lock(g_dgv.mutex);
      const auto it = g_dgv.texture_lods.find(self);
      return it != g_dgv.texture_lods.end() ? it->second : 0;
   }

   // Reads the bindings and MAXMIPLEVEL back from dgVoodoo, which holds the game's values at the call sites.
   void SyncLodStages(IDirect3DDevice9* device)
   {
      IDirect3DBaseTexture9* textures[STATE_BLOCK_STAGES] = {};
      DWORD levels[STATE_BLOCK_STAGES] = {};
      auto get_sampler_state = Original<decltype(&HookGetSamplerState)>(device, SLOT_GET_SAMPLER_STATE);
      for (DWORD i = 0; i < STATE_BLOCK_STAGES; i++)
      {
         if (SUCCEEDED(g_dgv.methods.get_texture(device, StateBlockStage(i), &textures[i])) && textures[i])
         {
            textures[i]->Release(); // kept unreferenced, as the device holds it
         }
         if (i < PIXEL_SAMPLERS)
         {
            get_sampler_state(device, i, D3DSAMP_MAXMIPLEVEL, &levels[i]);
         }
      }
      std::unique_lock lock(g_dgv.mutex);
      for (DWORD i = 0; i < STATE_BLOCK_STAGES; i++)
      {
         g_dgv.stage_textures[i] = textures[i];
         g_dgv.max_mip_levels[i] = i < PIXEL_SAMPLERS ? levels[i] : g_dgv.vertex_samplers[i - PIXEL_SAMPLERS][D3DSAMP_MAXMIPLEVEL];
         g_dgv.applied_max_mip_levels[i] = g_dgv.max_mip_levels[i];
      }
      g_dgv_max_mip_level_stages.fetch_or(ALL_STAGES, std::memory_order_relaxed);
   }

   // For the "stages" bits: game_values puts the game's MAXMIPLEVEL back (before a state block reads or overwrites
   // dgVoodoo's); otherwise each stage is raised to its texture's LOD. Only stages that change reach dgVoodoo.
   void ApplyMaxMipLevels(IDirect3DDevice9* device, uint32_t stages, bool game_values)
   {
      DWORD changed[STATE_BLOCK_STAGES][2]; // index, value
      int count = 0;
      {
         std::unique_lock lock(g_dgv.mutex);
         for (; stages; stages &= stages - 1)
         {
            const DWORD i = DWORD(std::countr_zero(stages));
            DWORD level = g_dgv.max_mip_levels[i];
            if (!game_values && g_dgv.stage_textures[i])
            {
               if (const auto it = g_dgv.texture_lods.find(g_dgv.stage_textures[i]); it != g_dgv.texture_lods.end())
               {
                  level = (std::max)(level, it->second);
               }
            }
            if (level != g_dgv.applied_max_mip_levels[i])
            {
               g_dgv.applied_max_mip_levels[i] = level;
               changed[count][0] = i;
               changed[count++][1] = level;
            }
         }
      }
      for (int i = 0; i < count; i++)
      {
         g_dgv.methods.set_sampler_state(device, StateBlockStage(changed[i][0]), D3DSAMP_MAXMIPLEVEL, changed[i][1]);
      }
   }

   DWORD STDMETHODCALLTYPE HookDgvSetLOD(IDirect3DBaseTexture9* self, DWORD lod)
   {
      if (g_fix_active)
      {
         HookSetLOD(self, lod); // its count; dgVoodoo's stub does nothing
      }
      bool managed = false;
      {
         std::lock_guard lock(g_state.mutex);
         const Resource* resource = FindResource(self);
         managed = resource && resource->pool == D3DPOOL_MANAGED;
      }
      if (!managed)
         return 0;
      lod = (std::min)(lod, self->GetLevelCount() - 1);
      if (lod && !g_dgv_texture_lod_set.load(std::memory_order_relaxed))
      {
         SyncLodStages(g_dgv.device);
         g_dgv_texture_lod_set.store(true, std::memory_order_relaxed);
      }
      g_dgv_max_mip_level_stages.fetch_or(ALL_STAGES, std::memory_order_relaxed);
      std::unique_lock lock(g_dgv.mutex);
      return std::exchange(g_dgv.texture_lods[self], lod);
   }

   HRESULT STDMETHODCALLTYPE HookSetTexture(IDirect3DDevice9* self, DWORD sampler, IDirect3DBaseTexture9* texture)
   {
      const HRESULT hr = Original<decltype(&HookSetTexture)>(self, SLOT_SET_TEXTURE)(self, sampler, texture);
      if (const int index = StageIndex(sampler); SUCCEEDED(hr) && index >= 0 && g_dgv_texture_lod_set.load(std::memory_order_relaxed) && !g_dgv_recording.load(std::memory_order_relaxed))
      {
         std::unique_lock lock(g_dgv.mutex);
         g_dgv.stage_textures[index] = texture;
         g_dgv_max_mip_level_stages.fetch_or(1u << index, std::memory_order_relaxed);
      }
      return hr;
   }

   HRESULT STDMETHODCALLTYPE HookBeginStateBlock(IDirect3DDevice9* self)
   {
      const HRESULT hr = Original<decltype(&HookBeginStateBlock)>(self, SLOT_BEGIN_STATE_BLOCK)(self);
      g_dgv_recording.store(SUCCEEDED(hr), std::memory_order_relaxed);
      return hr;
   }

   template <int slot, typename... Args>
   HRESULT STDMETHODCALLTYPE HookDraw(IDirect3DDevice9* self, Args... args)
   {
      ReplacedBlend replaced[TEXTURE_STAGES * 2];
      const int replaced_count = g_dgv_premultiplied_blend_set.load(std::memory_order_relaxed) ? ReplacePremultipliedBlends(self, replaced) : 0;
      if (const uint32_t stages = g_dgv_texture_lod_set.load(std::memory_order_relaxed) ? g_dgv_max_mip_level_stages.exchange(0, std::memory_order_relaxed) : 0)
      {
         ApplyMaxMipLevels(self, stages, false);
      }
      const HRESULT hr = Original<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, Args...)>(self, slot)(self, args...);
      for (int i = 0; i < replaced_count; i++)
      {
         const ReplacedBlend& entry = replaced[i];
         auto set = g_dgv.methods.set_texture_stage_state;
         set(self, entry.stage, entry.op->op, D3DTOP_BLENDTEXTUREALPHAPM);
         set(self, entry.stage, entry.op->arg0, entry.args[0]);
         set(self, entry.stage, entry.op->arg1, entry.args[1]);
         set(self, entry.stage, entry.op->arg2, entry.args[2]);
      }
      ReleaseUnboundTargets();
      return hr;
   }

   // A windowed device reports the desktop refresh rate (must be 0, and Reset then rejects the copy), IMMEDIATE as DEFAULT,
   // and A8R8G8B8 for a D3DFMT_UNKNOWN back buffer: the implicit swap chain reports what the game asked for.
   HRESULT STDMETHODCALLTYPE HookGetPresentParameters(IDirect3DSwapChain9* self, D3DPRESENT_PARAMETERS* params)
   {
      const HRESULT hr = Original<decltype(&HookGetPresentParameters)>(self, SLOT_GET_PRESENT_PARAMETERS)(self, params);
      std::shared_lock lock(g_dgv.mutex);
      const auto it = g_dgv.swap_chains.find(self);
      if (FAILED(hr) || !params || it == g_dgv.swap_chains.end())
         return hr;
      const AppPresentation& app = it->second;
      params->PresentationInterval = app.interval;
      if (params->Windowed)
      {
         params->FullScreen_RefreshRateInHz = 0;
         if (app.unknown_format)
         {
            params->BackBufferFormat = app.display_format;
         }
      }
      return hr;
   }

   // D3DSBT_ALL blocks restore no textures; D3DSBT_VERTEXSTATE Apply writes zeros into the pixel samplers' states, which
   // that block type must not touch.
   void CaptureTextures(IDirect3DDevice9* device, StateBlockFix* fix, std::vector<IDirect3DBaseTexture9*>* released)
   {
      for (DWORD i = 0; i < STATE_BLOCK_STAGES; i++)
      {
         released->push_back(fix->textures[i]);
         fix->textures[i] = nullptr;
         g_dgv.methods.get_texture(device, StateBlockStage(i), &fix->textures[i]); // referenced
      }
   }

   void ReleaseTextures(const std::vector<IDirect3DBaseTexture9*>& textures)
   {
      for (IDirect3DBaseTexture9* texture : textures)
      {
         if (texture)
         {
            texture->Release();
         }
      }
   }

   HRESULT STDMETHODCALLTYPE HookStateBlockCapture(IDirect3DStateBlock9* self)
   {
      if (g_dgv_texture_lod_set.load(std::memory_order_relaxed))
      {
         ApplyMaxMipLevels(g_dgv.device, ALL_STAGES, true);
         g_dgv_max_mip_level_stages.fetch_or(ALL_STAGES, std::memory_order_relaxed);
      }
      const HRESULT hr = Original<decltype(&HookStateBlockCapture)>(self, SLOT_STATE_BLOCK_CAPTURE)(self);
      std::vector<IDirect3DBaseTexture9*> released;
      {
         std::unique_lock lock(g_dgv.mutex);
         if (auto it = g_dgv.state_blocks.find(self); SUCCEEDED(hr) && it != g_dgv.state_blocks.end() && it->second.type == D3DSBT_ALL)
         {
            CaptureTextures(g_dgv.device, &it->second, &released);
         }
      }
      ReleaseTextures(released);
      return hr;
   }

   HRESULT ApplyStateBlock(IDirect3DStateBlock9* self)
   {
      auto apply = Original<HRESULT(STDMETHODCALLTYPE*)(IDirect3DStateBlock9*)>(self, SLOT_STATE_BLOCK_APPLY);
      std::optional<StateBlockFix> fix;
      {
         std::shared_lock lock(g_dgv.mutex);
         if (auto it = g_dgv.state_blocks.find(self); it != g_dgv.state_blocks.end())
         {
            fix = it->second; // the textures stay referenced by the block's own entry
         }
      }
      if (!fix)
         return apply(self);
      IDirect3DDevice9* device = g_dgv.device;
      if (fix->type == D3DSBT_VERTEXSTATE)
      {
         auto get = Original<decltype(&HookGetSamplerState)>(device, SLOT_GET_SAMPLER_STATE);
         auto set = Original<decltype(&HookSetSamplerState)>(device, SLOT_SET_SAMPLER_STATE);
         DWORD saved[PIXEL_SAMPLERS][SAMPLER_STATES] = {};
         for (DWORD sampler = 0; sampler < PIXEL_SAMPLERS; sampler++)
         {
            for (DWORD type = D3DSAMP_ADDRESSU; type < D3DSAMP_DMAPOFFSET; type++) // DMAPOFFSET is vertex state
            {
               get(device, sampler, D3DSAMPLERSTATETYPE(type), &saved[sampler][type]);
            }
         }
         const HRESULT hr = apply(self);
         for (DWORD sampler = 0; sampler < PIXEL_SAMPLERS; sampler++)
         {
            for (DWORD type = D3DSAMP_ADDRESSU; type < D3DSAMP_DMAPOFFSET; type++)
            {
               set(device, sampler, D3DSAMPLERSTATETYPE(type), saved[sampler][type]);
            }
            // The Apply also drops the stage's texture binding for draws while GetTexture still reports it: rebinding
            // through null restores it (verify_sbstate).
            IDirect3DBaseTexture9* texture = nullptr;
            if (SUCCEEDED(g_dgv.methods.get_texture(device, sampler, &texture)) && texture)
            {
               g_dgv.methods.set_texture(device, sampler, nullptr);
               g_dgv.methods.set_texture(device, sampler, texture);
               texture->Release();
            }
         }
         return hr;
      }
      const HRESULT hr = apply(self);
      for (DWORD i = 0; SUCCEEDED(hr) && i < STATE_BLOCK_STAGES; i++)
      {
         g_dgv.methods.set_texture(device, StateBlockStage(i), fix->textures[i]);
      }
      return hr;
   }

   HRESULT STDMETHODCALLTYPE HookStateBlockApply(IDirect3DStateBlock9* self)
   {
      const bool lods = g_dgv_texture_lod_set.load(std::memory_order_relaxed);
      if (lods)
      {
         ApplyMaxMipLevels(g_dgv.device, ALL_STAGES, true);
      }
      const HRESULT hr = ApplyStateBlock(self);
      if (lods)
      {
         SyncLodStages(g_dgv.device);
      }
      return hr;
   }

   ULONG STDMETHODCALLTYPE HookStateBlockRelease(IDirect3DStateBlock9* self)
   {
      const ULONG refs = Original<decltype(&HookStateBlockRelease)>(self, SLOT_RELEASE)(self);
      if (refs != 0)
         return refs;
      std::vector<IDirect3DBaseTexture9*> released;
      {
         std::unique_lock lock(g_dgv.mutex);
         if (auto it = g_dgv.state_blocks.find(self); it != g_dgv.state_blocks.end())
         {
            released.assign(std::begin(it->second.textures), std::end(it->second.textures));
            g_dgv.state_blocks.erase(it);
         }
      }
      ReleaseTextures(released);
      return refs;
   }

   // Every block type: they all capture and apply MAXMIPLEVEL.
   void PatchStateBlock(IDirect3DStateBlock9* block)
   {
      Patch(block, SLOT_STATE_BLOCK_CAPTURE, (void*)&HookStateBlockCapture);
      Patch(block, SLOT_STATE_BLOCK_APPLY, (void*)&HookStateBlockApply);
      Patch(block, SLOT_RELEASE, (void*)&HookStateBlockRelease);
   }

   HRESULT STDMETHODCALLTYPE HookEndStateBlock(IDirect3DDevice9* self, IDirect3DStateBlock9** block)
   {
      const HRESULT hr = Original<decltype(&HookEndStateBlock)>(self, SLOT_END_STATE_BLOCK)(self, block);
      g_dgv_recording.store(false, std::memory_order_relaxed);
      if (SUCCEEDED(hr) && block && *block)
      {
         PatchStateBlock(*block);
      }
      return hr;
   }

   HRESULT STDMETHODCALLTYPE HookCreateStateBlock(IDirect3DDevice9* self, D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** block)
   {
      if (g_dgv_texture_lod_set.load(std::memory_order_relaxed))
      {
         ApplyMaxMipLevels(self, ALL_STAGES, true);
         g_dgv_max_mip_level_stages.fetch_or(ALL_STAGES, std::memory_order_relaxed);
      }
      const HRESULT hr = Original<decltype(&HookCreateStateBlock)>(self, SLOT_CREATE_STATE_BLOCK)(self, type, block);
      if (FAILED(hr) || !block || !*block)
         return hr;
      PatchStateBlock(*block);
      if (type != D3DSBT_ALL && type != D3DSBT_VERTEXSTATE)
         return hr;
      std::vector<IDirect3DBaseTexture9*> released;
      std::unique_lock lock(g_dgv.mutex);
      StateBlockFix& fix = g_dgv.state_blocks[*block];
      fix.type = type;
      if (type == D3DSBT_ALL)
      {
         CaptureTextures(self, &fix, &released); // nothing captured before: "released" stays null
      }
      return hr;
   }

   // UpdateTexture skips every level where the source's level-0 size shifted by the level reaches 0 on an axis
   // (non-square textures lose their smallest mips); those levels are copied here. 2D textures only: volumes have no
   // UpdateSurface, cubes are square.
   void CopySkippedLevels(IDirect3DDevice9* device, IDirect3DBaseTexture9* source, IDirect3DBaseTexture9* destination)
   {
      if (source->GetType() != D3DRTYPE_TEXTURE || destination->GetType() != D3DRTYPE_TEXTURE)
         return;
      auto* source_2d = static_cast<IDirect3DTexture9*>(source);
      auto* destination_2d = static_cast<IDirect3DTexture9*>(destination);
      D3DSURFACE_DESC top;
      if (FAILED(source_2d->GetLevelDesc(0, &top)))
         return;
      const DWORD skipped = source_2d->GetLevelCount() - destination_2d->GetLevelCount(); // D3D9 matches the smallest levels
      auto update_surface = Original<UpdateSurfaceFn>(device, SLOT_UPDATE_SURFACE);
      for (DWORD level = 0; level < destination_2d->GetLevelCount(); level++)
      {
         const DWORD source_level = level + skipped;
         if ((top.Width >> source_level) != 0 && (top.Height >> source_level) != 0)
            continue;
         IDirect3DSurface9* from = nullptr;
         IDirect3DSurface9* to = nullptr;
         if (SUCCEEDED(source_2d->GetSurfaceLevel(source_level, &from)) && SUCCEEDED(destination_2d->GetSurfaceLevel(level, &to)))
         {
            update_surface(device, from, nullptr, to, nullptr);
         }
         if (from)
         {
            from->Release();
         }
         if (to)
         {
            to->Release();
         }
      }
   }

   // dgVoodoo's D3D11 sampler caches (FUN_5f37e6b4 and FUN_5f3768af in 2.87.6, one per backend path) hash filter,
   // address modes, anisotropy and comparison, then match a bucket entry on MipLODBias, BorderColor[0], BorderColor[2]
   // and MaxLOD only. A desc that differs in MinLOD (MAXMIPLEVEL) or border green/alpha gets the cached sampler, so
   // MAXMIPLEVEL never takes effect (the desc's MinLOD is right; no new CreateSamplerState follows). Each compare is
   // redirected to code comparing all seven fields the entry stores. In memory only, when the original bytes of every
   // site match; the file stays as shipped.
   void PatchSamplerCacheCompare()
   {
      struct Site
      {
         DWORD compare_rva, match_rva, next_entry_rva;
         BYTE desc_modrm; // mov eax, [desc + d8]: 0x46 esi, 0x43 ebx
         BYTE entry_sib;  // cmp eax, [edx + entry + d8]: 0x1a ebx, 0x3a edi
         BYTE original[37];
      };
      static constexpr Site SITES[] = {
         {0xbe6fc, 0xbe731, 0xbe72b, 0x46, 0x1a,
            {0x8b, 0x46, 0x10, 0x3b, 0x04, 0x1a, 0x75, 0x27, 0x8b, 0x46, 0x1c, 0x3b, 0x44, 0x1a, 0x04, 0x75, 0x1e, 0x8b, 0x46,
               0x24, 0x3b, 0x44, 0x1a, 0x0c, 0x75, 0x15, 0x8b, 0x46, 0x30, 0x3b, 0x44, 0x1a, 0x14, 0x74, 0x12, 0xeb, 0x0a}},
         {0xb6921, 0xb6956, 0xb6950, 0x43, 0x3a,
            {0x8b, 0x43, 0x10, 0x3b, 0x04, 0x3a, 0x75, 0x27, 0x8b, 0x43, 0x1c, 0x3b, 0x44, 0x3a, 0x04, 0x75, 0x1e, 0x8b, 0x43,
               0x24, 0x3b, 0x44, 0x3a, 0x0c, 0x75, 0x15, 0x8b, 0x43, 0x30, 0x3b, 0x44, 0x3a, 0x14, 0x74, 0x12, 0xeb, 0x0a}},
      };
      // {desc offset, entry offset}: MipLODBias, BorderColor[0..3], MinLOD, MaxLOD
      static constexpr BYTE FIELDS[][2] = {{0x10, 0x00}, {0x1c, 0x04}, {0x20, 0x08}, {0x24, 0x0c}, {0x28, 0x10}, {0x2c, 0x18}, {0x30, 0x14}};
      static std::once_flag once;
      std::call_once(once, []
         {
            BYTE* base = (BYTE*)Chain();
            MODULEINFO info = {};
            GetModuleInformation(GetCurrentProcess(), Chain(), &info, sizeof(info));
            for (const Site& site : SITES)
            {
               if (info.SizeOfImage < site.compare_rva + sizeof(site.original) || memcmp(base + site.compare_rva, site.original, sizeof(site.original)) != 0)
               {
                  fprintf(g_log, "dgVoodoo sampler cache compare not patched (unknown build)\n");
                  return;
               }
            }
            constexpr size_t FIELD_CODE = 13; // mov eax, [desc + d8]; cmp eax, [edx + entry + d8]; jne rel32
            constexpr size_t SITE_CODE = std::size(FIELDS) * FIELD_CODE + 10;
            auto* cave = (BYTE*)VirtualAlloc(nullptr, std::size(SITES) * SITE_CODE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!cave)
               return;
            BYTE* at = cave;
            auto emit_rel32 = [&](const BYTE* target)
            {
               const int32_t rel = int32_t(target - (at + 4));
               memcpy(at, &rel, 4);
               at += 4;
            };
            for (const Site& site : SITES)
            {
               const BYTE* no_match = at + std::size(FIELDS) * FIELD_CODE + 5;
               for (const auto& [desc, entry] : FIELDS)
               {
                  for (const BYTE b : {BYTE(0x8b), site.desc_modrm, desc, BYTE(0x3b), BYTE(0x44), site.entry_sib, entry, BYTE(0x0f), BYTE(0x85)})
                  {
                     *at++ = b;
                  }
                  emit_rel32(no_match);
               }
               *at++ = 0xe9;
               emit_rel32(base + site.match_rva);
               *at++ = 0xe9;
               emit_rel32(base + site.next_entry_rva);
            }
            DWORD old_protect;
            VirtualProtect(cave, std::size(SITES) * SITE_CODE, PAGE_EXECUTE_READ, &old_protect);
            for (size_t i = 0; i < std::size(SITES); i++)
            {
               BYTE* compare = base + SITES[i].compare_rva;
               BYTE jump[sizeof(Site::original)];
               std::fill(std::begin(jump), std::end(jump), BYTE(0x90)); // nop
               jump[0] = 0xe9;
               const int32_t rel = int32_t(cave + i * SITE_CODE - (compare + 5));
               memcpy(jump + 1, &rel, 4);
               VirtualProtect(compare, sizeof(jump), PAGE_EXECUTE_READWRITE, &old_protect);
               memcpy(compare, jump, sizeof(jump));
               VirtualProtect(compare, sizeof(jump), old_protect, &old_protect);
               FlushInstructionCache(GetCurrentProcess(), compare, sizeof(jump));
            }
            fprintf(g_log, "dgVoodoo sampler cache compares patched\n"); });
   }

   // Takes the methods, hooks the device and its implicit swap chain, sets the documented point defaults. Before the
   // CSMT layer starts, so the vtable still holds dgVoodoo's methods. "app_params": the game's parameters as passed,
   // before CreateDevice wrote the actual back buffer format and size into them.
   void StartDgVoodooFixes(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS* app_params)
   {
      g_dgv_fixes = !g_dgv_fixes_disabled && ChainIsDgVoodoo();
      fprintf(g_log, "dgVoodoo workarounds %s\n", g_dgv_fixes ? "on" : (g_dgv_fixes_disabled ? "off (d3d9_dgvfix.off)" : "off (not dgVoodoo)"));
      if (!g_dgv_fixes)
         return;
      void** vtable = *(void***)device;
      {
         std::unique_lock lock(g_dgv.mutex);
         g_dgv.device = device;
         g_dgv.methods = {
            .set_render_state = (decltype(DgvMethods::set_render_state))vtable[SLOT_SET_RENDER_STATE],
            .get_texture = (decltype(DgvMethods::get_texture))vtable[SLOT_GET_TEXTURE],
            .set_texture = (decltype(DgvMethods::set_texture))vtable[SLOT_SET_TEXTURE],
            .get_texture_stage_state = (decltype(DgvMethods::get_texture_stage_state))vtable[SLOT_GET_TEXTURE_STAGE_STATE],
            .set_texture_stage_state = (decltype(DgvMethods::set_texture_stage_state))vtable[SLOT_SET_TEXTURE_STAGE_STATE],
            .set_sampler_state = (decltype(DgvMethods::set_sampler_state))vtable[SLOT_SET_SAMPLER_STATE],
         };
         ResetVertexSamplers();
         g_dgv.gamma_ramps.clear();
         g_dgv.texture_lods.clear();
      }
      PatchSamplerCacheCompare();
      if (IDirect3DSwapChain9* swap_chain = nullptr; SUCCEEDED(device->GetSwapChain(0, &swap_chain)))
      {
         AppPresentation app = {
            .device = device,
            .interval = app_params ? app_params->PresentationInterval : D3DPRESENT_INTERVAL_DEFAULT,
            .unknown_format = app_params && app_params->BackBufferFormat == D3DFMT_UNKNOWN,
         };
         if (D3DDISPLAYMODE mode; SUCCEEDED(device->GetDisplayMode(0, &mode)))
         {
            app.display_format = mode.Format;
         }
         {
            std::unique_lock lock(g_dgv.mutex);
            std::erase_if(g_dgv.swap_chains, [&](const auto& entry)
               { return entry.second.device == device; });
            g_dgv.swap_chains[swap_chain] = app; // the device keeps its swap chain alive; a freed device's entry is
                                                 // dropped when an allocation at that address becomes a new device
         }
         Patch(swap_chain, SLOT_GET_PRESENT_PARAMETERS, (void*)&HookGetPresentParameters);
         swap_chain->Release();
      }
      SetDocumentedPointDefaults(device);
      Patch(device, SLOT_SET_RENDER_TARGET, (void*)&HookSetRenderTarget);
      Patch(device, SLOT_CREATE_STATE_BLOCK, (void*)&HookCreateStateBlock);
      Patch(device, SLOT_BEGIN_STATE_BLOCK, (void*)&HookBeginStateBlock);
      Patch(device, SLOT_END_STATE_BLOCK, (void*)&HookEndStateBlock);
      Patch(device, SLOT_SET_TEXTURE, (void*)&HookSetTexture);
      Patch(device, SLOT_GET_SAMPLER_STATE, (void*)&HookGetSamplerState);
      Patch(device, SLOT_SET_SAMPLER_STATE, (void*)&HookSetSamplerState);
      Patch(device, SLOT_SET_TEXTURE_STAGE_STATE, (void*)&HookSetTextureStageState);
      Patch(device, SLOT_CREATE_VERTEX_SHADER, (void*)&HookCreateVertexShader);
      Patch(device, SLOT_SET_GAMMA_RAMP, (void*)&HookSetGammaRamp);
      Patch(device, SLOT_GET_GAMMA_RAMP, (void*)&HookGetGammaRamp);
      Patch(device, SLOT_DRAW_PRIMITIVE, (void*)&HookDraw<SLOT_DRAW_PRIMITIVE, D3DPRIMITIVETYPE, UINT, UINT>);
      Patch(device, SLOT_DRAW_PRIMITIVE + 1, (void*)&HookDraw<SLOT_DRAW_PRIMITIVE + 1, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT>);
      Patch(device, SLOT_DRAW_PRIMITIVE + 2, (void*)&HookDraw<SLOT_DRAW_PRIMITIVE + 2, D3DPRIMITIVETYPE, UINT, const void*, UINT>);
      Patch(device, SLOT_DRAW_PRIMITIVE + 3,
         (void*)&HookDraw<SLOT_DRAW_PRIMITIVE + 3, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT>);
   }

   // Methods shared by 2D and cube textures.
   void PatchBaseTexture(IDirect3DBaseTexture9* texture)
   {
      Patch(texture, SLOT_GET_LEVEL_DESC, (void*)&HookGetLevelDesc);
      if (g_dgv_fixes)
      {
         {
            std::unique_lock lock(g_dgv.mutex);
            g_dgv.texture_lods.erase(texture); // a freed texture's LOD at this address
         }
         Patch(texture, SLOT_SET_LOD, (void*)&HookDgvSetLOD);
         Patch(texture, SLOT_GET_LOD, (void*)&HookDgvGetLOD);
      }
      else if (g_fix_active)
      {
         Patch(texture, SLOT_SET_LOD, (void*)&HookSetLOD);
      }
   }

   // ---- device hooks ----------------------------------------------------------

   HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9* self, UINT width,
      UINT height, UINT levels,
      DWORD usage, D3DFORMAT format,
      D3DPOOL pool,
      IDirect3DTexture9** texture,
      HANDLE* shared)
   {
      const bool replace = g_fix_active && pool == D3DPOOL_MANAGED;
      auto create = Original<CreateTextureFn>(self, SLOT_CREATE_TEXTURE);
      HRESULT hr = create(self, width, height, levels, usage, format,
         replace ? D3DPOOL_DEFAULT : pool, texture, shared);
      if (FAILED(hr))
         return CreateFailed(hr, "Texture", width, height, format, pool, replace,
            usage);
      std::unique_ptr<Shadow> shadow;
      if (replace)
      {
         shadow = CreateShadow(*texture, self, format, width, height, 1, usage);
         if (!shadow)
         {
            // No section: a DYNAMIC texture the runtime locks itself (MANAGED isn't
            // allowed on D3D9Ex).
            (*texture)->Release();
            hr = create(self, width, height, levels, usage | D3DUSAGE_DYNAMIC, format,
               D3DPOOL_DEFAULT, texture, shared);
            if (FAILED(hr))
               return CreateFailed(hr, "Texture", width, height, format, pool, replace,
                  usage);
            g_totals.shadow_fallbacks++;
         }
      }
      IDirect3DTexture9* tex = *texture;
      PatchBaseTexture(tex);
      Patch(tex, SLOT_LOCK, (void*)&HookTextureLockRect);
      Patch(tex, SLOT_UNLOCK, (void*)&HookTextureUnlockRect);
      IDirect3DSurface9* surface = nullptr;
      tex->GetSurfaceLevel(0, &surface);
      PatchSurfaces(surface);
      const UINT actual_levels = tex->GetLevelCount();
      Track(tex, {.buffer = false,
                    .pool = pool,
                    .usage = usage,
                    .bytes = ChainBytes(format, width, height, 1, actual_levels),
                    .subresources = actual_levels,
                    .shadow = std::move(shadow)});
      return hr;
   }

   HRESULT STDMETHODCALLTYPE HookCreateVolumeTexture(
      IDirect3DDevice9* self, UINT width, UINT height, UINT depth, UINT levels,
      DWORD usage, D3DFORMAT format, D3DPOOL pool,
      IDirect3DVolumeTexture9** texture, HANDLE* shared)
   {
      const bool replace = g_fix_active && pool == D3DPOOL_MANAGED;
      const HRESULT hr = Original<decltype(&HookCreateVolumeTexture)>(
         self, SLOT_CREATE_VOLUME_TEXTURE)(
         self, width, height, depth, levels,
         replace ? (usage | D3DUSAGE_DYNAMIC) : usage, format,
         replace ? D3DPOOL_DEFAULT : pool, texture, shared);
      if (FAILED(hr))
         return CreateFailed(hr, "VolumeTexture", width, height, format, pool,
            replace, usage);
      if (replace)
      {
         g_totals.converted_volumes++;
      }
      IDirect3DVolumeTexture9* tex = *texture;
      Patch(tex, SLOT_LOCK, (void*)&HookVolumeLockBox);
      const UINT actual_levels = tex->GetLevelCount();
      Track(tex, {.buffer = false,
                    .pool = pool,
                    .usage = usage,
                    .bytes = ChainBytes(format, width, height, depth, actual_levels),
                    .subresources = actual_levels});
      return hr;
   }

   HRESULT STDMETHODCALLTYPE HookCreateCubeTexture(IDirect3DDevice9* self,
      UINT edge, UINT levels,
      DWORD usage, D3DFORMAT format,
      D3DPOOL pool,
      IDirect3DCubeTexture9** texture,
      HANDLE* shared)
   {
      const bool replace = g_fix_active && pool == D3DPOOL_MANAGED;
      auto create = Original<CreateCubeTextureFn>(self, SLOT_CREATE_CUBE_TEXTURE);
      HRESULT hr = create(self, edge, levels, usage, format,
         replace ? D3DPOOL_DEFAULT : pool, texture, shared);
      if (FAILED(hr))
         return CreateFailed(hr, "CubeTexture", edge, edge, format, pool, replace,
            usage);
      std::unique_ptr<Shadow> shadow;
      if (replace)
      {
         shadow = CreateShadow(*texture, self, format, edge, edge, 6, usage);
         if (!shadow)
         {
            // As for 2D textures: fall back to a DYNAMIC texture.
            (*texture)->Release();
            hr = create(self, edge, levels, usage | D3DUSAGE_DYNAMIC, format,
               D3DPOOL_DEFAULT, texture, shared);
            if (FAILED(hr))
               return CreateFailed(hr, "CubeTexture", edge, edge, format, pool,
                  replace, usage);
            g_totals.shadow_fallbacks++;
         }
      }
      IDirect3DCubeTexture9* tex = *texture;
      PatchBaseTexture(tex);
      Patch(tex, SLOT_LOCK, (void*)&HookCubeLockRect);
      Patch(tex, SLOT_UNLOCK, (void*)&HookCubeUnlockRect);
      IDirect3DSurface9* surface = nullptr;
      tex->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, &surface);
      PatchSurfaces(surface);
      const UINT actual_levels = tex->GetLevelCount();
      Track(tex, {.buffer = false,
                    .pool = pool,
                    .usage = usage,
                    .bytes = ChainBytes(format, edge, edge, 1, actual_levels) * 6,
                    .subresources = actual_levels * 6,
                    .shadow = std::move(shadow)});
      return hr;
   }

   HRESULT STDMETHODCALLTYPE HookCreateVertexBuffer(
      IDirect3DDevice9* self, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
      IDirect3DVertexBuffer9** buffer, HANDLE* shared)
   {
      const bool replace = g_fix_active && pool == D3DPOOL_MANAGED;
      const HRESULT hr = Original<decltype(&HookCreateVertexBuffer)>(
         self, SLOT_CREATE_VERTEX_BUFFER)(self, length, usage, fvf,
         replace ? D3DPOOL_DEFAULT : pool, buffer,
         shared);
      if (FAILED(hr))
         return CreateFailed(hr, "VertexBuffer", length, 0, fvf, pool, replace,
            usage);
      if (replace)
      {
         g_totals.converted_buffers++;
      }
      Track(*buffer, {.buffer = true,
                        .pool = pool,
                        .usage = usage,
                        .bytes = length,
                        .subresources = 1});
      return hr;
   }

   HRESULT STDMETHODCALLTYPE HookCreateIndexBuffer(IDirect3DDevice9* self,
      UINT length, DWORD usage,
      D3DFORMAT format, D3DPOOL pool,
      IDirect3DIndexBuffer9** buffer,
      HANDLE* shared)
   {
      const bool replace = g_fix_active && pool == D3DPOOL_MANAGED;
      const HRESULT hr = Original<decltype(&HookCreateIndexBuffer)>(
         self, SLOT_CREATE_INDEX_BUFFER)(self, length, usage, format,
         replace ? D3DPOOL_DEFAULT : pool, buffer,
         shared);
      if (FAILED(hr))
         return CreateFailed(hr, "IndexBuffer", length, 0, format, pool, replace,
            usage);
      if (replace)
      {
         g_totals.converted_buffers++;
      }
      Track(*buffer, {.buffer = true,
                        .pool = pool,
                        .usage = usage,
                        .bytes = length,
                        .subresources = 1});
      return hr;
   }

   // Counts game uploads into a replaced texture (its shadow doesn't see them).
   // The destination is either a level surface or the texture itself.
   void CountUpdateInto(void* destination)
   {
      std::lock_guard lock(g_state.mutex);
      const Resource* resource = FindResource(destination);
      if (g_state.surfaces.contains(destination) ||
          (resource && resource->shadow))
      {
         Count(UPDATES_INTO_REPLACED);
      }
   }

   HRESULT STDMETHODCALLTYPE HookUpdateSurface(IDirect3DDevice9* self,
      IDirect3DSurface9* source,
      const RECT* rect,
      IDirect3DSurface9* destination,
      const POINT* point)
   {
      CountUpdateInto(destination);
      return Original<UpdateSurfaceFn>(self, SLOT_UPDATE_SURFACE)(
         self, source, rect, destination, point);
   }

   HRESULT STDMETHODCALLTYPE
   HookUpdateTexture(IDirect3DDevice9* self, IDirect3DBaseTexture9* source,
      IDirect3DBaseTexture9* destination)
   {
      CountUpdateInto(destination);
      const HRESULT hr = Original<decltype(&HookUpdateTexture)>(self, SLOT_UPDATE_TEXTURE)(self, source, destination);
      if (SUCCEEDED(hr) && g_dgv_fixes && source && destination)
      {
         CopySkippedLevels(self, source, destination);
      }
      return hr;
   }

   // Every pooled staging texture holds a device reference, so the game's last
   // Release would otherwise leave the device (and the pool) alive forever. After
   // the flush the device is gone (verified on Windows d3d9; dgVoodoo's staging
   // textures are assumed to reference the device the same way).
   ULONG STDMETHODCALLTYPE HookDeviceRelease(IDirect3DDevice9* self)
   {
      const ULONG refs =
         Original<decltype(&HookDeviceRelease)>(self, SLOT_RELEASE)(self);
      if (refs == 0 || refs != g_staging_pool.Size())
         return refs;
      fprintf(g_log, "device released by the game, flushing %lu staging textures\n",
         refs);
      g_staging_pool.Flush();
      Report("device released");
      return 0;
   }

   HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* self,
      D3DPRESENT_PARAMETERS* params)
   {
      g_totals.resets++;
      Report("before Reset");
      D3DPRESENT_PARAMETERS fixed_params;
      if (g_dgv_fixes && params)
      {
         // Reset unbinds RT1..3 and fails while the unbound surfaces we hold (DEFAULT resources) are alive.
         {
            std::unique_lock lock(g_dgv.mutex);
            std::fill(std::begin(g_dgv.bound_targets), std::end(g_dgv.bound_targets), nullptr);
         }
         g_dgv_unbound_pending.store(true, std::memory_order_release);
         ReleaseUnboundTargets();
         // A copy of dgVoodoo's own GetPresentParameters carries the desktop refresh rate, which its Reset rejects.
         fixed_params = *params;
         if (fixed_params.Windowed)
         {
            fixed_params.FullScreen_RefreshRateInHz = 0;
         }
         std::unique_lock lock(g_dgv.mutex);
         for (auto& [swap_chain, app] : g_dgv.swap_chains)
         {
            if (app.device == self)
            {
               app.interval = params->PresentationInterval;
               app.unknown_format = params->BackBufferFormat == D3DFMT_UNKNOWN;
            }
         }
      }
      D3DPRESENT_PARAMETERS* reset_params = (g_dgv_fixes && params) ? &fixed_params : params;
      const HRESULT hr = Original<decltype(&HookReset)>(self, SLOT_RESET)(self, reset_params);
      if (g_dgv_fixes && params)
      {
         params->BackBufferWidth = reset_params->BackBufferWidth; // the runtime fills these in (zero size, UNKNOWN format)
         params->BackBufferHeight = reset_params->BackBufferHeight;
         params->BackBufferFormat = reset_params->BackBufferFormat;
         params->BackBufferCount = reset_params->BackBufferCount;
      }
      if (SUCCEEDED(hr) && g_dgv_fixes)
      {
         {
            std::unique_lock lock(g_dgv.mutex);
            ResetVertexSamplers();
         }
         SetDocumentedPointDefaults(self);
         if (g_dgv_texture_lod_set.load(std::memory_order_relaxed))
         {
            SyncLodStages(self);
         }
      }
      fprintf(g_log, "Reset -> 0x%08lX\n", hr);
      fflush(g_log);
      return hr;
   }

   // Whether the runtime takes a SYSTEMMEM texture over our memory (D3D9Ex user
   // memory; Windows d3d9 does, dgVoodoo doesn't). A property of the runtime, so
   // it is asked once, before the device's CreateTexture is hooked.
   bool AcceptsUserMemory(IDirect3DDevice9* device)
   {
      void* memory =
         VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      HANDLE user_memory = memory;
      IDirect3DTexture9* texture = nullptr;
      auto create = Original<CreateTextureFn>(device, SLOT_CREATE_TEXTURE);
      if (!create)
      {
         create = (CreateTextureFn)(*(void***)device)[SLOT_CREATE_TEXTURE];
      }
      const bool accepted =
         memory && SUCCEEDED(create(device, 1, 1, 1, 0, D3DFMT_A8R8G8B8,
                      D3DPOOL_SYSTEMMEM, &texture, &user_memory));
      if (texture)
      {
         texture->Release();
      }
      if (memory)
      {
         VirtualFree(memory, 0, MEM_RELEASE);
      }
      return accepted;
   }

   // The device's hooks and the CSMT layer, after CreateDevice or CreateDeviceEx ("ex_device": a D3D9Ex device the VA fix
   // applies to; "app_params": the game's parameters before the call filled them in)
   HRESULT OnDeviceCreated(HRESULT hr, IDirect3DDevice9** device, bool ex_device, bool csmt, DWORD behavior,
      const D3DPRESENT_PARAMETERS* params, const std::optional<D3DPRESENT_PARAMETERS>& app_params)
   {
      fprintf(g_log, "CreateDevice behavior 0x%08lX %ux%u windowed %d -> 0x%08lX\n",
         behavior, params ? params->BackBufferWidth : 0,
         params ? params->BackBufferHeight : 0, params ? params->Windowed : -1,
         hr);
      if (FAILED(hr))
      {
         fflush(g_log);
         return hr;
      }
      g_fix_active = ex_device;
      IDirect3DDevice9* dev = *device;
      g_available_texture_mb = UINT(dev->GetAvailableTextureMem() / MB);
      fprintf(g_log, "available texture memory %u MB\n", g_available_texture_mb);
      if (g_fix_active)
      {
         g_user_memory = AcceptsUserMemory(dev);
         if (g_user_memory)
         {
            g_staging_arena.Reserve(); // while the ceiling is still empty
         }
         fprintf(g_log, "VA fix: user-memory staging %s, staging arena %p\n",
            g_user_memory ? "yes" : "no (runtime-allocated staging)",
            g_staging_arena.Base());
         Patch(dev, SLOT_RELEASE, (void*)&HookDeviceRelease);
      }
      fflush(g_log);
      Patch(dev, SLOT_RESET, (void*)&HookReset);
      Patch(dev, SLOT_CREATE_TEXTURE, (void*)&HookCreateTexture);
      Patch(dev, SLOT_CREATE_VOLUME_TEXTURE, (void*)&HookCreateVolumeTexture);
      Patch(dev, SLOT_CREATE_CUBE_TEXTURE, (void*)&HookCreateCubeTexture);
      Patch(dev, SLOT_CREATE_VERTEX_BUFFER, (void*)&HookCreateVertexBuffer);
      Patch(dev, SLOT_CREATE_INDEX_BUFFER, (void*)&HookCreateIndexBuffer);
      Patch(dev, SLOT_UPDATE_SURFACE, (void*)&HookUpdateSurface);
      Patch(dev, SLOT_UPDATE_TEXTURE, (void*)&HookUpdateTexture);
      StartDgVoodooFixes(dev, app_params ? &*app_params : nullptr);
      if (csmt)
      {
         Csmt::Start(dev, g_log); // last: it patches above the hooks above
      }
      else if (g_csmt_requested)
      {
         fprintf(g_log, "CSMT: requested, but the runtime isn't dgVoodoo: off\n");
      }
      fflush(g_log);
      return hr;
   }

   HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* self, UINT adapter,
      D3DDEVTYPE type, HWND window,
      DWORD behavior,
      D3DPRESENT_PARAMETERS* params,
      IDirect3DDevice9** device)
   {
      // CSMT runs only on dgVoodoo: on a native runtime NVIDIA's D3D9 driver hung
      // when work recorded on the worker was waited for from the game thread (a
      // query's GetData, a managed texture's UnlockRect), with or without
      // MULTITHREADED, and the driver already threads its own work there. Under
      // MULTITHREADED dgVoodoo takes its own critical section in every call; with
      // CSMT only one thread calls it at a time, so the flag only costs.
      const bool csmt = g_csmt_requested && ChainIsDgVoodoo();
      if (g_strip_multithreaded || csmt)
      {
         behavior &= ~D3DCREATE_MULTITHREADED;
      }
      const std::optional<D3DPRESENT_PARAMETERS> app_params = (params ? std::optional(*params) : std::nullopt);
      HRESULT hr = E_FAIL;
      bool ex_device = false;
      if (IDirect3D9Ex* ex = nullptr;
         g_fix_requested && params &&
         SUCCEEDED(self->QueryInterface(IID_IDirect3D9Ex, (void**)&ex)))
      {
         D3DDISPLAYMODEEX mode = {.Size = sizeof(mode),
            .Width = params->BackBufferWidth,
            .Height = params->BackBufferHeight,
            .RefreshRate = params->FullScreen_RefreshRateInHz,
            .Format = params->BackBufferFormat,
            .ScanLineOrdering =
               D3DSCANLINEORDERING_PROGRESSIVE};
         // Below our CreateDeviceEx hook (a Direct3DCreate9Ex object has one): this device is set up once, here
         using CreateDeviceExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
            D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
         hr = Original<CreateDeviceExFn>(ex, SLOT_CREATE_DEVICE_EX)(ex, adapter, type, window, behavior, params,
            params->Windowed ? nullptr : &mode, (IDirect3DDevice9Ex**)device);
         ex->Release();
         ex_device = SUCCEEDED(hr);
         fprintf(g_log, "VA fix: CreateDeviceEx -> 0x%08lX%s\n", hr,
            ex_device ? "" : ", falling back to CreateDevice (fix off)");
      }
      if (!ex_device)
      {
         hr = Original<decltype(&HookCreateDevice)>(self, SLOT_CREATE_DEVICE)(
            self, adapter, type, window, behavior, params, device);
      }
      return OnDeviceCreated(hr, device, ex_device, csmt, behavior, params, app_params);
   }

   // A D3D9Ex game's device (Bayonetta): set up as CreateDevice's, the VA fix applying when requested
   HRESULT STDMETHODCALLTYPE HookCreateDeviceEx(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND window, DWORD behavior,
      D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** device)
   {
      const bool csmt = g_csmt_requested && ChainIsDgVoodoo();
      if (g_strip_multithreaded || csmt)
      {
         behavior &= ~D3DCREATE_MULTITHREADED;
      }
      const std::optional<D3DPRESENT_PARAMETERS> app_params = (params ? std::optional(*params) : std::nullopt);
      const HRESULT hr = Original<decltype(&HookCreateDeviceEx)>(self, SLOT_CREATE_DEVICE_EX)(self, adapter, type, window, behavior,
         params, mode, device);
      fprintf(g_log, "CreateDeviceEx:\n");
      return OnDeviceCreated(hr, (IDirect3DDevice9**)device, g_fix_requested, csmt, behavior, params, app_params);
   }

   DWORD WINAPI ReportThread(void*)
   {
      char interval[16] = {};
      const DWORD interval_ms =
         (GetEnvironmentVariableA("D3D9_MEMLOG_MS", interval, sizeof(interval))
               ? atoi(interval)
               : 5000);
      for (;;)
      {
         Sleep(interval_ms);
         Report("tick");
         Csmt::Report(g_log, interval_ms / 1000.0);
         fflush(g_log);
      }
   }

} // namespace

// The vertex shader constants the CSMT layer passed down last (see "VertexConstantMirror"), for Luma
extern "C" const VertexConstantMirror* WINAPI LumaGetVertexConstants()
{
   return &Csmt::g_vertex_constants;
}

namespace
{
   void HookDirect3D(IDirect3D9* d3d, bool ex)
   {
      Patch(d3d, SLOT_CREATE_DEVICE, (void*)&HookCreateDevice);
      if (ex)
      {
         Patch(d3d, SLOT_CREATE_DEVICE_EX, (void*)&HookCreateDeviceEx);
      }
      if (g_log_enabled)
      {
         static const HANDLE thread = CreateThread(nullptr, 0, ReportThread, nullptr, 0, nullptr);
      }
   }
} // namespace

extern "C" IDirect3D9* WINAPI ProxyDirect3DCreate9(UINT sdk_version)
{
   HMODULE chain = Chain();
   IDirect3D9* d3d = nullptr;
   // VA fix: an IDirect3D9Ex, so CreateDevice can make a D3D9Ex device.
   using CreateEx = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
   if (auto create_ex =
          ((chain && g_fix_requested)
                ? (CreateEx)GetProcAddress(chain, "Direct3DCreate9Ex")
                : nullptr))
   {
      IDirect3D9Ex* ex = nullptr;
      const HRESULT hr = create_ex(sdk_version, &ex);
      fprintf(g_log, "VA fix: Direct3DCreate9Ex -> 0x%08lX\n", hr);
      d3d = (SUCCEEDED(hr) ? ex : nullptr);
   }
   using Create = IDirect3D9*(WINAPI*)(UINT);
   if (auto create =
          ((!d3d && chain) ? (Create)GetProcAddress(chain, "Direct3DCreate9")
                           : nullptr))
   {
      d3d = create(sdk_version);
   }
   fprintf(g_log, "Direct3DCreate9(%u) -> %p\n", sdk_version, d3d);
   fflush(g_log);
   if (d3d)
   {
      HookDirect3D(d3d, false);
   }
   return d3d;
}

// D3D9Ex games create their IDirect3D9Ex here: its CreateDevice and CreateDeviceEx are hooked as Direct3DCreate9's CreateDevice
extern "C" HRESULT WINAPI ProxyDirect3DCreate9Ex(UINT sdk_version, IDirect3D9Ex** d3d)
{
   using CreateEx = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
   HMODULE chain = Chain();
   auto create_ex = (chain ? (CreateEx)GetProcAddress(chain, "Direct3DCreate9Ex") : nullptr);
   const HRESULT hr = (create_ex ? create_ex(sdk_version, d3d) : D3DERR_NOTAVAILABLE);
   fprintf(g_log, "Direct3DCreate9Ex(%u) -> 0x%08lX\n", sdk_version, hr);
   fflush(g_log);
   if (SUCCEEDED(hr) && d3d && *d3d)
   {
      HookDirect3D(*d3d, true);
   }
   return hr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
   if (reason == DLL_PROCESS_ATTACH)
   {
      DisableThreadLibraryCalls(instance);
      GetModuleFileNameW(instance, g_dir, MAX_PATH);
      *(wcsrchr(g_dir, L'\\') + 1) = L'\0';
      wchar_t log_flag_path[MAX_PATH];
      swprintf_s(log_flag_path, L"%sd3d9_memlog.on", g_dir);
      g_log_enabled = GetFileAttributesW(log_flag_path) != INVALID_FILE_ATTRIBUTES;
      wchar_t log_path[MAX_PATH];
      swprintf_s(log_path, L"%sd3d9_memlog.log", g_dir);
      g_log = _wfopen((g_log_enabled ? log_path : L"NUL"), (g_log_enabled ? L"a" : L"w"));
      if (!g_log)
         return FALSE;
      wchar_t flag_path[MAX_PATH];
      swprintf_s(flag_path, L"%sd3d9_vafix.on", g_dir);
      g_fix_requested = GetFileAttributesW(flag_path) != INVALID_FILE_ATTRIBUTES;
      swprintf_s(flag_path, L"%sd3d9_nomt.on", g_dir);
      g_strip_multithreaded = GetFileAttributesW(flag_path) != INVALID_FILE_ATTRIBUTES;
      swprintf_s(flag_path, L"%sd3d9_csmt.on", g_dir);
      g_csmt_requested = GetFileAttributesW(flag_path) != INVALID_FILE_ATTRIBUTES;
      swprintf_s(flag_path, L"%sd3d9_dgvfix.off", g_dir);
      g_dgv_fixes_disabled = GetFileAttributesW(flag_path) != INVALID_FILE_ATTRIBUTES;
      fprintf(g_log, "---- d3d9_memlog attached, pid %lu, VA fix %s, multithreaded flag %s, CSMT %s\n",
         GetCurrentProcessId(), g_fix_requested ? "requested" : "off",
         g_strip_multithreaded ? "stripped" : (g_csmt_requested ? "stripped with CSMT" : "kept"),
         g_csmt_requested ? "requested" : "off");
      fflush(g_log);
   }
   return TRUE;
}
