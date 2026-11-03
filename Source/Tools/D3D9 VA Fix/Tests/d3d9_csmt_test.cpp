// x86: renders a deterministic scene through a d3d9.dll and prints a CRC of every frame, read back with
// GetRenderTargetData, so the CSMT layer can be compared against direct calls (run_csmt_tests.ps1 runs both and
// diffs the output). Everything a deferred call must keep intact is exercised: shader constants (F/I/B), a DYNAMIC
// vertex buffer locked with DISCARD then NOOVERWRITE, MANAGED vertex/index buffers, UP draws whose arrays are
// overwritten right after the call, a texture released while bound, render to texture through GetSurfaceLevel,
// StretchRect, ColorFill, Clear with rects, viewport, scissor, occlusion and event queries.
//   d3d9_csmt_test.exe <d3d9.dll> [frames=N] [reset] [redevice] [thread] [bench=DRAWS] [work=MS]
// reset: Reset in every frame of the second half (DEFAULT resources released and recreated); redevice: a second device after the first is
// released; thread: a second thread creates, fills, reads and releases textures while the scene renders;
// bench=N: N extra draws per frame with a state change each (prints ms per frame); work=MS: busy CPU work per frame
// on the rendering thread, as a game's own render thread has.
// constants: every D3D11 draw dgVoodoo makes compares the vertex constants it uploaded (vc4) with the proxy's mirror
// ("VertexConstantMirror", what Luma reads instead of copying vc4 back); prints "constants: compared N mismatched M".
#include <atomic>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <d3d9.h>
#include <d3dcommon.h>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>

#include "../vertex_constants.h"
#include "crash_report.h"
#include <tlhelp32.h> // after windows.h

#pragma comment(lib, "d3d9.lib") // "import" mode
#pragma comment(lib, "d3d11.lib")

namespace
{
   constexpr UINT TARGET_SIZE = 256;
   int g_failures = 0;

#define CHECK_HR(expression)                                                       \
   do                                                                              \
   {                                                                               \
      const HRESULT check_hr = (expression);                                       \
      if (FAILED(check_hr))                                                        \
      {                                                                            \
         g_failures++;                                                             \
         printf("FAIL line %d: %s -> 0x%08lX\n", __LINE__, #expression, check_hr); \
      }                                                                            \
   } while (0)

   uint32_t Crc32(const void* data, size_t bytes, uint32_t crc = 0)
   {
      static uint32_t table[256] = {};
      if (!table[1])
      {
         for (uint32_t i = 0; i < 256; i++)
         {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
            {
               c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
         }
      }
      crc = ~crc;
      for (size_t i = 0; i < bytes; i++)
      {
         crc = table[(crc ^ ((const uint8_t*)data)[i]) & 0xFF] ^ (crc >> 8);
      }
      return ~crc;
   }

   using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
      LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

   ID3DBlob* Compile(const char* source, const char* target)
   {
      static const auto compile = (D3DCompileFn)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
      ID3DBlob* code = nullptr;
      ID3DBlob* errors = nullptr;
      if (!compile || FAILED(compile(source, strlen(source), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &errors)))
      {
         printf("FAIL compile %s: %s\n", target, errors ? (const char*)errors->GetBufferPointer() : "no d3dcompiler_47");
         g_failures++;
      }
      if (errors)
      {
         errors->Release();
      }
      return code;
   }

   // Positions in pixels (scaled by c0/c1 to clip space), color and uv.
   const char* VERTEX_SHADER = R"(
float4 scale_offset : register(c0);
float4 tint : register(c1);
struct Out { float4 position : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; };
Out main(float2 position : POSITION, float4 color : COLOR0, float2 uv : TEXCOORD0)
{
   Out o;
   o.position = float4(position * scale_offset.xy + scale_offset.zw, 0.5, 1);
   o.color = color * tint;
   o.uv = uv;
   return o;
}
)";

   // An int loop count and a bool select, so I and B constants change the result.
   const char* PIXEL_SHADER = R"(
float4 add : register(c0);
int repeat : register(i0);
bool use_texture : register(b0);
sampler2D image : register(s0);
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0) : COLOR
{
   float4 result = color;
   for (int i = 0; i < repeat; i++)
      result += add;
   if (use_texture)
      result *= tex2D(image, uv);
   return result;
}
)";

   struct Vertex
   {
      float x, y;
      D3DCOLOR color;
      float u, v;
   };

   const D3DVERTEXELEMENT9 ELEMENTS[] = {{0, 0, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 8, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0}, {0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};

   void Quad(Vertex* v, float x, float y, float w, float h, D3DCOLOR color)
   {
      v[0] = {x, y, color, 0, 0};
      v[1] = {x + w, y, color, 1, 0};
      v[2] = {x, y + h, color, 0, 1};
      v[3] = {x + w, y + h, color, 1, 1};
   }

   uint32_t Pattern(UINT x, UINT y, UINT seed)
   {
      return ((x * 2654435761u) ^ (y * 40503u) ^ seed) | 0xFF000000u;
   }

   IDirect3DTexture9* PatternTexture(IDirect3DDevice9* device, UINT size, UINT seed, D3DPOOL pool)
   {
      IDirect3DTexture9* texture = nullptr;
      CHECK_HR(device->CreateTexture(size, size, 1, pool == D3DPOOL_DEFAULT ? D3DUSAGE_DYNAMIC : 0, D3DFMT_A8R8G8B8,
         pool, &texture, nullptr));
      if (!texture)
         return nullptr;
      D3DLOCKED_RECT locked;
      CHECK_HR(texture->LockRect(0, &locked, nullptr, 0));
      for (UINT y = 0; y < size; y++)
      {
         for (UINT x = 0; x < size; x++)
         {
            ((uint32_t*)((char*)locked.pBits + y * locked.Pitch))[x] = Pattern(x, y, seed);
         }
      }
      texture->UnlockRect(0);
      return texture;
   }

   // Resources that die with Reset (DEFAULT pool) live here; the rest in Scene.
   struct DefaultResources
   {
      IDirect3DSurface9* target = nullptr;    // rendered and read back
      IDirect3DSurface9* readback = nullptr;  // SYSTEMMEM copy of target
      IDirect3DTexture9* offscreen = nullptr; // render-to-texture
      IDirect3DVertexBuffer9* dynamic = nullptr;
      IDirect3DQuery9* occlusion = nullptr;
      IDirect3DQuery9* event = nullptr;

      void Create(IDirect3DDevice9* device)
      {
         CHECK_HR(device->CreateRenderTarget(TARGET_SIZE, TARGET_SIZE, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr));
         CHECK_HR(device->CreateOffscreenPlainSurface(TARGET_SIZE, TARGET_SIZE, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
         CHECK_HR(device->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &offscreen, nullptr));
         CHECK_HR(device->CreateVertexBuffer(64 * 1024, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &dynamic, nullptr));
         CHECK_HR(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &occlusion));
         CHECK_HR(device->CreateQuery(D3DQUERYTYPE_EVENT, &event));
      }

      void Release()
      {
         for (IUnknown** object : {(IUnknown**)&target, (IUnknown**)&readback, (IUnknown**)&offscreen, (IUnknown**)&dynamic,
                 (IUnknown**)&occlusion, (IUnknown**)&event})
         {
            if (*object)
            {
               (*object)->Release();
               *object = nullptr;
            }
         }
      }
   };

   struct Scene
   {
      IDirect3DDevice9* device = nullptr;
      IDirect3DVertexShader9* vs = nullptr;
      IDirect3DPixelShader9* ps = nullptr;
      IDirect3DVertexDeclaration9* declaration = nullptr;
      IDirect3DVertexBuffer9* static_vertices = nullptr;
      IDirect3DIndexBuffer9* static_indices = nullptr;
      IDirect3DTexture9* texture = nullptr;
      DefaultResources defaults;
      UINT dynamic_offset = 0;

      void Create(IDirect3DDevice9* d)
      {
         device = d;
         if (ID3DBlob* code = Compile(VERTEX_SHADER, "vs_3_0"))
         {
            CHECK_HR(device->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &vs));
            code->Release();
         }
         if (ID3DBlob* code = Compile(PIXEL_SHADER, "ps_3_0"))
         {
            CHECK_HR(device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps));
            code->Release();
         }
         CHECK_HR(device->CreateVertexDeclaration(ELEMENTS, &declaration));
         CHECK_HR(device->CreateVertexBuffer(16 * sizeof(Vertex), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &static_vertices, nullptr));
         CHECK_HR(device->CreateIndexBuffer(6 * 4 * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &static_indices, nullptr));
         Vertex* v = nullptr;
         CHECK_HR(static_vertices->Lock(0, 0, (void**)&v, 0));
         for (int i = 0; i < 4; i++)
         {
            Quad(v + i * 4, 10.0f + i * 50, 200, 40, 40, D3DCOLOR_ARGB(255, 60 * i, 255 - 60 * i, 128));
         }
         static_vertices->Unlock();
         WORD* indices = nullptr;
         CHECK_HR(static_indices->Lock(0, 0, (void**)&indices, 0));
         for (WORD i = 0; i < 4; i++)
         {
            const WORD base = i * 4;
            const WORD quad[6] = {base, WORD(base + 1), WORD(base + 2), WORD(base + 2), WORD(base + 1), WORD(base + 3)};
            memcpy(indices + i * 6, quad, sizeof(quad));
         }
         static_indices->Unlock();
         texture = PatternTexture(device, 64, 7, D3DPOOL_MANAGED);
         defaults.Create(device);
      }

      void Release()
      {
         defaults.Release();
         for (IUnknown* object : {(IUnknown*)vs, (IUnknown*)ps, (IUnknown*)declaration, (IUnknown*)static_vertices,
                 (IUnknown*)static_indices, (IUnknown*)texture})
         {
            if (object)
            {
               object->Release();
            }
         }
      }

      void SetPixelSpace(UINT width, UINT height)
      {
         const float scale_offset[4] = {2.0f / width, -2.0f / height, -1.0f, 1.0f};
         CHECK_HR(device->SetVertexShaderConstantF(0, scale_offset, 1));
      }

      // A quad from the DYNAMIC buffer: DISCARD at the start of a frame, NOOVERWRITE after.
      void DynamicQuad(float x, float y, float w, float h, D3DCOLOR color, bool first)
      {
         if (first || dynamic_offset + 4 * sizeof(Vertex) > 64 * 1024)
         {
            dynamic_offset = 0;
         }
         Vertex* v = nullptr;
         CHECK_HR(defaults.dynamic->Lock(dynamic_offset, 4 * sizeof(Vertex), (void**)&v, dynamic_offset ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD));
         if (v)
         {
            Quad(v, x, y, w, h, color);
         }
         defaults.dynamic->Unlock();
         CHECK_HR(device->SetStreamSource(0, defaults.dynamic, 0, sizeof(Vertex)));
         CHECK_HR(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, dynamic_offset / sizeof(Vertex), 2));
         dynamic_offset += 4 * sizeof(Vertex);
      }

      // Returns the frame's CRC; prints the query results.
      uint32_t Frame(UINT frame, UINT bench_draws, bool print_queries)
      {
         CHECK_HR(device->BeginScene());
         CHECK_HR(device->SetVertexShader(vs));
         CHECK_HR(device->SetPixelShader(ps));
         CHECK_HR(device->SetVertexDeclaration(declaration));
         CHECK_HR(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
         CHECK_HR(device->SetRenderState(D3DRS_ZENABLE, FALSE));
         CHECK_HR(device->SetRenderState(D3DRS_LIGHTING, FALSE));
         CHECK_HR(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
         CHECK_HR(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));

         // Render to texture: level surface fetched, bound and released every frame.
         IDirect3DSurface9* level = nullptr;
         CHECK_HR(defaults.offscreen->GetSurfaceLevel(0, &level));
         CHECK_HR(device->SetRenderTarget(0, level));
         level->Release();
         CHECK_HR(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, frame * 13 % 256, 40, 90), 1, 0));
         SetPixelSpace(64, 64);
         const float white[4] = {1, 1, 1, 1};
         CHECK_HR(device->SetVertexShaderConstantF(1, white, 1));
         const int repeat[4] = {0, 0, 0, 0};
         CHECK_HR(device->SetPixelShaderConstantI(0, repeat, 1));
         const BOOL no_texture = FALSE;
         CHECK_HR(device->SetPixelShaderConstantB(0, &no_texture, 1));
         DynamicQuad(8.0f + frame % 16, 8, 30, 30, D3DCOLOR_ARGB(255, 255, 200, 0), true);

         // Main target.
         CHECK_HR(device->SetRenderTarget(0, defaults.target));
         const D3DVIEWPORT9 viewport = {0, 0, TARGET_SIZE, TARGET_SIZE, 0, 1};
         CHECK_HR(device->SetViewport(&viewport));
         CHECK_HR(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 20, 20, frame * 7 % 256), 1, 0));
         const D3DRECT rects[2] = {{0, 0, 16, 16}, {240, 240, 256, 256}};
         CHECK_HR(device->Clear(2, rects, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 255, 0, 255), 1, 0));
         SetPixelSpace(TARGET_SIZE, TARGET_SIZE);

         // Constants: F tint, I loop count, B texture switch, set from stack arrays that change right after.
         float tint[4] = {1.0f, 0.5f + (frame % 8) / 16.0f, 1.0f, 1.0f};
         CHECK_HR(device->SetVertexShaderConstantF(1, tint, 1));
         tint[1] = 0;
         float add[4] = {0.02f, 0.01f * (frame % 5), 0.0f, 0.0f};
         CHECK_HR(device->SetPixelShaderConstantF(0, add, 1));
         add[0] = 9;
         int repeat_more[4] = {int(frame % 4), 0, 0, 0};
         CHECK_HR(device->SetPixelShaderConstantI(0, repeat_more, 1));
         repeat_more[0] = 99;
         BOOL use_texture = TRUE;
         CHECK_HR(device->SetPixelShaderConstantB(0, &use_texture, 1));
         use_texture = FALSE;

         // Static buffers, managed texture.
         CHECK_HR(device->SetTexture(0, texture));
         CHECK_HR(device->SetStreamSource(0, static_vertices, 0, sizeof(Vertex)));
         CHECK_HR(device->SetIndices(static_indices));
         CHECK_HR(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 16, 0, 8));

         // A texture created, bound and released by the game before the draw that samples it.
         if (IDirect3DTexture9* temporary = PatternTexture(device, 32, frame + 100, D3DPOOL_MANAGED))
         {
            static const bool late_release = GetEnvironmentVariableA("CSMT_TEST_LATE_RELEASE", nullptr, 0) > 0;
            CHECK_HR(device->SetTexture(0, temporary));
            if (!late_release)
            {
               temporary->Release();
            }
            DynamicQuad(100, 20, 60, 60, D3DCOLOR_ARGB(255, 255, 255, 255), true);
            if (late_release)
            {
               temporary->Release();
            }
         }

         // The render-to-texture result, sampled.
         CHECK_HR(device->SetTexture(0, defaults.offscreen));
         DynamicQuad(170, 20, 64, 64, D3DCOLOR_ARGB(255, 255, 255, 255), false);
         CHECK_HR(device->SetTexture(0, nullptr));
         const BOOL texture_off = FALSE;
         CHECK_HR(device->SetPixelShaderConstantB(0, &texture_off, 1));

         // UP draws; the arrays are overwritten at once (a deferred call must have copied them).
         Vertex up[4];
         Quad(up, 20, 100, 50, 30, D3DCOLOR_ARGB(255, 0, 255, frame * 3 % 256));
         CHECK_HR(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, up, sizeof(Vertex)));
         memset(up, 0x7F, sizeof(up));
         Vertex up_indexed[8];
         Quad(up_indexed, 0, 0, 1, 1, 0);                                                        // unused, below min_vertex
         Quad(up_indexed + 4, 80.0f + frame % 10, 100, 40, 50, D3DCOLOR_ARGB(255, 255, 80, 80)); // vertices 4..7
         WORD up_indices[6] = {4, 5, 6, 6, 5, 7};
         CHECK_HR(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 4, 4, 2, up_indices, D3DFMT_INDEX16, up_indexed, sizeof(Vertex)));
         memset(up_indexed, 0x7F, sizeof(up_indexed));
         memset(up_indices, 0, sizeof(up_indices));

         // A NOOVERWRITE lock over two quads that writes only the second: the first must keep its vertices.
         {
            Vertex* v = nullptr;
            CHECK_HR(defaults.dynamic->Lock(0, 8 * sizeof(Vertex), (void**)&v, D3DLOCK_DISCARD));
            Quad(v, 30, 160, 20, 20, D3DCOLOR_ARGB(255, 255, 255, 0));
            Quad(v + 4, 60, 160, 20, 20, D3DCOLOR_ARGB(255, 0, 255, 255));
            defaults.dynamic->Unlock();
            CHECK_HR(device->SetStreamSource(0, defaults.dynamic, 0, sizeof(Vertex)));
            CHECK_HR(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 4, 2));
            CHECK_HR(defaults.dynamic->Lock(0, 8 * sizeof(Vertex), (void**)&v, D3DLOCK_NOOVERWRITE));
            Quad(v + 4, 90.0f + frame % 7, 160, 20, 20, D3DCOLOR_ARGB(255, 255, 0, 0));
            defaults.dynamic->Unlock();
            CHECK_HR(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
            CHECK_HR(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 4, 2));
            dynamic_offset = 8 * sizeof(Vertex);
         }

         // Lifetime churn: textures created, their levels fetched twice (a layer may cache the second), drawn into and
         // released, 64 times. Addresses get reused, so anything kept past a release is read after free.
         {
            IDirect3DSurface9* target = nullptr;
            CHECK_HR(device->GetRenderTarget(0, &target));
            for (int i = 0; i < 64; i++)
            {
               // Sizes change every round, so a description kept past a release no longer matches.
               const UINT edge = 16u << (i % 3);
               IDirect3DTexture9* churn = nullptr;
               IDirect3DCubeTexture9* cube = nullptr;
               IDirect3DVertexBuffer9* buffer = nullptr;
               CHECK_HR(device->CreateTexture(edge, edge, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &churn, nullptr));
               CHECK_HR(device->CreateCubeTexture(edge, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cube, nullptr));
               CHECK_HR(device->CreateVertexBuffer(edge * 64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &buffer, nullptr));
               D3DSURFACE_DESC level_desc = {};
               if (churn && SUCCEEDED(churn->GetLevelDesc(0, &level_desc)) && level_desc.Width != edge)
               {
                  g_failures++;
                  printf("FAIL churn %d: texture level width %u, want %u\n", i, level_desc.Width, edge);
               }
               if (cube && SUCCEEDED(cube->GetLevelDesc(0, &level_desc)) && level_desc.Width != edge)
               {
                  g_failures++;
                  printf("FAIL churn %d: cube level width %u, want %u\n", i, level_desc.Width, edge);
               }
               if (D3DVERTEXBUFFER_DESC buffer_desc = {}; buffer && SUCCEEDED(buffer->GetDesc(&buffer_desc)) && buffer_desc.Size != edge * 64)
               {
                  g_failures++;
                  printf("FAIL churn %d: buffer size %u, want %u\n", i, buffer_desc.Size, edge * 64);
               }
               if (buffer)
               {
                  buffer->Release();
               }
               IDirect3DSurface9* levels[4] = {};
               if (churn)
               {
                  CHECK_HR(churn->GetSurfaceLevel(0, &levels[0]));
                  CHECK_HR(churn->GetSurfaceLevel(0, &levels[1]));
               }
               if (cube)
               {
                  CHECK_HR(cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_Y, 0, &levels[2]));
                  CHECK_HR(cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_Y, 0, &levels[3]));
               }
               for (IDirect3DSurface9* level : levels)
               {
                  if (D3DSURFACE_DESC surface_desc = {}; level && SUCCEEDED(level->GetDesc(&surface_desc)) && surface_desc.Width != edge)
                  {
                     g_failures++;
                     printf("FAIL churn %d: surface width %u, want %u\n", i, surface_desc.Width, edge);
                  }
                  if (level)
                  {
                     CHECK_HR(device->SetRenderTarget(0, level));
                     CHECK_HR(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, i, 0, 0), 1, 0));
                  }
               }
               CHECK_HR(device->SetRenderTarget(0, target));
               for (IDirect3DSurface9* level : levels)
               {
                  if (level)
                  {
                     level->Release();
                  }
               }
               if (churn)
               {
                  churn->Release();
               }
               if (cube)
               {
                  cube->Release();
               }
            }
            target->Release();
         }

         // Public reference counts: a Release loop must end, after as many calls as references.
         {
            IDirect3DSurface9* level0 = nullptr;
            CHECK_HR(defaults.offscreen->GetSurfaceLevel(0, &level0));
            level0->AddRef();
            level0->AddRef();
            const ULONG expected = level0->AddRef(); // the texture's references plus ours
            int releases = 0;
            while (level0->Release() > expected - 4 && releases < 100)
            {
               releases++;
            }
            if (releases != 3)
            {
               g_failures++;
               printf("FAIL frame %u: Release loop ran %d times (want 3), AddRef said %lu\n", frame, releases, expected);
            }
         }

         // State block: recorded with a value equal to the current one (a repeat filter must not drop it), applied,
         // then the pre-Apply value set again (must not be dropped as a repeat either).
         {
            CHECK_HR(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED));
            CHECK_HR(device->BeginStateBlock());
            CHECK_HR(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED));
            IDirect3DStateBlock9* block = nullptr;
            CHECK_HR(device->EndStateBlock(&block));
            CHECK_HR(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));
            DynamicQuad(120, 200, 20, 20, D3DCOLOR_ARGB(255, 200, 220, 240), false);
            if (block)
            {
               CHECK_HR(block->Apply());
               DynamicQuad(145, 200, 20, 20, D3DCOLOR_ARGB(255, 200, 220, 240), false);
               block->Release();
            }
            CHECK_HR(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));
            DynamicQuad(170, 200, 20, 20, D3DCOLOR_ARGB(255, 200, 220, 240), false);
         }

         // Scissor.
         const RECT scissor = {130, 100, 180, 130};
         CHECK_HR(device->SetScissorRect(&scissor));
         CHECK_HR(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE));
         DynamicQuad(120, 90, 80, 60, D3DCOLOR_ARGB(255, 80, 80, 255), false);
         CHECK_HR(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));

         // Occlusion query around a 32x16 quad: 512 samples.
         CHECK_HR(defaults.occlusion->Issue(D3DISSUE_BEGIN));
         DynamicQuad(200, 150, 32, 16, D3DCOLOR_ARGB(255, 200, 200, 200), false);
         CHECK_HR(defaults.occlusion->Issue(D3DISSUE_END));

         // Bench: draws with a state change each.
         for (UINT i = 0; i < bench_draws; i++)
         {
            CHECK_HR(device->SetRenderState(D3DRS_BLENDFACTOR, i));
            const float c[4] = {float(i % 7) / 7, 0, 0, 0};
            CHECK_HR(device->SetPixelShaderConstantF(0, c, 1));
            CHECK_HR(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 16, 0, 2));
         }
         if (bench_draws)
         {
            CHECK_HR(device->SetPixelShaderConstantF(0, add, 1)); // changed above; any value, same in both runs
         }
         CHECK_HR(device->EndScene());

         // ColorFill and StretchRect between surfaces.
         const RECT fill = {0, 240, 32, 256};
         CHECK_HR(device->ColorFill(defaults.target, &fill, D3DCOLOR_ARGB(255, 0, 128, 255)));
         IDirect3DSurface9* offscreen_level = nullptr;
         CHECK_HR(defaults.offscreen->GetSurfaceLevel(0, &offscreen_level));
         const RECT from = {0, 0, 64, 64};
         const RECT to = {180, 180, 244, 244};
         CHECK_HR(device->StretchRect(offscreen_level, &from, defaults.target, &to, D3DTEXF_POINT));
         offscreen_level->Release();

         CHECK_HR(defaults.event->Issue(D3DISSUE_END));
         DWORD samples = 0;
         HRESULT hr;
         // Results within 2 s, or the frame fails (a hang would block the whole run).
         const ULONGLONG deadline = GetTickCount64() + 2000;
         while ((hr = defaults.occlusion->GetData(&samples, sizeof(samples), D3DGETDATA_FLUSH)) == S_FALSE && GetTickCount64() < deadline)
         {
            YieldProcessor();
         }
         BOOL done = FALSE;
         while ((hr = defaults.event->GetData(&done, sizeof(done), D3DGETDATA_FLUSH)) == S_FALSE && GetTickCount64() < deadline)
         {
            YieldProcessor();
         }
         if (print_queries)
         {
            printf("frame %u occlusion %lu event %d\n", frame, samples, done);
         }
         if (samples != 512 || !done)
         {
            g_failures++;
            printf("FAIL frame %u: occlusion %lu (want 512), event %d\n", frame, samples, done);
         }

         CHECK_HR(device->GetRenderTargetData(defaults.target, defaults.readback));
         D3DLOCKED_RECT locked;
         uint32_t crc = 0;
         if (SUCCEEDED(defaults.readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
         {
            for (UINT y = 0; y < TARGET_SIZE; y++)
            {
               crc = Crc32((char*)locked.pBits + y * locked.Pitch, TARGET_SIZE * 4, crc);
            }
            // CSMT_TEST_DUMP=<folder>: frame N as raw BGRA, to locate a CRC difference.
            if (char folder[MAX_PATH]; GetEnvironmentVariableA("CSMT_TEST_DUMP", folder, sizeof(folder)))
            {
               char path[MAX_PATH + 32];
               sprintf_s(path, "%s\\frame%u.raw", folder, frame);
               if (FILE* file = nullptr; fopen_s(&file, path, "wb") == 0)
               {
                  for (UINT y = 0; y < TARGET_SIZE; y++)
                  {
                     fwrite((char*)locked.pBits + y * locked.Pitch, 4, TARGET_SIZE, file);
                  }
                  fclose(file);
               }
            }
            defaults.readback->UnlockRect();
         }
         return crc;
      }
   };

   // Every thread of the process: suspended, then its stack's code addresses printed (module+offset, for
   // llvm-symbolizer), for a hang.
   void DumpThreads()
   {
      const DWORD self = GetCurrentThreadId();
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 entry = {.dwSize = sizeof(entry)};
      for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
      {
         if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self)
            continue;
         HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
         if (!thread)
            continue;
         SuspendThread(thread);
         CONTEXT context = {.ContextFlags = CONTEXT_CONTROL};
         if (GetThreadContext(thread, &context))
         {
            printf("THREAD %lu\n", entry.th32ThreadID);
            PrintAddress("  at", (void*)context.Eip);
            auto* stack = (void**)context.Esp;
            MEMORY_BASIC_INFORMATION region;
            const size_t words = (VirtualQuery(stack, &region, sizeof(region))
                                     ? ((char*)region.BaseAddress + region.RegionSize - (char*)stack) / sizeof(void*)
                                     : 0);
            for (size_t i = 0, found = 0; i < words && i < 4096 && found < 16; i++)
            {
               MEMORY_BASIC_INFORMATION mbi;
               if (!VirtualQuery(stack[i], &mbi, sizeof(mbi)) || !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                  continue;
               PrintAddress("  stack", stack[i]);
               found++;
            }
         }
         CloseHandle(thread);
      }
      CloseHandle(snapshot);
      fflush(stdout);
   }

   // Frames finished so far; the watchdog dumps every thread and exits when it stops moving.
   std::atomic<uint32_t> g_progress = 0;

   void StartWatchdog(DWORD stall_ms)
   {
      std::thread(
         [stall_ms]
         {
            uint32_t seen = g_progress;
            for (;;)
            {
               Sleep(stall_ms);
               const uint32_t now = g_progress;
               if (now == seen)
               {
                  printf("HANG no frame finished for %lu ms\n", stall_ms);
                  DumpThreads();
                  TerminateProcess(GetCurrentProcess(), 3);
               }
               seen = now;
            }
         })
         .detach();
   }

   void BusyWork(double ms)
   {
      LARGE_INTEGER frequency, begin, now;
      QueryPerformanceFrequency(&frequency);
      QueryPerformanceCounter(&begin);
      do
      {
         QueryPerformanceCounter(&now);
      } while ((now.QuadPart - begin.QuadPart) * 1000.0 / frequency.QuadPart < ms);
   }
} // namespace

namespace Vc4Check
{
   // dgVoodoo's translated vertex shaders read the D3D9 constants from a dynamic buffer at VS b4 of this size (2.87.3).
   // The immediate context's vtable lives with its device, so dgVoodoo's own D3D11CreateDevice call is hooked (see
   // "CreateDevice") and the context it creates is patched.
   constexpr UINT VC4_BYTES = VertexConstantMirror::ROWS * 16;
   constexpr int DRAW_INDEXED = 12, DRAW = 13, MAP = 14, UNMAP = 15, DRAW_INDEXED_INSTANCED = 20, DRAW_INSTANCED = 21;
   struct Vtable
   {
      void** table = nullptr;
      void* original[22] = {};
   };
   Vtable g_vtables[4];
   GetVertexConstantMirrorFn g_get_mirror = nullptr;
   std::mutex g_mutex; // texture maps of the "thread" mode's second thread come through these hooks too
   struct Upload
   {
      std::vector<uint8_t> bytes;
      uint64_t serial = 0;            // Uploads of this buffer so far
      uint64_t compared_serial = 0;   // The one the last compared draw used
      uint32_t mirror_generation = 0; // The mirror's at the upload
   };
   std::unordered_map<ID3D11Resource*, Upload> g_uploads;
   std::pair<ID3D11Resource*, void*> g_mapped = {};
   uint32_t g_compared = 0, g_mismatched = 0, g_without_mirror = 0;
   std::atomic<uint32_t> g_maps{0}, g_draws{0};
   uint32_t g_vc4_draws = 0, g_stale_draws = 0, g_uploaded = 0, g_unknown_draws = 0, g_internal_draws = 0;
   std::unordered_map<UINT, uint32_t> g_mapped_sizes; // Constant buffers mapped with DISCARD, by size (diagnosis)

   void** Original(void* self)
   {
      void** const table = *(void***)self;
      for (Vtable& vtable : g_vtables)
      {
         if (vtable.table == table)
            return vtable.original;
      }
      return nullptr;
   }

   void Compare(ID3D11DeviceContext* self)
   {
      g_draws++;
      ID3D11Buffer* buffer = nullptr;
      self->VSGetConstantBuffers(4, 1, &buffer);
      if (!buffer)
         return;
      const std::lock_guard lock(g_mutex);
      const auto upload = g_uploads.find(buffer);
      buffer->Release();
      if (upload == g_uploads.end())
         return;
      g_vc4_draws++;
      const bool fresh = upload->second.serial != upload->second.compared_serial;
      if (!fresh)
      {
         g_stale_draws++; // Still compared: dgVoodoo uploads only changed constants, so the last upload is this draw's too
      }
      upload->second.compared_serial = upload->second.serial;
      const VertexConstantMirror* const mirror = (g_get_mirror ? g_get_mirror() : nullptr);
      if (!mirror || !mirror->active)
      {
         g_without_mirror++;
         return;
      }
      // Constants set since the last upload that dgVoodoo didn't upload: a draw of its own (ColorFill, StretchRect, a Clear with
      // rects) that leaves vc4 bound without reading it. A translated game shader's draw uploads changed constants first.
      if (!fresh && mirror->generation != upload->second.mirror_generation)
      {
         g_internal_draws++;
         return;
      }
      g_compared++;
      // Rows the mirror doesn't know (after a state block's Apply) can't be checked: such a draw is counted apart
      bool mismatched = false, unknown = false;
      for (uint32_t row = 0; row < VertexConstantMirror::ROWS; row++)
      {
         const uint32_t* const uploaded = (const uint32_t*)(upload->second.bytes.data() + row * 16);
         if (!mirror->known[row])
         {
            unknown |= std::memcmp(mirror->rows[row], uploaded, 16) != 0;
            continue;
         }
         if (std::memcmp(mirror->rows[row], uploaded, 16) == 0)
            continue;
         if (g_mismatched < 12)
         {
            printf("FAIL constants: draw %u (%s upload) row %u: mirror %08X %08X %08X %08X, vc4 %08X %08X %08X %08X\n", g_compared,
               fresh ? "new" : "old", row, mirror->rows[row][0], mirror->rows[row][1], mirror->rows[row][2], mirror->rows[row][3], uploaded[0],
               uploaded[1], uploaded[2], uploaded[3]);
         }
         mismatched = true;
      }
      if (mismatched)
      {
         g_mismatched++;
      }
      if (unknown)
      {
         g_unknown_draws++;
      }
   }

   HRESULT STDMETHODCALLTYPE Map(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT subresource, D3D11_MAP type, UINT flags,
      D3D11_MAPPED_SUBRESOURCE* mapped)
   {
      const HRESULT hr = ((decltype(&Map))Original(self)[MAP])(self, resource, subresource, type, flags, mapped);
      g_maps++;
      D3D11_RESOURCE_DIMENSION dimension;
      resource->GetType(&dimension);
      if (SUCCEEDED(hr) && mapped && type == D3D11_MAP_WRITE_DISCARD && dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
      {
         D3D11_BUFFER_DESC desc;
         static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
         if (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)
         {
            const std::lock_guard lock(g_mutex);
            g_mapped_sizes[desc.ByteWidth]++;
         }
         if (desc.ByteWidth == VC4_BYTES && (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
         {
            const std::lock_guard lock(g_mutex);
            g_mapped = {resource, mapped->pData};
         }
      }
      return hr;
   }

   void STDMETHODCALLTYPE Unmap(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT subresource)
   {
      {
         const std::lock_guard lock(g_mutex);
         if (g_mapped.first == resource)
         {
            Upload& upload = g_uploads[resource];
            upload.bytes.assign((const uint8_t*)g_mapped.second, (const uint8_t*)g_mapped.second + VC4_BYTES);
            upload.serial++;
            g_uploaded++;
            const VertexConstantMirror* const mirror = (g_get_mirror ? g_get_mirror() : nullptr);
            upload.mirror_generation = (mirror ? mirror->generation : 0);
            g_mapped = {};
         }
      }
      ((decltype(&Unmap))Original(self)[UNMAP])(self, resource, subresource);
   }

   void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* self, UINT a, UINT b, INT c)
   {
      Compare(self);
      ((decltype(&DrawIndexed))Original(self)[DRAW_INDEXED])(self, a, b, c);
   }

   void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* self, UINT a, UINT b)
   {
      Compare(self);
      ((decltype(&Draw))Original(self)[DRAW])(self, a, b);
   }

   void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* self, UINT a, UINT b, UINT c, INT d, UINT e)
   {
      Compare(self);
      ((decltype(&DrawIndexedInstanced))Original(self)[DRAW_INDEXED_INSTANCED])(self, a, b, c, d, e);
   }

   void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext* self, UINT a, UINT b, UINT c, UINT d)
   {
      Compare(self);
      ((decltype(&DrawInstanced))Original(self)[DRAW_INSTANCED])(self, a, b, c, d);
   }

   // A device's immediate context vtable lives with the device (not in d3d11.dll): hooked when dgVoodoo creates its device
   void HookContext(ID3D11DeviceContext* context)
   {
      if (Original(context))
         return;
      for (Vtable& vtable : g_vtables)
      {
         if (vtable.table)
            continue;
         void** const table = *(void***)context;
         std::memcpy(vtable.original, table, sizeof(vtable.original));
         DWORD protection;
         VirtualProtect(table, sizeof(vtable.original), PAGE_READWRITE, &protection);
         table[DRAW_INDEXED] = (void*)&DrawIndexed;
         table[DRAW] = (void*)&Draw;
         table[MAP] = (void*)&Map;
         table[UNMAP] = (void*)&Unmap;
         table[DRAW_INDEXED_INSTANCED] = (void*)&DrawIndexedInstanced;
         table[DRAW_INSTANCED] = (void*)&DrawInstanced;
         VirtualProtect(table, sizeof(vtable.original), protection, &protection);
         vtable.table = table;
         return;
      }
   }

   // D3D11CreateDevice, which dgVoodoo finds with GetProcAddress: a jump to "CreateDevice", lifted while the real one runs
   uint8_t* g_create_device = nullptr;
   uint8_t g_create_device_bytes[5] = {};
   std::mutex g_create_device_mutex;

   void PatchCreateDevice(bool hooked);

   HRESULT WINAPI CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
      UINT level_count, UINT sdk_version, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
   {
      const std::lock_guard lock(g_create_device_mutex);
      PatchCreateDevice(false);
      const HRESULT hr = ((decltype(&CreateDevice))g_create_device)(adapter, type, software, flags, levels, level_count, sdk_version, device, level, context);
      PatchCreateDevice(true);
      if (SUCCEEDED(hr) && device && *device)
      {
         ID3D11DeviceContext* immediate = nullptr;
         (*device)->GetImmediateContext(&immediate);
         if (immediate)
         {
            HookContext(immediate);
            immediate->Release();
         }
      }
      return hr;
   }

   void PatchCreateDevice(bool hooked)
   {
      DWORD protection;
      VirtualProtect(g_create_device, 5, PAGE_EXECUTE_READWRITE, &protection);
      if (hooked)
      {
         g_create_device[0] = 0xE9; // jmp rel32
         const int32_t offset = int32_t((uint8_t*)&CreateDevice - (g_create_device + 5));
         std::memcpy(g_create_device + 1, &offset, 4);
      }
      else
      {
         std::memcpy(g_create_device, g_create_device_bytes, 5);
      }
      VirtualProtect(g_create_device, 5, protection, &protection);
      FlushInstructionCache(GetCurrentProcess(), g_create_device, 5);
   }

   // Before dgVoodoo creates its device
   bool Install()
   {
      g_get_mirror = (GetVertexConstantMirrorFn)GetProcAddress(GetModuleHandleA("d3d9.dll"), kGetVertexConstantMirrorExport);
      g_create_device = (uint8_t*)GetProcAddress(LoadLibraryA("d3d11.dll"), "D3D11CreateDevice");
      if (!g_create_device)
         return false;
      std::memcpy(g_create_device_bytes, g_create_device, 5);
      PatchCreateDevice(true);
      return true;
   }

   void Report()
   {
      const std::lock_guard lock(g_mutex);
      printf("constants: compared %u mismatched %u unknown %u internal %u without mirror %u (export %s)\n", g_compared, g_mismatched, g_unknown_draws, g_internal_draws, g_without_mirror,
         g_get_mirror ? "found" : "missing");
      printf("constants: vc4 uploads %u, draws with vc4 at b4 %u (%u without a new upload)\n", g_uploaded, g_vc4_draws, g_stale_draws);
      printf("constants: hooked maps %u draws %u, vtables %p %p\n", g_maps.load(), g_draws.load(), (void*)g_vtables[0].table,
         (void*)g_vtables[1].table);
      for (const auto& [size, count] : g_mapped_sizes)
      {
         printf("constants: discarded constant buffer %u bytes x%u\n", size, count);
      }
      if (g_mismatched)
      {
         g_failures++;
      }
   }
} // namespace Vc4Check

int main(int argc, char** argv)
{
   SetUnhandledExceptionFilter(&CrashReport);
   StartWatchdog(8000);
   if (argc < 2)
      return printf("usage: d3d9_csmt_test <d3d9.dll> [frames=N] [reset] [redevice] [thread] [ex] [bench=N] [work=MS]\n"), 2;
   UINT frames = 6, bench_draws = 0;
   double work_ms = 0;
   bool reset = false, redevice = false, thread = false, constants = false, ex = false;
   for (int i = 2; i < argc; i++)
   {
      constants |= !strcmp(argv[i], "constants");
      sscanf_s(argv[i], "frames=%u", &frames);
      sscanf_s(argv[i], "bench=%u", &bench_draws);
      sscanf_s(argv[i], "work=%lf", &work_ms);
      reset |= !strcmp(argv[i], "reset");
      redevice |= !strcmp(argv[i], "redevice");
      thread |= !strcmp(argv[i], "thread");
      ex |= !strcmp(argv[i], "ex"); // A D3D9Ex game's path (Bayonetta): Direct3DCreate9Ex and CreateDeviceEx
   }
   // "import": d3d9.dll resolved by the loader from the exe's folder, as a game's import does (dgVoodoo behind the
   // proxy recurses into itself when the proxy is loaded by path instead).
   HMODULE d3d9 = (strcmp(argv[1], "import") ? LoadLibraryA(argv[1]) : GetModuleHandleA("d3d9.dll"));
   auto create9 = (!strcmp(argv[1], "import") ? &Direct3DCreate9
                   : d3d9                     ? (IDirect3D9 * (WINAPI*)(UINT)) GetProcAddress(d3d9, "Direct3DCreate9")
                                              : nullptr);
   if (constants && !Vc4Check::Install())
      return printf("FAIL constants: no D3D11CreateDevice to hook\n"), 1;
   IDirect3D9Ex* d3d_ex = nullptr;
   if (ex)
   {
      auto create9ex = (!strcmp(argv[1], "import") ? &Direct3DCreate9Ex
                        : d3d9                     ? (HRESULT(WINAPI*)(UINT, IDirect3D9Ex**))GetProcAddress(d3d9, "Direct3DCreate9Ex")
                                                   : nullptr);
      if (!create9ex || FAILED(create9ex(D3D_SDK_VERSION, &d3d_ex)))
         return printf("FAIL Direct3DCreate9Ex (%s)\n", argv[1]), 1;
   }
   IDirect3D9* d3d = (ex ? d3d_ex : (create9 ? create9(D3D_SDK_VERSION) : nullptr));
   if (!d3d)
      return printf("FAIL Direct3DCreate9 (%s)\n", argv[1]), 1;
   WNDCLASSA wc = {.lpfnWndProc = DefWindowProcA, .hInstance = GetModuleHandleA(nullptr), .lpszClassName = "d3d9csmt"};
   RegisterClassA(&wc);
   // Never shown or activated: a test that hangs must not take the desktop's input.
   HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "d3d9csmt", "d3d9csmt", WS_POPUP, 0, 0, TARGET_SIZE, TARGET_SIZE, nullptr, nullptr, wc.hInstance, nullptr);
   D3DPRESENT_PARAMETERS pp = {.BackBufferWidth = TARGET_SIZE,
      .BackBufferHeight = TARGET_SIZE,
      .BackBufferFormat = D3DFMT_X8R8G8B8,
      .BackBufferCount = 1,
      .SwapEffect = D3DSWAPEFFECT_DISCARD,
      .hDeviceWindow = hwnd,
      .Windowed = TRUE,
      .PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE};

   for (int round = 0; round < (redevice ? 2 : 1); round++)
   {
      IDirect3DDevice9* device = nullptr;
      // MULTITHREADED as UE3 asks: the second thread needs it without the CSMT layer.
      constexpr DWORD behavior = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE;
      HRESULT hr = (ex ? d3d_ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, behavior, &pp, nullptr, (IDirect3DDevice9Ex**)&device)
                       : d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, behavior, &pp, &device));
      if (FAILED(hr))
         return printf("FAIL CreateDevice 0x%08lX\n", hr), 1;
      Scene scene;
      scene.Create(device);

      std::atomic<bool> stop = false;
      std::atomic<int> thread_rounds = 0;
      std::thread worker;
      if (thread)
      {
         worker = std::thread(
            [&]
            {
               for (UINT i = 0; !stop; i++)
               {
                  IDirect3DTexture9* texture = PatternTexture(device, 16, i, D3DPOOL_MANAGED);
                  if (!texture)
                     continue;
                  D3DSURFACE_DESC desc;
                  texture->GetLevelDesc(0, &desc);
                  D3DLOCKED_RECT locked;
                  if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY)))
                  {
                     if (((uint32_t*)locked.pBits)[5] != Pattern(5, 0, i))
                     {
                        g_failures++;
                        printf("FAIL thread texture %u content\n", i);
                     }
                     texture->UnlockRect(0);
                  }
                  texture->Release();
                  thread_rounds++;
               }
            });
      }

      LARGE_INTEGER frequency, begin, end;
      QueryPerformanceFrequency(&frequency);
      QueryPerformanceCounter(&begin);
      for (UINT frame = 0; frame < frames; frame++)
      {
         if (reset && frame >= frames / 2) // every frame of the second half
         {
            scene.defaults.Release();
            hr = device->Reset(&pp);
            printf("reset 0x%08lX\n", hr);
            if (FAILED(hr))
               g_failures++;
            scene.defaults.Create(device);
         }
         if (work_ms > 0)
         {
            BusyWork(work_ms);
         }
         const uint32_t crc = scene.Frame(frame, bench_draws, frame == 0);
         g_progress++;
         if (!bench_draws)
         {
            printf("crc %u %08X\n", frame + round * 1000, crc);
         }
         CHECK_HR(device->Present(nullptr, nullptr, nullptr, nullptr));
         MSG msg;
         while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
         {
            DispatchMessageA(&msg);
         }
      }
      QueryPerformanceCounter(&end);
      if (bench_draws || work_ms > 0)
      {
         printf("bench %u draws, work %.1f ms: %.3f ms per frame\n", bench_draws, work_ms,
            (end.QuadPart - begin.QuadPart) * 1000.0 / frequency.QuadPart / frames);
      }
      stop = true;
      if (worker.joinable())
      {
         worker.join();
         printf("thread rounds %d\n", thread_rounds.load());
      }
      scene.Release();
      const ULONG refs = device->Release();
      printf("device released, refs %lu\n", refs);
   }
   if (constants)
   {
      Vc4Check::Report();
   }
   d3d->Release();
   printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
   return g_failures ? 1 : 0;
}
