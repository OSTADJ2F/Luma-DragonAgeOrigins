#pragma once

// D3D12 state Core tracks from ReShade events, given that D3D12 has no state queries:
// the root signatures, what each command list binds, and a shadow copy of the descriptor heaps to resolve bound tables to descriptors.
// Release builds only track what game code can query (the pipeline, the root signatures, the render targets and the depth stencil), the rest is
// for the frame capture (DEVELOPMENT).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include <concurrent_unordered_map.h>
#include <concurrent_vector.h>
#include <d3d12.h>
#include <include/reshade.hpp>

namespace D3D12State
{
   // A root signature parameter, deep copied as ReShade's arrays only live through "init_pipeline_layout"
   struct RootParameter
   {
      reshade::api::pipeline_layout_param_type type = reshade::api::pipeline_layout_param_type::push_descriptors;
      reshade::api::constant_range constants = {};        // Root constants
      std::vector<reshade::api::descriptor_range> ranges; // Descriptor table ranges, or the root descriptor
#if DEVELOPMENT
      // The static samplers of a root signature are one parameter of a sampler range each (ReShade's D3D12 hook makes it so), these are their descs by range
      std::vector<reshade::api::sampler_desc> static_samplers;
#endif
   };

   struct PipelineLayouts
   {
      std::shared_mutex mutex;
      std::unordered_map<uint64_t, std::vector<RootParameter>> parameters_by_layout;
   };

   void AddPipelineLayout(PipelineLayouts* layouts, reshade::api::pipeline_layout layout, uint32_t param_count, const reshade::api::pipeline_layout_param* params)
   {
      std::vector<RootParameter> parameters(param_count);
      for (uint32_t i = 0; i < param_count; i++)
      {
         const reshade::api::pipeline_layout_param& param = params[i];
         RootParameter& parameter = parameters[i];
         parameter.type = param.type;
         switch (param.type)
         {
         case reshade::api::pipeline_layout_param_type::push_constants:
            parameter.constants = param.push_constants;
            break;
         case reshade::api::pipeline_layout_param_type::push_descriptors:
            parameter.ranges.push_back(param.push_descriptors);
            break;
         case reshade::api::pipeline_layout_param_type::descriptor_table:
         case reshade::api::pipeline_layout_param_type::push_descriptors_with_ranges:
            parameter.ranges.assign(param.descriptor_table.ranges, param.descriptor_table.ranges + param.descriptor_table.count);
            break;
         case reshade::api::pipeline_layout_param_type::descriptor_table_with_flags:
         case reshade::api::pipeline_layout_param_type::push_descriptors_with_ranges_and_flags:
            // The derived range type has another stride
            for (uint32_t j = 0; j < param.descriptor_table_with_flags.count; j++)
            {
               const reshade::api::descriptor_range_with_flags& range = param.descriptor_table_with_flags.ranges[j];
               parameter.ranges.push_back(range);
#if DEVELOPMENT
               if (range.static_samplers)
               {
                  parameter.static_samplers.push_back(*range.static_samplers);
               }
#endif
            }
            break;
         }
      }
      const std::unique_lock lock(layouts->mutex);
      layouts->parameters_by_layout[layout.handle] = std::move(parameters);
   }

#if DEVELOPMENT
   // The static samplers of a root signature 1.2, which ReShade doesn't pass to "init_pipeline_layout": one more parameter of a sampler range each, like the one it makes of the
   // static samplers of the older versions. Call it from the "init_pipeline_layout" event, after "AddPipelineLayout()"
   void AddStaticSamplers(PipelineLayouts* layouts, reshade::api::pipeline_layout layout, std::span<const D3D12_STATIC_SAMPLER_DESC1> static_samplers)
   {
      if (static_samplers.empty())
         return;
      RootParameter parameter = {.type = reshade::api::pipeline_layout_param_type::push_descriptors_with_ranges_and_flags};
      for (const D3D12_STATIC_SAMPLER_DESC1& sampler : static_samplers)
      {
         parameter.ranges.push_back({.dx_register_index = sampler.ShaderRegister, .dx_register_space = sampler.RegisterSpace, .count = 1});
         reshade::api::sampler_desc& desc = parameter.static_samplers.emplace_back();
         desc.filter = reshade::api::filter_mode(sampler.Filter);
         desc.address_u = reshade::api::texture_address_mode(sampler.AddressU);
         desc.address_v = reshade::api::texture_address_mode(sampler.AddressV);
         desc.address_w = reshade::api::texture_address_mode(sampler.AddressW);
         desc.mip_lod_bias = sampler.MipLODBias;
         desc.max_anisotropy = float(sampler.MaxAnisotropy);
         desc.compare_op = reshade::api::compare_op(uint32_t(sampler.ComparisonFunc) - 1); // ReShade's are zero based
         desc.min_lod = sampler.MinLOD;
         desc.max_lod = sampler.MaxLOD;
      }
      const std::unique_lock lock(layouts->mutex);
      if (const auto found = layouts->parameters_by_layout.find(layout.handle); found != layouts->parameters_by_layout.end())
      {
         found->second.push_back(std::move(parameter));
      }
   }
#endif

   void RemovePipelineLayout(PipelineLayouts* layouts, reshade::api::pipeline_layout layout)
   {
      const std::unique_lock lock(layouts->mutex);
      layouts->parameters_by_layout.erase(layout.handle);
   }

#if DEVELOPMENT || ENABLE_D3D12_PASSES
   // What a command list bound to a root parameter (to restore it after a Luma pass, and for the frame capture)
   struct RootBinding
   {
      reshade::api::descriptor_table table = {};                                                      // Descriptor table (a GPU descriptor handle)
      reshade::api::descriptor_type descriptor_type = reshade::api::descriptor_type::constant_buffer; // Root descriptor
      uint64_t descriptor = 0;                                                                        // Root descriptor: the buffer of a CBV, the GPU virtual address of an SRV/UAV (that's all ReShade passes for them)
      uint64_t descriptor_offset = 0;                                                                 // Root CBV offset in its buffer
      std::vector<uint32_t> constants;                                                                // Root constants
   };
#endif

   // Graphics and compute have their own root signature and root arguments
   struct RootState
   {
      reshade::api::pipeline_layout layout = {};
#if DEVELOPMENT || ENABLE_D3D12_PASSES
      std::vector<RootBinding> bindings; // By root parameter index
#endif
   };

   // What a view descriptor held when it was written or bound. D3D12 copies descriptors by value and reads CPU descriptors (render
   // targets) when they are bound, while ReShade's view handles are descriptor addresses, which games rewrite right after.
   struct ViewInfo
   {
      uint64_t resource = 0; // Null for a null descriptor
      reshade::api::format format = reshade::api::format::unknown;
      uint32_t first_level = 0;
      // The array layers (W slices of 3D render targets) the view covers, UINT32_MAX for all the remaining ones: readbacks only
      // transition the subresources a view covers, the others can be in any state
      uint32_t first_layer = 0;
      uint32_t layer_count = UINT32_MAX;
   };

   ViewInfo GetViewInfo(reshade::api::device* device, reshade::api::resource_view view, const reshade::api::resource_view_desc& desc)
   {
      if (view.handle == 0)
         return {};
      const bool is_texture = desc.type != reshade::api::resource_view_type::buffer;
      // ReShade leaves the layer count at 0 for the views of textures that aren't layered: they cover their one layer
      return {.resource = device->get_resource_from_view(view).handle, .format = desc.format, .first_level = (is_texture ? desc.texture.first_level : 0), .first_layer = (is_texture ? desc.texture.first_layer : 0), .layer_count = (is_texture ? (std::max)(desc.texture.layers, 1u) : UINT32_MAX)};
   }

   ViewInfo GetViewInfo(reshade::api::device* device, reshade::api::resource_view view)
   {
      return (view.handle != 0 ? GetViewInfo(device, view, device->get_resource_view_desc(view)) : ViewInfo{});
   }

#if DEVELOPMENT || ENABLE_DRAW_DISPATCH_DATA_CACHE
   // A bound index or vertex buffer: ReShade resolves their GPU virtual address to the buffer
   struct BufferBinding
   {
      uint64_t buffer = 0;
      uint64_t offset = 0;
      uint32_t stride = 0; // The index size for index buffers
   };
#endif

   struct CommandListState
   {
      reshade::api::pipeline pipeline = {};
      RootState graphics;
      RootState compute;
#if DEVELOPMENT || ENABLE_D3D12_PASSES
      reshade::api::primitive_topology primitive_topology = reshade::api::primitive_topology::undefined;
      // "OMSetBlendFactor()", as ReShade passes it: RGBA8 (the float factors truncated to 8 bit)
      uint32_t blend_constant = 0;
      bool has_blend_constant = false;
      uint32_t stencil_ref = 0; // "OMSetStencilRef()", which ReShade passes as the front and back references alike
#endif
#if ENABLE_D3D12_PASSES
      // What a Luma pass replaces besides the above (see "RestoreGraphics()")
      std::vector<reshade::api::viewport> viewports;
      std::vector<reshade::api::rect> scissor_rects;
      std::vector<reshade::api::resource_view_desc> render_target_descs; // By slot, next to "render_target_views": what the views are made again of
      reshade::api::resource_view_desc depth_stencil_desc = {};
#endif
#if DEVELOPMENT
      BufferBinding index_buffer;
#endif
#if DEVELOPMENT || ENABLE_DRAW_DISPATCH_DATA_CACHE
      std::vector<BufferBinding> vertex_buffers; // By slot
#endif
#if DEVELOPMENT
      std::vector<reshade::api::resource_view> render_targets;
      reshade::api::resource_view depth_stencil = {};
#endif
      std::vector<ViewInfo> render_target_views;
      ViewInfo depth_stencil_view;
      bool in_render_pass = false; // Copies and barriers are invalid between "BeginRenderPass()" and "EndRenderPass()" (of a native render pass: see "D3D12Clones::BeginRenderPassDetour()")
   };

   // Setting a different root signature invalidates all root arguments, setting the same one keeps them
   void SetRootSignature(RootState* root, reshade::api::pipeline_layout layout)
   {
      if (root->layout != layout)
      {
         root->layout = layout;
#if DEVELOPMENT || ENABLE_D3D12_PASSES
         root->bindings.clear();
#endif
      }
   }

#if DEVELOPMENT || ENABLE_D3D12_PASSES
   RootBinding* GetRootBinding(RootState* root, reshade::api::pipeline_layout layout, uint32_t param)
   {
      SetRootSignature(root, layout);
      if (root->bindings.size() <= param)
      {
         root->bindings.resize(param + 1);
      }
      return &root->bindings[param];
   }
#endif

   // ReShade passes "all_graphics" for the graphics root signature and arguments, "all_compute" (with ray tracing) for the compute ones
   template <typename Function>
   void ForEachRootState(CommandListState* state, reshade::api::shader_stage stages, Function&& function)
   {
      if ((stages & reshade::api::shader_stage::all_graphics) != 0)
      {
         function(&state->graphics);
      }
      if ((stages & reshade::api::shader_stage::all_compute) != 0)
      {
         function(&state->compute);
      }
   }

   // "Set*RootSignature()" arrives here with no tables, which is all release builds without Luma passes track
   void BindDescriptorTables(CommandListState* state, reshade::api::shader_stage stages, reshade::api::pipeline_layout layout, uint32_t first, uint32_t count, const reshade::api::descriptor_table* tables)
   {
      ForEachRootState(state, stages, [&](RootState* root)
         {
            SetRootSignature(root, layout);
#if DEVELOPMENT || ENABLE_D3D12_PASSES
            for (uint32_t i = 0; i < count; i++)
            {
               GetRootBinding(root, layout, first + i)->table = tables[i];
            }
#endif
         });
   }

#if DEVELOPMENT || ENABLE_D3D12_PASSES
   void PushDescriptors(CommandListState* state, reshade::api::shader_stage stages, reshade::api::pipeline_layout layout, uint32_t param, const reshade::api::descriptor_table_update& update)
   {
      if (update.count == 0)
         return;
      ForEachRootState(state, stages, [&](RootState* root)
         {
            RootBinding* binding = GetRootBinding(root, layout, param);
            binding->descriptor_type = update.type;
            if (update.type == reshade::api::descriptor_type::constant_buffer)
            {
               const auto& range = *static_cast<const reshade::api::buffer_range*>(update.descriptors);
               binding->descriptor = range.buffer.handle;
               binding->descriptor_offset = range.offset;
            }
            else
            {
               binding->descriptor = *static_cast<const uint64_t*>(update.descriptors);
               binding->descriptor_offset = 0;
            } });
   }

   void PushConstants(CommandListState* state, reshade::api::shader_stage stages, reshade::api::pipeline_layout layout, uint32_t param, uint32_t first, uint32_t count, const void* values)
   {
      ForEachRootState(state, stages, [&](RootState* root)
         {
            RootBinding* binding = GetRootBinding(root, layout, param);
            binding->constants.resize((std::max)(binding->constants.size(), size_t(first) + count));
            std::memcpy(binding->constants.data() + first, values, count * sizeof(uint32_t)); });
   }

   // "IASetPrimitiveTopology()", "OMSetBlendFactor()" and "OMSetStencilRef()" arrive as dynamic states
   void BindPipelineStates(CommandListState* state, uint32_t count, const reshade::api::dynamic_state* states, const uint32_t* values)
   {
      for (uint32_t i = 0; i < count; i++)
      {
         if (states[i] == reshade::api::dynamic_state::primitive_topology)
         {
            state->primitive_topology = static_cast<reshade::api::primitive_topology>(values[i]);
         }
         else if (states[i] == reshade::api::dynamic_state::blend_constant)
         {
            state->blend_constant = values[i];
            state->has_blend_constant = true;
         }
         else if (states[i] == reshade::api::dynamic_state::front_stencil_reference_value)
         {
            state->stencil_ref = values[i];
         }
      }
   }
#endif

#if DEVELOPMENT || ENABLE_DRAW_DISPATCH_DATA_CACHE
   void BindVertexBuffers(CommandListState* state, uint32_t first, uint32_t count, const reshade::api::resource* buffers, const uint64_t* offsets, const uint32_t* strides)
   {
      if (state->vertex_buffers.size() < size_t(first) + count)
      {
         state->vertex_buffers.resize(size_t(first) + count);
      }
      for (uint32_t i = 0; i < count; i++)
      {
         state->vertex_buffers[first + i] = {.buffer = buffers[i].handle, .offset = offsets[i], .stride = strides[i]};
      }
   }
#endif

   void BindRenderTargets(CommandListState* state, reshade::api::device* device, uint32_t count, const reshade::api::resource_view* rtvs, reshade::api::resource_view dsv)
   {
#if DEVELOPMENT
      state->render_targets.assign(rtvs, rtvs + count);
      state->depth_stencil = dsv;
#endif
      state->render_target_views.resize(count);
#if ENABLE_D3D12_PASSES // Each view's desc is looked up once, for its "ViewInfo" and to make the view again
      state->render_target_descs.resize(count);
      for (uint32_t i = 0; i < count; i++)
      {
         state->render_target_descs[i] = (rtvs[i].handle != 0 ? device->get_resource_view_desc(rtvs[i]) : reshade::api::resource_view_desc{});
         state->render_target_views[i] = GetViewInfo(device, rtvs[i], state->render_target_descs[i]);
      }
      state->depth_stencil_desc = (dsv.handle != 0 ? device->get_resource_view_desc(dsv) : reshade::api::resource_view_desc{});
      state->depth_stencil_view = GetViewInfo(device, dsv, state->depth_stencil_desc);
#else
      for (uint32_t i = 0; i < count; i++)
      {
         state->render_target_views[i] = GetViewInfo(device, rtvs[i]);
      }
      state->depth_stencil_view = GetViewInfo(device, dsv);
#endif
   }

   void BeginRenderPass(CommandListState* state, reshade::api::device* device, uint32_t count, const reshade::api::render_pass_render_target_desc* rts, const reshade::api::render_pass_depth_stencil_desc* ds)
   {
      const uint32_t rtv_count = (std::min)(count, uint32_t(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT));
      reshade::api::resource_view rtvs[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
      for (uint32_t i = 0; i < rtv_count; i++)
      {
         rtvs[i] = rts[i].view;
      }
      BindRenderTargets(state, device, rtv_count, rtvs, ds ? ds->view : reshade::api::resource_view{});
      state->in_render_pass = true;
   }

#if ENABLE_D3D12_PASSES
   void BindViewports(CommandListState* state, uint32_t first, uint32_t count, const reshade::api::viewport* viewports)
   {
      state->viewports.resize((std::max)(state->viewports.size(), size_t(first) + count));
      std::copy_n(viewports, count, state->viewports.begin() + first);
   }

   void BindScissorRects(CommandListState* state, uint32_t first, uint32_t count, const reshade::api::rect* rects)
   {
      state->scissor_rects.resize((std::max)(state->scissor_rects.size(), size_t(first) + count));
      std::copy_n(rects, count, state->scissor_rects.begin() + first);
   }

   // The descriptor heaps a game set on a command list, which the hook of its "SetDescriptorHeaps()" (see "d3d12_clones.h") keeps as private data of the native list,
   // with the proxy list and the hooked function to set them again through: ReShade only gives a game's heaps back after its own when the game set both types or
   // bound tables from them. Dropped when the list is reset.
   struct GameDescriptorHeaps
   {
      void(STDMETHODCALLTYPE* set_heaps)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*) = nullptr;
      ID3D12GraphicsCommandList* proxy_list = nullptr;
      UINT count = 0;
      ID3D12DescriptorHeap* heaps[2] = {};
   };
   constexpr GUID GAME_DESCRIPTOR_HEAPS_GUID = {0x0c6e3f51, 0x8a2d, 0x4b7e, {0xa1, 0x94, 0x3d, 0x52, 0xe8, 0x07, 0x6b, 0xc2}};

   // Binds "root" again on the graphics or the compute bind point after a Luma pass: the game's descriptor heaps ("GameDescriptorHeaps", else what ReShade's
   // "bind_descriptor_tables()" gives back: the heaps the game had, or the heaps of the tables), the root signature, then the root arguments. Root CBVs and
   // root SRVs/UAVs go natively: ReShade passes the latter as GPU virtual addresses, which its own API can't take back. Only the heaps without a root signature.
   // ponytail: without the hook (no MinHook), a game that set its descriptor heaps and bound no table of a heap type yet gets ReShade's heap of that type back
   void RestoreRoot(reshade::api::command_list* cmd_list, const RootState& root, bool compute)
   {
      auto* const native_list = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
      GameDescriptorHeaps game_heaps;
      UINT game_heaps_size = sizeof(game_heaps);
      if (SUCCEEDED(native_list->GetPrivateData(GAME_DESCRIPTOR_HEAPS_GUID, &game_heaps_size, &game_heaps)) && game_heaps_size == sizeof(game_heaps))
      {
         game_heaps.set_heaps(game_heaps.proxy_list, game_heaps.count, game_heaps.heaps);
      }
      if (root.layout.handle == 0)
      {
         return;
      }
      const reshade::api::shader_stage stages = (compute ? reshade::api::shader_stage::all_compute : reshade::api::shader_stage::all_graphics);
      cmd_list->bind_descriptor_tables(stages, root.layout, 0, 0, nullptr);
      for (uint32_t param = 0; param < root.bindings.size(); param++)
      {
         const RootBinding& binding = root.bindings[param];
         if (binding.table.handle != 0)
         {
            cmd_list->bind_descriptor_tables(stages, root.layout, param, 1, &binding.table);
         }
         else if (binding.descriptor != 0)
         {
            switch (binding.descriptor_type)
            {
            case reshade::api::descriptor_type::constant_buffer:
            {
               const D3D12_GPU_VIRTUAL_ADDRESS address = reinterpret_cast<ID3D12Resource*>(binding.descriptor)->GetGPUVirtualAddress() + binding.descriptor_offset;
               if (compute)
               {
                  native_list->SetComputeRootConstantBufferView(param, address);
               }
               else
               {
                  native_list->SetGraphicsRootConstantBufferView(param, address);
               }
               break;
            }
            case reshade::api::descriptor_type::buffer_unordered_access_view:
               if (compute)
               {
                  native_list->SetComputeRootUnorderedAccessView(param, binding.descriptor);
               }
               else
               {
                  native_list->SetGraphicsRootUnorderedAccessView(param, binding.descriptor);
               }
               break;
            default:
               if (compute)
               {
                  native_list->SetComputeRootShaderResourceView(param, binding.descriptor);
               }
               else
               {
                  native_list->SetGraphicsRootShaderResourceView(param, binding.descriptor);
               }
               break;
            }
         }
         else if (!binding.constants.empty())
         {
            if (compute)
            {
               native_list->SetComputeRoot32BitConstants(param, UINT(binding.constants.size()), binding.constants.data(), 0);
            }
            else
            {
               native_list->SetGraphicsRoot32BitConstants(param, UINT(binding.constants.size()), binding.constants.data(), 0);
            }
         }
      }
   }

   // Binds again, after a Luma pass recorded on the game's command list through ReShade's API, the graphics state it replaced: the root
   // signature and its arguments ("RestoreRoot()"), "pipeline" (what Luma really had bound: a replacement or a render target format variant),
   // the render targets and depth stencil (their views are made again: a game rewrites a CPU descriptor right after binding it), the viewports,
   // the scissors, the topology and the blend factor. The pass must not touch anything else (index and vertex buffers, stencil reference).
   // ponytail: the blend factor comes back as ReShade passes it, 8 bit, so a game's factor that isn't a multiple of 1/255 comes back a little lower
   void RestoreGraphics(reshade::api::command_list* cmd_list, const CommandListState& state, reshade::api::pipeline pipeline)
   {
      reshade::api::device* const device = cmd_list->get_device();
      auto* const native_list = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
      RestoreRoot(cmd_list, state.graphics, false);
      if (state.has_blend_constant)
      {
         const float blend_factor[4] = {float(state.blend_constant & 0xFF) / 255.f, float((state.blend_constant >> 8) & 0xFF) / 255.f, float((state.blend_constant >> 16) & 0xFF) / 255.f, float((state.blend_constant >> 24) & 0xFF) / 255.f};
         native_list->OMSetBlendFactor(blend_factor);
      }
      // "all", not "all_graphics": the latter replays the topology and blend factor ReShade stores for the pipelines it creates (clones)
      if (pipeline.handle != 0)
      {
         cmd_list->bind_pipeline(reshade::api::pipeline_stage::all, pipeline);
      }

      const uint32_t render_target_count = uint32_t((std::min)(state.render_target_views.size(), size_t(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)));
      reshade::api::resource_view render_targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
      for (uint32_t i = 0; i < render_target_count; i++)
      {
         // A null resource makes a null view, as the game's was
         device->create_resource_view({state.render_target_views[i].resource}, reshade::api::resource_usage::render_target, state.render_target_descs[i], &render_targets[i]);
      }
      reshade::api::resource_view depth_stencil = {};
      if (state.depth_stencil_view.resource != 0)
      {
         device->create_resource_view({state.depth_stencil_view.resource}, reshade::api::resource_usage::depth_stencil, state.depth_stencil_desc, &depth_stencil);
      }
      // D3D12 reads the descriptors when they are bound, so the views can go right after
      cmd_list->bind_render_targets_and_depth_stencil(render_target_count, render_targets, depth_stencil);
      for (const reshade::api::resource_view view : render_targets)
      {
         if (view.handle != 0)
         {
            device->destroy_resource_view(view);
         }
      }
      if (depth_stencil.handle != 0)
      {
         device->destroy_resource_view(depth_stencil);
      }

      if (!state.viewports.empty())
      {
         cmd_list->bind_viewports(0, uint32_t(state.viewports.size()), state.viewports.data());
      }
      if (!state.scissor_rects.empty())
      {
         cmd_list->bind_scissor_rects(0, uint32_t(state.scissor_rects.size()), state.scissor_rects.data());
      }
      if (state.primitive_topology != reshade::api::primitive_topology::undefined)
      {
         const reshade::api::dynamic_state topology_state = reshade::api::dynamic_state::primitive_topology;
         const uint32_t topology = uint32_t(state.primitive_topology);
         cmd_list->bind_pipeline_states(1, &topology_state, &topology);
      }
   }

   // The compute counterpart of "RestoreGraphics()": the compute root signature and its arguments, and "pipeline" (one pipeline is bound
   // for both). The game's descriptor heaps come back through the graphics root signature if it has no compute one.
   void RestoreCompute(reshade::api::command_list* cmd_list, const CommandListState& state, reshade::api::pipeline pipeline)
   {
      if (state.compute.layout.handle != 0)
      {
         RestoreRoot(cmd_list, state.compute, true);
      }
      else
      {
         RestoreRoot(cmd_list, state.graphics, false);
      }
      if (pipeline.handle != 0)
      {
         cmd_list->bind_pipeline(reshade::api::pipeline_stage::all_compute, pipeline);
      }
   }
#endif

   // What a game wrote into a descriptor heap slot
   struct Descriptor
   {
      reshade::api::descriptor_type type = reshade::api::descriptor_type::sampler;
      uint64_t handle = 0; // The view or sampler, or the buffer of a constant buffer view
      uint64_t offset = 0; // Constant buffer view range in its buffer
      uint64_t size = 0;
      ViewInfo view; // Views only, resolved when written as the copies of a descriptor outlive what its address holds next
   };

   // By heap (CPU only and shader visible ones), indexed by descriptor. Lock free like ReShade's "examples/utils/descriptor_tracking": games write
   // descriptors on all their recording threads (a "CopyDescriptorsSimple()" per draw), and one mutex made them wait on each other. The maps are
   // insert only and growing a "concurrent_vector" never moves its elements, so a found descriptor stays valid.
   // ponytail: a slot read while another thread writes it can be torn (D3D12 only forbids conflicting writes); the readers are diagnostics
   struct DescriptorHeaps
   {
      concurrency::concurrent_unordered_map<uint64_t, concurrency::concurrent_vector<Descriptor>> descriptors_by_heap;
      // The descs of the samplers a game created, by the CPU descriptor it created them in ("init_sampler"): the handle its descriptors and their copies keep.
      // ponytail: a game that creates another sampler in the same slot changes what its older copies read
      concurrency::concurrent_unordered_map<uint64_t, reshade::api::sampler_desc> sampler_descs;
   };

   // "get_descriptor_heap_offset()" maps both CPU and GPU descriptor handles
   concurrency::concurrent_vector<Descriptor>* GetHeapDescriptors(DescriptorHeaps* heaps, reshade::api::device* device, reshade::api::descriptor_table table, uint32_t binding, uint32_t count, uint32_t* offset)
   {
      reshade::api::descriptor_heap heap = {};
      device->get_descriptor_heap_offset(table, binding, 0, &heap, offset);
      if (heap.handle == 0)
         return nullptr;
      concurrency::concurrent_vector<Descriptor>& descriptors = heaps->descriptors_by_heap[heap.handle];
      descriptors.grow_to_at_least(size_t(*offset) + count);
      return &descriptors;
   }

   // What a table slot holds, nullptr if the game never wrote it
   const Descriptor* FindDescriptor(const DescriptorHeaps& heaps, reshade::api::device* device, reshade::api::descriptor_table table, uint32_t binding)
   {
      reshade::api::descriptor_heap heap = {};
      uint32_t offset = 0;
      device->get_descriptor_heap_offset(table, binding, 0, &heap, &offset);
      const auto it = heaps.descriptors_by_heap.find(heap.handle);
      if (it == heaps.descriptors_by_heap.end() || offset >= it->second.size())
         return nullptr;
      return &it->second[offset];
   }

   void UpdateDescriptors(DescriptorHeaps* heaps, reshade::api::device* device, uint32_t count, const reshade::api::descriptor_table_update* updates)
   {
      for (uint32_t i = 0; i < count; i++)
      {
         const reshade::api::descriptor_table_update& update = updates[i];
         uint32_t offset = 0;
         concurrency::concurrent_vector<Descriptor>* descriptors = GetHeapDescriptors(heaps, device, update.table, update.binding + update.array_offset, update.count, &offset);
         if (!descriptors)
            continue;
         for (uint32_t j = 0; j < update.count; j++)
         {
            Descriptor& descriptor = (*descriptors)[offset + j];
            descriptor = {.type = update.type};
            switch (update.type)
            {
            case reshade::api::descriptor_type::constant_buffer:
            case reshade::api::descriptor_type::shader_storage_buffer:
            {
               const auto& range = static_cast<const reshade::api::buffer_range*>(update.descriptors)[j];
               descriptor.handle = range.buffer.handle;
               descriptor.offset = range.offset;
               descriptor.size = range.size;
               break;
            }
            case reshade::api::descriptor_type::sampler_with_resource_view:
               descriptor.handle = static_cast<const reshade::api::sampler_with_resource_view*>(update.descriptors)[j].view.handle;
               descriptor.view = GetViewInfo(device, {descriptor.handle});
               break;
            case reshade::api::descriptor_type::sampler:
               descriptor.handle = static_cast<const uint64_t*>(update.descriptors)[j];
               break;
            default: // Views are a single handle
               descriptor.handle = static_cast<const uint64_t*>(update.descriptors)[j];
               descriptor.view = GetViewInfo(device, {descriptor.handle});
               break;
            }
         }
      }
   }

   void CopyDescriptors(DescriptorHeaps* heaps, reshade::api::device* device, uint32_t count, const reshade::api::descriptor_table_copy* copies)
   {
      for (uint32_t i = 0; i < count; i++)
      {
         const reshade::api::descriptor_table_copy& copy = copies[i];
         uint32_t source_offset = 0;
         uint32_t dest_offset = 0;
         concurrency::concurrent_vector<Descriptor>* source = GetHeapDescriptors(heaps, device, copy.source_table, copy.source_binding + copy.source_array_offset, copy.count, &source_offset);
         concurrency::concurrent_vector<Descriptor>* dest = GetHeapDescriptors(heaps, device, copy.dest_table, copy.dest_binding + copy.dest_array_offset, copy.count, &dest_offset);
         if (!source || !dest)
            continue;
         // Ranges within a heap can overlap: then a destination after the source is copied from its end
         const bool backward = (source == dest && dest_offset > source_offset);
         for (uint32_t j = 0; j < copy.count; j++)
         {
            const uint32_t k = backward ? (copy.count - 1 - j) : j;
            (*dest)[dest_offset + k] = (*source)[source_offset + k];
         }
      }
   }

   // Buffers by GPU virtual address: ReShade passes root SRVs and UAVs as one
   struct BufferAddress
   {
      uint64_t resource = 0;
      uint64_t size = 0;
   };

   struct BufferAddresses
   {
      std::shared_mutex mutex;
      std::map<uint64_t, BufferAddress> buffers_by_address;
   };

   void AddBuffer(BufferAddresses* addresses, reshade::api::resource buffer, uint64_t size)
   {
      const D3D12_GPU_VIRTUAL_ADDRESS address = reinterpret_cast<ID3D12Resource*>(buffer.handle)->GetGPUVirtualAddress();
      if (address == 0)
         return;
      const std::unique_lock lock(addresses->mutex);
      addresses->buffers_by_address[address] = {.resource = buffer.handle, .size = size};
   }

   // Textures have no address. Placed buffers aliasing the same memory share one: only the last created is kept.
   void RemoveBuffer(BufferAddresses* addresses, reshade::api::resource resource)
   {
      const D3D12_GPU_VIRTUAL_ADDRESS address = reinterpret_cast<ID3D12Resource*>(resource.handle)->GetGPUVirtualAddress();
      if (address == 0)
         return;
      const std::unique_lock lock(addresses->mutex);
      if (const auto it = addresses->buffers_by_address.find(address); it != addresses->buffers_by_address.end() && it->second.resource == resource.handle)
      {
         addresses->buffers_by_address.erase(it);
      }
   }

   // The live resources the game created (and their clones). A descriptor heap slot keeps the resource of the view last written to it after the game destroyed it,
   // and tables span slots their shaders never read: a resource is only queried if it's in here.
   // ponytail: the resources Luma creates aren't in here, a game descriptor viewing one shows as unbound
   struct LiveResources
   {
      std::shared_mutex mutex;
      std::unordered_set<uint64_t> resources;
   };

   // The buffer holding "address", none if unknown. Expects "addresses.mutex" to be locked (shared is enough).
   BufferAddress FindBuffer(const BufferAddresses& addresses, uint64_t address)
   {
      auto it = addresses.buffers_by_address.upper_bound(address);
      if (it == addresses.buffers_by_address.begin())
         return {};
      --it;
      return (address - it->first < it->second.size) ? it->second : BufferAddress{};
   }
} // namespace D3D12State
