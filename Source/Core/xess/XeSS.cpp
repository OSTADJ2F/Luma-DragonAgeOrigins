#include "XeSS.h"

#if ENABLE_XESS

#include "xess/xess_d3d12.h"

#include "../includes/debug.h"

#include <include/reshade.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>
#include <wrl/client.h>
#include <d3d12.h>

#if DX12
namespace Intel::inline D3D12
#else
namespace Intel
#endif
{
   struct XeSSInstanceData : public SR::InstanceData
   {
      HMODULE module = nullptr;
      decltype(&xessD3D12CreateContext) create_context = nullptr;
      decltype(&xessD3D12Init) init = nullptr;
      decltype(&xessD3D12Execute) execute = nullptr;
      decltype(&xessDestroyContext) destroy_context = nullptr;
      decltype(&xessGetOptimalInputResolution) get_optimal_input_resolution = nullptr;
      decltype(&xessSetVelocityScale) set_velocity_scale = nullptr;
      decltype(&xessSetLoggingCallback) set_logging_callback = nullptr;
      Microsoft::WRL::ComPtr<ID3D12Device> device;
      std::string version; // "version_name"

      xess_context_handle_t context = nullptr;
      // Reinitializing a context needs its command lists done, so a settings change creates a new one instead
      SR::DeferredReleases<xess_context_handle_t> retired_contexts;
   };

   bool Intel::XeSS::Init(SR::InstanceData*& data, SR::Device* device, IDXGIAdapter* adapter)
   {
      if (data)
      {
         Deinit(data); // This will also null the pointer
      }

      // From the game's folder, where the package puts it (not from the current directory)
      HMODULE module = LoadLibraryExW(L"libxess.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!module)
      {
         reshade::log::message(reshade::log::level::warning, "XeSS: libxess.dll not found next to the game's executable");
         return false;
      }

      auto& custom_data = reinterpret_cast<XeSSInstanceData*&>(data);
      custom_data = new XeSSInstanceData();
      custom_data->module = module;
      custom_data->device = device;
      custom_data->min_resolution = 32;
      custom_data->create_context = reinterpret_cast<decltype(&xessD3D12CreateContext)>(GetProcAddress(module, "xessD3D12CreateContext"));
      custom_data->init = reinterpret_cast<decltype(&xessD3D12Init)>(GetProcAddress(module, "xessD3D12Init"));
      custom_data->execute = reinterpret_cast<decltype(&xessD3D12Execute)>(GetProcAddress(module, "xessD3D12Execute"));
      custom_data->destroy_context = reinterpret_cast<decltype(&xessDestroyContext)>(GetProcAddress(module, "xessDestroyContext"));
      custom_data->get_optimal_input_resolution = reinterpret_cast<decltype(&xessGetOptimalInputResolution)>(GetProcAddress(module, "xessGetOptimalInputResolution"));
      custom_data->set_velocity_scale = reinterpret_cast<decltype(&xessSetVelocityScale)>(GetProcAddress(module, "xessSetVelocityScale"));
      custom_data->set_logging_callback = reinterpret_cast<decltype(&xessSetLoggingCallback)>(GetProcAddress(module, "xessSetLoggingCallback"));
      const auto get_version = reinterpret_cast<decltype(&xessGetVersion)>(GetProcAddress(module, "xessGetVersion"));
      const auto is_optimal_driver = reinterpret_cast<decltype(&xessIsOptimalDriver)>(GetProcAddress(module, "xessIsOptimalDriver"));
      if (!custom_data->create_context || !custom_data->init || !custom_data->execute || !custom_data->destroy_context || !custom_data->get_optimal_input_resolution || !custom_data->set_velocity_scale || !custom_data->set_logging_callback || !get_version || !is_optimal_driver)
      {
         return false;
      }

      xess_version_t version = {};
      get_version(&version);
      custom_data->version = std::format("{}.{}.{}", version.major, version.minor, version.patch);
      custom_data->version_name = custom_data->version.c_str();

      // A context is the only support check (it fails without SM 6.4). It's destroyed right away: the ones that run are created by "UpdateSettings()"
      xess_context_handle_t probe_context = nullptr;
      const xess_result_t result = custom_data->create_context(device, &probe_context);
      if (result != XESS_RESULT_SUCCESS)
      {
         reshade::log::message(reshade::log::level::warning, std::format("XeSS {}: not supported on this device ({})", custom_data->version, int(result)).c_str());
         return false;
      }
      if (is_optimal_driver(probe_context) == XESS_RESULT_WARNING_OLD_DRIVER)
      {
         reshade::log::message(reshade::log::level::warning, std::format("XeSS {}: the GPU driver is older than XeSS recommends", custom_data->version).c_str());
      }
      custom_data->destroy_context(probe_context);
      reshade::log::message(reshade::log::level::info, std::format("XeSS {}: supported", custom_data->version).c_str());

      custom_data->is_supported = true;
      return true;
   }

   void Intel::XeSS::Deinit(SR::InstanceData*& data, SR::Device* optional_device)
   {
      auto& custom_data = reinterpret_cast<XeSSInstanceData*&>(data);
      if (!custom_data)
      {
         return;
      }

      // The device is going away, nothing of it runs anymore
      custom_data->retired_contexts.Retire(&custom_data->context);
      custom_data->retired_contexts.ReleaseAll([&](xess_context_handle_t* retired)
         { custom_data->destroy_context(*retired); });
      HMODULE module = custom_data->module;
      delete custom_data;
      custom_data = nullptr;
      FreeLibrary(module);
   }

   // The context (XeSS's own textures). The next "UpdateSettings()" creates it again.
   void Intel::XeSS::ReleaseResources(SR::InstanceData* data)
   {
      if (data)
      {
         auto* const custom_data = static_cast<XeSSInstanceData*>(data);
         custom_data->retired_contexts.Retire(&custom_data->context);
      }
   }

   void Intel::XeSS::ReleaseRetired(SR::InstanceData* data)
   {
      auto* const custom_data = static_cast<XeSSInstanceData*>(data);
      custom_data->retired_contexts.Release([&](xess_context_handle_t* retired)
         { custom_data->destroy_context(*retired); });
   }

   bool Intel::XeSS::UpdateSettings(SR::InstanceData* data, SR::CommandList* command_list, const SR::SettingsData& settings_data)
   {
      auto* custom_data = static_cast<XeSSInstanceData*>(data);
      if (!command_list || !custom_data || !custom_data->is_supported)
         return false;

      if (settings_data == custom_data->settings_data && custom_data->context)
      {
         return true;
      }

      custom_data->retired_contexts.Retire(&custom_data->context);

      if (custom_data->create_context(custom_data->device.Get(), &custom_data->context) != XESS_RESULT_SUCCESS)
      {
         // No retry policy here: without a context "Draw()" fails, and the games then suppress SR until an upscaler is picked again
         custom_data->context = nullptr;
         ASSERT_ONCE_MSG(false, "Couldn't create the XeSS context");
         return false;
      }
#if DEVELOPMENT
      custom_data->set_logging_callback(custom_data->context, XESS_LOGGING_LEVEL_WARNING, [](const char* message, xess_logging_level_t level)
         { reshade::log::message(level == XESS_LOGGING_LEVEL_ERROR ? reshade::log::level::error : reshade::log::level::warning, (std::string("XeSS: ") + message).c_str()); });
#endif

      const xess_2d_t output_resolution = {.x = settings_data.output_width, .y = settings_data.output_height};
      // The preset sets the model's upscaling range: native AA when nothing is upscaled, the widest range (1x-3x) for dynamic resolution, otherwise the preset
      // whose optimal render size is the closest to ours
      xess_quality_settings_t quality = XESS_QUALITY_SETTING_AA;
      if (settings_data.dynamic_resolution)
      {
         quality = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
      }
      else if (settings_data.render_width < settings_data.output_width || settings_data.render_height < settings_data.output_height)
      {
         unsigned int best_delta = UINT_MAX;
         for (int i = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE; i < XESS_QUALITY_SETTING_AA; i++)
         {
            xess_2d_t optimal = {}, min = {}, max = {};
            if (custom_data->get_optimal_input_resolution(custom_data->context, &output_resolution, xess_quality_settings_t(i), &optimal, &min, &max) != XESS_RESULT_SUCCESS)
               continue;
            const unsigned int delta = unsigned(std::abs(int(optimal.x) - int(settings_data.render_width)) + std::abs(int(optimal.y) - int(settings_data.render_height)));
            if (delta < best_delta)
            {
               best_delta = delta;
               quality = xess_quality_settings_t(i);
            }
         }
      }

      const xess_d3d12_init_params_t init_params = {
         .outputResolution = output_resolution,
         .qualitySetting = quality,
         .initFlags = uint32_t((settings_data.inverted_depth ? XESS_INIT_FLAG_INVERTED_DEPTH : 0) | (settings_data.mvs_jittered ? XESS_INIT_FLAG_JITTERED_MV : 0) | (settings_data.auto_exposure ? XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE : 0) | (settings_data.hdr ? 0 : XESS_INIT_FLAG_LDR_INPUT_COLOR)),
      };
      // Compiles its pipelines synchronously (a few hundred ms, once per context)
      if (custom_data->init(custom_data->context, &init_params) != XESS_RESULT_SUCCESS)
      {
         custom_data->retired_contexts.Retire(&custom_data->context);
         ASSERT_ONCE_MSG(false, "Couldn't initialize the XeSS context");
         return false;
      }
      // Both in pixels from the current position to the previous one, as Luma's convention
      custom_data->set_velocity_scale(custom_data->context, settings_data.mvs_x_scale, settings_data.mvs_y_scale);

      custom_data->settings_data = settings_data;
      return true;
   }

   // The minimum the XeSS guide asks for (the same formula as FSR's)
   int Intel::XeSS::GetJitterPhases(const SR::InstanceData* data) const
   {
      return SR::GetFsrJitterPhases(data->settings_data.render_width, data->settings_data.output_width);
   }

   float Intel::XeSS::GetMipLODBias(const SR::InstanceData* data) const
   {
      return SR::GetXeSSMipLODBias(data->settings_data.render_height, data->settings_data.output_height);
   }

   bool Intel::XeSS::Draw(const SR::InstanceData* data, SR::CommandList* command_list, const DrawData& draw_data)
   {
      const auto* custom_data = static_cast<const XeSSInstanceData*>(data);
      // No context: "UpdateSettings()" failed or wasn't called since "ReleaseResources()"
      if (!custom_data->context)
      {
         return false;
      }

      // XeSS reads the inputs in "D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE" and writes the output in "D3D12_RESOURCE_STATE_UNORDERED_ACCESS",
      // without barriers of its own (see "DrawData")
      // ponytail: no exposure texture or responsive mask (XeSS needs their flags at the context creation), add them with a D3D12 consumer that has them.
      const xess_d3d12_execute_params_t execute_params = {
         .pColorTexture = draw_data.source_color,
         .pVelocityTexture = draw_data.motion_vectors,
         .pDepthTexture = draw_data.depth_buffer,
         .pOutputTexture = draw_data.output_color,
         .jitterOffsetX = draw_data.jitter_x,
         .jitterOffsetY = draw_data.jitter_y,
         // Applied to the input and undone on the output: a pre-exposed input gets its exposure back
         .exposureScale = (draw_data.pre_exposure == 0.f ? 1.f : (1.f / draw_data.pre_exposure)),
         .resetHistory = uint32_t(draw_data.reset ? 1 : 0),
         // 0: the settings' render size (like DLSS)
         .inputWidth = (draw_data.render_width ? draw_data.render_width : custom_data->settings_data.render_width),
         .inputHeight = (draw_data.render_height ? draw_data.render_height : custom_data->settings_data.render_height),
      };

      const xess_result_t result = custom_data->execute(custom_data->context, command_list, &execute_params);
      ASSERT_ONCE_MSG(result == XESS_RESULT_SUCCESS, "XeSS execution failed");
      return result == XESS_RESULT_SUCCESS;
   }
} // namespace Intel

#endif
