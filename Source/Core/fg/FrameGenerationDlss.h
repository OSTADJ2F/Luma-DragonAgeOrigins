#pragma once

// DLSS frame generation (DLSS-G) through Streamline with manual hooking: our D3D12 device, queue and the super resolution's NGX stay native, only the
// swapchain is handed to Streamline ("slUpgradeInterface"). Reflex/PCL markers come from the game. sl.interposer.dll and the plugins are loaded
// from the exe's folder; nothing is linked.
#include "FrameGeneration.h"

#include "../../External/Streamline/include/sl.h"
#include "../../External/Streamline/include/sl_dlss_g.h"
#include "../../External/Streamline/include/sl_pcl.h"
#include "../../External/Streamline/include/sl_reflex.h"

#include <DirectXMath.h>

#include <cstdio>

namespace FrameGeneration
{
   class DlssGenerator : public Generator
   {
   public:
      ~DlssGenerator() override
      {
         Destroy();
      }

      bool Create(const CreateDesc& desc) override
      {
         Destroy();
         create_desc = desc;
         queue = desc.queue;
         if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
         {
            return Fail("no device");
         }
         if (!ready && !InitializeStreamline())
         {
            return false;
         }

         // The swapchain is made by Streamline's factory proxy (the creation hook is mandatory), on the native queue
         Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
         if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
         {
            return Fail("no DXGI factory");
         }
         BOOL tearing_supported = FALSE;
         factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing_supported, sizeof(tearing_supported));
         tearing = tearing_supported != FALSE;
         const DXGI_SWAP_CHAIN_DESC1 swapchain_desc = MakeSwapchainDesc(desc, buffer_count, tearing);
         Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain1;
         IDXGIFactory5* upgraded_factory = factory.Detach();
         const sl::Result factory_result = sl_upgrade_interface(reinterpret_cast<void**>(&upgraded_factory));
         factory.Attach(upgraded_factory);
         if (factory_result != sl::Result::eOk)
         {
            return Fail("slUpgradeInterface of the factory failed", int(factory_result));
         }
         if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), desc.window, &swapchain_desc, nullptr, nullptr, &swapchain1)) || FAILED(swapchain1.As(&swapchain)))
         {
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

         options = {};
         options.mode = sl::DLSSGMode::eOff;
         options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
         options.numBackBuffers = buffer_count;
         options.mvecDepthWidth = desc.render_width;
         options.mvecDepthHeight = desc.render_height;
         options.colorWidth = desc.width;
         options.colorHeight = desc.height;
         if (const sl::Result result = dlssg_set_options(viewport, options); result != sl::Result::eOk)
         {
            return Fail("slDLSSGSetOptions failed", int(result));
         }
         return true;
      }

      bool Record(ID3D12GraphicsCommandList* list, const Frame& frame) override
      {
         if (frame.generate != (options.mode == sl::DLSSGMode::eOn))
         {
            options.mode = frame.generate ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
            if (dlssg_set_options(viewport, options) != sl::Result::eOk)
            {
               return false;
            }
         }
         present_id = uint32_t(frame.n);
         sl::FrameToken* token = nullptr;
         sl_get_new_frame_token(token, &present_id);

         using namespace DirectX;
         // The matrices are row-major with row vectors (DirectXMath's), as Streamline's: viewToClip = projection, and
         // clipToPrevClip = clipToView * viewToWorld * worldToPrevView * prevViewToPrevClip
         const XMMATRIX view = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(frame.view));
         const XMMATRIX projection = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(frame.projection));
         const bool has_previous = has_previous_matrices && !frame.reset;
         const XMMATRIX previous_view = has_previous ? XMLoadFloat4x4(&previous_view_matrix) : view;
         const XMMATRIX previous_projection = has_previous ? XMLoadFloat4x4(&previous_projection_matrix) : projection;
         XMStoreFloat4x4(&previous_view_matrix, view);
         XMStoreFloat4x4(&previous_projection_matrix, projection);
         has_previous_matrices = true;

         if (frame.generate)
         {
            const XMMATRIX view_to_world = XMMatrixInverse(nullptr, view);
            const XMMATRIX clip_to_view = XMMatrixInverse(nullptr, projection);
            const XMMATRIX clip_to_prev_clip = clip_to_view * view_to_world * previous_view * previous_projection;
            const XMMATRIX prev_clip_to_clip = XMMatrixInverse(nullptr, clip_to_prev_clip);
            XMFLOAT4X4 camera, clip_to_view_float, clip_to_prev_clip_float, prev_clip_to_clip_float;
            XMStoreFloat4x4(&camera, view_to_world); // Rows: the camera's right, up and forward axes in the world, then its position
            XMStoreFloat4x4(&clip_to_view_float, clip_to_view);
            XMStoreFloat4x4(&clip_to_prev_clip_float, clip_to_prev_clip);
            XMStoreFloat4x4(&prev_clip_to_clip_float, prev_clip_to_clip);

            sl::Constants constants = {};
            constants.cameraViewToClip = ToSl(frame.projection);
            constants.clipToCameraView = ToSl(&clip_to_view_float.m[0][0]);
            constants.clipToLensClip = ToSl(identity);
            constants.clipToPrevClip = ToSl(&clip_to_prev_clip_float.m[0][0]);
            constants.prevClipToClip = ToSl(&prev_clip_to_clip_float.m[0][0]);
            constants.jitterOffset = {frame.jitter_x, frame.jitter_y};
            // The motion vectors are in pixels once multiplied by the scale; Streamline wants them in [-1, 1] (the render size's UV)
            constants.mvecScale = {create_desc.mvs_x_scale / float(frame.render_width), create_desc.mvs_y_scale / float(frame.render_height)};
            constants.cameraPinholeOffset = {0.f, 0.f};
            constants.cameraPos = {camera.m[3][0], camera.m[3][1], camera.m[3][2]};
            constants.cameraRight = {camera.m[0][0], camera.m[0][1], camera.m[0][2]};
            constants.cameraUp = {camera.m[1][0], camera.m[1][1], camera.m[1][2]};
            constants.cameraFwd = {camera.m[2][0], camera.m[2][1], camera.m[2][2]};
            constants.cameraNear = frame.near_plane;
            constants.cameraFar = frame.far_plane;
            constants.cameraFOV = frame.vert_fov;
            constants.cameraAspectRatio = float(create_desc.width) / float(create_desc.height);
            constants.motionVectorsInvalidValue = 0.f;
            constants.depthInverted = create_desc.inverted_depth ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            constants.cameraMotionIncluded = sl::Boolean::eTrue;
            constants.motionVectors3D = sl::Boolean::eFalse;
            constants.reset = (frame.reset || !was_generating) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            constants.motionVectorsJittered = create_desc.mvs_jittered ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            if (sl_set_constants(constants, *token, viewport) != sl::Result::eOk)
            {
               return false;
            }

            // Read by DLSS-G at the present ("eValidUntilPresent", no copies of Streamline's); the textures are in COMMON then, as before and after
            sl::Resource depth(sl::ResourceType::eTex2d, frame.depth, D3D12_RESOURCE_STATE_COMMON);
            sl::Resource motion(sl::ResourceType::eTex2d, frame.motion, D3D12_RESOURCE_STATE_COMMON);
            sl::Resource hudless(sl::ResourceType::eTex2d, frame.hudless, D3D12_RESOURCE_STATE_COMMON);
            const sl::Extent render_extent = {.width = frame.render_width, .height = frame.render_height};
            const sl::Extent display_extent = {.width = create_desc.width, .height = create_desc.height};
            const sl::ResourceTag tags[] = {
               {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &render_extent},
               {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &render_extent},
               {&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &display_extent},
            };
            if (sl_set_tag_for_frame(*token, viewport, tags, uint32_t(std::size(tags)), list) != sl::Result::eOk)
            {
               return false;
            }
         }
         was_generating = frame.generate;

         // The color into the proxy's current buffer (DLSS-G fails without a "GetCurrentBackBufferIndex" call each frame)
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
         sl::FrameToken* token = nullptr;
         sl_get_new_frame_token(token, &present_id);
         pcl_set_marker(sl::PCLMarker::ePresentStart, *token);
         const HRESULT result = swapchain->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
         pcl_set_marker(sl::PCLMarker::ePresentEnd, *token);

         // DLSS-G only generates while the game's window is in front. Logged on each change, with what it did.
         const HWND root = GetAncestor(create_desc.window, GA_ROOT);
         const bool in_front = GetForegroundWindow() == root;
         sl::DLSSGState state = {};
         if (dlssg_get_state(viewport, state, nullptr) == sl::Result::eOk && (state.status != last_status || in_front != was_in_front))
         {
            last_status = state.status;
            was_in_front = in_front;
            Log("DLSS-G status 0x%X, %u frames presented for the last present, the game's window %s\n", unsigned(state.status), state.numFramesActuallyPresented,
               in_front ? "in front" : "NOT in front (no generation)");
         }
         presents++;
         presented_frames += state.numFramesActuallyPresented;
         return SUCCEEDED(result);
      }

      void Destroy() override
      {
         if (presents)
         {
            Log("DLSS-G: %llu presents showed %llu frames\n", (unsigned long long)presents, (unsigned long long)presented_frames);
            presents = presented_frames = 0;
         }
         if (queue)
         {
            if (dlssg_set_options && options.mode != sl::DLSSGMode::eOff)
            {
               options.mode = sl::DLSSGMode::eOff;
               dlssg_set_options(viewport, options);
            }
            DrainQueue(queue.Get());
         }
         for (auto& buffer : buffers)
         {
            buffer.Reset();
         }
         swapchain.Reset();
         queue.Reset();
         device.Reset();
         was_generating = false;
         has_previous_matrices = false;
         options = {};
      }

      // Streamline stays up between sessions (it can't be initialized again) and goes down once the super resolution's NGX has: it shuts NGX down
      // too, and the super resolution crashed on that when it ran first
      static void Shutdown()
      {
         if (initialized)
         {
            sl_shutdown();
            initialized = ready = false;
         }
      }

      void Marker(LatencyMarker marker, uint64_t n) override
      {
         if (!pcl_set_marker)
         {
            return;
         }
         // The first four of PCL's markers are the protocol's, in the same order
         static_assert(int(kRenderSubmitEnd) == int(sl::PCLMarker::eRenderSubmitEnd));
         sl::FrameToken* token = nullptr;
         const uint32_t index = uint32_t(n);
         sl_get_new_frame_token(token, &index);
         pcl_set_marker(sl::PCLMarker(marker), *token);
      }

      void Sleep(uint64_t n, HANDLE sleep_event) override
      {
         if (reflex_sleep)
         {
            sl::FrameToken* token = nullptr;
            const uint32_t index = uint32_t(n);
            sl_get_new_frame_token(token, &index);
            reflex_sleep(*token);
         }
         SetEvent(sleep_event);
      }

   private:
      static constexpr UINT buffer_count = 3;
      static constexpr float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

      // Once per process (the device is the helper's only one)
      bool InitializeStreamline()
      {
         // In a "Streamline" folder next to the exe, not next to it: NGX loads every feature dll it finds next to the exe at each
         // init, and the game's own D3D11 NGX (DLSS on D3D11) crashed in nvngx_dlssg.dll's D3D11 init (MELE, its third device)
         wchar_t streamline_directory[MAX_PATH] = {};
         GetModuleFileNameW(nullptr, streamline_directory, MAX_PATH);
         *wcsrchr(streamline_directory, L'\\') = 0;
         wcscat_s(streamline_directory, L"\\Streamline");
         if (!interposer)
         {
            const std::wstring interposer_path = std::wstring(streamline_directory) + L"\\sl.interposer.dll";
            interposer = LoadLibraryExW(interposer_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!interposer)
            {
               return Fail("Streamline\\sl.interposer.dll not found next to the exe");
            }
            sl_init = Import<PFun_slInit>("slInit");
            sl_shutdown = Import<PFun_slShutdown>("slShutdown");
            sl_is_feature_supported = Import<PFun_slIsFeatureSupported>("slIsFeatureSupported");
            sl_set_d3d_device = Import<PFun_slSetD3DDevice>("slSetD3DDevice");
            sl_upgrade_interface = Import<PFun_slUpgradeInterface>("slUpgradeInterface");
            sl_get_feature_function = Import<PFun_slGetFeatureFunction>("slGetFeatureFunction");
            sl_get_new_frame_token = Import<PFun_slGetNewFrameToken>("slGetNewFrameToken");
            sl_set_constants = Import<PFun_slSetConstants>("slSetConstants");
            sl_set_tag_for_frame = Import<PFun_slSetTagForFrame>("slSetTagForFrame");
         }
         if (!sl_init || !sl_shutdown || !sl_is_feature_supported || !sl_set_d3d_device || !sl_upgrade_interface || !sl_get_feature_function || !sl_get_new_frame_token || !sl_set_constants ||
             !sl_set_tag_for_frame)
         {
            return Fail("sl.interposer.dll lacks its entry points");
         }
         if (!initialized)
         {
            wchar_t exe_directory[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe_directory, MAX_PATH);
            *wcsrchr(exe_directory, L'\\') = 0;
            const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
            sl::Preferences preferences = {};
            preferences.logLevel = sl::LogLevel::eDefault;
            preferences.logMessageCallback = LogCallback;
            preferences.pathToLogsAndData = exe_directory;
            const wchar_t* plugin_paths[] = {streamline_directory};
            preferences.pathsToPlugins = plugin_paths;
            preferences.numPathsToPlugins = uint32_t(std::size(plugin_paths));
            preferences.featuresToLoad = features;
            preferences.numFeaturesToLoad = uint32_t(std::size(features));
            preferences.engine = sl::EngineType::eCustom;
            preferences.engineVersion = "1.0";
            preferences.projectId = "d8238c51-1f2f-438d-a309-38c16e33c716"; // Luma's (Source/Core/dlss/DLSS.cpp)
            preferences.renderAPI = sl::RenderAPI::eD3D12;
            // Manual hooking; the frame based tagging is needed, "slSetTagForFrame" fails (result 19) without it; the command list state tracking isn't
            preferences.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
            if (const sl::Result result = sl_init(preferences, sl::kSDKVersion); result != sl::Result::eOk)
            {
               return Fail("slInit failed", int(result));
            }
            initialized = true;
         }

         const LUID luid = device->GetAdapterLuid();
         sl::AdapterInfo adapter_info = {};
         adapter_info.deviceLUID = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&luid));
         adapter_info.deviceLUIDSizeInBytes = sizeof(luid);
         if (const sl::Result result = sl_is_feature_supported(sl::kFeatureDLSS_G, adapter_info); result != sl::Result::eOk)
         {
            return Fail("DLSS-G isn't supported (RTX 40 and hardware accelerated GPU scheduling are needed)", int(result));
         }
         if (const sl::Result result = sl_set_d3d_device(device.Get()); result != sl::Result::eOk)
         {
            return Fail("slSetD3DDevice failed", int(result));
         }
         // Only after the device is set
         if (sl_get_feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void*&>(dlssg_set_options)) != sl::Result::eOk ||
             sl_get_feature_function(sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void*&>(dlssg_get_state)) != sl::Result::eOk ||
             sl_get_feature_function(sl::kFeatureReflex, "slReflexSetOptions", reinterpret_cast<void*&>(reflex_set_options)) != sl::Result::eOk ||
             sl_get_feature_function(sl::kFeatureReflex, "slReflexSleep", reinterpret_cast<void*&>(reflex_sleep)) != sl::Result::eOk ||
             sl_get_feature_function(sl::kFeaturePCL, "slPCLSetMarker", reinterpret_cast<void*&>(pcl_set_marker)) != sl::Result::eOk)
         {
            return Fail("couldn't get the DLSS-G, Reflex or PCL functions");
         }
         sl::ReflexOptions reflex = {};
         reflex.mode = sl::ReflexMode::eLowLatency;
         reflex_set_options(reflex);
         ready = true;
         return true;
      }

      template <typename Function>
      static Function* Import(const char* name)
      {
         return reinterpret_cast<Function*>(GetProcAddress(interposer, name));
      }

      static sl::float4x4 ToSl(const float* matrix)
      {
         sl::float4x4 result;
         for (uint32_t i = 0; i < 4; i++)
         {
            result[i] = sl::float4(matrix[i * 4], matrix[i * 4 + 1], matrix[i * 4 + 2], matrix[i * 4 + 3]);
         }
         return result;
      }

      bool Fail(const char* reason, int result = 0)
      {
         Log("FAIL DLSS frame generation: %s (result %d)\n", reason, result);
         Destroy();
         return false;
      }

      static void LogCallback(sl::LogType type, const char* message)
      {
         if (type != sl::LogType::eInfo)
         {
            Log("Streamline: %s\n", message);
         }
      }

      CreateDesc create_desc;
      // Process-wide, see "Shutdown()"
      static inline HMODULE interposer = nullptr;
      static inline PFun_slInit* sl_init = nullptr;
      static inline PFun_slShutdown* sl_shutdown = nullptr;
      static inline PFun_slIsFeatureSupported* sl_is_feature_supported = nullptr;
      static inline PFun_slSetD3DDevice* sl_set_d3d_device = nullptr;
      static inline PFun_slUpgradeInterface* sl_upgrade_interface = nullptr;
      static inline PFun_slGetFeatureFunction* sl_get_feature_function = nullptr;
      static inline PFun_slGetNewFrameToken* sl_get_new_frame_token = nullptr;
      static inline PFun_slSetConstants* sl_set_constants = nullptr;
      static inline PFun_slSetTagForFrame* sl_set_tag_for_frame = nullptr;
      static inline PFun_slDLSSGSetOptions* dlssg_set_options = nullptr;
      static inline PFun_slDLSSGGetState* dlssg_get_state = nullptr;
      static inline PFun_slReflexSetOptions* reflex_set_options = nullptr;
      static inline PFun_slReflexSleep* reflex_sleep = nullptr;
      static inline PFun_slPCLSetMarker* pcl_set_marker = nullptr;
      Microsoft::WRL::ComPtr<ID3D12Device> device;
      Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
      Microsoft::WRL::ComPtr<IDXGISwapChain4> swapchain;
      Microsoft::WRL::ComPtr<ID3D12Resource> buffers[buffer_count];
      const sl::ViewportHandle viewport = sl::ViewportHandle(0);
      sl::DLSSGOptions options = {};
      static inline bool initialized = false; // slInit is done
      static inline bool ready = false;       // and the device, support and functions
      bool tearing = false;
      bool was_generating = false;
      bool has_previous_matrices = false;
      bool was_in_front = true;
      uint32_t present_id = 0;
      sl::DLSSGStatus last_status = sl::DLSSGStatus::eOk;
      uint64_t presents = 0, presented_frames = 0; // Of the session: the presents, and the frames DLSS-G says it showed for them
      DirectX::XMFLOAT4X4 previous_view_matrix, previous_projection_matrix;
   };
} // namespace FrameGeneration
