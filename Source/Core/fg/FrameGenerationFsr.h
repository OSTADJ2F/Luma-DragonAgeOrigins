#pragma once

// FSR frame generation (the FidelityFX API: FSR 4 on RDNA4, else 3.1): the dll's swapchain proxy owns the presents and their pacing, the
// frame generation context interpolates from the proxy's callback.
#include "FrameGeneration.h"

#include "api/include/dx12/ffx_api_dx12.h"
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace FrameGeneration
{
   class FsrGenerator : public Generator
   {
   public:
      ~FsrGenerator() override
      {
         Destroy();
      }

      bool Create(const CreateDesc& desc) override
      {
         Destroy();
         create_desc = desc;
         // The game's folder holds the loader (and the dlls it loads); the super resolution has loaded it already
         module = LoadLibraryExW(L"amd_fidelityfx_loader_dx12.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
         if (!module)
         {
            return Fail("amd_fidelityfx_loader_dx12.dll not found next to the exe");
         }
         create_context = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
         destroy_context = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module, "ffxDestroyContext"));
         configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module, "ffxConfigure"));
         dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
         if (!create_context || !destroy_context || !configure || !dispatch)
         {
            return Fail("the FidelityFX loader lacks its entry points");
         }
         queue = desc.queue;
         if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
         {
            return Fail("no device");
         }

#if DEVELOPMENT
         ffxConfigureDescGlobalDebug1 debug_desc = {};
         debug_desc.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
         debug_desc.fpMessage = [](uint32_t type, const wchar_t* message)
         {
            Log("FFX frame generation %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message ? message : L"");
         };
         debug_desc.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
         configure(nullptr, &debug_desc.header);
#endif

         Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
         if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
         {
            return Fail("no DXGI factory");
         }
         BOOL tearing_supported = FALSE;
         factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing_supported, sizeof(tearing_supported));
         tearing = tearing_supported != FALSE;

         swapchain_desc = MakeSwapchainDesc(desc, buffer_count, tearing);
         swapchain_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
         swapchain_version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
         swapchain_create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
         swapchain_create.header.pNext = &swapchain_version.header;
         swapchain_create.swapchain = &swapchain;
         swapchain_create.hwnd = desc.window;
         swapchain_create.desc = &swapchain_desc;
         swapchain_create.dxgiFactory = factory.Get();
         swapchain_create.gameQueue = queue.Get();
         if (create_context(&swapchain_context, &swapchain_create.header, nullptr) != FFX_API_RETURN_OK || !swapchain)
         {
            swapchain_context = nullptr;
            return Fail("couldn't create the swapchain");
         }
         factory->MakeWindowAssociation(desc.window, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);

         SetOutputColorSpace(swapchain.Get(), desc);
         for (UINT i = 0; i < buffer_count; i++)
         {
            if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]))))
            {
               return Fail("no swapchain buffer");
            }
         }

         backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
         backend.header.pNext = &generation_version.header;
         backend.device = device.Get();
         generation_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
         generation_version.header.pNext = &hudless_create.header;
         generation_version.version = FFX_FRAMEGENERATION_VERSION;
         hudless_create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
         hudless_create.hudlessBackBufferFormat = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
         generation_create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
         generation_create.header.pNext = &backend.header;
         generation_create.flags = (desc.hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0) | (desc.inverted_depth ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0);
#if DEVELOPMENT
         generation_create.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
#endif
         generation_create.displaySize = {desc.width, desc.height};
         generation_create.maxRenderSize = {desc.render_width, desc.render_height};
         generation_create.backBufferFormat = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
         if (create_context(&context, &generation_create.header, nullptr) != FFX_API_RETURN_OK)
         {
            context = nullptr;
            return Fail("couldn't create the frame generation context");
         }

#if DEVELOPMENT
         char dump_path[MAX_PATH] = {};
         if (GetEnvironmentVariableA("LUMA_UPSCALER_FG_DUMP", dump_path, MAX_PATH))
         {
            dump.directory = dump_path;
            if (!dump.Create(device.Get(), buffers[0].Get()))
            {
               dump.directory.clear();
            }
         }
#endif
         return true;
      }

      bool Record(ID3D12GraphicsCommandList* list, const Frame& frame) override
      {
         // Disabled frames are presented as they are
         ffxConfigureDescFrameGeneration configure_desc = {};
         configure_desc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
         configure_desc.swapChain = swapchain.Get();
         configure_desc.frameGenerationCallback = DispatchCallback;
         configure_desc.frameGenerationCallbackUserContext = this;
         // A present callback replaces the proxy's own copy into its swapchain buffer (it is only set to read frames back)
#if DEVELOPMENT
         if (!dump.directory.empty())
         {
            configure_desc.presentCallback = PresentCallback;
            configure_desc.presentCallbackUserContext = this;
         }
#endif
         configure_desc.frameGenerationEnabled = frame.generate;
         configure_desc.allowAsyncWorkloads = false;
         // The context transitions the inputs from COMMON for its passes and back
         configure_desc.HUDLessColor = frame.generate ? ffxApiGetResourceDX12(frame.hudless, FFX_API_RESOURCE_STATE_COMMON) : FfxApiResource{};
         configure_desc.generationRect = {0, 0, int32_t(create_desc.width), int32_t(create_desc.height)};
         configure_desc.frameID = frame.n;
         if (configure(&context, &configure_desc.header) != FFX_API_RETURN_OK)
         {
            return false;
         }

         if (frame.generate)
         {
            // The camera from the view matrix (row vectors: view = [R; t], so the world axes are R's columns)
            const float* view = frame.view;
            ffxDispatchDescFrameGenerationPrepareV2 prepare = {};
            prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
            prepare.frameID = frame.n;
            prepare.commandList = list;
            prepare.renderSize = {frame.render_width, frame.render_height};
            prepare.jitterOffset = {-frame.jitter_x, -frame.jitter_y};
            prepare.motionVectorScale = {create_desc.mvs_x_scale, create_desc.mvs_y_scale};
            prepare.frameTimeDelta = frame.frame_ms;
            // The first generated frame after plain ones has no prepared previous frame
            prepare.reset = frame.reset || !was_generating;
            // Swapped for an inverted depth, as the super resolution does
            prepare.cameraNear = create_desc.inverted_depth ? frame.far_plane : frame.near_plane;
            prepare.cameraFar = create_desc.inverted_depth ? frame.near_plane : frame.far_plane;
            prepare.cameraFovAngleVertical = frame.vert_fov;
            prepare.viewSpaceToMetersFactor = 1.f;
            prepare.depth = ffxApiGetResourceDX12(frame.depth, FFX_API_RESOURCE_STATE_COMMON);
            prepare.motionVectors = ffxApiGetResourceDX12(frame.motion, FFX_API_RESOURCE_STATE_COMMON);
            for (int i = 0; i < 3; i++)
            {
               prepare.cameraRight[i] = view[i * 4 + 0];
               prepare.cameraUp[i] = view[i * 4 + 1];
               prepare.cameraForward[i] = view[i * 4 + 2];
               prepare.cameraPosition[i] = -(view[i * 4 + 0] * view[12] + view[i * 4 + 1] * view[13] + view[i * 4 + 2] * view[14]);
            }
            if (dispatch(&context, &prepare.header) != FFX_API_RETURN_OK)
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
         const HRESULT result = swapchain->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
         return SUCCEEDED(result);
      }

      void Destroy() override
      {
         if (context)
         {
            // Disabled, the callbacks and the HUD-less color cleared, then the contexts in the reverse order of their creation
            ffxConfigureDescFrameGeneration configure_desc = {};
            configure_desc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            configure_desc.swapChain = swapchain.Get();
            configure(&context, &configure_desc.header);
            destroy_context(&context, nullptr);
            context = nullptr;
         }
         if (swapchain_context)
         {
            destroy_context(&swapchain_context, nullptr);
            swapchain_context = nullptr;
         }
         swapchain.Reset();
         for (auto& buffer : buffers)
         {
            buffer.Reset();
         }
         if (queue)
         {
            // The proxy's last presents are done before the dump is read
            DrainQueue(queue.Get());
         }
#if DEVELOPMENT
         if (!dump.directory.empty())
         {
            dump.Write();
            dump.Reset();
         }
#endif
         queue.Reset();
         device.Reset();
         if (module)
         {
            FreeLibrary(module);
            module = nullptr;
         }
      }

   private:
      static constexpr UINT buffer_count = 3;

      bool Fail(const char* reason)
      {
         Log("FAIL FSR frame generation: %s\n", reason);
         Destroy();
         return false;
      }

      static ffxReturnCode_t DispatchCallback(ffxDispatchDescFrameGeneration* params, void* user_context)
      {
         auto* const self = static_cast<FsrGenerator*>(user_context);
         return self->dispatch(&self->context, &params->header);
      }

#if DEVELOPMENT
      // FidelityFX's resource states as D3D12's
      static D3D12_RESOURCE_STATES ToD3D12State(uint32_t state)
      {
         D3D12_RESOURCE_STATES result = D3D12_RESOURCE_STATE_COMMON;
         if (state & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)
            result |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
         if (state & FFX_API_RESOURCE_STATE_COMPUTE_READ)
            result |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
         if (state & FFX_API_RESOURCE_STATE_PIXEL_READ)
            result |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
         if (state & FFX_API_RESOURCE_STATE_COPY_SRC)
            result |= D3D12_RESOURCE_STATE_COPY_SOURCE;
         if (state & FFX_API_RESOURCE_STATE_COPY_DEST)
            result |= D3D12_RESOURCE_STATE_COPY_DEST;
         if (state & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT)
            result |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
         if (state & FFX_API_RESOURCE_STATE_RENDER_TARGET)
            result |= D3D12_RESOURCE_STATE_RENDER_TARGET;
         if (state & FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT)
            result |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
         return result; // COMMON and PRESENT are both 0
      }

      // Development: "LUMA_UPSCALER_FG_DUMP=<dir>" reads back a few of the presented frames, real and generated (the offline test checks them)
      struct Dump
      {
         static constexpr uint32_t skipped = 30; // Presents before the first one kept (the generator settles)
         static constexpr uint32_t kept = 12;

         struct Entry
         {
            uint32_t sequence = 0;
            uint64_t frame_id = 0;
            bool generated = false;
         };

         std::string directory;
         Microsoft::WRL::ComPtr<ID3D12Resource> readback[kept];
         D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
         uint32_t rows = 0;
         uint64_t row_size = 0;
         std::atomic<uint32_t> presents = 0;
         std::mutex mutex;
         std::vector<Entry> entries;

         bool Create(ID3D12Device* device, ID3D12Resource* swapchain_buffer)
         {
            const D3D12_RESOURCE_DESC texture_desc = swapchain_buffer->GetDesc();
            uint64_t total = 0;
            UINT row_count = 0;
            device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, &row_count, &row_size, &total);
            rows = row_count;
            const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_READBACK};
            const D3D12_RESOURCE_DESC buffer = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
               .Width = total,
               .Height = 1,
               .DepthOrArraySize = 1,
               .MipLevels = 1,
               .SampleDesc = {.Count = 1},
               .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
            for (auto& resource : readback)
            {
               if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource))))
                  return false;
            }
            return true;
         }

         void Reset()
         {
            directory.clear();
            entries.clear();
            presents = 0;
            for (auto& resource : readback)
               resource.Reset();
         }

         // After the GPU is done: the kept frames as tightly packed R10G10B10A2 files
         void Write()
         {
            for (const Entry& entry : entries)
            {
               const auto& frame_readback = readback[entry.sequence - skipped];
               char path[MAX_PATH] = {};
               snprintf(path, MAX_PATH, "%s\\fg_%03u_%s_f%llu.raw", directory.c_str(), entry.sequence, entry.generated ? "gen" : "real", (unsigned long long)entry.frame_id);
               void* mapped = nullptr;
               FILE* file = nullptr;
               if (SUCCEEDED(frame_readback->Map(0, nullptr, &mapped)) && (file = fopen(path, "wb")))
               {
                  for (uint32_t row = 0; row < rows; row++)
                  {
                     fwrite(static_cast<const char*>(mapped) + size_t(row) * footprint.Footprint.RowPitch, 1, size_t(row_size), file);
                  }
                  fclose(file);
               }
               frame_readback->Unmap(0, nullptr);
            }
            Log("frame generation dump: %zu frames of %u presents in %s\n", entries.size(), presents.load(), directory.c_str());
         }
      };

      // Copies the frame into the swapchain's buffer (what the proxy does without a callback) and keeps some of them
      static ffxReturnCode_t PresentCallback(ffxCallbackDescFrameGenerationPresent* params, void* user_context)
      {
         auto* const self = static_cast<FsrGenerator*>(user_context);
         auto* const list = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
         auto* const source = static_cast<ID3D12Resource*>(params->currentBackBuffer.resource);
         auto* const target = static_cast<ID3D12Resource*>(params->outputSwapChainBuffer.resource);
         const D3D12_RESOURCE_STATES source_state = ToD3D12State(params->currentBackBuffer.state);
         const D3D12_RESOURCE_STATES target_state = ToD3D12State(params->outputSwapChainBuffer.state);
         Dump& dump = self->dump;
         const uint32_t sequence = dump.presents.fetch_add(1);
         const bool keep = sequence >= Dump::skipped && sequence < Dump::skipped + Dump::kept;

         D3D12_RESOURCE_BARRIER to_copy[2], from_copy[2];
         UINT count = 0;
         if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE && !(source == target && source_state == D3D12_RESOURCE_STATE_COPY_DEST))
         {
            to_copy[count] = Transition(source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            from_copy[count++] = Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
         }
         if (source != target && target_state != D3D12_RESOURCE_STATE_COPY_DEST)
         {
            to_copy[count] = Transition(target, target_state, D3D12_RESOURCE_STATE_COPY_DEST);
            from_copy[count++] = Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, target_state);
         }
         if (count)
            list->ResourceBarrier(count, to_copy);
         if (source != target)
            list->CopyResource(target, source);
         if (keep)
         {
            const D3D12_TEXTURE_COPY_LOCATION destination = {
               .pResource = dump.readback[sequence - Dump::skipped].Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = dump.footprint};
            const D3D12_TEXTURE_COPY_LOCATION origin = {.pResource = source, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
            list->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);
            const std::lock_guard lock(dump.mutex);
            dump.entries.push_back({.sequence = sequence, .frame_id = params->frameID, .generated = params->isGeneratedFrame});
         }
         if (count)
            list->ResourceBarrier(count, from_copy);
         return FFX_API_RETURN_OK;
      }

      Dump dump;
#else
      struct
      {
         std::string directory;
      } dump;
#endif

      CreateDesc create_desc;
      HMODULE module = nullptr;
      PfnFfxCreateContext create_context = nullptr;
      PfnFfxDestroyContext destroy_context = nullptr;
      PfnFfxConfigure configure = nullptr;
      PfnFfxDispatch dispatch = nullptr;
      Microsoft::WRL::ComPtr<ID3D12Device> device;
      Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
      Microsoft::WRL::ComPtr<IDXGISwapChain4> swapchain;
      Microsoft::WRL::ComPtr<ID3D12Resource> buffers[buffer_count];
      bool tearing = false;
      bool was_generating = false;

      // The create descriptions, which the dll keeps pointing to until its contexts are destroyed
      DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
      ffxCreateContextDescFrameGenerationSwapChainVersionDX12 swapchain_version = {};
      ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 swapchain_create = {};
      ffxCreateBackendDX12Desc backend = {};
      ffxCreateContextDescFrameGenerationVersion generation_version = {};
      ffxCreateContextDescFrameGenerationHudless hudless_create = {};
      ffxCreateContextDescFrameGeneration generation_create = {};
      ffxContext swapchain_context = nullptr;
      ffxContext context = nullptr;
   };
} // namespace FrameGeneration
