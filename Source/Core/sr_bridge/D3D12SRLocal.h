#pragma once

// The in-process D3D12 super resolution of an x64 D3D11 mod built with "UseLumaD3D12SR": DLSS, FSR (FSR 4 / 3.1) and XeSS run on
// D3D12 in the game's own process ("D3D12SR.h") next to the D3D11 DLSS and FSR 3, and the user picks the API ("ApiSwitch"). Like the 32-bit games'
// bridge ("SRBridge.h") the D3D12 side works on NT-handle shared textures (zero copy for the game's own shared ones, else a copy per frame)
// between two shared fences, but it is a call on the game's thread: no helper process, pipe or protocol lines. The game's GPU waits for the
// output in the same frame.
#if ENABLE_D3D12_SR
#include "D3D12SR.h"
#include "SRBridgeD3D11.h"
#include "../utils/system.h"

#include <include/reshade.hpp>

#include <filesystem>
#include <format>

namespace SRBridge
{
   // The persisted "SuperResolutionAPI" pick, in this order
   enum class Api
   {
      D3D11,
      D3D12,
   };
   inline Api api_setting = Api::D3D11; // Cheaper: D3D12 costs ~0.3-0.5 ms a frame (two fence hops and copies between the APIs)

   struct LocalInstanceData : public SR::InstanceData, public Link
   {
      SR::Type type = SR::Type::None;
      LUID luid = {};
      std::unique_ptr<SR::D3D12::LocalHost> host; // Created at the first draw, and again after a restart
      bool settings_pending = false;              // Changed since the host started: applied before the next frame
      bool failed = false;

      ~LocalInstanceData()
      {
         Stop();
      }

      void Stop()
      {
         version_name = nullptr; // Points into the host
         host.reset();           // Waits for its GPU work: before the shared objects go
         Reset();
         settings_pending = false;
      }

      bool Fail(const std::string& reason)
      {
         Stop();
         failed = true;
         reshade::log::message(reshade::log::level::warning, ("[SR Bridge] " + reason).c_str());
         return false;
      }
   };

   // One per SR type, registered as that type's D3D12 implementation
   class LocalBridge : public SR::SuperResolutionImpl
   {
   public:
      explicit LocalBridge(SR::Type type) : type(type)
      {
      }

      virtual bool HasInit(const SR::InstanceData* data) const override
      {
         return data != nullptr;
      }
      virtual bool IsSupported(const SR::InstanceData* data) const override
      {
         return data && data->is_supported;
      }

      // No D3D12 objects yet (Core initializes every type at device creation): supported if the upscaler's dlls (and for DLSS, NVIDIA's GPU) are there
      virtual bool Init(SR::InstanceData*& data, ID3D11Device* device, IDXGIAdapter* adapter = nullptr) override
      {
         if (data)
         {
            Deinit(data);
         }
         auto* const custom_data = new LocalInstanceData();
         data = custom_data;
         custom_data->type = type;
         custom_data->min_resolution = 32;
         custom_data->requires_unordered_access_output_texture = false; // The upscaler writes its own copy
         DXGI_ADAPTER_DESC adapter_desc = {};
         if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&custom_data->device))) || !adapter || FAILED(adapter->GetDesc(&adapter_desc)))
         {
            return false;
         }
         custom_data->luid = adapter_desc.AdapterLuid;
         const std::filesystem::path directory = System::GetModulePath().parent_path();
         std::error_code error;
         const auto has = [&](const wchar_t* name)
         { return std::filesystem::exists(directory / name, error); };
         switch (type)
         {
         case SR::Type::DLSS:
            custom_data->is_supported = adapter_desc.VendorId == 0x10DE && has(L"nvngx_dlss.dll");
            break;
         case SR::Type::FSR:
            custom_data->is_supported = has(L"amd_fidelityfx_loader_dx12.dll") && has(L"amd_fidelityfx_upscaler_dx12.dll");
            break;
         case SR::Type::XeSS:
            custom_data->is_supported = has(L"libxess.dll");
            break;
         }
         return custom_data->is_supported;
      }

      virtual void Deinit(SR::InstanceData*& data, ID3D11Device* optional_device = nullptr) override
      {
         delete static_cast<LocalInstanceData*>(data);
         data = nullptr;
      }

      // Frees the D3D12 upscaler and the shared copies; the next draw starts them again
      virtual void ReleaseResources(SR::InstanceData* data) override
      {
         if (auto* const local = static_cast<LocalInstanceData*>(data))
         {
            local->Stop();
            // A new pick starts over after a failure (games stop drawing a failed upscaler until it's picked again)
            local->failed = false;
         }
      }

      // The host starts on the next draw with these, or a running one recreates the upscaler with them
      virtual bool UpdateSettings(SR::InstanceData* data, ID3D11DeviceContext* command_list, const SR::SettingsData& settings_data) override
      {
         auto* const local = static_cast<LocalInstanceData*>(data);
         if (!local || !local->is_supported || !command_list)
         {
            return false;
         }
         if (settings_data == local->settings_data)
         {
            return !local->failed;
         }
         local->settings_data = settings_data;
         local->settings_pending = local->host != nullptr;
         local->failed = false;
         return true;
      }

      // Until the upscaler is ready, the color is copied to the output (so the game doesn't treat it as a failure)
      virtual bool Draw(const SR::InstanceData* data, ID3D11DeviceContext* command_list, const DrawData& draw_data) override
      {
         auto& local = *const_cast<LocalInstanceData*>(static_cast<const LocalInstanceData*>(data));
         // Until picked again ("ReleaseResources") or new settings
         if (local.failed)
         {
            return false;
         }
         using namespace SRBridgeProtocol;
         ID3D11Resource* const resources[kCount] = {draw_data.source_color, draw_data.motion_vectors, draw_data.depth_buffer, draw_data.bias_mask, draw_data.transparency_alpha,
            draw_data.exposure, draw_data.output_color};
         if (local.host)
         {
            if (const int changed = local.FindNewInput(resources); changed >= 0)
            {
               // Recreated: the upscaler starts again on them
               reshade::log::message(reshade::log::level::info, std::format("[SR Bridge] restarting {}: a new {} texture", SR::GetTypeName(type), Link::names[changed]).c_str());
               local.Stop();
            }
         }
         if (!local.host)
         {
            HANDLE handles[kCount + 2] = {}; // The textures', then the fences'
            std::string error = local.Open(command_list, resources, handles);
            if (error.empty())
            {
               uint64_t texture_handles[kCount] = {};
               for (int i = 0; i < kCount; i++)
               {
                  texture_handles[i] = uint64_t(uintptr_t(handles[i]));
               }
               const uint64_t fence_handles[2] = {uint64_t(uintptr_t(handles[kCount])), uint64_t(uintptr_t(handles[kCount + 1]))};
               local.host = SR::D3D12::CreateLocalHost(type, local.luid, local.settings_data, texture_handles, fence_handles, &error);
               if (local.host)
               {
                  local.version_name = local.host->GetVersionName();
                  reshade::log::message(reshade::log::level::info,
                     std::format("[SR Bridge] started {} in process ({}x{} -> {}x{})", SR::GetTypeName(type), local.settings_data.render_width, local.settings_data.render_height,
                        local.settings_data.output_width, local.settings_data.output_height)
                        .c_str());
               }
            }
            // The D3D12 objects hold their own references to what the handles name
            for (HANDLE handle : handles)
            {
               if (handle)
               {
                  CloseHandle(handle);
               }
            }
            if (!local.host)
            {
               return local.Fail(error);
            }
         }

         if (local.frame == 0)
         {
            if (!local.IsReadySignaled())
            {
               // The D3D12 queue hasn't signaled it yet: the color as it is
               local.CopyColorToOutput(command_list, draw_data);
               return true;
            }
            local.frame = kReady;
            reshade::log::message(reshade::log::level::info, std::format("[SR Bridge] running {}", SR::GetTypeName(type)).c_str());
         }
         if (local.settings_pending)
         {
            if (!local.host->UpdateSettings(local.settings_data))
            {
               return local.Fail("the upscaler couldn't be recreated for the new settings");
            }
            local.version_name = local.host->GetVersionName();
            local.settings_pending = false;
         }

         const UINT render_width = (draw_data.render_width ? draw_data.render_width : local.settings_data.render_width);
         const UINT render_height = (draw_data.render_height ? draw_data.render_height : local.settings_data.render_height);
         const uint64_t frame = local.Submit(command_list, resources, render_width, render_height);
         const bool drawn = local.host->RunFrame({
            .n = frame,
            .jitter_x = draw_data.jitter_x,
            .jitter_y = draw_data.jitter_y,
            .reset = draw_data.reset ? 1 : 0,
            .render_width = render_width,
            .render_height = render_height,
            .pre_exposure = draw_data.pre_exposure,
            .sharpness = draw_data.user_sharpness,
            .near_plane = draw_data.near_plane,
            .far_plane = draw_data.far_plane,
            .vert_fov = draw_data.vert_fov,
         });
         SR_BRIDGE_PROFILE(command_list, 3);
         if (!drawn)
         {
            return local.Fail("the upscaler failed to draw");
         }
         local.WaitOutput(command_list, draw_data.output_color, frame);
         return true;
      }

      // Once the upscaler is created and signaled
      virtual bool IsReady(const SR::InstanceData* data) const override
      {
         return data && static_cast<const LocalInstanceData*>(data)->frame != 0;
      }

      // The running upscaler's own value, else the same formulas on the settings now (also while new settings wait for the next draw)
      virtual int GetJitterPhases(const SR::InstanceData* data) const override
      {
         const auto* const local = static_cast<const LocalInstanceData*>(data);
         if (local->host && !local->settings_pending)
         {
            return local->host->GetJitterPhases();
         }
         const SR::SettingsData& settings = data->settings_data;
         return type == SR::Type::DLSS ? SR::GetDlssJitterPhases(settings.render_height, settings.output_height) : SR::GetFsrJitterPhases(settings.render_width, settings.output_width);
      }

      virtual float GetMipLODBias(const SR::InstanceData* data) const override
      {
         const auto* const local = static_cast<const LocalInstanceData*>(data);
         if (local->host && !local->settings_pending)
         {
            return local->host->GetMipLODBias();
         }
         const SR::SettingsData& settings = data->settings_data;
         return type == SR::Type::XeSS ? SR::GetXeSSMipLODBias(settings.render_height, settings.output_height) : SR::GetMipLODBias(settings.render_height, settings.output_height);
      }

   private:
      SR::Type type;
   };

   // The instance of an "ApiSwitch": the data of each API's implementation (nullptr where there is none), and the one in use. The fields Core reads
   // are copies of the active one's.
   struct ApiInstanceData : public SR::InstanceData
   {
      SR::InstanceData* inner[2] = {}; // By "Api"
      Api active = Api::D3D12;
   };

   // Registered as an SR type's implementation: forwards to its D3D11 implementation (DLSS, FSR 3; none for XeSS) or to the in-process D3D12 one
   // ("LocalBridge"), whichever "api_setting" picks. Switching frees the old one's resources
   // ("ReleaseResources()") and starts the new one with the settings Core gave last, at the next "UpdateSettings()" or "Draw()".
   class ApiSwitch : public SR::SuperResolutionImpl
   {
   public:
      ApiSwitch(std::unique_ptr<SR::SuperResolutionImpl> d3d11, std::unique_ptr<SR::SuperResolutionImpl> d3d12) : impls{std::move(d3d11), std::move(d3d12)}
      {
      }

      virtual bool HasInit(const SR::InstanceData* data) const override
      {
         return data != nullptr;
      }
      virtual bool IsSupported(const SR::InstanceData* data) const override
      {
         return data && data->is_supported;
      }

      virtual bool Init(SR::InstanceData*& data, ID3D11Device* device, IDXGIAdapter* adapter = nullptr) override
      {
         if (data)
         {
            Deinit(data, device);
         }
         auto* const custom_data = new ApiInstanceData();
         data = custom_data;
         for (const Api api : {Api::D3D11, Api::D3D12})
         {
            if (impls[int(api)])
            {
               impls[int(api)]->Init(custom_data->inner[int(api)], device, adapter);
            }
         }
         if (SR::InstanceData*& d3d11 = custom_data->inner[int(Api::D3D11)]; d3d11 && !d3d11->is_supported)
         {
            impls[int(Api::D3D11)]->Deinit(d3d11, device);
         }
         custom_data->is_supported = IsInnerSupported(*custom_data, Api::D3D11) || IsInnerSupported(*custom_data, Api::D3D12);
         custom_data->active = GetWantedApi(*custom_data);
         Mirror(*custom_data);
         return custom_data->is_supported;
      }

      virtual void Deinit(SR::InstanceData*& data, ID3D11Device* optional_device = nullptr) override
      {
         auto* const custom_data = static_cast<ApiInstanceData*>(data);
         if (custom_data)
         {
            for (const Api api : {Api::D3D11, Api::D3D12})
            {
               if (custom_data->inner[int(api)])
               {
                  impls[int(api)]->Deinit(custom_data->inner[int(api)], optional_device);
               }
            }
         }
         delete custom_data;
         data = nullptr;
      }

      virtual void ReleaseResources(SR::InstanceData* data) override
      {
         auto& custom_data = *static_cast<ApiInstanceData*>(data);
         for (const Api api : {Api::D3D11, Api::D3D12})
         {
            if (custom_data.inner[int(api)])
            {
               impls[int(api)]->ReleaseResources(custom_data.inner[int(api)]);
            }
         }
      }

      virtual void ReleaseRetired(SR::InstanceData* data) override
      {
         auto& custom_data = *static_cast<ApiInstanceData*>(data);
         for (const Api api : {Api::D3D11, Api::D3D12})
         {
            if (custom_data.inner[int(api)])
            {
               impls[int(api)]->ReleaseRetired(custom_data.inner[int(api)]);
            }
         }
      }

      virtual bool UpdateSettings(SR::InstanceData* data, ID3D11DeviceContext* command_list, const SR::SettingsData& settings_data) override
      {
         auto& custom_data = *static_cast<ApiInstanceData*>(data);
         Switch(custom_data, command_list);
         const bool updated = impls[int(custom_data.active)]->UpdateSettings(custom_data.inner[int(custom_data.active)], command_list, settings_data);
         if (updated)
         {
            custom_data.settings_data = settings_data;
         }
         Mirror(custom_data);
         return updated;
      }

      virtual bool Draw(const SR::InstanceData* data, ID3D11DeviceContext* command_list, const DrawData& draw_data) override
      {
         auto& custom_data = *const_cast<ApiInstanceData*>(static_cast<const ApiInstanceData*>(data));
         Switch(custom_data, command_list);
         const bool drawn = impls[int(custom_data.active)]->Draw(custom_data.inner[int(custom_data.active)], command_list, draw_data);
         Mirror(custom_data); // The version name once the upscaler started
         return drawn;
      }

      virtual int GetJitterPhases(const SR::InstanceData* data) const override
      {
         const auto& custom_data = *static_cast<const ApiInstanceData*>(data);
         return impls[int(custom_data.active)]->GetJitterPhases(custom_data.inner[int(custom_data.active)]);
      }

      virtual bool IsReady(const SR::InstanceData* data) const override
      {
         const auto& custom_data = *static_cast<const ApiInstanceData*>(data);
         return impls[int(custom_data.active)]->IsReady(custom_data.inner[int(custom_data.active)]);
      }

      virtual float GetMipLODBias(const SR::InstanceData* data) const override
      {
         const auto& custom_data = *static_cast<const ApiInstanceData*>(data);
         return impls[int(custom_data.active)]->GetMipLODBias(custom_data.inner[int(custom_data.active)]);
      }

      virtual bool NeedsStateRestoration() const override
      {
         return impls[int(Api::D3D11)] && impls[int(Api::D3D11)]->NeedsStateRestoration();
      }

      // Whether both APIs are there for the user to pick between (XeSS has only D3D12)
      static bool CanPick(const SR::InstanceData* data)
      {
         const auto* const custom_data = static_cast<const ApiInstanceData*>(data);
         return custom_data && IsInnerSupported(*custom_data, Api::D3D11) && IsInnerSupported(*custom_data, Api::D3D12);
      }

   private:
      static bool IsInnerSupported(const ApiInstanceData& data, Api api)
      {
         return data.inner[int(api)] && data.inner[int(api)]->is_supported;
      }

      static Api GetWantedApi(const ApiInstanceData& data)
      {
         // The one that exists, else the pick
         if (!IsInnerSupported(data, Api::D3D11))
         {
            return Api::D3D12;
         }
         if (!IsInnerSupported(data, Api::D3D12))
         {
            return Api::D3D11;
         }
         return api_setting;
      }

      // The fields Core reads from the active implementation's instance
      static void Mirror(ApiInstanceData& data)
      {
         const SR::InstanceData* const active = data.inner[int(data.active)];
         if (!active)
         {
            return;
         }
         data.supports_dynamic_resolution = active->supports_dynamic_resolution;
         data.supports_upscaling = active->supports_upscaling;
         data.supports_non_uniform_aspect_ratio_upscaling = active->supports_non_uniform_aspect_ratio_upscaling;
         data.supports_arbitrary_resolutions = active->supports_arbitrary_resolutions;
         data.supports_arbitrary_jitter_phases = active->supports_arbitrary_jitter_phases;
         data.supports_sdr = active->supports_sdr;
         data.supports_scrgb_hdr = active->supports_scrgb_hdr;
         data.requires_unordered_access_output_texture = active->requires_unordered_access_output_texture;
         data.automatically_restores_pipeline_state = active->automatically_restores_pipeline_state;
         data.min_resolution = active->min_resolution;
         data.version_name = active->version_name.load();
      }

      // Frees the API that was in use and starts the new one with the settings Core gave last (if any)
      void Switch(ApiInstanceData& data, ID3D11DeviceContext* command_list)
      {
         const Api wanted = GetWantedApi(data);
         if (wanted == data.active)
         {
            return;
         }
         impls[int(data.active)]->ReleaseResources(data.inner[int(data.active)]);
         data.active = wanted;
         reshade::log::message(reshade::log::level::info, std::format("[SR Bridge] super resolution API: {}", wanted == Api::D3D12 ? "D3D12" : "D3D11").c_str());
         if (command_list)
         {
            impls[int(wanted)]->UpdateSettings(data.inner[int(wanted)], command_list, data.settings_data);
         }
         Mirror(data);
      }

      std::unique_ptr<SR::SuperResolutionImpl> impls[2]; // By "Api"
   };
} // namespace SRBridge
#endif
