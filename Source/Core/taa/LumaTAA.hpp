#pragma once

// Luma TAA ("Shaders/Global/Luma_TAA_CS.hlsl", design and measurements in "docs/Luma-TAA.md"): temporal anti-aliasing, at native
// resolution or upscaling (TAAU, render smaller than output), as an SR implementation. It runs its compute passes on the game's own device and context, so it needs no SDK and
// works on any GPU, x86 included (no SR bridge). Core registers its shaders and attaches the "DeviceData" after "Init()".
#if ENABLE_LUMA_TAA
namespace LumaTAA
{
   // "TAA_QUALITY" of the resolve: 0 Low, 1 Medium (adds the shading change and a depth clip against the previous frame's depth),
   // 2 High (adds the 2x history), 3 Ultra (adds a depth clip against a previous depth reconstructed by the shading change pass). Core's UI sets it (saved as
   // "LumaTAAQuality"); "UpdateSettings()" applies it.
   inline std::atomic<int> quality = 2;
   constexpr const char* quality_names[] = {"Low", "Medium", "High", "Ultra"};
   constexpr uint32_t resolve_shader_hashes[] = {Math::CompileTimeStringHash("Luma TAA Low CS"), Math::CompileTimeStringHash("Luma TAA Medium CS"), Math::CompileTimeStringHash("Luma TAA High CS"), Math::CompileTimeStringHash("Luma TAA Ultra CS")};
   // "TAA_UPSCALE" (one thread per output pixel; the history and the per pixel states at the output resolution, so High equals Medium)
   constexpr uint32_t upscale_shader_hashes[] = {Math::CompileTimeStringHash("Luma TAA Low Upscale CS"), Math::CompileTimeStringHash("Luma TAA Medium Upscale CS"), Math::CompileTimeStringHash("Luma TAA High Upscale CS"), Math::CompileTimeStringHash("Luma TAA Ultra Upscale CS")};
   constexpr uint32_t shading_change_shader_hash = Math::CompileTimeStringHash("Luma TAA Shading Change CS");
   // Ultra's shading change also reconstructs the previous depth (one pass reads the depth and motion vectors once)
   constexpr uint32_t shading_change_ultra_shader_hash = Math::CompileTimeStringHash("Luma TAA Shading Change Ultra CS");
   // From Medium quality the resolve reads the shading change ("TAA_SHADING_CHANGE") that a pass at the render resolution writes
   // first. The flickering analysis ("TAA_FLICKER") is off at every level, so its state textures aren't created or bound.
   constexpr int medium_quality = 1;
   constexpr int ultra_quality = 3;
   // Halton(2, 3) phases, as DLAA and FSR at native resolution. The history window (~25 frames, ~6 samples per 2x history texel)
   // doesn't average a whole cycle of 16, which left static aliasing pulsing at the cycle's period: 8 flickered ~25% less in
   // ME3LE and the lab (16 had 15-30% lower static error). Upscaling takes more (FSR's 8 x scale^2), so every output pixel gets
   // a sample within a cycle
   constexpr int jitter_phases = 8;
   // From High quality the history is kept at 2x2 the render resolution ("TAA_HISTORY_2X"): repeated reprojection then blurs far
   // less, and each history texel accumulates the samples that land in its quarter pixel
   constexpr int history_2x_quality = 2;

   // Mirrors "LumaTAAData" in "Luma_TAA_CS.hlsl"
   struct alignas(16) CBData
   {
      float render_resolution[2];
      float inv_render_resolution[2];
      float motion_vector_scale[2];
      float depth_near_far[2];
      uint32_t flags;
      float jitter[2]; // Offset of this frame's sample from the pixel center, in pixels (the opposite of "DrawData" jitter)
      float padding;
      float output_resolution[2]; // "TAA_UPSCALE"
      float inv_output_resolution[2];
      float previous_jitter[2]; // The previous frame's "jitter"
      float padding2[2];
   };
   static_assert(sizeof(CBData) == 80);
   constexpr uint32_t flag_reset = 1u << 0;
   constexpr uint32_t flag_inverted_depth = 1u << 1;
   constexpr uint32_t flag_reactive_mask = 1u << 2;
   constexpr UINT reconstructed_depth_clear_value = 0x7F7FFFFF; // asuint(FLT_MAX): nothing reconstructed

   inline bool IsUpscaling(const SR::SettingsData& settings_data)
   {
      return settings_data.render_width != settings_data.output_width || settings_data.render_height != settings_data.output_height;
   }

   struct TAAInstanceData : SR::InstanceData
   {
      DeviceData* device_data = nullptr; // Its native shaders and linear sampler. Set by Core right after "Init()".

      // Created by "UpdateSettings()" at the output resolution. The history alternates between the two: rgb linear color, alpha the
      // accumulated weight.
      com_ptr<ID3D11Texture2D> history[2];
      com_ptr<ID3D11ShaderResourceView> history_srvs[2];
      com_ptr<ID3D11UnorderedAccessView> history_uavs[2];
      com_ptr<ID3D11Texture2D> reconstructed_depth; // Ultra only, R32_UINT
      com_ptr<ID3D11ShaderResourceView> reconstructed_depth_srv;
      com_ptr<ID3D11UnorderedAccessView> reconstructed_depth_uav;
      // Thin feature lock frames left, or a moving pixel's speed (R8_UINT; R32_UINT from Medium, whose high 16 bits hold the depth
      // clip's farthest depth), alternating with the history
      com_ptr<ID3D11Texture2D> locks[2];
      com_ptr<ID3D11ShaderResourceView> lock_srvs[2];
      com_ptr<ID3D11UnorderedAccessView> lock_uavs[2];
      // Shading change from Medium: the input luminance (R16_FLOAT at the render resolution, alternating with the history), and the
      // 2x2, 4x4 and 8x8 block means of the differences (R16G16_FLOAT, mips 0-2, a 4x4 texel mip 0 block per 8x8 dispatch group)
      com_ptr<ID3D11Texture2D> luminance[2];
      com_ptr<ID3D11ShaderResourceView> luminance_srvs[2];
      com_ptr<ID3D11UnorderedAccessView> luminance_uavs[2];
      com_ptr<ID3D11Texture2D> shading_change;
      com_ptr<ID3D11ShaderResourceView> shading_change_srv;
      com_ptr<ID3D11UnorderedAccessView> shading_change_uavs[3];
      com_ptr<ID3D11Buffer> cbuffer;
      // The quality level the resources were created for, latched by "UpdateSettings()" so "Draw()" never runs a resolve on resources
      // of another level
      int level = 0;

      // Per frame state ("Draw()" takes const data). The input views are kept while the same resources come back.
      mutable uint32_t history_index = 0;
      mutable bool history_valid = false;
      mutable float previous_jitter[2] = {};
      mutable com_ptr<ID3D11ShaderResourceView> source_color_srv;
      mutable com_ptr<ID3D11ShaderResourceView> depth_srv;
      mutable com_ptr<ID3D11ShaderResourceView> motion_vectors_srv;
      mutable com_ptr<ID3D11ShaderResourceView> reactive_mask_srv; // "DrawData::bias_mask", optional
      mutable com_ptr<ID3D11UnorderedAccessView> output_uav;
   };

   class TAA final : public SR::SuperResolutionImpl
   {
   public:
      bool HasInit(const SR::InstanceData* data) const override
      {
         return data != nullptr;
      }

      bool IsSupported(const SR::InstanceData* data) const override
      {
         return data != nullptr && data->is_supported;
      }

      bool Init(SR::InstanceData*& data, ID3D11Device* device, IDXGIAdapter* adapter = nullptr) override
      {
         Deinit(data, device);
         auto* const taa_data = new TAAInstanceData();
         // cs_5_0 with typed UAV stores, a typed R32_UINT UAV atomic and groupshared memory
         taa_data->is_supported = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0;
         taa_data->supports_dynamic_resolution = false;
         taa_data->supports_upscaling = true;
         taa_data->supports_arbitrary_jitter_phases = false;
         taa_data->supports_scrgb_hdr = true; // The per channel clip and the linear blend take negative (scRGB) channels as they are
         taa_data->automatically_restores_pipeline_state = true;
         data = taa_data;
         return taa_data->is_supported;
      }

      void Deinit(SR::InstanceData*& data, ID3D11Device* optional_device = nullptr) override
      {
         delete static_cast<TAAInstanceData*>(data);
         data = nullptr;
      }

      void ReleaseResources(SR::InstanceData* data) override
      {
         auto* const taa_data = static_cast<TAAInstanceData*>(data);
         if (!taa_data)
            return;
         for (int i = 0; i < 2; i++)
         {
            taa_data->history[i].reset();
            taa_data->history_srvs[i].reset();
            taa_data->history_uavs[i].reset();
            taa_data->locks[i].reset();
            taa_data->lock_srvs[i].reset();
            taa_data->lock_uavs[i].reset();
            taa_data->luminance[i].reset();
            taa_data->luminance_srvs[i].reset();
            taa_data->luminance_uavs[i].reset();
         }
         taa_data->shading_change.reset();
         taa_data->shading_change_srv.reset();
         for (auto& uav : taa_data->shading_change_uavs)
         {
            uav.reset();
         }
         taa_data->reconstructed_depth.reset();
         taa_data->reconstructed_depth_srv.reset();
         taa_data->reconstructed_depth_uav.reset();
         taa_data->cbuffer.reset();
         taa_data->source_color_srv.reset();
         taa_data->depth_srv.reset();
         taa_data->motion_vectors_srv.reset();
         taa_data->reactive_mask_srv.reset();
         taa_data->output_uav.reset();
         taa_data->history_valid = false;
         taa_data->settings_data = {};
      }

      bool UpdateSettings(SR::InstanceData* data, ID3D11DeviceContext* command_list, const SR::SettingsData& settings_data) override
      {
         auto* const taa_data = static_cast<TAAInstanceData*>(data);
         if (!taa_data || !taa_data->is_supported)
            return false;
         if (settings_data.render_width > settings_data.output_width || settings_data.render_height > settings_data.output_height)
            return false;
         const int level = quality.load();
         if (settings_data == taa_data->settings_data && level == taa_data->level && taa_data->history[0] && taa_data->cbuffer)
            return true;

         // Each level has its own set of resources, and the lock state of another level isn't valid here: start over
         ReleaseResources(taa_data);
         com_ptr<ID3D11Device> device;
         command_list->GetDevice(&device);
         // The history and the per pixel states are at the output resolution (equal to the render one without upscaling), the native
         // history at 2x2 from High; the reconstructed depth is at the render resolution
         const UINT history_scale = (level >= history_2x_quality && !IsUpscaling(settings_data) ? 2 : 1);
         const CD3D11_TEXTURE2D_DESC history_desc(DXGI_FORMAT_R16G16B16A16_FLOAT, settings_data.output_width * history_scale, settings_data.output_height * history_scale, 1, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
         const CD3D11_TEXTURE2D_DESC reconstructed_depth_desc(DXGI_FORMAT_R32_UINT, settings_data.render_width, settings_data.render_height, 1, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
         const CD3D11_TEXTURE2D_DESC lock_desc((level >= medium_quality ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R8_UINT), settings_data.output_width, settings_data.output_height, 1, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
         const CD3D11_BUFFER_DESC cbuffer_desc(sizeof(CBData), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
         bool created = SUCCEEDED(device->CreateBuffer(&cbuffer_desc, nullptr, &taa_data->cbuffer));
         for (int i = 0; i < 2 && created; i++)
         {
            created = SUCCEEDED(device->CreateTexture2D(&history_desc, nullptr, &taa_data->history[i])) && SUCCEEDED(device->CreateShaderResourceView(taa_data->history[i].get(), nullptr, &taa_data->history_srvs[i])) && SUCCEEDED(device->CreateUnorderedAccessView(taa_data->history[i].get(), nullptr, &taa_data->history_uavs[i]));
            created = created && SUCCEEDED(device->CreateTexture2D(&lock_desc, nullptr, &taa_data->locks[i])) && SUCCEEDED(device->CreateShaderResourceView(taa_data->locks[i].get(), nullptr, &taa_data->lock_srvs[i])) && SUCCEEDED(device->CreateUnorderedAccessView(taa_data->locks[i].get(), nullptr, &taa_data->lock_uavs[i]));
            if (created)
            {
               const UINT no_lock[4] = {};
               command_list->ClearUnorderedAccessViewUint(taa_data->lock_uavs[i].get(), no_lock);
               const FLOAT no_history[4] = {};
               command_list->ClearUnorderedAccessViewFloat(taa_data->history_uavs[i].get(), no_history);
            }
         }
         if (created && level >= medium_quality)
         {
            const CD3D11_TEXTURE2D_DESC luminance_desc(DXGI_FORMAT_R16_FLOAT, settings_data.render_width, settings_data.render_height, 1, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
            for (int i = 0; i < 2 && created; i++)
            {
               created = SUCCEEDED(device->CreateTexture2D(&luminance_desc, nullptr, &taa_data->luminance[i])) && SUCCEEDED(device->CreateShaderResourceView(taa_data->luminance[i].get(), nullptr, &taa_data->luminance_srvs[i])) && SUCCEEDED(device->CreateUnorderedAccessView(taa_data->luminance[i].get(), nullptr, &taa_data->luminance_uavs[i]));
            }
            const CD3D11_TEXTURE2D_DESC shading_change_desc(DXGI_FORMAT_R16G16_FLOAT, (settings_data.render_width + 7) / 8 * 4, (settings_data.render_height + 7) / 8 * 4, 1, UINT(std::size(taa_data->shading_change_uavs)), D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
            created = created && SUCCEEDED(device->CreateTexture2D(&shading_change_desc, nullptr, &taa_data->shading_change)) && SUCCEEDED(device->CreateShaderResourceView(taa_data->shading_change.get(), nullptr, &taa_data->shading_change_srv));
            for (UINT mip = 0; mip < UINT(std::size(taa_data->shading_change_uavs)) && created; mip++)
            {
               const CD3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc(D3D11_UAV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R16G16_FLOAT, mip);
               created = SUCCEEDED(device->CreateUnorderedAccessView(taa_data->shading_change.get(), &uav_desc, &taa_data->shading_change_uavs[mip]));
            }
         }
         if (created && level >= ultra_quality)
         {
            created = SUCCEEDED(device->CreateTexture2D(&reconstructed_depth_desc, nullptr, &taa_data->reconstructed_depth)) && SUCCEEDED(device->CreateShaderResourceView(taa_data->reconstructed_depth.get(), nullptr, &taa_data->reconstructed_depth_srv)) && SUCCEEDED(device->CreateUnorderedAccessView(taa_data->reconstructed_depth.get(), nullptr, &taa_data->reconstructed_depth_uav));
         }
         if (!created)
         {
            ReleaseResources(taa_data);
            return false;
         }
         taa_data->settings_data = settings_data;
         taa_data->level = level;
         return true;
      }

      // Until Core has compiled its shaders (asynchronously, after boot) and "UpdateSettings()" created the history
      bool IsReady(const SR::InstanceData* data) const override
      {
         const auto* const taa_data = static_cast<const TAAInstanceData*>(data);
         if (!taa_data || !taa_data->device_data || !taa_data->history[0])
            return false;
         const int level = taa_data->level;
         const auto& shaders = taa_data->device_data->native_compute_shaders;
         const uint32_t* const shader_hashes = (IsUpscaling(taa_data->settings_data) ? upscale_shader_hashes : resolve_shader_hashes);
         return FindShader(shaders, shader_hashes[level]) && (level < medium_quality || FindShader(shaders, (level >= ultra_quality ? shading_change_ultra_shader_hash : shading_change_shader_hash)));
      }

      int GetJitterPhases(const SR::InstanceData* data) const override
      {
         const auto* const taa_data = static_cast<const TAAInstanceData*>(data);
         if (!taa_data || !IsUpscaling(taa_data->settings_data))
            return jitter_phases;
         return (std::max)(jitter_phases, SR::GetFsrJitterPhases(taa_data->settings_data.render_width, taa_data->settings_data.output_width));
      }

      bool Draw(const SR::InstanceData* data, ID3D11DeviceContext* command_list, const DrawData& draw_data) override
      {
         const auto* const taa_data = static_cast<const TAAInstanceData*>(data);
         if (!taa_data || !taa_data->device_data || !taa_data->history[0] || !draw_data.output_color || !draw_data.source_color || !draw_data.motion_vectors || !draw_data.depth_buffer)
            return false;
         if (!IsReady(taa_data))
         {
            // The shaders are still compiling (boot or a live reload): pass the color through, as the SR bridge does while its helper
            // starts, rather than failing (games then fall back to their own anti-aliasing until the user picks the upscaler again)
            com_ptr<ID3D11Texture2D> source, output;
            if (FAILED(draw_data.source_color->QueryInterface(&source)) || FAILED(draw_data.output_color->QueryInterface(&output)))
               return false;
            D3D11_TEXTURE2D_DESC source_desc, output_desc;
            source->GetDesc(&source_desc);
            output->GetDesc(&output_desc);
            if (source_desc.Width != output_desc.Width || source_desc.Height != output_desc.Height || !AreFormatsCopyCompatible(source_desc.Format, output_desc.Format))
               return false;
            command_list->CopyResource(output.get(), source.get());
            taa_data->history_valid = false;
            return true;
         }
         const int level = taa_data->level;
         const auto& shaders = taa_data->device_data->native_compute_shaders;
         const uint32_t* const shader_hashes = (IsUpscaling(taa_data->settings_data) ? upscale_shader_hashes : resolve_shader_hashes);
         ID3D11ComputeShader* const resolve_shader = FindShader(shaders, shader_hashes[level]);
         const bool reconstruct_depth = (level >= ultra_quality);
         ID3D11ComputeShader* const shading_change_shader = (level >= medium_quality ? FindShader(shaders, (reconstruct_depth ? shading_change_ultra_shader_hash : shading_change_shader_hash)) : nullptr);
         const SR::SettingsData& settings_data = taa_data->settings_data;

         com_ptr<ID3D11Device> device;
         command_list->GetDevice(&device);
         // Views are recreated only when the game hands in a different resource (a view keeps its resource alive, so a cached one can't
         // match a new resource at a recycled address). Typeless resources are viewed with their float/unorm format.
         const auto update_srv = [&](ID3D11Resource* resource, com_ptr<ID3D11ShaderResourceView>* srv)
         {
            if (*srv)
            {
               com_ptr<ID3D11Resource> viewed;
               (*srv)->GetResource(&viewed);
               if (viewed.get() == resource)
                  return true;
            }
            srv->reset();
            if (SUCCEEDED(device->CreateShaderResourceView(resource, nullptr, &*srv)))
               return true;
            com_ptr<ID3D11Texture2D> texture;
            if (FAILED(resource->QueryInterface(&texture)))
               return false;
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);
            // ReShade's format values are DXGI's; depth typeless formats map to their depth channel's SRV format (R24_UNORM_X8, R32_FLOAT_X8X24)
            const auto format = DXGI_FORMAT(reshade::api::format_to_default_typed(reshade::api::format(desc.Format)));
            if (format == desc.Format)
               return false;
            const CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(D3D11_SRV_DIMENSION_TEXTURE2D, format);
            return SUCCEEDED(device->CreateShaderResourceView(resource, &srv_desc, &*srv));
         };
         bool views = update_srv(draw_data.source_color, std::addressof(taa_data->source_color_srv)) && update_srv(draw_data.depth_buffer, std::addressof(taa_data->depth_srv)) && update_srv(draw_data.motion_vectors, std::addressof(taa_data->motion_vectors_srv));
         // The reactive mask (AMD FSR 2 semantics) is optional: "flag_reactive_mask" tells the shader one is bound
         if (!draw_data.bias_mask || !update_srv(draw_data.bias_mask, std::addressof(taa_data->reactive_mask_srv)))
         {
            taa_data->reactive_mask_srv.reset();
         }
         if (views)
         {
            com_ptr<ID3D11Resource> viewed;
            if (taa_data->output_uav)
            {
               taa_data->output_uav->GetResource(&viewed);
            }
            if (viewed.get() != draw_data.output_color)
            {
               taa_data->output_uav.reset();
               views = SUCCEEDED(device->CreateUnorderedAccessView(draw_data.output_color, nullptr, &taa_data->output_uav));
            }
         }
         if (!views)
            return false;

         const bool reset = draw_data.reset || !taa_data->history_valid;
         // "DrawData" jitter offsets the projection, so the pixel center samples the scene at minus that offset
         const float jitter[2] = {-draw_data.jitter_x, -draw_data.jitter_y};
         D3D11_MAPPED_SUBRESOURCE mapped;
         if (FAILED(command_list->Map(taa_data->cbuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
         const float render_width = float(settings_data.render_width), render_height = float(settings_data.render_height);
         const float output_width = float(settings_data.output_width), output_height = float(settings_data.output_height);
         const CBData cb_data = {
            .render_resolution = {render_width, render_height},
            .inv_render_resolution = {1.f / render_width, 1.f / render_height},
            .motion_vector_scale = {settings_data.mvs_x_scale, settings_data.mvs_y_scale},
            .depth_near_far = {draw_data.near_plane, draw_data.far_plane},
            .flags = (reset ? flag_reset : 0u) | (settings_data.inverted_depth ? flag_inverted_depth : 0u) |
                     (taa_data->reactive_mask_srv ? flag_reactive_mask : 0u),
            .jitter = {jitter[0], jitter[1]},
            .output_resolution = {output_width, output_height},
            .inv_output_resolution = {1.f / output_width, 1.f / output_height},
            .previous_jitter = {taa_data->previous_jitter[0], taa_data->previous_jitter[1]},
         };
         std::memcpy(mapped.pData, &cb_data, sizeof(cb_data));
         command_list->Unmap(taa_data->cbuffer.get(), 0);

         // The games call this from their own passes: leave their compute state as it was
         DrawStateStack<DrawStateStackType::Compute> compute_state;
         compute_state.Cache(command_list, taa_data->device_data->uav_max_count);

         // The resolve runs per output pixel when upscaling (per render pixel otherwise), the depth reconstruction per render pixel
         const UINT resolve_groups_x = (settings_data.output_width + 7) / 8, resolve_groups_y = (settings_data.output_height + 7) / 8;
         const UINT render_groups_x = (settings_data.render_width + 7) / 8, render_groups_y = (settings_data.render_height + 7) / 8;
         ID3D11Buffer* const cbuffer = taa_data->cbuffer.get();
         ID3D11SamplerState* const sampler = taa_data->device_data->sampler_state_linear.get();
         command_list->CSSetConstantBuffers(0, 1, &cbuffer);
         command_list->CSSetSamplers(0, 1, &sampler);
         ID3D11ShaderResourceView* const null_srvs[10] = {};
         ID3D11UnorderedAccessView* const null_uavs[5] = {};
         const uint32_t write_index = taa_data->history_index;
         if (shading_change_shader)
         {
            // At Ultra it also scatters the reconstructed previous depth (u4) with InterlockedMin, so that starts cleared
            if (reconstruct_depth)
            {
               const UINT clear_value[4] = {reconstructed_depth_clear_value, reconstructed_depth_clear_value, reconstructed_depth_clear_value, reconstructed_depth_clear_value};
               command_list->ClearUnorderedAccessViewUint(taa_data->reconstructed_depth_uav.get(), clear_value);
            }
            ID3D11ShaderResourceView* const srvs[9] = {taa_data->source_color_srv.get(), taa_data->depth_srv.get(), taa_data->motion_vectors_srv.get(), nullptr, nullptr, nullptr, nullptr, nullptr, taa_data->luminance_srvs[write_index ^ 1].get()};
            ID3D11UnorderedAccessView* const uavs[5] = {taa_data->luminance_uavs[write_index].get(), taa_data->shading_change_uavs[0].get(), taa_data->shading_change_uavs[1].get(), taa_data->shading_change_uavs[2].get(), (reconstruct_depth ? taa_data->reconstructed_depth_uav.get() : nullptr)};
            command_list->CSSetShaderResources(0, 9, srvs);
            command_list->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
            command_list->CSSetShader(shading_change_shader, nullptr, 0);
            command_list->Dispatch(render_groups_x, render_groups_y, 1);
            command_list->CSSetUnorderedAccessViews(0, 5, null_uavs, nullptr);
         }
         ID3D11ShaderResourceView* const srvs[10] = {taa_data->source_color_srv.get(), taa_data->depth_srv.get(), taa_data->motion_vectors_srv.get(), taa_data->history_srvs[write_index ^ 1].get(), taa_data->reconstructed_depth_srv.get(), taa_data->lock_srvs[write_index ^ 1].get(), nullptr, taa_data->reactive_mask_srv.get(), nullptr, taa_data->shading_change_srv.get()};
         ID3D11UnorderedAccessView* const uavs[4] = {taa_data->history_uavs[write_index].get(), taa_data->output_uav.get(), nullptr, taa_data->lock_uavs[write_index].get()};
         command_list->CSSetShaderResources(0, 10, srvs);
         command_list->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
         command_list->CSSetShader(resolve_shader, nullptr, 0);
         command_list->Dispatch(resolve_groups_x, resolve_groups_y, 1);
         command_list->CSSetShaderResources(0, 10, null_srvs);
         command_list->CSSetUnorderedAccessViews(0, 4, null_uavs, nullptr);

         compute_state.Restore(command_list);
         taa_data->history_index = write_index ^ 1;
         taa_data->history_valid = true;
         taa_data->previous_jitter[0] = jitter[0];
         taa_data->previous_jitter[1] = jitter[1];
         return true;
      }
   };
} // namespace LumaTAA
#endif // ENABLE_LUMA_TAA
