#pragma once

// The D3D12 frame capture entries, the counterpart of "AddTraceDrawCallData()" in "draw.hpp". D3D12 has no state queries, so the
// bindings come from the state Core tracks ("d3d12_state.h"), filtered by the register declarations of the pipeline's shaders.

#include <charconv>
#include <regex>
#include <sstream>
#include <string_view>

// "SetName()" debug names
std::optional<std::string> GetD3DNameW(ID3D12Object* obj)
{
   if (obj == nullptr)
      return std::nullopt;

   wchar_t name[128] = {};
   UINT size = sizeof(name);
   if (obj->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name) != S_OK || size == 0)
      return std::nullopt;
   char c_name[128] = {};
   size_t out_size = 0;
   if (wcstombs_s(&out_size, c_name, sizeof(c_name), name, _TRUNCATE) != 0 || out_size <= 1)
      return std::nullopt;
   return std::string(c_name, out_size - 1); // Without the null terminator
}

// The hash the capture and the MCP identify a resource by, from its pointer like the D3D11 capture
std::string ResourceHashString(uint64_t handle)
{
   return std::to_string(std::hash<void*>{}(reinterpret_cast<void*>(handle)));
}

// Like the D3D11 "GetResourceInfo()": "size" is width, height, array size (depth for 3D) and mips (samples for MS textures), and
// the buffer size in "x" for buffers
void GetResourceInfo(reshade::api::device* device, reshade::api::resource resource, uint4& size, DXGI_FORMAT& format, std::string* type_name = nullptr, std::string* hash = nullptr, std::string* debug_name = nullptr, bool* render_target_flag = nullptr, bool* unordered_access_flag = nullptr)
{
   size = {};
   format = DXGI_FORMAT_UNKNOWN;
   if (resource.handle == 0)
      return;

   if (hash)
      *hash = ResourceHashString(resource.handle);
   if (debug_name)
      *debug_name = GetD3DNameW(reinterpret_cast<ID3D12Resource*>(resource.handle)).value_or("");

   const reshade::api::resource_desc desc = device->get_resource_desc(resource);
   if (render_target_flag)
      *render_target_flag = (desc.usage & reshade::api::resource_usage::render_target) != 0;
   if (unordered_access_flag)
      *unordered_access_flag = (desc.usage & reshade::api::resource_usage::unordered_access) != 0;
   if (desc.type == reshade::api::resource_type::buffer)
   {
      size = uint4{uint32_t(desc.buffer.size), 0, 0, 0};
      if (type_name)
         *type_name = "Buffer"; // Like the D3D11 one, the capture list matches this exact name
      return;
   }

   size = uint4{desc.texture.width, desc.texture.height, desc.texture.depth_or_layers, desc.texture.samples > 1 ? desc.texture.samples : desc.texture.levels};
   format = DXGI_FORMAT(desc.texture.format);
   if (!type_name)
      return;
   switch (desc.type)
   {
   case reshade::api::resource_type::texture_1d:
      *type_name = desc.texture.depth_or_layers > 1 ? "Texture 1D Array" : "Texture 1D";
      break;
   case reshade::api::resource_type::texture_3d:
      *type_name = "Texture 3D";
      break;
   default:
      if (desc.texture.samples > 1)
         *type_name = desc.texture.depth_or_layers > 1 ? "Texture 2D MS Array" : "Texture 2D MS";
      else if (desc.texture.depth_or_layers == 6 && (desc.flags & reshade::api::resource_flags::cube_compatible) != 0)
         *type_name = "Texture 2D Cube";
      else
         *type_name = desc.texture.depth_or_layers > 1 ? "Texture 2D Array" : "Texture 2D";
      break;
   }
}

namespace D3D12Trace
{
   // Reads the "Resource Bindings" table that both the FXC and the DXC disassemblies print, e.g. "// source  texture  float4  2d  T0
   // t0,space1  1". FXC prints none for shaders that bind nothing, and neither does it for stripped reflections, so DXBC falls back on its
   // "dcl_*" declarations. DXC prints it from the DXIL module's metadata when the reflection part is stripped, so false means
   // the disassembly failed.
   bool ParseResourceBindings(CachedShader* shader)
   {
      const bool is_dxbc = shader->type_and_version.size() > 3 && shader->type_and_version[3] < '6';
      std::vector<CachedShader::ResourceBinding> declarations; // From "dcl_*" lines
      std::istringstream stream(shader->disasm);
      std::string line;
      bool found_table = false;
      bool found_rows = false; // The rows start after the dashed line under the header
      bool read_table = false;
      while (std::getline(stream, line))
      {
         const size_t first = line.find_first_not_of("/; \t\r");
         const std::string_view text = (first != std::string::npos) ? std::string_view(line).substr(first) : std::string_view();
         // SM 6.6 "ResourceDescriptorHeap[]" and "SamplerDescriptorHeap[]" (listed under "shader requires additional functionality")
         if (text.starts_with("Resource descriptor heap indexing") || text.starts_with("Sampler descriptor heap indexing"))
         {
            shader->uses_descriptor_heap = true;
            continue;
         }
         if (is_dxbc && text.starts_with("dcl_"))
         {
            // SM 5.1 declares ranges ("T0[3:5], space=1", "*" if unbounded), SM 5.0 single registers ("t3", "CB2[16]" for b2)
            static const std::regex range_pattern(R"(\b[a-z]+\d+\[(\d+):(\d+|\*)\].*space=(\d+))", std::regex_constants::icase);
            static const std::regex register_pattern(R"((?:^|[\s,])(?:cb|t|u|s)(\d+)(?=[\[\s,]|$))", std::regex_constants::icase);
            CachedShader::ResourceBinding declaration;
            if (text.starts_with("dcl_constantbuffer"))
               declaration.type = 'b';
            else if (text.starts_with("dcl_resource"))
               declaration.type = 't';
            else if (text.starts_with("dcl_uav"))
               declaration.type = 'u';
            else if (text.starts_with("dcl_sampler"))
               declaration.type = 's';
            else
               continue;
            const std::string declaration_line(text);
            const std::string operands = declaration_line.substr(declaration_line.find(' ') + 1); // Without the "dcl_*" name
            if (std::smatch match; std::regex_search(declaration_line, match, range_pattern))
            {
               declaration.index = uint32_t(std::stoul(match[1].str()));
               declaration.count = (match[2].str() == "*") ? UINT32_MAX : (uint32_t(std::stoul(match[2].str())) - declaration.index + 1);
               declaration.space = uint32_t(std::stoul(match[3].str()));
               declarations.push_back(declaration);
            }
            else if (std::regex_search(operands, match, register_pattern))
            {
               declaration.index = uint32_t(std::stoul(match[1].str()));
               declarations.push_back(declaration);
            }
            continue;
         }
         if (read_table)
            continue;
         if (!found_table)
         {
            found_table = text.starts_with("Resource Bindings:");
            continue;
         }
         if (text.starts_with('-'))
         {
            found_rows = true;
            continue;
         }
         if (!found_rows)
         {
            // Older FXCs print "Space Slot Elements" columns, without the bind column: their "dcl_*" lines are read then
            if (text.starts_with("Name") && text.find("HLSL Bind") == std::string_view::npos)
            {
               found_table = false;
               read_table = true;
            }
            continue;
         }
         if (text.empty())
         {
            read_table = true;
            continue;
         }

         // The last two columns are the "HLSL Bind" ("t0", "cb2,space1") and the "Count" ("1", "unbounded")
         std::vector<std::string_view> columns;
         for (size_t begin = 0; begin < text.size();)
         {
            const size_t end = (std::min)(text.find_first_of(" \t\r", begin), text.size());
            if (end > begin)
               columns.push_back(text.substr(begin, end - begin));
            begin = end + 1;
         }
         if (columns.size() < 3)
            continue;
         std::string_view bind = columns[columns.size() - 2];
         const std::string_view count = columns.back();

         CachedShader::ResourceBinding binding;
         if (bind.starts_with("cb"))
         {
            binding.type = 'b';
            bind.remove_prefix(2);
         }
         else if (bind.starts_with('t') || bind.starts_with('u') || bind.starts_with('s'))
         {
            binding.type = bind.front();
            bind.remove_prefix(1);
         }
         else
         {
            continue;
         }
         const auto [index_end, index_error] = std::from_chars(bind.data(), bind.data() + bind.size(), binding.index);
         if (index_error != std::errc())
            continue;
         if (constexpr std::string_view space_prefix = ",space"; std::string_view(index_end, bind.data() + bind.size()).starts_with(space_prefix))
         {
            std::from_chars(index_end + space_prefix.size(), bind.data() + bind.size(), binding.space);
         }
         if (count == "unbounded")
            binding.count = UINT32_MAX;
         else
            std::from_chars(count.data(), count.data() + count.size(), binding.count);
         shader->bindings.push_back(binding);
      }
      if (found_table)
         return true;
      // A shader that binds nothing still has a disassembly, a failed one would have no declarations either
      if (is_dxbc && shader->disasm.find("dcl_") != std::string::npos)
      {
         shader->bindings = std::move(declarations);
         return true;
      }
      return false;
   }

   // Fills the capture's 't' (SRV), 'u' (UAV), 'r' (render target) or 'd' (depth stencil) entry at "index". Root SRVs and UAVs have no
   // descriptor, only the buffer in "info"; a descriptor without a resource is a null one (or a slot never written), shown as
   // referenced but not bound.
   void FillView(reshade::api::device* device, TraceDrawCallData* entry, char type, uint32_t index, bool is_descriptor, uint64_t view, const D3D12State::ViewInfo& info)
   {
      uint4 ds_size = {};
      uint3 dsv_size = {};
      UINT dsv_mip = 0;
      DXGI_FORMAT* view_format = &entry->dsv_format;
      DXGI_FORMAT* format = &entry->ds_format;
      uint4* size = &ds_size;
      uint3* view_size = &dsv_size;
      UINT* view_mip = &dsv_mip;
      std::string* type_name = nullptr;
      std::string* hash = &entry->ds_hash;
      std::string* debug_name = &entry->ds_debug_name;
      bool* render_target_flag = nullptr;
      bool* unordered_access_flag = nullptr;
      D3D12State::ViewInfo* view_info = &entry->ds_view; // For readbacks, SRVs are read at the pass that wrote them
      switch (type)
      {
      case 't':
         entry->srvs[index] = view;
         view_format = &entry->srv_format[index], format = &entry->sr_format[index], size = &entry->sr_size[index], view_size = &entry->srv_size[index], view_mip = &entry->srv_mip[index];
         type_name = &entry->sr_type_name[index], hash = &entry->sr_hash[index], debug_name = &entry->sr_debug_name[index];
         render_target_flag = &entry->sr_is_rt[index], unordered_access_flag = &entry->sr_is_ua[index];
         view_info = nullptr;
         break;
      case 'u':
         entry->uavs[index] = view;
         view_format = &entry->uav_format[index], format = &entry->ua_format[index], size = &entry->ua_size[index], view_size = &entry->uav_size[index], view_mip = &entry->uav_mip[index];
         type_name = &entry->ua_type_name[index], hash = &entry->ua_hash[index], debug_name = &entry->ua_debug_name[index];
         render_target_flag = &entry->ua_is_rt[index];
         view_info = &entry->ua_view[index];
         break;
      case 'r':
         entry->rtvs[index] = view;
         view_format = &entry->rtv_format[index], format = &entry->rt_format[index], size = &entry->rt_size[index], view_size = &entry->rtv_size[index], view_mip = &entry->rtv_mip[index];
         type_name = &entry->rt_type_name[index], hash = &entry->rt_hash[index], debug_name = &entry->rt_debug_name[index];
         view_info = &entry->rt_view[index];
         break;
      }
      if (view_info)
      {
         *view_info = info;
      }
      if (is_descriptor)
      {
         *view_format = (info.resource != 0) ? DXGI_FORMAT(info.format) : DXGI_FORMAT(-1);
         *view_mip = info.first_level;
      }
      GetResourceInfo(device, {info.resource}, *size, *format, type_name, hash, debug_name, render_target_flag, unordered_access_flag); // D3D12 views are descriptors, they have no names
      const bool is_3d = type_name && *type_name == "Texture 3D";
      *view_size = uint3{(std::max)(size->x >> *view_mip, size->x ? 1u : 0u), (std::max)(size->y >> *view_mip, size->y ? 1u : 0u), is_3d ? (std::max)(size->z >> *view_mip, 1u) : size->z};
      if (type == 'd')
         entry->ds_size = uint2{ds_size.x, ds_size.y};
   }

   // The render targets and the depth stencil a command list has bound, for the draw entries and the render pass ones
   void FillBoundTargets(reshade::api::device* device, const DeviceData* device_data, TraceDrawCallData* entry, const D3D12State::CommandListState& state)
   {
      for (size_t i = 0; i < state.render_targets.size() && i < TraceDrawCallData::rtvs_size; i++)
      {
         if (state.render_targets[i].handle == 0)
            continue;
         FillView(device, entry, 'r', uint32_t(i), true, state.render_targets[i].handle, state.render_target_views[i]);
         entry->rt_is_swapchain[i] = device_data->back_buffers.contains(state.render_target_views[i].resource);
      }
      if (state.depth_stencil.handle != 0)
      {
         FillView(device, entry, 'd', 0, true, state.depth_stencil.handle, state.depth_stencil_view);
      }
   }

   // The fixed function state of a graphics pipeline for the capture, in the fields of the D3D11 one so its writers serve both (the D3D11 and D3D12 enums are numbered alike,
   // ReShade's are zero based). ReShade has these subobjects for every graphics pipeline, the D3D12 defaults stand in for the ones a pipeline hasn't ("state_bound" tells for
   // the depth stencil and rasterizer ones). D3D12 has no stencil masks per face on the D3D11 desc, the front face's are kept. Returns the depth stencil state as ReShade has it.
   // ponytail: the alpha blend factor constants (D3D12 only) read as the shared blend factor, a "front_and_back" cull mode as back
   reshade::api::depth_stencil_desc FillRenderState(TraceDrawCallData* entry, const Shader::CachedPipeline* pipeline)
   {
      reshade::api::blend_desc blend = {};
      reshade::api::rasterizer_desc rasterizer = {};
      reshade::api::depth_stencil_desc depth_stencil = {};
      for (uint32_t i = 0; pipeline && pipeline->subobjects_cache && i < pipeline->subobject_count; i++)
      {
         const reshade::api::pipeline_subobject& subobject = pipeline->subobjects_cache[i];
         if (!subobject.data)
            continue;
         switch (subobject.type)
         {
         case reshade::api::pipeline_subobject_type::blend_state:
            blend = *static_cast<const reshade::api::blend_desc*>(subobject.data);
            break;
         case reshade::api::pipeline_subobject_type::rasterizer_state:
            rasterizer = *static_cast<const reshade::api::rasterizer_desc*>(subobject.data);
            entry->has_rasterizer_state = true;
            break;
         case reshade::api::pipeline_subobject_type::depth_stencil_state:
            depth_stencil = *static_cast<const reshade::api::depth_stencil_desc*>(subobject.data);
            entry->has_depth_stencil_state = true;
            break;
         default:
            break;
         }
      }

      static constexpr D3D12_BLEND blend_factors[] = {D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
         D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_ALPHA_FACTOR, D3D12_BLEND_INV_ALPHA_FACTOR, D3D12_BLEND_SRC_ALPHA_SAT, D3D12_BLEND_SRC1_COLOR, D3D12_BLEND_INV_SRC1_COLOR, D3D12_BLEND_SRC1_ALPHA, D3D12_BLEND_INV_SRC1_ALPHA};
      const auto to_blend = [](reshade::api::blend_factor factor)
      { return blend_factors[(std::min)(size_t(factor), std::size(blend_factors) - 1)]; };
      entry->blend_desc.AlphaToCoverageEnable = blend.alpha_to_coverage_enable;
      entry->blend_desc.IndependentBlendEnable = TRUE;
      for (uint32_t i = 0; i < TraceDrawCallData::rtvs_size; i++)
      {
         entry->blend_desc.RenderTarget[i] = {
            .BlendEnable = blend.blend_enable[i],
            .LogicOpEnable = blend.logic_op_enable[i],
            .SrcBlend = to_blend(blend.source_color_blend_factor[i]),
            .DestBlend = to_blend(blend.dest_color_blend_factor[i]),
            .BlendOp = D3D12_BLEND_OP(uint32_t(blend.color_blend_op[i]) + 1),
            .SrcBlendAlpha = to_blend(blend.source_alpha_blend_factor[i]),
            .DestBlendAlpha = to_blend(blend.dest_alpha_blend_factor[i]),
            .BlendOpAlpha = D3D12_BLEND_OP(uint32_t(blend.alpha_blend_op[i]) + 1),
            .LogicOp = D3D12_LOGIC_OP(blend.logic_op[i]),
            .RenderTargetWriteMask = blend.render_target_write_mask[i],
         };
      }

      const auto to_stencil_face = [](reshade::api::stencil_op fail_op, reshade::api::stencil_op depth_fail_op, reshade::api::stencil_op pass_op, reshade::api::compare_op func) -> D3D12_DEPTH_STENCILOP_DESC
      {
         return {.StencilFailOp = D3D12_STENCIL_OP(uint32_t(fail_op) + 1), .StencilDepthFailOp = D3D12_STENCIL_OP(uint32_t(depth_fail_op) + 1), .StencilPassOp = D3D12_STENCIL_OP(uint32_t(pass_op) + 1), .StencilFunc = D3D12_COMPARISON_FUNC(uint32_t(func) + 1)};
      };
      entry->depth_stencil_desc = {
         .DepthEnable = depth_stencil.depth_enable,
         .DepthWriteMask = (depth_stencil.depth_write_mask ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO),
         .DepthFunc = D3D12_COMPARISON_FUNC(uint32_t(depth_stencil.depth_func) + 1),
         .StencilEnable = depth_stencil.stencil_enable,
         .StencilReadMask = depth_stencil.front_stencil_read_mask,
         .StencilWriteMask = depth_stencil.front_stencil_write_mask,
         .FrontFace = to_stencil_face(depth_stencil.front_stencil_fail_op, depth_stencil.front_stencil_depth_fail_op, depth_stencil.front_stencil_pass_op, depth_stencil.front_stencil_func),
         .BackFace = to_stencil_face(depth_stencil.back_stencil_fail_op, depth_stencil.back_stencil_depth_fail_op, depth_stencil.back_stencil_pass_op, depth_stencil.back_stencil_func),
      };

      entry->rasterizer_desc = {
         .FillMode = (rasterizer.fill_mode == reshade::api::fill_mode::wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID),
         .CullMode = D3D12_CULL_MODE((std::min)(uint32_t(rasterizer.cull_mode), 2u) + 1),
         .FrontCounterClockwise = rasterizer.front_counter_clockwise,
         .DepthBias = INT(rasterizer.depth_bias),
         .DepthBiasClamp = rasterizer.depth_bias_clamp,
         .SlopeScaledDepthBias = rasterizer.slope_scaled_depth_bias,
         .DepthClipEnable = rasterizer.depth_clip_enable,
         .MultisampleEnable = rasterizer.multisample_enable,
         .AntialiasedLineEnable = rasterizer.antialiased_line_enable,
      };
      entry->scissor_enable = rasterizer.scissor_enable;
      return depth_stencil;
   }

   // A packed sampler entry. A null "desc" (a descriptor that was never written, or whose sampler wasn't seen created) leaves it unbound.
   void FillSampler(TraceDrawCallData* entry, uint32_t index, const reshade::api::sampler_desc* desc)
   {
      if (!desc)
         return;
      entry->samplers_filter[index] = D3D12_FILTER(desc->filter);
      entry->samplers_address_u[index] = D3D12_TEXTURE_ADDRESS_MODE(desc->address_u);
      entry->samplers_address_v[index] = D3D12_TEXTURE_ADDRESS_MODE(desc->address_v);
      entry->samplers_address_w[index] = D3D12_TEXTURE_ADDRESS_MODE(desc->address_w);
      entry->samplers_mip_lod_bias[index] = desc->mip_lod_bias;
      entry->samplers_comparison_func[index] = D3D12_COMPARISON_FUNC(uint32_t(desc->compare_op) + 1);
   }

   // Reads the table descriptors of a command list's entries ("TraceDrawCallData::table_descriptors"), at its execution
   void ResolveTableDescriptors(std::vector<TraceDrawCallData>* entries, DeviceData* device_data)
   {
      reshade::api::device* device = device_data->reshade_device;
      const std::shared_lock lock_live(device_data->live_resources.mutex);
      for (TraceDrawCallData& entry : *entries)
      {
         for (const TraceDrawCallData::TableDescriptor& table_descriptor : entry.table_descriptors)
         {
            const D3D12State::Descriptor* descriptor = D3D12State::FindDescriptor(device_data->descriptor_heaps, device, table_descriptor.table, table_descriptor.binding);
            if (table_descriptor.type == 's')
            {
               const auto sampler = device_data->descriptor_heaps.sampler_descs.find(descriptor ? descriptor->handle : 0);
               FillSampler(&entry, table_descriptor.index, (sampler != device_data->descriptor_heaps.sampler_descs.end()) ? &sampler->second : nullptr);
            }
            else if (table_descriptor.type != 'b')
            {
               // A view of a destroyed resource is a stale slot, shown as unbound
               const bool is_live = descriptor && (device_data->live_resources.resources.contains(descriptor->view.resource) || device_data->back_buffers.contains(descriptor->view.resource));
               FillView(device, &entry, table_descriptor.type, table_descriptor.index, true, is_live ? descriptor->handle : 0, is_live ? descriptor->view : D3D12State::ViewInfo{});
            }
            else if (descriptor && descriptor->handle != 0)
            {
               entry.cb_resource[table_descriptor.index] = descriptor->handle;
               entry.cb_hash[table_descriptor.index] = ResourceHashString(descriptor->handle);
               entry.cb_first_constant[table_descriptor.index] = UINT(descriptor->offset / 16);
               entry.cb_num_constants[table_descriptor.index] = UINT((descriptor->size + 15) / 16);
            }
         }
         entry.table_descriptors.clear();
      }
   }

   // A draw or dispatch's "D3D12Readback::DrawSignature", from its arguments and the resources of its first render target and depth stencil
   // (as a frame capture entry or the command list's state holds them), so readbacks find it again on any thread
   D3D12Readback::DrawSignature MakeDrawSignature(const DeviceData& device_data, const DrawDispatchData& draw_dispatch_data, bool compute, uint64_t rtv_resource, uint64_t dsv_resource)
   {
      return {
         .rtv_resource = (compute || device_data.back_buffers.contains(rtv_resource)) ? 0 : rtv_resource,
         .dsv_resource = compute ? 0 : dsv_resource,
         .compute = compute,
         .arguments = {draw_dispatch_data.vertex_count, draw_dispatch_data.index_count, draw_dispatch_data.instance_count, draw_dispatch_data.first_vertex, draw_dispatch_data.first_index, draw_dispatch_data.dispatch_count.x, draw_dispatch_data.dispatch_count.y, draw_dispatch_data.dispatch_count.z, draw_dispatch_data.indirect},
      };
   }

   // What a draw takes from the command list besides its pipeline and root arguments: the stencil reference, blend constants, viewports, scissors and targets, and what the
   // pipeline's depth stencil state "desc" does with the bound depth stencil. A bundle's draws take them from the list that executes the bundle.
   void FillOutputState(reshade::api::device* device, const DeviceData* device_data, TraceDrawCallData* entry, const D3D12State::CommandListState& state, const reshade::api::depth_stencil_desc& desc)
   {
      entry->stencil_ref = state.stencil_ref;
      if (state.has_blend_constant)
      {
         for (uint32_t i = 0; i < 4; i++)
         {
            entry->blend_factor[i] = float((state.blend_constant >> (8 * i)) & 0xFF) / 255.f;
         }
      }
#if ENABLE_D3D12_PASSES // The viewports and scissors are only tracked with the passes' state
      entry->viewport_count = UINT((std::min)(state.viewports.size(), std::size(entry->viewports)));
      for (UINT i = 0; i < entry->viewport_count; i++)
      {
         const reshade::api::viewport& viewport = state.viewports[i];
         entry->viewports[i] = {.TopLeftX = viewport.x, .TopLeftY = viewport.y, .Width = viewport.width, .Height = viewport.height, .MinDepth = viewport.min_depth, .MaxDepth = viewport.max_depth};
      }
      entry->scissor_count = UINT((std::min)(state.scissor_rects.size(), std::size(entry->scissor_rects)));
      for (UINT i = 0; i < entry->scissor_count; i++)
      {
         const reshade::api::rect& rect = state.scissor_rects[i];
         entry->scissor_rects[i] = {.left = rect.left, .top = rect.top, .right = rect.right, .bottom = rect.bottom};
      }
#endif

      FillBoundTargets(device, device_data, entry, state);
      if (state.depth_stencil.handle != 0)
      {
         using DepthStateType = TraceDrawCallData::DepthStateType;
         // Same classification as the D3D11 capture
         if (desc.depth_enable)
         {
            const bool test = desc.depth_func != reshade::api::compare_op::always;
            if (desc.depth_write_mask)
               entry->depth_state = test ? DepthStateType::TestAndWrite : DepthStateType::WriteOnly;
            else
               entry->depth_state = test ? DepthStateType::TestOnly : DepthStateType::Disabled;
         }
         const bool has_stencil = entry->dsv_format == DXGI_FORMAT_D24_UNORM_S8_UINT || entry->dsv_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
         const bool stencil_writes = desc.front_stencil_pass_op != reshade::api::stencil_op::keep || desc.front_stencil_fail_op != reshade::api::stencil_op::keep || desc.front_stencil_depth_fail_op != reshade::api::stencil_op::keep || desc.back_stencil_pass_op != reshade::api::stencil_op::keep || desc.back_stencil_fail_op != reshade::api::stencil_op::keep || desc.back_stencil_depth_fail_op != reshade::api::stencil_op::keep;
         const bool stencil_tests = desc.front_stencil_func != reshade::api::compare_op::always || desc.back_stencil_func != reshade::api::compare_op::always;
         const bool stencil_write = desc.stencil_enable && has_stencil && (desc.front_stencil_write_mask != 0 || desc.back_stencil_write_mask != 0) && stencil_writes;
         const bool stencil_read = desc.stencil_enable && has_stencil && (desc.front_stencil_read_mask != 0 || desc.back_stencil_read_mask != 0) && stencil_tests;
         if (stencil_read || stencil_write)
         {
            entry->stencil_state = stencil_read ? (stencil_write ? DepthStateType::TestAndWrite : DepthStateType::TestOnly) : DepthStateType::WriteOnly;
         }
      }
   }

   // Expects "s_mutex_generic" (shared), "s_mutex_dumping" and the command list's "mutex_trace" to be locked. The table descriptors are
   // read later, by "ResolveTableDescriptors()".
   void AddTraceDrawCallData(std::vector<TraceDrawCallData>* trace_draw_calls_data, DeviceData* device_data, const reshade::api::command_list* cmd_list, const D3D12State::CommandListState& state,
      std::unordered_map<uint32_t, CachedShader*>& shader_cache, const DrawDispatchData& draw_dispatch_data)
   {
      reshade::api::device* device = device_data->reshade_device;
      TraceDrawCallData& trace_draw_call_data = trace_draw_calls_data->emplace_back();
      trace_draw_call_data.pipeline_handle = state.pipeline.handle;
      trace_draw_call_data.command_list = cmd_list;
      trace_draw_call_data.draw_dispatch_data = draw_dispatch_data;

      // The registers the pipeline's shaders declare. Without them every bound descriptor is listed (up to the per type limits).
      const Shader::CachedPipeline* pipeline = nullptr;
      std::vector<CachedShader::ResourceBinding> shader_bindings;
      bool found_shader_bindings = false;
      if (const auto it = device_data->pipeline_cache_by_pipeline_handle.find(state.pipeline.handle); it != device_data->pipeline_cache_by_pipeline_handle.end() && it->second)
      {
         pipeline = it->second;
         found_shader_bindings = !pipeline->shader_hashes.empty();
         for (const uint32_t shader_hash : pipeline->shader_hashes)
         {
            const auto shader_it = shader_cache.find(shader_hash);
            CachedShader* cached_shader = (shader_it != shader_cache.end()) ? shader_it->second : nullptr;
            if (!cached_shader)
            {
               found_shader_bindings = false;
               continue;
            }
            // Lazily, as disassembling every shader at creation would be slow
            if (!cached_shader->parsed_bindings)
            {
               cached_shader->parsed_bindings = true;
               if (cached_shader->disasm.empty())
               {
                  cached_shader->disasm = Shader::DisassembleShader(cached_shader->data, cached_shader->size).value_or("DISASSEMBLY FAILED");
               }
               cached_shader->found_bindings = ParseResourceBindings(cached_shader);
            }
            found_shader_bindings &= cached_shader->found_bindings;
            trace_draw_call_data.uses_descriptor_heap |= cached_shader->uses_descriptor_heap;
            shader_bindings.append_range(cached_shader->bindings);
         }
      }
      // A command list has one pipeline slot, so it also tells "ExecuteIndirect()" draws and dispatches apart
      const bool is_dispatch = pipeline ? pipeline->HasComputeShader() : (draw_dispatch_data.dispatch_count.x != 0);

      // Render targets and the depth stencil as they were bound, CPU descriptors are read at that time
      if (!is_dispatch)
      {
         trace_draw_call_data.primitive_topology = D3D_PRIMITIVE_TOPOLOGY(state.primitive_topology); // ReShade keeps the D3D values
         trace_draw_call_data.index_buffer_hash = ResourceHashString(state.index_buffer.buffer);
         trace_draw_call_data.index_buffer_format = (state.index_buffer.stride == 2) ? DXGI_FORMAT_R16_UINT : ((state.index_buffer.stride == 4) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN);
         trace_draw_call_data.index_buffer_offset = UINT(state.index_buffer.offset);
         // Like the D3D11 capture, one entry per input element, with the vertex buffer bound to its slot
         for (uint32_t i = 0; pipeline && i < pipeline->subobject_count; i++)
         {
            if (pipeline->subobjects_cache[i].type != reshade::api::pipeline_subobject_type::input_layout)
               continue;
            const auto* elements = static_cast<const reshade::api::input_element*>(pipeline->subobjects_cache[i].data);
            for (uint32_t j = 0; j < pipeline->subobjects_cache[i].count; j++)
            {
               const uint32_t slot = elements[j].buffer_binding;
               trace_draw_call_data.input_layouts_formats.push_back(DXGI_FORMAT(elements[j].format));
               trace_draw_call_data.vertex_buffer_hashes.push_back(ResourceHashString((slot < state.vertex_buffers.size()) ? state.vertex_buffers[slot].buffer : 0));
            }
         }

         // Without a pipeline's state the D3D12 defaults apply (depth test and write, no stencil)
         const reshade::api::depth_stencil_desc desc = FillRenderState(&trace_draw_call_data, pipeline);
         FillOutputState(device, device_data, &trace_draw_call_data, state, desc);
      }

      // The next packed SRV, UAV, constant buffer or sampler entry. UINT32_MAX if the pipeline's shaders don't declare the register, or if the entries are all taken (then the
      // entry is marked "truncated")
      auto AddEntry = [&](char type, const TraceDrawCallData::ShaderRegister& shader_register) -> uint32_t
      {
         const auto declares_register = [&](const CachedShader::ResourceBinding& binding)
         {
            return binding.type == type && binding.space == shader_register.space && shader_register.index >= binding.index && shader_register.index - binding.index < binding.count;
         };
         if (found_shader_bindings && std::ranges::none_of(shader_bindings, declares_register))
            return UINT32_MAX;
         uint32_t* count = &trace_draw_call_data.cb_count;
         TraceDrawCallData::ShaderRegister* registers = trace_draw_call_data.cb_register;
         uint32_t max_count = uint32_t(std::size(trace_draw_call_data.cbs));
         if (type == 't')
            count = &trace_draw_call_data.srv_count, registers = trace_draw_call_data.sr_register, max_count = uint32_t(TraceDrawCallData::srvs_size);
         else if (type == 'u')
            count = &trace_draw_call_data.uav_count, registers = trace_draw_call_data.ua_register, max_count = uint32_t(TraceDrawCallData::uavs_size);
         else if (type == 's')
            count = &trace_draw_call_data.sampler_count, registers = trace_draw_call_data.sa_register, max_count = uint32_t(TraceDrawCallData::samplers_size);
         if (*count >= max_count)
         {
            trace_draw_call_data.truncated = true;
            return UINT32_MAX;
         }
         registers[*count] = shader_register;
         if (type == 'b')
            trace_draw_call_data.cbs[*count] = true;
         return (*count)++;
      };

      auto IsShaderResourceView = [](reshade::api::descriptor_type type)
      {
         return type == reshade::api::descriptor_type::shader_resource_view || type == reshade::api::descriptor_type::buffer_shader_resource_view || type == reshade::api::descriptor_type::acceleration_structure;
      };
      auto IsUnorderedAccessView = [](reshade::api::descriptor_type type)
      {
         return type == reshade::api::descriptor_type::unordered_access_view || type == reshade::api::descriptor_type::buffer_unordered_access_view;
      };

      const D3D12State::RootState& root = is_dispatch ? state.compute : state.graphics;
      const std::shared_lock lock_layouts(device_data->pipeline_layouts.mutex);
      const auto layout_it = device_data->pipeline_layouts.parameters_by_layout.find(root.layout.handle);
#if ENABLE_MINHOOK
      // ExecuteIndirect(): what its signature sets per command, the root arguments by the register of their root parameter
      if (draw_dispatch_data.indirect && D3D12CommandSignatures::current_call)
      {
         const D3D12CommandSignatures::Call& call = *D3D12CommandSignatures::current_call;
         trace_draw_call_data.indirect_buffer = reinterpret_cast<uint64_t>(call.argument_buffer);
         trace_draw_call_data.indirect_offset = call.argument_offset;
         trace_draw_call_data.indirect_max_count = call.max_count;
         trace_draw_call_data.indirect_count_buffer = call.count_buffer != nullptr;
         if (call.signature)
         {
            trace_draw_call_data.indirect_signature_known = true;
            trace_draw_call_data.indirect_stride = call.signature->stride;
            const std::vector<D3D12State::RootParameter>* parameters = (layout_it != device_data->pipeline_layouts.parameters_by_layout.end() ? &layout_it->second : nullptr);
            for (const D3D12_INDIRECT_ARGUMENT_DESC& desc : call.signature->arguments)
            {
               TraceDrawCallData::IndirectArgument& argument = trace_draw_call_data.indirect_arguments.emplace_back();
               argument.type = desc.Type;
               char register_type = 0;
               switch (desc.Type)
               {
               case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
                  argument.slot = desc.VertexBuffer.Slot;
                  break;
               case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
                  argument.root_parameter = desc.Constant.RootParameterIndex;
                  argument.constant_offset = desc.Constant.DestOffsetIn32BitValues;
                  argument.constant_count = desc.Constant.Num32BitValuesToSet;
                  register_type = 'b';
                  break;
               case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
                  argument.root_parameter = desc.ConstantBufferView.RootParameterIndex;
                  register_type = 'b';
                  break;
               case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
                  argument.root_parameter = desc.ShaderResourceView.RootParameterIndex;
                  register_type = 't';
                  break;
               case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
                  argument.root_parameter = desc.UnorderedAccessView.RootParameterIndex;
                  register_type = 'u';
                  break;
               default:
                  break;
               }
               if (!parameters || argument.root_parameter >= parameters->size())
                  continue;
               const D3D12State::RootParameter& parameter = (*parameters)[argument.root_parameter];
               if (parameter.type == reshade::api::pipeline_layout_param_type::push_constants)
               {
                  argument.shader_register = {.index = parameter.constants.dx_register_index, .space = parameter.constants.dx_register_space};
                  argument.register_type = register_type;
               }
               else if (!parameter.ranges.empty())
               {
                  argument.shader_register = {.index = parameter.ranges[0].dx_register_index, .space = parameter.ranges[0].dx_register_space};
                  argument.register_type = register_type;
               }
            }
         }
      }
#endif
      if (layout_it == device_data->pipeline_layouts.parameters_by_layout.end())
         return;
      const std::shared_lock lock_buffers(device_data->buffer_addresses.mutex);
      // The static samplers belong to the root signature, no command binds them
      for (const D3D12State::RootParameter& parameter : layout_it->second)
      {
         if (parameter.static_samplers.size() != parameter.ranges.size())
            continue;
         for (size_t i = 0; i < parameter.ranges.size(); i++)
         {
            const TraceDrawCallData::ShaderRegister shader_register = {.index = parameter.ranges[i].dx_register_index, .space = parameter.ranges[i].dx_register_space};
            if (const uint32_t index = AddEntry('s', shader_register); index != UINT32_MAX)
            {
               FillSampler(&trace_draw_call_data, index, &parameter.static_samplers[i]);
            }
         }
      }
      for (size_t param_index = 0; param_index < layout_it->second.size() && param_index < root.bindings.size(); param_index++)
      {
         const D3D12State::RootParameter& parameter = layout_it->second[param_index];
         const D3D12State::RootBinding& binding = root.bindings[param_index];
         switch (parameter.type)
         {
         case reshade::api::pipeline_layout_param_type::push_constants:
         {
            const TraceDrawCallData::ShaderRegister shader_register = {.index = parameter.constants.dx_register_index, .space = parameter.constants.dx_register_space};
            if (binding.constants.empty())
               break;
            if (const uint32_t i = AddEntry('b', shader_register); i != UINT32_MAX)
            {
               trace_draw_call_data.cb_num_constants[i] = UINT((binding.constants.size() + 3) / 4);
               trace_draw_call_data.cb_root_constants[i] = binding.constants;
            }
            break;
         }
         case reshade::api::pipeline_layout_param_type::push_descriptors:
         case reshade::api::pipeline_layout_param_type::push_descriptors_with_ranges_and_flags:
         {
            // Root descriptors (static samplers come as sampler ranges). Their GPU virtual address is recorded with the command.
            if (parameter.ranges.size() != 1 || binding.descriptor == 0)
               break;
            const reshade::api::descriptor_range& range = parameter.ranges[0];
            const TraceDrawCallData::ShaderRegister shader_register = {.index = range.dx_register_index, .space = range.dx_register_space};
            if (range.type == reshade::api::descriptor_type::constant_buffer)
            {
               if (const uint32_t i = AddEntry('b', shader_register); i != UINT32_MAX)
               {
                  trace_draw_call_data.cb_resource[i] = binding.descriptor;
                  trace_draw_call_data.cb_hash[i] = ResourceHashString(binding.descriptor);
                  trace_draw_call_data.cb_first_constant[i] = UINT(binding.descriptor_offset / 16);
               }
               break;
            }
            // Root SRVs and UAVs are only buffers (and acceleration structures), as a GPU virtual address in them
            const bool is_srv = IsShaderResourceView(range.type);
            if (!is_srv && !IsUnorderedAccessView(range.type))
               break;
            const char type = is_srv ? 't' : 'u';
            if (const uint32_t i = AddEntry(type, shader_register); i != UINT32_MAX)
            {
               FillView(device, &trace_draw_call_data, type, i, false, 0, {.resource = D3D12State::FindBuffer(device_data->buffer_addresses, binding.descriptor).resource});
            }
            break;
         }
         case reshade::api::pipeline_layout_param_type::descriptor_table:
         case reshade::api::pipeline_layout_param_type::descriptor_table_with_flags:
         {
            if (binding.table.handle == 0)
               break;
            for (const reshade::api::descriptor_range& range : parameter.ranges)
            {
               const bool is_srv = IsShaderResourceView(range.type);
               const bool is_uav = IsUnorderedAccessView(range.type);
               const bool is_sampler = (range.type == reshade::api::descriptor_type::sampler);
               if (!is_srv && !is_uav && !is_sampler && range.type != reshade::api::descriptor_type::constant_buffer)
                  continue;
               const char type = is_srv ? 't' : (is_uav ? 'u' : (is_sampler ? 's' : 'b'));
               // ponytail: bindless (unbounded) ranges stop at the per type limit, reading "ResourceDescriptorHeap" is out of the capture's scope
               const uint32_t count = (std::min)(range.count, uint32_t(TraceDrawCallData::srvs_size));
               trace_draw_call_data.truncated |= (range.count > count);
               for (uint32_t i = 0; i < count; i++)
               {
                  if (const uint32_t index = AddEntry(type, {.index = range.dx_register_index + i, .space = range.dx_register_space}); index != UINT32_MAX)
                  {
                     trace_draw_call_data.table_descriptors.push_back({.table = binding.table, .binding = range.binding + i, .type = type, .index = index});
                  }
               }
            }
            break;
         }
         }
      }
   }

   // Empty without a box ("subresource_box" has the same layout)
   D3D12_BOX ToD3D12Box(const reshade::api::subresource_box* box)
   {
      if (!box)
         return {};
      return {.left = box->left, .top = box->top, .front = box->front, .right = box->right, .bottom = box->bottom, .back = box->back};
   }

   // Fills a copy, upload or resolve entry like the D3D11 capture does, with whole resources and no views ("IsRTVValid()" stays false, so
   // the entry isn't taken for a draw's output): the source in "sr_*[0]" and "copy_source", the resource it wrote in "rt_*[0]".
   // "rt_view[0]" is what it wrote there: everything for "CopyKind::Resource", else the mip and layer of "copy_info.dest_subresource"
   // (the plane, that index divided by mips * layers, is only in "copy_info"). Buffers have no subresources.
   void FillCopy(reshade::api::device* device, TraceDrawCallData* entry, reshade::api::resource source, reshade::api::resource dest, const TraceDrawCallData::CopyInfo& copy_info)
   {
      entry->type = TraceDrawCallData::TraceDrawCallType::CopyResource;
      entry->copy_info = copy_info;
      entry->copy_source = source.handle;
      FillView(device, entry, 't', 0, false, 0, {.resource = source.handle});

      D3D12State::ViewInfo written = {.resource = dest.handle};
      if (const reshade::api::resource_desc desc = device->get_resource_desc(dest); desc.type != reshade::api::resource_type::buffer)
      {
         written.format = desc.texture.format;
         if (copy_info.kind != TraceDrawCallData::CopyKind::Resource)
         {
            // The subresource index is mip + layer * mips + plane * mips * layers, 3D textures have no layers
            const uint32_t mips = (std::max)(uint32_t(desc.texture.levels), 1u); // Zero when unknown
            const uint32_t layers = ((desc.type == reshade::api::resource_type::texture_3d) ? 1u : (std::max)(uint32_t(desc.texture.depth_or_layers), 1u));
            written.first_level = copy_info.dest_subresource % mips;
            written.first_layer = (copy_info.dest_subresource / mips) % layers;
            written.layer_count = 1;
         }
      }
      FillView(device, entry, 'r', 0, false, 0, written);
   }

   // Fills a clear entry like the D3D11 capture does: the cleared resource in "rt_*[0]" (no view, see "FillCopy()") and the cleared view in
   // "rt_view[0]". The clear values are up to the caller ("TraceDrawCallData::blend_factor").
   void FillClear(reshade::api::device* device, TraceDrawCallData* entry, reshade::api::resource_view view, TraceDrawCallData::ClearKind kind)
   {
      entry->type = TraceDrawCallData::TraceDrawCallType::ClearResource;
      entry->clear_kind = kind;
      FillView(device, entry, 'r', 0, false, 0, D3D12State::GetViewInfo(device, view));
   }

   // Fills a Luma pass of the pass layer as a Custom entry, the D3D12 counterpart of the D3D11 "MoveInjectedPassTraces()": its inputs as SRVs (the registers are the order the pass binds
   // them in) and its outputs as render targets, or as unordered access views for a compute pass, each viewed as the format of its "ViewInfo". Readbacks don't re-issue these
   void FillLumaPass(reshade::api::device* device, TraceDrawCallData* entry, const char* name, std::span<const D3D12State::ViewInfo> inputs, std::initializer_list<D3D12State::ViewInfo> outputs, bool compute)
   {
      entry->type = TraceDrawCallData::TraceDrawCallType::Custom;
      entry->custom_name = name;
      for (const D3D12State::ViewInfo& input : inputs)
      {
         const uint32_t index = entry->srv_count++;
         entry->sr_register[index] = {.index = index};
         FillView(device, entry, 't', index, true, input.resource, input);
      }
      uint32_t output_index = 0;
      for (const D3D12State::ViewInfo& output : outputs)
      {
         if (compute)
         {
            entry->ua_register[output_index] = {.index = output_index};
            entry->uav_count++;
         }
         FillView(device, entry, compute ? 'u' : 'r', output_index++, true, output.resource, output);
      }
   }
} // namespace D3D12Trace
