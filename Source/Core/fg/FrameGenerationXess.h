#pragma once

// XeSS frame generation (XeFG, libxess_fg.dll) with its mandatory XeLL latency library (libxell.dll): the dll's swapchain proxy owns the
// presents and their pacing. Both are loaded from the exe's folder; nothing is linked.
#include "FrameGeneration.h"

#include "xell/xell_d3d12.h"
#include "xess_fg/xefg_swapchain_d3d12.h"

#include <cstdio>

namespace FrameGeneration
{
   class XessGenerator : public Generator
   {
   public:
      ~XessGenerator() override
      {
         Destroy();
      }

      bool Create(const CreateDesc& desc) override
      {
         Destroy();
         create_desc = desc;
         xell_module = LoadLibraryExW(L"libxell.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
         fg_module = LoadLibraryExW(L"libxess_fg.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
         if (!xell_module || !fg_module)
         {
            return Fail("libxell.dll or libxess_fg.dll not found next to the exe");
         }
         xell_create = Import<decltype(&xellD3D12CreateContext)>(xell_module, "xellD3D12CreateContext");
         xell_destroy = Import<decltype(&xellDestroyContext)>(xell_module, "xellDestroyContext");
         xell_set_sleep_mode = Import<decltype(&xellSetSleepMode)>(xell_module, "xellSetSleepMode");
         xell_sleep = Import<decltype(&xellSleep)>(xell_module, "xellSleep");
         xell_add_marker = Import<decltype(&xellAddMarkerData)>(xell_module, "xellAddMarkerData");
         fg_create = Import<decltype(&xefgSwapChainD3D12CreateContext)>(fg_module, "xefgSwapChainD3D12CreateContext");
         fg_init = Import<decltype(&xefgSwapChainD3D12InitFromSwapChainDesc)>(fg_module, "xefgSwapChainD3D12InitFromSwapChainDesc");
         fg_get_swapchain = Import<decltype(&xefgSwapChainD3D12GetSwapChainPtr)>(fg_module, "xefgSwapChainD3D12GetSwapChainPtr");
         fg_tag_resource = Import<decltype(&xefgSwapChainD3D12TagFrameResource)>(fg_module, "xefgSwapChainD3D12TagFrameResource");
         fg_tag_constants = Import<decltype(&xefgSwapChainTagFrameConstants)>(fg_module, "xefgSwapChainTagFrameConstants");
         fg_set_enabled = Import<decltype(&xefgSwapChainSetEnabled)>(fg_module, "xefgSwapChainSetEnabled");
         fg_set_present_id = Import<decltype(&xefgSwapChainSetPresentId)>(fg_module, "xefgSwapChainSetPresentId");
         fg_present_status = Import<decltype(&xefgSwapChainGetLastPresentStatus)>(fg_module, "xefgSwapChainGetLastPresentStatus");
         fg_set_latency = Import<decltype(&xefgSwapChainSetLatencyReduction)>(fg_module, "xefgSwapChainSetLatencyReduction");
         fg_set_ui_composition = Import<decltype(&xefgSwapChainSetUiCompositionState)>(fg_module, "xefgSwapChainSetUiCompositionState");
         fg_set_logging = Import<decltype(&xefgSwapChainSetLoggingCallback)>(fg_module, "xefgSwapChainSetLoggingCallback");
         fg_destroy = Import<decltype(&xefgSwapChainDestroy)>(fg_module, "xefgSwapChainDestroy");
         if (!xell_create || !xell_destroy || !xell_set_sleep_mode || !xell_sleep || !xell_add_marker || !fg_create || !fg_init || !fg_get_swapchain || !fg_tag_resource || !fg_tag_constants ||
             !fg_set_enabled || !fg_set_present_id || !fg_present_status || !fg_set_latency || !fg_set_ui_composition || !fg_set_logging || !fg_destroy)
         {
            return Fail("the XeSS-FG or XeLL dll lacks its entry points");
         }
         queue = desc.queue;
         if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
         {
            return Fail("no device");
         }

         // XeLL first and enabled before the frame generation, destroyed after it. No frame limiter of its own (the interval 0).
         if (const xell_result_t result = xell_create(device.Get(), &xell); result != XELL_RESULT_SUCCESS)
         {
            return Fail("couldn't create the XeLL context", result);
         }
         const xell_sleep_params_t sleep_params = {.minimumIntervalUs = 0, .bLowLatencyMode = 1};
         if (const xell_result_t result = xell_set_sleep_mode(xell, &sleep_params); result != XELL_RESULT_SUCCESS)
         {
            return Fail("couldn't enable XeLL", result);
         }
         if (const xefg_swapchain_result_t result = fg_create(device.Get(), &fg); result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
         {
            fg = nullptr;
            return Fail("couldn't create the frame generation context (an Intel Arc GPU is needed)", result);
         }
         fg_set_logging(fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, LogCallback, nullptr);
         if (const xefg_swapchain_result_t result = fg_set_latency(fg, xell); result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
         {
            return Fail("couldn't attach XeLL", result);
         }

         Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
         if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
         {
            return Fail("no DXGI factory");
         }
         BOOL tearing_supported = FALSE;
         factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing_supported, sizeof(tearing_supported));
         tearing = tearing_supported != FALSE;

         // The motion vectors are in pixels (the scale below turns the shared UV deltas into them)
         xefg_swapchain_d3d12_init_params_t init_params = {
            .initFlags = uint32_t((desc.inverted_depth ? XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH : 0) | (desc.mvs_jittered ? XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV : 0)),
            .maxInterpolatedFrames = 1,
            .uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS,
         };
         const DXGI_SWAP_CHAIN_DESC1 swapchain_desc = MakeSwapchainDesc(desc, buffer_count, tearing);
         if (const xefg_swapchain_result_t result = fg_init(fg, desc.window, &swapchain_desc, nullptr, queue.Get(), factory.Get(), &init_params); result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
         {
            return Fail("couldn't initialize the swapchain", result);
         }
         if (const xefg_swapchain_result_t result = fg_get_swapchain(fg, IID_PPV_ARGS(&swapchain)); result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
         {
            return Fail("no swapchain", result);
         }
         factory->MakeWindowAssociation(desc.window, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
         // The HUD-less mode composes the UI only while this is enabled
         fg_set_ui_composition(fg, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
         SetOutputColorSpace(swapchain.Get(), desc);
         for (UINT i = 0; i < buffer_count; i++)
         {
            if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]))))
            {
               return Fail("no swapchain buffer");
            }
         }
         return true;
      }

      bool Record(ID3D12GraphicsCommandList* list, const Frame& frame) override
      {
         present_id = uint32_t(frame.n);
         // Disabled frames are presented as they are. Toggling discards the history, which is stale after a gap anyway: the first generated frame resets it.
         if (fg_set_enabled(fg, frame.generate ? 1 : 0) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
         {
            return false;
         }
         if (frame.generate)
         {
            xefg_swapchain_frame_constant_data_t constants = {
               .jitterOffsetX = frame.jitter_x,
               .jitterOffsetY = frame.jitter_y,
               .motionVectorScaleX = create_desc.mvs_x_scale,
               .motionVectorScaleY = create_desc.mvs_y_scale,
               .resetHistory = uint32_t(frame.reset || !was_generating),
               .frameRenderTime = frame.frame_ms,
            };
            memcpy(constants.viewMatrix, frame.view, sizeof(constants.viewMatrix));
            memcpy(constants.projectionMatrix, frame.projection, sizeof(constants.projectionMatrix));
            if (fg_tag_constants(fg, present_id, &constants) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
               return false;
            }
            // Copied by the library into this list ("ONLY_NOW"): the shared textures stay ours (COMMON between frames) and nothing of them is read at the
            // present. Depth and motion vectors have to share one size.
            struct Tag
            {
               xefg_swapchain_resource_type_t type;
               ID3D12Resource* resource;
               uint32_t width, height;
            };
            const Tag tags[] = {
               {XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, frame.hudless, create_desc.width, create_desc.height},
               {XEFG_SWAPCHAIN_RES_DEPTH, frame.depth, frame.render_width, frame.render_height},
               {XEFG_SWAPCHAIN_RES_MOTION_VECTOR, frame.motion, frame.render_width, frame.render_height},
            };
            constexpr D3D12_RESOURCE_STATES tag_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            D3D12_RESOURCE_BARRIER to_tag[3], from_tag[3];
            for (int i = 0; i < 3; i++)
            {
               to_tag[i] = Transition(tags[i].resource, D3D12_RESOURCE_STATE_COMMON, tag_state);
               from_tag[i] = Transition(tags[i].resource, tag_state, D3D12_RESOURCE_STATE_COMMON);
            }
            list->ResourceBarrier(3, to_tag);
            bool tagged = true;
            for (const Tag& tag : tags)
            {
               const xefg_swapchain_d3d12_resource_data_t data = {
                  .type = tag.type,
                  .validity = XEFG_SWAPCHAIN_RV_ONLY_NOW,
                  .resourceBase = {0, 0},
                  .resourceSize = {tag.width, tag.height},
                  .pResource = tag.resource,
                  .incomingState = tag_state,
               };
               tagged = tagged && fg_tag_resource(fg, list, present_id, &data) == XEFG_SWAPCHAIN_RESULT_SUCCESS;
            }
            list->ResourceBarrier(3, from_tag);
            if (!tagged)
            {
               return false;
            }
         }
         was_generating = frame.generate;

         // The color into the proxy's current buffer (presented when the proxy decides to)
         ID3D12Resource* const buffer = buffers[swapchain->GetCurrentBackBufferIndex()].Get();
         const D3D12_RESOURCE_BARRIER to_copy[] = {Transition(frame.color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
         list->ResourceBarrier(2, to_copy);
         list->CopyResource(buffer, frame.color);
         const D3D12_RESOURCE_BARRIER from_copy[] = {Transition(frame.color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            Transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)};
         list->ResourceBarrier(2, from_copy);
         return true;
      }

      bool Present() override
      {
         if (fg_set_present_id(fg, present_id) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
         {
            return false;
         }
         xell_add_marker(xell, present_id, XELL_PRESENT_START);
         const HRESULT result = swapchain->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
         xell_add_marker(xell, present_id, XELL_PRESENT_END);
         // The previous present's outcome (warnings are normal while the history builds up); an error is logged when it changes
         xefg_swapchain_present_status_t status = {};
         if (fg_present_status(fg, &status) == XEFG_SWAPCHAIN_RESULT_SUCCESS && status.frameGenResult != last_result)
         {
            last_result = status.frameGenResult;
            Log("XeFG present status %d (generation %s, %u frames presented)\n", int(status.frameGenResult), status.isFrameGenEnabled ? "on" : "off", status.framesPresented);
         }
         presents++;
         presented_frames += status.framesPresented;
         return SUCCEEDED(result);
      }

      void Destroy() override
      {
         if (presents)
         {
            Log("XeFG: %llu presents showed %llu frames\n", (unsigned long long)presents, (unsigned long long)presented_frames);
            presents = presented_frames = 0;
         }
         if (queue)
         {
            DrainQueue(queue.Get());
         }
         for (auto& buffer : buffers)
         {
            buffer.Reset();
         }
         swapchain.Reset();
         // The frame generation (it owns the swapchain) before XeLL
         if (fg)
         {
            fg_destroy(fg);
            fg = nullptr;
         }
         if (xell)
         {
            xell_destroy(xell);
            xell = nullptr;
         }
         queue.Reset();
         device.Reset();
         for (HMODULE* module : {&fg_module, &xell_module})
         {
            if (*module)
            {
               FreeLibrary(*module);
               *module = nullptr;
            }
         }
         was_generating = false;
         last_result = XEFG_SWAPCHAIN_RESULT_SUCCESS;
      }

      void Marker(LatencyMarker marker, uint64_t n) override
      {
         if (!xell)
         {
            return;
         }
         switch (marker)
         {
         case kSimulationStart:
            xell_add_marker(xell, uint32_t(n), XELL_SIMULATION_START);
            break;
         case kSimulationEnd:
            xell_add_marker(xell, uint32_t(n), XELL_SIMULATION_END);
            break;
         case kRenderSubmitStart:
            xell_add_marker(xell, uint32_t(n), XELL_RENDERSUBMIT_START);
            break;
         case kRenderSubmitEnd:
            xell_add_marker(xell, uint32_t(n), XELL_RENDERSUBMIT_END);
            break;
         }
      }

      void Sleep(uint64_t n, HANDLE sleep_event) override
      {
         if (xell)
         {
            xell_sleep(xell, uint32_t(n));
         }
         SetEvent(sleep_event);
      }

   private:
      static constexpr UINT buffer_count = 3;

      template <typename Function>
      static Function Import(HMODULE module, const char* name)
      {
         return reinterpret_cast<Function>(GetProcAddress(module, name));
      }

      bool Fail(const char* reason, int result = 0)
      {
         Log("FAIL XeSS frame generation: %s (result %d)\n", reason, result);
         Destroy();
         return false;
      }

      static void LogCallback(const char* message, xefg_swapchain_logging_level_t level, void*)
      {
         Log("XeFG %s: %s\n", level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR ? "error" : "warning", message ? message : "");
      }

      CreateDesc create_desc;
      HMODULE xell_module = nullptr, fg_module = nullptr;
      decltype(&xellD3D12CreateContext) xell_create = nullptr;
      decltype(&xellDestroyContext) xell_destroy = nullptr;
      decltype(&xellSetSleepMode) xell_set_sleep_mode = nullptr;
      decltype(&xellSleep) xell_sleep = nullptr;
      decltype(&xellAddMarkerData) xell_add_marker = nullptr;
      decltype(&xefgSwapChainD3D12CreateContext) fg_create = nullptr;
      decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) fg_init = nullptr;
      decltype(&xefgSwapChainD3D12GetSwapChainPtr) fg_get_swapchain = nullptr;
      decltype(&xefgSwapChainD3D12TagFrameResource) fg_tag_resource = nullptr;
      decltype(&xefgSwapChainTagFrameConstants) fg_tag_constants = nullptr;
      decltype(&xefgSwapChainSetEnabled) fg_set_enabled = nullptr;
      decltype(&xefgSwapChainSetPresentId) fg_set_present_id = nullptr;
      decltype(&xefgSwapChainGetLastPresentStatus) fg_present_status = nullptr;
      decltype(&xefgSwapChainSetLatencyReduction) fg_set_latency = nullptr;
      decltype(&xefgSwapChainSetUiCompositionState) fg_set_ui_composition = nullptr;
      decltype(&xefgSwapChainSetLoggingCallback) fg_set_logging = nullptr;
      decltype(&xefgSwapChainDestroy) fg_destroy = nullptr;
      Microsoft::WRL::ComPtr<ID3D12Device> device;
      Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
      Microsoft::WRL::ComPtr<IDXGISwapChain4> swapchain;
      Microsoft::WRL::ComPtr<ID3D12Resource> buffers[buffer_count];
      xell_context_handle_t xell = nullptr;
      xefg_swapchain_handle_t fg = nullptr;
      bool tearing = false;
      bool was_generating = false;
      uint32_t present_id = 0;
      xefg_swapchain_result_t last_result = XEFG_SWAPCHAIN_RESULT_SUCCESS;
      uint64_t presents = 0, presented_frames = 0; // Of the session: the presents, and the frames the library says it showed for them
   };
} // namespace FrameGeneration
