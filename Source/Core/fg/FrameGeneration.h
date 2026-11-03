#pragma once

// Frame generation in the game's process, on the D3D12 side of an x64 D3D11 mod ("UseLumaD3D12SR", "unity_build_d3d12.cpp"): a presenter (a
// swapchain on a disabled child window of the game's window) and the vendor's generator behind one interface ("FrameGenerationFsr.h"; XeFG and DLSS-G
// come through the same one). The host that drives them is in "unity_build_d3d12.cpp", the game's D3D11 side in "sr_bridge/D3D12SRLocal.h".
#include "FrameGenerationTypes.h"

#include <include/reshade.hpp>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <thread>

namespace FrameGeneration
{
   // To ReShade's log
   inline void Log(const char* format, ...)
   {
      char text[512] = "[Frame Generation] ";
      const size_t prefix = strlen(text);
      va_list arguments;
      va_start(arguments, format);
      vsnprintf(text + prefix, sizeof(text) - prefix, format, arguments);
      va_end(arguments);
      reshade::log::message(reshade::log::level::info, text);
   }

   struct CreateDesc
   {
      HWND window = nullptr; // The child window to present into (a "PresenterWindow")
      uint32_t width = 0, height = 0;
      bool hdr = false; // HDR10 PQ if set, else sRGB
      float min_nits = 0.f, max_nits = 0.f;
      ID3D12CommandQueue* queue = nullptr; // The queue the super resolution runs on: the generator interpolates and presents on it

      // The super resolution inputs the generator reads: the render size and their conventions
      uint32_t render_width = 0, render_height = 0;
      bool inverted_depth = false;
      float mvs_x_scale = 1.f, mvs_y_scale = 1.f;
      bool mvs_jittered = false;
   };

   inline D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
   {
      return {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
         .Transition = {.pResource = resource, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, .StateBefore = before, .StateAfter = after}};
   }

   // The one swapchain format of all generators: R10G10B10A2, flip, with tearing presents where the system allows them (a windowed flip swapchain
   // needs them for variable refresh, else every present waits for the display's refresh)
   inline DXGI_SWAP_CHAIN_DESC1 MakeSwapchainDesc(const CreateDesc& desc, UINT buffer_count, bool tearing)
   {
      return {
         .Width = desc.width,
         .Height = desc.height,
         .Format = DXGI_FORMAT_R10G10B10A2_UNORM,
         .SampleDesc = {.Count = 1},
         .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
         .BufferCount = buffer_count,
         .Scaling = DXGI_SCALING_STRETCH,
         .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
         .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
         .Flags = UINT(tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
      };
   }

   // HDR10 (PQ, BT.2020) with its static metadata, else sRGB
   inline void SetOutputColorSpace(IDXGISwapChain4* swapchain, const CreateDesc& desc)
   {
      if (!desc.hdr)
      {
         swapchain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
         return;
      }
      swapchain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
      // In units of 0.0001 nits; the primaries and white point (BT.2020, D65) in units of 0.00002
      const UINT16 max_nits = UINT16(desc.max_nits > 0.f ? desc.max_nits : 1000.f);
      DXGI_HDR_METADATA_HDR10 metadata = {
         .RedPrimary = {35400, 14600},
         .GreenPrimary = {8500, 39850},
         .BluePrimary = {6550, 2300},
         .WhitePoint = {15635, 16450},
         .MaxMasteringLuminance = UINT(max_nits) * 10000,
         .MinMasteringLuminance = UINT(std::lround(desc.min_nits * 10000.f)),
         .MaxContentLightLevel = max_nits,
         .MaxFrameAverageLightLevel = max_nits,
      };
      swapchain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(metadata), &metadata);
   }

   // Waits (bounded) for everything submitted to the queue so far
   inline void DrainQueue(ID3D12CommandQueue* queue)
   {
      Microsoft::WRL::ComPtr<ID3D12Device> device;
      Microsoft::WRL::ComPtr<ID3D12Fence> fence;
      if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
      {
         return;
      }
      queue->Signal(fence.Get(), 1);
      const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      fence->SetEventOnCompletion(1, event);
      WaitForSingleObject(event, 3000);
      CloseHandle(event);
   }

   // A frame to present. The textures are in COMMON on entry and are left in COMMON.
   struct Frame
   {
      uint64_t n = 0; // Steps by one for each frame
      bool generate = false;
      bool reset = false;
      float frame_ms = 0.f;
      float jitter_x = 0.f, jitter_y = 0.f;
      float view[16] = {}, projection[16] = {}; // Row-major, row vectors (clip = p * view * projection), without jitter
      ID3D12Resource* color = nullptr;          // As presented (with the UI), the swapchain's size and format
      ID3D12Resource* hudless = nullptr;        // The same without the UI
      ID3D12Resource* depth = nullptr;          // The last super resolution frame's inputs, at the render size
      ID3D12Resource* motion = nullptr;
      uint32_t render_width = 0, render_height = 0;
      float near_plane = 0.f, far_plane = 0.f, vert_fov = 0.f; // Of the last super resolution frame
   };

   class Generator
   {
   public:
      virtual ~Generator() = default;

      // Creates the swapchain on the window and the generator; false if it can't (the caller then reports "kFailed")
      virtual bool Create(const CreateDesc& desc) = 0;
      // Records a frame's work into an open direct list: the generator's inputs and the copy of the color into the swapchain
      virtual bool Record(ID3D12GraphicsCommandList* list, const Frame& frame) = 0;
      // Once the list from "Record()" is executed on the queue: presents (and generates), returns false if the swapchain failed
      virtual bool Present() = 0;
      // Waits for the GPU's work on the queue, then releases everything (not the window)
      virtual void Destroy() = 0;

      // The game's latency markers and sleep (Reflex for DLSS-G, XeLL for XeFG); FSR has none: the sleep ends at once
      virtual void Marker(LatencyMarker marker, uint64_t n)
      {
      }
      virtual void Sleep(uint64_t n, HANDLE sleep_event)
      {
         SetEvent(sleep_event);
      }
   };

   // The child window over the game's client area, on its own thread that only pumps messages: a child shares its parent's input queue, so a
   // thread that blocks (the game's) would hang the game's window thread on the messages it sends to its child.
   class PresenterWindow
   {
   public:
      ~PresenterWindow()
      {
         Destroy();
      }

      // Returns once the window exists
      bool Create(HWND parent)
      {
         Destroy();
         const HANDLE created = CreateEventW(nullptr, TRUE, FALSE, nullptr);
         thread = std::thread(
            [&]
            {
               // A child can't differ from its parent's DPI awareness
               SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(parent));
               // The addon's module (the window procedure is in it), not the exe's
               HMODULE instance = nullptr;
               GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&WindowProc), &instance);
               WNDCLASSEXW window_class = {.cbSize = sizeof(window_class), .lpfnWndProc = WindowProc, .hInstance = instance, .lpszClassName = L"LumaFrameGenerationPresenter"};
               RegisterClassExW(&window_class);
               RECT client = {};
               GetClientRect(parent, &client);
               // Disabled: the hit test skips it, so the mouse goes to the game's window as without it. Hidden until "Show()": a visible child
               // over the game's swapchain makes DWM compose the game's presents (no independent flip, no VRR) while it presents nothing.
               window = CreateWindowExW(WS_EX_NOPARENTNOTIFY, window_class.lpszClassName, L"", WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0, client.right, client.bottom, parent,
                  nullptr, instance, nullptr);
               SetEvent(created);
               MSG message;
               while (window && GetMessageW(&message, nullptr, 0, 0) > 0)
               {
                  TranslateMessage(&message);
                  DispatchMessageW(&message);
               }
               UnregisterClassW(window_class.lpszClassName, instance); // The window procedure must not outlive the addon
            });
         WaitForSingleObject(created, INFINITE);
         CloseHandle(created);
         return window != nullptr;
      }

      HWND Get() const
      {
         return window;
      }

      // Once the swapchain presented (from any thread: the window's thread shows it)
      void Show()
      {
         if (window && !shown)
         {
            ShowWindowAsync(window, SW_SHOWNA);
            shown = true;
         }
      }

      void Destroy()
      {
         if (window)
         {
            PostMessageW(window, WM_CLOSE, 0, 0); // Destroyed by its own thread, which then ends
         }
         if (thread.joinable())
         {
            thread.join();
         }
         window = nullptr;
         shown = false;
      }

   private:
      static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
      {
         switch (message)
         {
         case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
         case WM_CLOSE:
            DestroyWindow(window);
            return 0;
         case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
         default:
            return DefWindowProcW(window, message, w_param, l_param);
         }
      }

      HWND window = nullptr;
      bool shown = false;
      std::thread thread;
   };
} // namespace FrameGeneration
