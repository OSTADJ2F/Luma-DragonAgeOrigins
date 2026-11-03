// x86: what does a D3D9 runtime (dgVoodoo with a given VRAM setting) report
// through GetAvailableTextureMem, and how many DEFAULT textures does it really
// allow? Run: vram_probe.exe <d3d9.dll> [max MB to try, default 6144]
#include <cstdio>
#include <d3d9.h>
#include <vector>
#include <windows.h>

int main(int argc, char** argv)
{
   if (argc < 2)
      return printf("usage: vram_probe <d3d9.dll> [max MB]\n"), 2;
   const int max_mb = argc > 2 ? atoi(argv[2]) : 6144;
   HMODULE d3d9 = LoadLibraryA(argv[1]);
   auto create9 = d3d9 ? (IDirect3D9 * (WINAPI*)(UINT))
                            GetProcAddress(d3d9, "Direct3DCreate9")
                       : nullptr;
   IDirect3D9* d3d = create9 ? create9(D3D_SDK_VERSION) : nullptr;
   if (!d3d)
      return printf("Direct3DCreate9 failed\n"), 1;
   WNDCLASSA wc = {.lpfnWndProc = DefWindowProcA,
      .hInstance = GetModuleHandleA(nullptr),
      .lpszClassName = "vramprobe"};
   RegisterClassA(&wc);
   HWND hwnd = CreateWindowA("vramprobe", "vramprobe", WS_OVERLAPPEDWINDOW, 0, 0,
      256, 256, nullptr, nullptr, wc.hInstance, nullptr);
   D3DPRESENT_PARAMETERS pp = {.BackBufferWidth = 256,
      .BackBufferHeight = 256,
      .BackBufferFormat = D3DFMT_X8R8G8B8,
      .BackBufferCount = 1,
      .SwapEffect = D3DSWAPEFFECT_DISCARD,
      .hDeviceWindow = hwnd,
      .Windowed = TRUE};
   IDirect3DDevice9* dev = nullptr;
   if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
          D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
      return printf("CreateDevice failed\n"), 1;

   const UINT reported = dev->GetAvailableTextureMem();
   printf("GetAvailableTextureMem: %u bytes = %.1f MB (as signed int: %d)\n",
      reported, reported / 1048576.0, int(reported));

   // 2048x2048 DXT5 with mips = 5.33 MB each.
   std::vector<IDirect3DTexture9*> textures;
   constexpr double TEXTURE_MB = 5592416.0 / 1048576.0; // 2048^2 DXT5 chain
   HRESULT hr = S_OK;
   while (textures.size() * TEXTURE_MB < max_mb)
   {
      IDirect3DTexture9* tex = nullptr;
      hr = dev->CreateTexture(2048, 2048, 0, 0, D3DFMT_DXT5, D3DPOOL_DEFAULT,
         &tex, nullptr);
      if (FAILED(hr))
         break;
      textures.push_back(tex);
   }
   printf("DEFAULT textures created: %zu = %.0f MB (%s 0x%08lX)\n",
      textures.size(), textures.size() * TEXTURE_MB,
      FAILED(hr) ? "stopped by" : "reached the max, last hr", hr);
   printf("GetAvailableTextureMem after: %.1f MB\n",
      dev->GetAvailableTextureMem() / 1048576.0);
   for (auto* tex : textures)
      tex->Release();
   dev->Release();
   d3d->Release();
   return 0;
}
