#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if DX11
#include "managed_resources.h"
#else
#include "d3d12_passes.h"
#include "d3d12_state.h"
#include "d3d12_upload_ring.h"
#include "d3d12_resource_creation.h" // see "DX12" and "ENABLE_MINHOOK"
#if DEVELOPMENT
#include "d3d12_readback.hpp"
#endif
#endif
#include "patch.hpp"
#include "debug.h"
#include "resource_upgrades.hpp"
#if LUMA_USE_DXP
#include "recipes.h"
#endif

// Forward declarations
struct GameDeviceData;
struct CommandListData;
struct DeviceData;
namespace
{
   namespace
   {
      // Defined in "ResourceUpgradeManager" (also aliased in "core.hpp")
      using AutoTextureFormatUpgradeShaderHash = ResourceUpgradeManager::AutoTextureFormatUpgradeShaderHash;
   }
}

enum class DrawOrDispatchOverrideType
{
   None,
   Skip,
   Replaced,
};

// Same signature as "Game::OnDrawOrDispatch()". See "draw_callbacks_by_shader_hashes" and "dispatch_callbacks_by_shader_hashes".
using DrawOrDispatchCallback = DrawOrDispatchOverrideType(*)(NativeDevice* native_device, NativeCommandList* native_device_context, CommandListData& cmd_list_data, DeviceData& device_data, reshade::api::shader_stage stages, const Shader::ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes, bool is_custom_pass, bool& updated_cbuffers, std::function<void()>* original_draw_dispatch_func);

enum class ShaderReplaceDrawType
{
   None,
   // Skips the pixel or compute shader (this might draw black or leave the previous target textures value persisting, occasionally it can crash if the engine does weird things)
   Skip,
   // Tries to draw purple (magenta) instead. Doesn't always work (it will probably skip the shader if it doesn't, and send some warnings due to pixel and vertex shader signatures not matching)
   Purple,
   // Needs to be last (see "allow_replace_draw_nans")
   NaN,
   // TODO: Add a way to draw on a black render target to see the raw difference? Add a way to only draw 1 on alpha or RGB? Not very needed.
};
enum class ShaderCustomDepthStencilType
{
   None,
   IgnoreTestWriteDepth_IgnoreStencil,
   IgnoreTestDepth_IgnoreStencil,
};

struct DrawDispatchData
{
   // Vertex Shader
   uint32_t vertex_count = 0;
   uint32_t instance_count = 0;
   uint32_t first_vertex = 0;
   uint32_t first_instance = 0;

   uint32_t index_count = 0;
   uint32_t first_index = 0;
   int32_t vertex_offset = 0;

   bool indexed = false;

   // Compute Shader
   uint3 dispatch_count = {};

   // Shared
   bool indirect = false;
};

#if DX12
// The command a D3D12 draw event stands for, to re-issue it natively. "ExecuteIndirect()" comes without its command signature, and ReShade passes
// "DispatchMesh()" with X as Y: those ("Other") can't be re-issued.
enum class D3D12DrawCommand : uint8_t
{
   Draw,
   DrawIndexed,
   Dispatch,
   Other,
};
#endif

// A frame capture entry. D3D12 fills the D3D11 named render state, viewport and sampler fields with its own, alike numbered, values (see "D3D12Trace").
struct TraceDrawCallData
{
   TraceDrawCallData()
   {
      thread_id = std::this_thread::get_id();

      std::fill_n(samplers_filter, samplers_size, static_cast<D3D11_FILTER>(-1)); // Default to no sampler
   }

   enum class TraceDrawCallType
   {
      // Any type of shader (including compute)
      Shader,
      // Copy resource and similar function
      CopyResource,
      ClearResource,
      BindPipeline,
      BindResource,
      CPURead,
      CPUWrite,
      Present,
      CreateCommandList,
      AppendCommandList,
      ResetCommmandList,
      FlushCommandList,
        Custom, // Custom draw call for custom passes we added/replaced
      // D3D12 render passes (the game's draws between them are inside the pass)
      BeginRenderPass,
      EndRenderPass,
   };

   TraceDrawCallType type = TraceDrawCallType::Shader;

#if 1 // For now add a new "TraceDrawCallData" per shader (e.g. one for vertex and one for pixel, instead of doing it per draw call), this is due to legacy code that would require too much refactor
   uint64_t pipeline_handle = 0;
#else
   uint64_t pipeline_handles = 0; // The actual list of pipelines that run within the traced frame (within this deferred command list, and then merged into the immediate one later)
   ShaderHashesList shader_hashes;
#endif

   // Which shader variant actually ran for this draw (post-decision in
   // OnDrawOrDispatch_Custom). Frame capture only.
   Shader::ShaderVariant clone_variant = Shader::ShaderVariant::Original;
   bool patch_enabled = false;

#if DX11
   // The original command list (can be useful to have later)
   com_ptr<ID3D11DeviceContext> command_list = nullptr;
#else
   // The command list that recorded this call. It only identifies it, it might be destroyed by now.
   const reshade::api::command_list* command_list = nullptr;
#endif
   // The thread this call was made on (usually 1:1 with deferred (async) command lists)
   std::thread::id thread_id = {};

   // Depth/Stencil
   enum class DepthStateType // TODO: rename to DepthStancilStateType, and "depth_state_names" too
   {
      Disabled,
      TestAndWrite,
      TestOnly,
      WriteOnly,
      Custom,
      Invalid,
   };
   static constexpr const char* depth_state_names[] = { "Disabled", "Test and Write", "Test Only", "Write Only", "Custom" };

   DrawDispatchData draw_dispatch_data = {};

   // Vertex shader
   D3D_PRIMITIVE_TOPOLOGY primitive_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
   std::vector<std::string> vertex_buffer_hashes;
   std::string input_layout_hash;
   std::string index_buffer_hash;
   DXGI_FORMAT index_buffer_format = DXGI_FORMAT_UNKNOWN;
   std::vector<DXGI_FORMAT> input_layouts_formats;
   UINT index_buffer_offset = 0;

   DepthStateType depth_state = DepthStateType::Disabled;
   DepthStateType stencil_state = DepthStateType::Disabled;
   // Already includes all the render targets. The D3D12 capture converts its PSO's blend state to it (the D3D11 and D3D12 enums are numbered alike)
   D3D11_BLEND_DESC1 blend_desc = {};
   // The blend factor (D3D11 draws), or the clear values of clear entries (depth and stencil in the first two for depth clears)
   FLOAT blend_factor[4] = {1.f, 1.f, 1.f, 1.f};

   // D3D12 packs the constant buffers in binding order (see "cb_register"): a draw can have more of them than D3D11's slots
#if DX12
   static constexpr size_t cbs_size = 32;
#else
   static constexpr size_t cbs_size = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
#endif
   bool cbs[cbs_size] = {};
   std::string cb_hash[cbs_size] = {}; // Ptr hash (not content hash)
   // D3D11.1 partial buffer binding offsets (0/0 means data not available or full-buffer bind via legacy API)
   UINT cb_first_constant[cbs_size] = {};
   UINT cb_num_constants[cbs_size] = {};

   // D3D12 packs the samplers in binding order (see "sa_register"), with the D3D12 values in the D3D11 enums (they are numbered alike)
#if DX12
   static constexpr size_t samplers_size = 32;
#else
   static constexpr size_t samplers_size = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
#endif
   D3D11_FILTER samplers_filter[samplers_size] = {};
   D3D11_TEXTURE_ADDRESS_MODE samplers_address_u[samplers_size] = {};
   D3D11_TEXTURE_ADDRESS_MODE samplers_address_v[samplers_size] = {};
   D3D11_TEXTURE_ADDRESS_MODE samplers_address_w[samplers_size] = {};
   float samplers_mip_lod_bias[samplers_size] = {};

   // Render Target (Resource+Views)
   static constexpr size_t rtvs_size = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; // Max size (we can lower it if needed) // TODO: allocate these as a vector of structs
   uint64_t rtvs[rtvs_size] = {};                                       // View handles, only compared and looked up
   DXGI_FORMAT rt_format[rtvs_size] = {};                               // The format of the resource
   DXGI_FORMAT rtv_format[rtvs_size] = {};                              // The format of the view
   uint4 rt_size[rtvs_size] = {};                                       // 3th and 4th channels are Array, MS and Mips
   uint3 rtv_size[rtvs_size] = {};
   UINT rtv_mip[rtvs_size] = {};
   std::string rt_type_name[rtvs_size] = {};
   std::string rt_hash[rtvs_size] = {};                                    // Ptr hash (not content hash)
   std::string rt_debug_name[rtvs_size] = {}; // Debug name of the texture or the view
   bool rt_is_swapchain[rtvs_size] = {};
   // Shader Resource (Resource+Views)
#if GAME_BURNOUT_PARADISE_REMASTERED // TODO: remove these hacks... Burnout Paradise crashes due to too big allocations
   static constexpr size_t srvs_size = 16;
#else
   static constexpr size_t srvs_size = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
#endif
   uint64_t srvs[srvs_size] = {};
   DXGI_FORMAT srv_format[srvs_size] = {};                                   // The format of the view
   DXGI_FORMAT sr_format[srvs_size] = {};                            // The format of the resource
   uint4 sr_size[srvs_size] = {};                                            // 3th and 4th channels are Array, MS and Mips
   uint3 srv_size[srvs_size] = {};
   UINT srv_mip[srvs_size] = {};
   std::string sr_type_name[srvs_size] = {};
   std::string sr_hash[srvs_size] = {};                                          // Ptr hash (not content hash)
   std::string sr_debug_name[srvs_size] = {}; // Debug name of the texture or the view
   bool sr_is_rt[srvs_size] = {};
   bool sr_is_ua[srvs_size] = {};
   // Unordered Access (Resource+Views)
#if GAME_BURNOUT_PARADISE_REMASTERED
   static constexpr size_t uavs_size = 16;
#else
   static constexpr size_t uavs_size = D3D11_1_UAV_SLOT_COUNT;
#endif
   uint64_t uavs[uavs_size] = {};
   DXGI_FORMAT ua_format[uavs_size] = {};               // The format of the resource
   DXGI_FORMAT uav_format[uavs_size] = {};     // The format of the view
   uint4 ua_size[uavs_size] = {};                       // 3th and 4th channels are Array, MS and Mips
   uint3 uav_size[uavs_size] = {};
   UINT uav_mip[uavs_size] = {};
   std::string ua_type_name[uavs_size] = {};
   std::string ua_hash[uavs_size] = {};                    // Ptr hash (not content hash)
   std::string ua_debug_name[uavs_size] = {}; // Debug name of the texture or the view
   bool ua_is_rt[uavs_size] = {};
   // Depth Stencil (Resource+View)
   DXGI_FORMAT ds_format = {}; // The format of the resource
   DXGI_FORMAT dsv_format = {}; // The format of the view
   uint2 ds_size = {};
   std::string ds_hash = {}; // Ptr hash (not content hash)
   std::string ds_debug_name = {}; // Debug name of the texture or the view

#if DX12
   // D3D12 binds by root signature, not by slot: the SRV, UAV and constant buffer entries above are packed in binding order, the
   // first "*_count" are valid, and these are their registers
   struct ShaderRegister
   {
      uint32_t index = 0;
      uint32_t space = 0;
   };
   uint32_t srv_count = 0;
   uint32_t uav_count = 0;
   uint32_t cb_count = 0;
   uint32_t sampler_count = 0;
   ShaderRegister sr_register[srvs_size] = {};
   ShaderRegister ua_register[uavs_size] = {};
   ShaderRegister cb_register[cbs_size] = {};
   ShaderRegister sa_register[samplers_size] = {};
   // Only the comparison filters use it
   D3D11_COMPARISON_FUNC samplers_comparison_func[samplers_size] = {};
   std::vector<uint32_t> cb_root_constants[cbs_size]; // The values of root constants (they have no "cb_hash")
   bool uses_descriptor_heap = false; // A shader indexes the SM 6.6 descriptor heaps: those resources aren't listed, their indices usually are in the constants
   // ExecuteIndirect() (with the command signature hooks, see "D3D12CommandSignatures"): the argument buffer range, the most commands and what each command sets,
   // in the signature's order. The root arguments name their root parameter's register: those bindings are each command's, not the ones the list shows.
   struct IndirectArgument
   {
      D3D12_INDIRECT_ARGUMENT_TYPE type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
      uint32_t root_parameter = UINT32_MAX; // Root constants and views
      char register_type = 0;               // 'b', 't' or 'u', 0 if the root parameter isn't known
      ShaderRegister shader_register;
      uint32_t constant_offset = 0; // Root constants: the first 32 bit value and how many
      uint32_t constant_count = 0;
      uint32_t slot = 0; // Vertex buffers
   };
   bool indirect_signature_known = false;
   uint64_t indirect_buffer = 0;
   uint64_t indirect_offset = 0;
   uint32_t indirect_max_count = 0;
   uint32_t indirect_stride = 0;
   bool indirect_count_buffer = false;
   std::vector<IndirectArgument> indirect_arguments;
   // A bindless range or a type's packed entries went past their limit: the entries above are incomplete
   bool truncated = false;
   // The table descriptors of the entries above, read when the command list executes: until it's submitted, games can still write
   // them (root signature 1.0 and "DESCRIPTORS_VOLATILE" ranges)
   struct TableDescriptor
   {
      reshade::api::descriptor_table table = {};
      uint32_t binding = 0; // Offset from the table start
      char type = 0;        // 't', 'u', 'b' or 's'
      uint32_t index = 0;   // In the packed SRV, UAV or constant buffer entries
   };
   std::vector<TableDescriptor> table_descriptors;
   // What readbacks copy, which the hashes above can't give back: the render target, unordered access and depth stencil views, and the
   // buffers of constant buffer views (root constants have none). Copy, resolve and clear entries have no views: their "rt_view[0]" is
   // what they wrote (the cleared view, else the whole resource or the destination subresource's mip and layer) and "copy_source" is
   // the resource a copy read
   D3D12State::ViewInfo rt_view[rtvs_size] = {};
   D3D12State::ViewInfo ua_view[uavs_size] = {};
   D3D12State::ViewInfo ds_view = {};
   uint64_t cb_resource[cbs_size] = {};
   uint64_t copy_source = 0;
   // The view a clear entry cleared: readbacks re-issue render target and depth stencil clears, not unordered access ones
   enum class ClearKind : uint8_t
   {
      RenderTarget,
      DepthStencil,
      UnorderedAccess,
   };
   ClearKind clear_kind = ClearKind::RenderTarget;
#endif

   // Full depth/stencil and rasterizer state (pixel shader entries only). Without a bound state object D3D11 uses its defaults. The D3D12 capture
   // converts its PSO's, and tracks the viewports and scissors only with "ENABLE_D3D12_PASSES".
   bool has_depth_stencil_state = false;
   D3D11_DEPTH_STENCIL_DESC depth_stencil_desc = {};
   UINT stencil_ref = 0;
   bool has_rasterizer_state = false;
   D3D11_RASTERIZER_DESC rasterizer_desc = {};
#if DX12
   bool scissor_enable = false; // D3D12's rasterizer desc has no scissor flag
#endif
   UINT viewport_count = 0;
   D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
   UINT scissor_count = 0;
   D3D11_RECT scissor_rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

   // Copy entries (source in "sr_*[0]", destination in "rt_*[0]")
   enum class CopyKind : uint8_t
   {
      Resource,
      Region,
      Resolve,
      BufferRegion,
      // D3D12 only
      BufferToTexture, // An upload: "box" is the written size in texels, from the origin
      TextureToBuffer,
   };
   struct CopyInfo
   {
      CopyKind kind = CopyKind::Resource;
      UINT source_subresource = 0;
      UINT dest_subresource = 0;
      uint3 dest_offset = {}; // Bytes in "x" for buffer copies
      bool has_box = false;
      // Source region (bytes in "left"/"right" for buffer copies, but the written size in texels for D3D12 uploads)
#if DX11
      D3D11_BOX box = {};
#else
      D3D12_BOX box = {};
#endif
      DXGI_FORMAT resolve_format = DXGI_FORMAT_UNKNOWN;
   };
   CopyInfo copy_info;

   // Free text game code attached to this entry during the capture (see "Mcp::Annotate")
   std::string note;

   // TODO: these might not always be filled up!
   bool any_input_resources_format_upgraded = false;
   bool any_output_resources_format_upgraded = false;
   bool any_input_resources_scaled = false;
   bool any_output_resources_scaled = false;

   bool IsRTVValid(size_t index) const { return rtv_format[index] != DXGI_FORMAT_UNKNOWN && rtv_format[index] != DXGI_FORMAT(-1); }
   bool IsSRVValid(size_t index) const { return srv_format[index] != DXGI_FORMAT_UNKNOWN && srv_format[index] != DXGI_FORMAT(-1); }
   bool IsUAVValid(size_t index) const { return uav_format[index] != DXGI_FORMAT_UNKNOWN && uav_format[index] != DXGI_FORMAT(-1); }
   bool IsDSVValid() const { return dsv_format != DXGI_FORMAT_UNKNOWN && dsv_format != DXGI_FORMAT(-1); }

   const char* custom_name = "Unknown";
   bool predicted = false; // A custom entry listed before it ran (the swapchain encode, which runs in the native present after the capture closed)
};

// Applies to command lists and command queue (DirectX 11 command list and deferred or immediate contexts, though usually it's for "ID3D11DeviceContext").
// All runtime states are thread safe as long as they were in the original implementation.
struct __declspec(uuid("90d9d05b-fdf5-44ee-8650-3bfd0810667a")) CommandListData
{
   bool is_primary = false; // Immediate/Primary (as opposed to Async/Secondary/Deferred)
   // The "texture_mip_lod_bias_offset" this context's bound samplers were last swapped for ("RebindUpgradedSamplers")
   float applied_texture_mip_lod_bias_offset = 0.f;

   CB::LumaInstanceDataPadded cb_luma_instance_data = {};
   // Always start from dirty given that deferred command lists inherit the cbuffers data from the immediate ones, but we don't know when they will get joined, so we always need to assume the data was dirty and needs to be re-set from scratch
   bool force_cb_luma_instance_data_dirty = true;
   bool async_set_cb_luma_instance_data_settings = false;

   // Whether the luma global settings have been set in this command list (device context), in case it was a deferred one
   bool async_set_cb_luma_global_settings = false;

   std::atomic<bool> write_finished{false};

   // Only used when checking for "ChainTextureFormatUpgradesType::DirectAndIndirectDependencies", as it might be disabled at runtime.
   uint enable_chain_indirect_texture_format_upgrades = 0; // TODO: ChainTextureFormatUpgradesType

   // Force resource scaling to output resolution for this command list (set during recording).
   // Used when has_drawn_sr isn't available during recording, potential issue if sr fails to draw.
   bool force_scale = false;

   reshade::api::pipeline pipeline_state_original_compute_shader = reshade::api::pipeline(0);
   reshade::api::pipeline pipeline_state_original_vertex_shader = reshade::api::pipeline(0);
   reshade::api::pipeline pipeline_state_original_pixel_shader = reshade::api::pipeline(0);

   // Patch clone handles (hash -> clone pipeline). Original handle is always
   // derivable from pipeline_state_original_*_shader (the bound pipeline).
   std::unordered_map<uint32_t, reshade::api::pipeline> patch_clone_handles;

   Shader::ShaderHashesList<OneShaderPerPipeline> pipeline_state_original_graphics_shader_hashes;
   Shader::ShaderHashesList<OneShaderPerPipeline> pipeline_state_original_compute_shader_hashes;
   bool pipeline_state_has_custom_vertex_shader = false;
   bool pipeline_state_has_custom_pixel_shader = false;
   bool pipeline_state_has_custom_graphics_shader = false;
   bool pipeline_state_has_custom_compute_shader = false;
   // D3D11: the cached pipelines bound to the pixel and compute stages, valid while "pipeline_cache_generation" still equals the one read
   // before each was looked up (a pipeline destruction raises it), so the draws read their redirections without "s_mutex_generic"
   const Shader::CachedPipeline* bound_pixel_cached_pipeline = nullptr;
   const Shader::CachedPipeline* bound_compute_cached_pipeline = nullptr;
   uint32_t bound_pixel_cached_pipeline_generation = UINT32_MAX;
   uint32_t bound_compute_cached_pipeline_generation = UINT32_MAX;

   // Determined by "draw_callbacks_by_shader_hashes" when binding shaders
   DrawOrDispatchCallback draw_callback = nullptr;
   // Determined by "dispatch_callbacks_by_shader_hashes" when binding shaders
   DrawOrDispatchCallback dispatch_callback = nullptr;

   // Pre-cached state to know if a target UI shader is bound, only relevant for the UI separation feature.
   bool is_known_ui_shader_bound = false;

   const AutoTextureFormatUpgradeShaderHash* auto_graphics_texture_format_upgrades = nullptr;
   const AutoTextureFormatUpgradeShaderHash* auto_compute_texture_format_upgrades = nullptr;

   // What a command list reset clears, and a D3D12 pipeline bind too (a D3D12 list has one pipeline, so a bind replaces all of it).
   // The D3D12 bundle hand-over ("OnExecuteSecondaryCommandList()") must copy these same fields.
   void ResetBoundPipeline()
   {
      pipeline_state_original_compute_shader = {};
      pipeline_state_original_vertex_shader = {};
      pipeline_state_original_pixel_shader = {};
      pipeline_state_original_graphics_shader_hashes.Clear();
      pipeline_state_original_compute_shader_hashes.Clear();
      pipeline_state_has_custom_vertex_shader = false;
      pipeline_state_has_custom_pixel_shader = false;
      pipeline_state_has_custom_graphics_shader = false;
      pipeline_state_has_custom_compute_shader = false;
      bound_pixel_cached_pipeline = nullptr;
      bound_compute_cached_pipeline = nullptr;
      bound_pixel_cached_pipeline_generation = UINT32_MAX;
      bound_compute_cached_pipeline_generation = UINT32_MAX;
      draw_callback = nullptr;
      dispatch_callback = nullptr;
      is_known_ui_shader_bound = false;
      auto_graphics_texture_format_upgrades = nullptr;
#if DX12
      luma_settings_param = -1;
      luma_data_param = -1;
      bound_cached_pipeline = nullptr;
      bound_base_pipeline = 0;
      bound_current_pipeline = 0;
      render_target_formats_dirty = false;
#endif
   }

   // ============================================================================
   // Patch variant API (game-facing). Per-context/per-bind state.
   // ============================================================================

#if DX11
   // Switch between original and patched shader for a given hash.
   // "stages" selects which original handle to use (vertex/pixel/compute).
   void UseShaderVariant(ID3D11DeviceContext* native_device_context, uint32_t shader_hash,
      reshade::api::shader_stage stages, Shader::ShaderVariant variant)
   {
      auto it = patch_clone_handles.find(shader_hash);
      if (it == patch_clone_handles.end()) return;

      reshade::api::pipeline bound_pipeline;
      if ((stages & reshade::api::shader_stage::vertex) != 0)
         bound_pipeline = pipeline_state_original_vertex_shader;
      else if ((stages & reshade::api::shader_stage::pixel) != 0)
         bound_pipeline = pipeline_state_original_pixel_shader;
      else if ((stages & reshade::api::shader_stage::compute) != 0)
         bound_pipeline = pipeline_state_original_compute_shader;
      else
         return;

      uint64_t handle = (variant == Shader::ShaderVariant::Patched)
         ? it->second.handle
         : bound_pipeline.handle;
      if (handle == 0) return;

      if ((stages & reshade::api::shader_stage::vertex) != 0)
         native_device_context->VSSetShader(reinterpret_cast<ID3D11VertexShader*>(handle), nullptr, 0);
      else if ((stages & reshade::api::shader_stage::pixel) != 0)
         native_device_context->PSSetShader(reinterpret_cast<ID3D11PixelShader*>(handle), nullptr, 0);
      else if ((stages & reshade::api::shader_stage::compute) != 0)
         native_device_context->CSSetShader(reinterpret_cast<ID3D11ComputeShader*>(handle), nullptr, 0);
   }
#endif // DX11

   enum ViewState
   {
      NotSet,
      // Could be null or valid
      Set,
      SetAndUpgraded,
   };

   // Note: views can be silently unbound by the API in DX11 if set to a new incompatible stage (e.g. can't set an SRV resource as RTV at the same time, the last set will win),
   // however, at worst we'll have false positives here, and we can always check for the current stage again.
   std::array<ViewState, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> ps_srvs_state = {};
   std::array<ViewState, D3D11_1_UAV_SLOT_COUNT> ps_uavs_state = {};
   std::array<ViewState, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> ps_rtvs_state = {};
   std::array<ViewState, 1> ps_dsvs_state = {};
   std::array<ViewState, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> cs_srvs_state = {};
   std::array<ViewState, D3D11_1_UAV_SLOT_COUNT> cs_uavs_state = {};
   bool any_upgraded_ps_srvs = false;
   bool any_upgraded_ps_uavs = false;
   bool any_upgraded_ps_rtvs = false;
   bool any_upgraded_ps_dsvs = false; // There's only one but we kept the name aligned
   bool any_upgraded_cs_srvs = false;
   bool any_upgraded_cs_uavs = false;

   // Whether we already scaled the currently bound viewports/scissors to match scaled (indirect upgraded) render targets.
   // Cleared whenever the game binds new ones (see "OnBindViewports()" and "OnBindScissorRects()"), so we never scale the same ones twice.
   bool viewports_scaled = false;
   bool scissors_scaled = false;

   void ResetUpgradedViews()
   {
      ps_srvs_state.fill(ViewState::NotSet);
      ps_uavs_state.fill(ViewState::NotSet);
      ps_rtvs_state.fill(ViewState::NotSet);
      ps_dsvs_state.fill(ViewState::NotSet);
      cs_srvs_state.fill(ViewState::NotSet);
      cs_uavs_state.fill(ViewState::NotSet);
      any_upgraded_ps_srvs = false;
      any_upgraded_ps_uavs = false;
      any_upgraded_ps_rtvs = false;
      any_upgraded_ps_dsvs = false;
      any_upgraded_cs_srvs = false;
      any_upgraded_cs_uavs = false;
   }
   void UpdateUpgradedPSSRVs()
   {
      any_upgraded_ps_srvs = std::any_of(ps_srvs_state.begin(), ps_srvs_state.end(), [](auto v) { return v == ViewState::SetAndUpgraded; });
   }
   void UpdateUpgradedPSUAVs()
   {
      any_upgraded_ps_uavs = std::any_of(ps_uavs_state.begin(), ps_uavs_state.end(), [](auto v) { return v == ViewState::SetAndUpgraded; });
   }
   void UpdateUpgradedPSRTVs()
   {
      any_upgraded_ps_rtvs = std::any_of(ps_rtvs_state.begin(), ps_rtvs_state.end(), [](auto v) { return v == ViewState::SetAndUpgraded; });
   }
   void UpdateUpgradedPSDSVs()
   {
      any_upgraded_ps_dsvs = std::any_of(ps_dsvs_state.begin(), ps_dsvs_state.end(), [](auto v) { return v == ViewState::SetAndUpgraded; });
   }
   void UpdateUpgradedCSSRVs()
   {
      any_upgraded_cs_srvs = std::any_of(cs_srvs_state.begin(), cs_srvs_state.end(), [](auto v) { return v == ViewState::SetAndUpgraded; });
   }
   void UpdateUpgradedCSUAVs()
   {
      any_upgraded_cs_uavs = std::any_of(cs_uavs_state.begin(), cs_uavs_state.end(), [](auto v) { return v == ViewState::SetAndUpgraded; });
   }

#if DX11
   static void SetConstantBuffers(ID3D11DeviceContext* device_context, reshade::api::shader_stage stages, uint32_t slot, ID3D11Buffer* const buffers, uint32_t num = 1)
   {
      if ((stages & reshade::api::shader_stage::vertex) == reshade::api::shader_stage::vertex)
         device_context->VSSetConstantBuffers(slot, num, &buffers);
      if ((stages & reshade::api::shader_stage::geometry) == reshade::api::shader_stage::geometry)
         device_context->GSSetConstantBuffers(slot, num, &buffers);
      if ((stages & reshade::api::shader_stage::pixel) == reshade::api::shader_stage::pixel)
         device_context->PSSetConstantBuffers(slot, num, &buffers);
      if ((stages & reshade::api::shader_stage::compute) == reshade::api::shader_stage::compute)
         device_context->CSSetConstantBuffers(slot, num, &buffers);
   }
#endif // DX11

#if ENABLE_AUTO_CBUFFER_RESTORATION
   com_ptr<ID3D11Buffer> original_constant_buffers[(uint8_t)Shader::Stage::Count][D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
   UINT original_constant_buffers_first_constant[(uint8_t)Shader::Stage::Count][D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
   UINT original_constant_buffers_num_constant[(uint8_t)Shader::Stage::Count][D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};

   void ClearOriginalConstantBuffers()
   {
      for (auto& original_constant_buffers_stage : original_constant_buffers)
         for (auto& original_constant_buffer : original_constant_buffers_stage)
            original_constant_buffer.reset();

      std::fill_n(&original_constant_buffers_first_constant[0][0], sizeof(original_constant_buffers_first_constant) / sizeof(UINT), 0);
      std::fill_n(&original_constant_buffers_num_constant[0][0], sizeof(original_constant_buffers_num_constant) / sizeof(UINT), 4096);
   }

   void RestoreOriginalConstantBuffers(ID3D11DeviceContext* device_context)
   {
      auto SetOriginalConstantBuffersStage = [&](ID3D11DeviceContext* device_context, bool use_device_context_1, Shader::Stage stage)
      {
         const size_t stage_index = (size_t)stage;
         ID3D11Buffer* const* original_constant_buffers_const = reinterpret_cast<ID3D11Buffer* const*>(original_constant_buffers[stage_index]);
         if (use_device_context_1)
         {
            if (stage == Shader::Stage::Vertex)
               static_cast<ID3D11DeviceContext1*>(device_context)->VSSetConstantBuffers1(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, original_constant_buffers_const, original_constant_buffers_first_constant[stage_index], original_constant_buffers_num_constant[stage_index]);
            else if (stage == Shader::Stage::Geometry)
               static_cast<ID3D11DeviceContext1*>(device_context)->GSSetConstantBuffers1(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, original_constant_buffers_const, original_constant_buffers_first_constant[stage_index], original_constant_buffers_num_constant[stage_index]);
            else if (stage == Shader::Stage::Pixel)
               static_cast<ID3D11DeviceContext1*>(device_context)->PSSetConstantBuffers1(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, original_constant_buffers_const, original_constant_buffers_first_constant[stage_index], original_constant_buffers_num_constant[stage_index]);
            else if (stage == Shader::Stage::Compute)
               static_cast<ID3D11DeviceContext1*>(device_context)->CSSetConstantBuffers1(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, original_constant_buffers_const, original_constant_buffers_first_constant[stage_index], original_constant_buffers_num_constant[stage_index]);
            return;
         }
         if (stage == Shader::Stage::Vertex)
            device_context->VSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, original_constant_buffers_const);
         else if (stage == Shader::Stage::Geometry)
            device_context->GSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, original_constant_buffers_const);
         else if (stage == Shader::Stage::Pixel)
            device_context->PSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, original_constant_buffers_const);
         else if (stage == Shader::Stage::Compute)
            device_context->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, original_constant_buffers_const);
      };

      com_ptr<ID3D11DeviceContext1> device_context_1;
      device_context->QueryInterface(&device_context_1);
      const bool use_device_context_1 = device_context_1 != nullptr;
      for (uint8_t stage = 0; stage < (uint8_t)Shader::Stage::Count; stage++)
      {
#if !GEOMETRY_SHADER_SUPPORT // Note: we assume the mod didn't modify geometry shaders bindings if "GEOMETRY_SHADER_SUPPORT" is false. It's an optimization.
         if (stage == (uint8_t)Shader::Stage::Geometry)
            continue;
#endif
         SetOriginalConstantBuffersStage(device_context, use_device_context_1, static_cast<Shader::Stage>(stage));
      }
   }
#endif // ENABLE_AUTO_CBUFFER_RESTORATION

#if DEVELOPMENT
   std::shared_mutex mutex_trace;
   std::vector<TraceDrawCallData> trace_draw_calls_data;

#if DX11
   void AddGraphicsCaptureCustomDrawCall(const char* name, class ID3D11DeviceContext* native_device_context, class ID3D11Resource* resource = nullptr, class ID3D11View* view = nullptr);
#endif // DX11

   bool requires_join = false;

   bool any_draw_done = false;
   bool any_dispatch_done = false;

   ShaderCustomDepthStencilType temp_custom_depth_stencil = ShaderCustomDepthStencilType::None;
#endif

#if DX12
   // ReShade's proxy of the list, through whose API Luma records its passes on it (see "DrawRCAS()")
   reshade::api::command_list* cmd_list = nullptr;
   // What the command list bound, as far as game code can query it on release builds too (see "d3d12_state.h")
   D3D12State::CommandListState d3d12_state;
   // A bundle (secondary command list): the command list that executes it inherits what it bound
   bool is_bundle = false;
   // The memory of the root CBVs Luma sets on this command list (see "d3d12_upload_ring.h"). The pool is the device's, held here too: this data can outlive the device's
   std::shared_ptr<D3D12UploadRing::Pool> upload_ring;
   D3D12UploadRing::Pages upload_pages;
   D3D12_GPU_VIRTUAL_ADDRESS luma_data_address = 0; // Of the last upload of "cb_luma_instance_data", 0 since the last reset
   // The root parameters of Luma's CBVs in the root signature of the bound pipeline, when the bind swapped in a replacement (-1 for the CBVs it doesn't have, see
   // "D3D12RootSignatures::LumaParams"). Plain integers: builds without MinHook have no root signature hook
   int32_t luma_settings_param = -1;
   int32_t luma_data_param = -1;
   // The pipeline the command list bound, for its draws to follow the formats of the render targets (see "UpdateD3D12PipelineVariant()"). Null if it isn't tracked. Valid while it's bound, like the hashes above
   const Shader::CachedPipeline* bound_cached_pipeline = nullptr;
   // What Luma set at the bind: the clone of the pipeline if it has an active one, else the pipeline. A render target format variant is made from it
   uint64_t bound_base_pipeline = 0;
   // What the command list really has bound now: "bound_base_pipeline" or one of its variants
   uint64_t bound_current_pipeline = 0;
   // Set by the binds of a pipeline and of render targets: the next graphics draw looks at the formats of the render targets again
   bool render_target_formats_dirty = false;
#endif
};

struct __declspec(uuid("cfebf6d4-d184-4e1a-ac14-09d088e560ca")) DeviceData
{
   // Only for "swapchains", "back_buffers" and "upgraded_resources" (and related) and "patch_context".
   // Device object creation etc is usually single threaded anyway, except for the destructor.
   std::shared_mutex mutex;

   std::thread thread_auto_loading;
   std::atomic<bool> thread_auto_loading_running = false;

   // TODO(Patch module): move the patch_context member below and this async clone
   // machinery (AsyncCloneEntry + the LUMA_PATCH_PROVIDERS-gated members) into
   // patch.hpp once the include-order rework lands (see core.hpp dispatch TODO).
#if LUMA_PATCH_PROVIDERS & (LUMA_PATCH_PROVIDER_BYTECODE_ASYNC | LUMA_PATCH_PROVIDER_RECIPE_ASYNC)
   // Unified async clone entry: mode 1 populates subobjects+layout (created at
   // present boundary), mode 2 populates pipeline_clone (created by worker).
   // Only one of the two paths is active per build configuration.
   struct AsyncCloneEntry
   {
      uint64_t pipeline_handle = 0;
      // Mode 1: subobjects built by worker, pipeline created at present boundary
      reshade::api::pipeline_subobject* subobjects = nullptr;
      uint32_t subobject_count = 0;
      reshade::api::pipeline_layout layout = {};
      // Mode 2: pipeline already compiled by worker
      reshade::api::pipeline pipeline_clone = {};
      // Shared
      reshade::api::device* device = nullptr;
      uint32_t shader_hash = 0; // shader the clone was built for (verify at publish)
      Shader::CloneOrigin clone_origin = Shader::CloneOrigin::None; // what the clone was built from (recorded by the worker)
   };
   // Lock-free ready queue. Single worker thread — atomic pointer is sufficient,
   // no mutex needed. Worker publishes to the queue (reads pointer, creates if
   // null, writes). Render thread drains at OnPresent via atomic exchange.
   struct AsyncReadyQueue
   {
      std::deque<AsyncCloneEntry> items;
   };
   std::atomic<AsyncReadyQueue*> async_ready_queue{nullptr};

   std::atomic<bool> async_shutdown = false;
   std::mutex async_jobs_mutex;
   std::condition_variable async_jobs_cv;
   uint64_t async_queue_version = 0;
#endif // async providers

   // Resource upgrade / mirroring state and configuration (see resource_upgrades.hpp).
   // Owns the mirror bookkeeping and upgrade/scale configuration; core.hpp passes the frame's
   // resolution/SR state into its methods. Migrated subsystem-by-subsystem.
   ResourceUpgradeManager resource_upgrades;

#if LUMA_PATCH_PROVIDERS != 0
   // Stored shader patches (both bytecode and recipe methods) + per-method
   // processed markers. See Patch::PatchContext.
   Patch::PatchContext patch_context;
#endif

#if LUMA_USE_DXP
   Recipes recipes;
#endif

   std::unordered_set<reshade::api::swapchain*> swapchains;
#if ENABLE_REFLEX
   Reflex::DeviceData reflex; // For the first swapchain (the only one expected)
#endif
   std::unordered_set<uint64_t> back_buffers; // From all the swapchains (whether they are upgraded or not)
#if DX12
   // The ones of "back_buffers" whose format family the swapchain upgrade changed (see "OnCreateSwapchain()"): only these can't take the game's copies through a buffer
   std::unordered_set<uint64_t> upgraded_back_buffers;
#endif
   NativeDevice* native_device = nullptr; // Doesn't need mutex, always valid given it's a ptr to "itself"
   reshade::api::device* reshade_device = nullptr; // Always valid as it's our parent. Not owned by this.
#if DX11
   com_ptr<ID3D11DeviceContext> primary_command_list; // The immediate/primary command list is always valid
   CommandListData* primary_command_list_data = nullptr; // The immediate/primary command list is always valid
#endif

   UINT uav_max_count = D3D11_1_UAV_SLOT_COUNT; // DX11.1. Use "D3D11_PS_CS_UAV_REGISTER_COUNT" for DX11.

   com_ptr<IDXGISwapChain3> GetMainNativeSwapchain() const
   {
      ASSERT_ONCE(swapchains.size() == 1);
      if (swapchains.empty()) return nullptr;
      IDXGISwapChain* native_swapchain = (IDXGISwapChain*)((*swapchains.begin())->get_native());
      com_ptr<IDXGISwapChain3> native_swapchain3;
      // The cast pointer is actually the same, we are just making sure the type is right.
      HRESULT hr = native_swapchain->QueryInterface(&native_swapchain3);
      ASSERT_ONCE(SUCCEEDED(hr));
      return native_swapchain3;
   }

   // Lock when removing a pipeline from "pipeline_cache_by_pipeline_handle" (and thus "pipeline_cache_by_pipeline_clone_handle", "pipeline_caches_by_shader_hash").
   // Only needs to also be locked if you don't already lock "s_mutex_generic" in other places, to make sure the pipelines you are reading aren't getting destroyed during read.
   std::shared_mutex pipeline_cache_destruction_mutex;
   // Pipelines by handle. Multiple pipelines can target the same shader, and even have multiple shaders within themselves.
   // This contains all pipelines (from the game) that we can replace shaders of (e.g. pixel shaders, vertex shaders, ...).
   // It's basically data we append (link) to pipelines, done manually because we have no other way.
   // The data here is allocated by itself. // TODO: make this a unique pointer for easier handling.
   std::unordered_map<uint64_t, Shader::CachedPipeline*> pipeline_cache_by_pipeline_handle;
   // Same as "pipeline_cache_by_pipeline_handle" but mapped to cloned (custom) pipeline handles.
   std::unordered_map<uint64_t, Shader::CachedPipeline*> pipeline_cache_by_pipeline_clone_handle;
   // All the pipelines linked to a shader. By original shader hash.
   std::unordered_map<uint32_t, std::unordered_set<Shader::CachedPipeline*>> pipeline_caches_by_shader_hash;

   std::unordered_set<uint64_t> pipelines_to_reload;
   static_assert(sizeof(reshade::api::pipeline::handle) == sizeof(uint64_t));

#if DX12 && DEVELOPMENT // For the frame capture, for now
   D3D12State::PipelineLayouts pipeline_layouts;
   D3D12State::DescriptorHeaps descriptor_heaps;
   D3D12State::BufferAddresses buffer_addresses;
   D3D12State::LiveResources live_resources;
   // The frame capture, joined from the command lists in submission order (the queue's immediate command list is ReShade's own)
   CommandListData trace_command_list_data;
   D3D12Readback::State readback; // The dev MCP's GPU copies
#if ENABLE_MINHOOK
   D3D12ResourceCreation::TextureKinds texture_kinds; // How the game created each live texture, for the dev MCP's "creation" (see "includes/d3d12_resource_creation.h")
#endif
#endif

#if DX11
   // Custom samplers mapped to original ones by texture LOD bias
   std::unordered_map<uint64_t, std::unordered_map<float, com_ptr<ID3D11SamplerState>>> custom_sampler_by_original_sampler;
#endif

#if ENABLE_SR
   SR::Type sr_type = SR::Type::None; // If active, the SR tech enabled by the user and supported+initialized correctly on this device
   std::atomic<SR::Type> sr_type_selected = SR::Type::None; // The overlay's and the MCP's pick, applied to "sr_type" at the next present
   // None was applied ("ApplySRType"): a game whose draws may still use its upscaler resources until its "OnPresent" latches the
   // selection releases them there, taking this with "exchange(false)" (and can set it for its own reasons)
   std::atomic<bool> release_sr_resources = false;
   // Exclusive for "ApplySRType()" (at present), shared by a game's SR draw ("LockSRForDraw()"): the type and its instance don't switch, nor do retired
   // upscaler objects go, while a list records the draw
   std::shared_mutex sr_mutex;
   std::map<SR::Type, SR::InstanceData*> sr_implementations_instances; // All implementations allowed by the current mod, might not all be compatible
   SR::InstanceData* GetSRInstanceData() const
   {
      auto it = sr_implementations_instances.find(sr_type);
      return (it != sr_implementations_instances.end()) ? it->second : nullptr;
   }
#endif

   // Resources:

#if ENABLE_SR
#if DX12
   com_ptr<ID3D12Resource> sr_output_color;
   com_ptr<ID3D12Resource> sr_exposure;
#else
   com_ptr<ID3D11Texture2D> sr_output_color;
   com_ptr<ID3D11Texture2D> sr_exposure;
#endif
   float sr_exposure_texture_value = 1.f;
#endif // ENABLE_SR

#if DX11 // D3D11 objects: the D3D12 Luma passes make their pipelines from the natives' bytecode ("CreateD3D12NativePipeline()"), its UI resources aren't ported, its cbuffers are in the upload ring below
   // Native Shaders (from "native_shaders_definitions")
   bool created_native_shaders = false;
   std::unordered_map<uint32_t, com_ptr<ID3D11VertexShader>> native_vertex_shaders;
#if GEOMETRY_SHADER_SUPPORT
   std::unordered_map<uint32_t, com_ptr<ID3D11GeometryShader>> native_geometry_shaders;
#endif
   std::unordered_map<uint32_t, com_ptr<ID3D11PixelShader>> native_pixel_shaders;
   std::unordered_map<uint32_t, com_ptr<ID3D11ComputeShader>> native_compute_shaders;

#if DEVELOPMENT
   std::unordered_map<const ID3D11InputLayout*, std::vector<D3D11_INPUT_ELEMENT_DESC>> input_layouts_descs;
#endif

   // Native Shaders Resources
   com_ptr<ID3D11Texture2D> temp_copy_source_texture;
   com_ptr<ID3D11Texture2D> temp_copy_target_texture;
   com_ptr<ID3D11Texture2D> display_composition_texture; // Temporary copy texture of the swapchain that we use to draw back to the swapchain with a display composition shader (e.g. to change brightness, transfer function, gamut etc)
   com_ptr<ID3D11ShaderResourceView> display_composition_srv;

   // CBuffers
   com_ptr<ID3D11Buffer> luma_global_settings;
   com_ptr<ID3D11Buffer> luma_instance_data;
   com_ptr<ID3D11Buffer> luma_ui_data;
#endif // DX11
   CB::LumaUIDataPadded cb_luma_ui_data = {};
   std::atomic<bool> cb_luma_global_settings_dirty = true;

#if DX12
   // The memory of the root CBVs Luma adds to the game's root signatures (see "d3d12_upload_ring.h"). Command lists hold the pool too, their data can outlive this
   std::shared_ptr<D3D12UploadRing::Pool> upload_ring;
   // The copies of the Luma settings, one per change: a command list keeps pointing at the one it recorded. Every command list may use the newest, so "settings_mutex"
   // serializes the uploads into these pages
   D3D12UploadRing::Pages settings_pages = {.shared = true};
   std::shared_mutex settings_mutex;
   std::atomic<D3D12_GPU_VIRTUAL_ADDRESS> settings_address = 0; // The newest copy, 0 before the first
   // The ring frees its pages by the fences it signals with this count at each present
   std::atomic<uint64_t> present_count = 0;

   // Bumped when any of Luma's native shaders changes (see "CompileCustomShaders()"): the pipelines made of them are made again
   std::atomic<uint32_t> native_shaders_generation = 0;

   // What a fullscreen pass of Luma's natives at present makes when it first draws (see "PrepareD3D12NativePass()"). "mutex" serializes the pass's draws and guards the rest
   struct NativePassD3D12
   {
      std::shared_mutex mutex;
      D3D12Passes::Context passes;                 // Its command list, run on the present queue
      com_ptr<ID3D12RootSignature> root_signature; // Its CBVs are at the registers the natives were compiled with
      com_ptr<ID3D12PipelineState> pipeline;       // Null if the last attempt to make it for the format and generation below failed
      DXGI_FORMAT pipeline_format = DXGI_FORMAT_UNKNOWN;
      uint32_t pipeline_generation = 0;
      bool pipeline_compute = false; // Made for a compute shader (see "PrepareD3D12NativePass()")
      CommandListData cmd_list_data; // Owns the memory of its root CBVs (root parameters 0 and 1), for "SetLumaConstantBuffers()"
      // Each of these failures is logged once per pass
      std::atomic_flag logged_no_passes;
      std::atomic_flag logged_no_frame;
      std::atomic_flag logged_no_submit;
   };
   // The display composition (see "DrawDisplayCompositionD3D12()")
   struct DisplayCompositionD3D12 : NativePassD3D12
   {
      com_ptr<ID3D12Resource> source;    // A copy of the back buffer, which the shader reads while it writes the back buffer
      uint32_t drawn_frame = UINT32_MAX; // The "FrameIndex" of the last present it drew in, for the dev MCP
      // The compute shader loads the back buffer it writes (a typed UAV load): the last format checked for it, and whether it supports it
      DXGI_FORMAT typed_load_format = DXGI_FORMAT_UNKNOWN;
      bool typed_load = false;
   } display_composition;
   // The encode of the scRGB proxy into the real back buffer (see "DrawSwapchainEncodeD3D12()")
   NativePassD3D12 swapchain_encode;
#if ENABLE_D3D12_PASSES
   // What a D3D12 pass drawn on the game's command lists ("DrawRCAS()", "DrawSMAA()") makes when it first draws (see "PrepareD3D12GamePass()").
   // "mutex" serializes its draws and guards the rest
   struct GamePassD3D12
   {
      std::shared_mutex mutex;
      reshade::api::pipeline_layout layout = {};                            // Made through ReShade, which its descriptor tables need
      std::vector<reshade::api::descriptor_table> tables;                   // Of ReShade's GPU heap, by present, by draw in it and by table of the draw
      std::unordered_map<uint64_t, com_ptr<ID3D12PipelineState>> pipelines; // By pixel shader hash and render target format, of the natives of "pipelines_generation"
      uint32_t pipelines_generation = 0;
      std::vector<std::pair<uint64_t, com_ptr<ID3D12PipelineState>>> retired; // Of older natives, with the present count they were replaced at: a game list recorded with them may still run (see "DestroyRetiredD3D12GamePassResources()")
      uint64_t frame = UINT64_MAX;                        // The present count of the last draw, and the draws since it
      uint32_t frame_draws = 0;
      bool failed = false;                                                        // ReShade refused the layout or the tables, which isn't tried again
      uint32_t fullscreen_source_count = 0;                                       // "DrawFullscreenPassD3D12()": the SRVs of the table of t0, fixed by the pass's first draw
      std::vector<std::pair<uint64_t, reshade::api::resource>> retired_resources; // Replaced textures, with the present count they were replaced at (see "RetireD3D12GamePassResource()")
   };
#if ENABLE_RCAS
   GamePassD3D12 rcas; // "SharpenParams" as 4 root constants (b0), then a table of t0
#endif
#if ENABLE_SMAA
   // 8 root constants (b1), a table of t0-t2 and the linear and point samplers (s0, s1)
   struct SMAAD3D12 : GamePassD3D12
   {
      // Uploaded through ReShade's immediate list, so at a present ("CreateSMAATexturesD3D12()"), once a draw asked for them
      std::atomic<bool> textures_wanted = false;
      reshade::api::resource area_texture = {};
      reshade::api::resource search_texture = {};
      reshade::api::resource_view area_srv = {};
      reshade::api::resource_view search_srv = {};
      // At the target's size, made again with it. In a shader resource state between draws
      reshade::api::resource edges = {};
      reshade::api::resource weights = {};
      reshade::api::resource_view edges_rtv = {};
      reshade::api::resource_view edges_srv = {};
      reshade::api::resource_view weights_rtv = {};
      reshade::api::resource_view weights_srv = {};
      // The neighborhood blend's, when RCAS follows it: at the target's size and format
      reshade::api::resource output = {};
      reshade::api::resource_view output_rtv = {};
      reshade::api::resource_view output_srv = {};
      reshade::api::format output_format = reshade::api::format::unknown;
      uint32_t width = 0;
      uint32_t height = 0;
   } smaa;
#endif
#if ENABLE_BLOOM
   // A table of t0 and u0. Its output (fp16, at the source's size) is in a shader resource state between draws
   struct KarisAverageD3D12 : GamePassD3D12
   {
      reshade::api::resource output = {};
      reshade::api::resource_view output_srv = {};
      reshade::api::resource_view output_uav = {};
      uint32_t width = 0;
      uint32_t height = 0;
   } karis_average;
   // "LumaBloom" as 8 root constants (b11), a table of t0 and the linear sampler (s0). The chains are made again with the scene's size or the mip count,
   // in a shader resource state between draws: Y is one texture with a mip per level (mip 0 is the bloom), X one texture per level (see "DrawBloom()")
   struct BloomD3D12 : GamePassD3D12
   {
      reshade::api::resource mips_y = {};
      std::vector<reshade::api::resource> mips_x;
      std::vector<reshade::api::resource_view> rtvs_y, srvs_y, rtvs_x, srvs_x;
      uint32_t scene_width = 0;
      uint32_t scene_height = 0;
      uint32_t mip_count = 0;
   } bloom;
#endif
   // "ConvertTextureCopyD3D12()": a table of t0, and the textures it goes through, one for each usage, size and format the game's copies had (see "GetD3D12CopyIntermediate()"): the render
   // target it draws into when the copy's destination can't be one (or may be in a layout), in a render target state between draws, and the texture it samples when the copy's source may be in a
   // layout (a copy of the source), in a copy destination state between draws
   struct TextureCopyD3D12 : GamePassD3D12
   {
      struct Intermediate
      {
         reshade::api::resource resource = {};
         reshade::api::resource_usage usage = reshade::api::resource_usage::undefined;
         uint32_t width = 0;
         uint32_t height = 0;
         reshade::api::format format = reshade::api::format::unknown;
      };
      std::vector<Intermediate> intermediates;
   } texture_copy;
#endif
#endif

#if DX11
   // UI
   com_ptr<ID3D11Texture2D> ui_texture;
   com_ptr<ID3D11RenderTargetView> ui_texture_rtv;
   com_ptr<ID3D11ShaderResourceView> ui_texture_srv;
   com_ptr<ID3D11RenderTargetView> ui_initial_original_rtv; // Leave nullptr to fall back on the current swapchain. This is an RTV but what matters is actually the resource behind it.
   com_ptr<ID3D11RenderTargetView> ui_latest_original_rtv;

   // Misc
   com_ptr<ID3D11SamplerState> sampler_state_linear;
   com_ptr<ID3D11SamplerState> sampler_state_point;
   com_ptr<ID3D11BlendState> default_blend_state; // No blend
   com_ptr<ID3D11DepthStencilState> default_depth_stencil_state; // Depth/Stencil disabled
#if DEVELOPMENT
   com_ptr<ID3D11DepthStencilState> depth_test_false_write_true_stencil_false_state;
#endif

   // Pointer to the current DX buffer for the "global per view" cbuffer.
   com_ptr<ID3D11Buffer> cb_per_view_global_buffer;
#if DEVELOPMENT
   std::unordered_set<ID3D11Buffer*> cb_per_view_global_buffers;
#endif
   void* cb_per_view_global_buffer_map_data = nullptr;
#endif // DX11
#if DEVELOPMENT
   DXGI_FORMAT debug_draw_texture_format = DXGI_FORMAT_UNKNOWN; // The view format, not the texture format
   uint4 debug_draw_texture_size = {}; // 3rd and 4th channels are Array/MS/Mips
#endif
#if DEVELOPMENT && DX11
   std::shared_ptr<void> debug_draw_frozen_draw_state_stack;
   com_ptr<ID3D11Resource> debug_draw_texture;

   struct TrackBufferData
   {
      std::string hash; // Resource ptr hash
      bool data_valid = false;
      std::vector<float> data;
      com_ptr<ID3D11Buffer> cb;
      
      UINT first_constant = 0; // D3D11.1: start offset in float4 units (0 if D3D11.0 or full-buffer bind)
      UINT num_constants = 0;  // D3D11.1: count in float4 units (0 if D3D11.0 info unavailable)

      void Clear()
      {
         hash.clear();
         data_valid = false;
         data.clear();
         cb.reset();
         first_constant = 0;
         num_constants = 0;
      }
   };
   TrackBufferData track_buffer_data;
#endif

   // Generic states that can be used by multiple games (you don't need to set them if you ignore the whole thing)

   // Whether the "main" post processing passes have finished drawing (it also implied we detected scene rendering and some cbuffers etc)
   std::atomic<bool> has_drawn_main_post_processing = false;
   // Useful to know if rendering was skipped in the previous frame (e.g. in case we were in a UI view)
   bool has_drawn_main_post_processing_previous = false;
   // Might not be used by all games but it's a global feature (not necessarily related to "ENABLE_SR", the game might have a native implementation)
   std::atomic<bool> has_drawn_sr = false;
   // Set to true once we can tell with certainty that TAA was active in the game
   std::atomic<bool> taa_detected = false;

#if ENABLE_SR
   std::atomic<bool> force_reset_sr = false;
   std::atomic<bool> sr_suppressed = false;
   std::atomic<float> sr_render_resolution_scale = 1.f;
#endif

   // TODO: make changes thread safe
   // Render resolution without padding.
   float2 render_resolution = { 1, 1 };
   float2 previous_render_resolution = { 1, 1 };
   // Note: this is the window/swapchain res
   float2 output_resolution = { 1, 1 };
   float2 display_resolution = { 1, 1 };

   // Last frame's scale-relevant state, used to detect when indirect mirrors (and anything else tied to
   // the render scale) must be invalidated because the render resolution or SR selection changed.
   float2 last_render_resolution = { 1, 1 };
#if ENABLE_SR
   SR::Type last_sr_type = SR::Type::None;
#endif

   // Live settings (set by the code, not directly by users):
   float default_user_peak_white = default_peak_white;
   float texture_mip_lod_bias_offset = 0.f;
#if DX12
   // The sampler upgrade the D3D12 sampler descriptors the game wrote have ("GetSamplersUpgradeState()"): "OnPresent()" writes them again when it changes
   std::tuple<float, int, int, bool, bool> applied_samplers_upgrade;
#endif
#if ENABLE_SR
   float sr_scene_exposure = 1.f;
   float sr_scene_pre_exposure = 1.f;
#endif

   std::atomic<bool> cloned_pipelines_changed = false; // Atomic so it doesn't rely on "s_mutex_generic"
   uint32_t cloned_pipeline_count = 0; // How many pipelines (shaders/passes) we replaced with custom ones and are currently "replaced" (if zero, we can assume the mod isn't doing much)

#if ENABLE_SR
   bool has_drawn_sr_imgui = false;
#endif

   // Per game custom data
   GameDeviceData* game = nullptr;

#if DX11
   std::vector<ID3D11Buffer*> GetLumaCBuffers() const
   {
      std::vector<ID3D11Buffer*> buffers;
      if (luma_global_settings)
         buffers.push_back(luma_global_settings.get());
      if (luma_instance_data)
         buffers.push_back(luma_instance_data.get());
      if (luma_ui_data)
         buffers.push_back(luma_ui_data.get());
      return buffers;
   }

   ManagedResources managed_resources;
#endif // DX11
};

// A Luma native shader ("native_*_shaders") by name, null until compiled. Looked up with "find": "operator[]" would insert on a miss,
// mutating a map Core's draw helpers read concurrently. A caller that can race a shader (re)load holds "s_mutex_shader_objects".
template <typename ShaderMap>
auto FindShader(const ShaderMap& shaders, uint32_t name)
{
   const auto it = shaders.find(name);
   return (it != shaders.end() ? it->second.get() : nullptr);
}

// Whether all the named Luma native shaders are compiled (shaders load asynchronously and reload live in development)
template <typename ShaderMap, typename... Names>
bool HasShaders(const ShaderMap& shaders, Names... names)
{
   return ((FindShader(shaders, names) != nullptr) && ...);
}

struct __declspec(uuid("c5805458-2c02-4ebf-b139-38b85118d971")) SwapchainData
{
   // Probably not particularly useful as the swapchain would be single threaded, if not for its destruction callback
   std::shared_mutex mutex;

   std::unordered_set<uint64_t> back_buffers;

#if DX11
   std::vector<com_ptr<ID3D11RenderTargetView>> display_composition_rtvs;
#endif

   // Whether the original SDR (vanilla) swapchain was linear space (e.g. sRGB formats)
    bool vanilla_was_linear_space = false;
};
