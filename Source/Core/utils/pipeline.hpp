#pragma once

#include "include/reshade.hpp"
#include "./format.hpp"

namespace Shader
{
   // Subobjects whose data is a "reshade::api::shader_desc" (with its code) we copy
   bool IsClonedShaderSubobject(reshade::api::pipeline_subobject_type type)
   {
      switch (type)
      {
      case reshade::api::pipeline_subobject_type::vertex_shader:
      case reshade::api::pipeline_subobject_type::geometry_shader:
      case reshade::api::pipeline_subobject_type::compute_shader:
      case reshade::api::pipeline_subobject_type::pixel_shader:
#if defined(DX12) && DX12
      case reshade::api::pipeline_subobject_type::hull_shader:
      case reshade::api::pipeline_subobject_type::domain_shader:
      case reshade::api::pipeline_subobject_type::amplification_shader:
      case reshade::api::pipeline_subobject_type::mesh_shader:
      case reshade::api::pipeline_subobject_type::raygen_shader:
      case reshade::api::pipeline_subobject_type::any_hit_shader:
      case reshade::api::pipeline_subobject_type::closest_hit_shader:
      case reshade::api::pipeline_subobject_type::miss_shader:
      case reshade::api::pipeline_subobject_type::intersection_shader:
      case reshade::api::pipeline_subobject_type::callable_shader:
#endif
         return true;
      default:
         return false;
      }
   }

   reshade::api::pipeline_subobject* ClonePipelineSubobjects(uint32_t subobject_count, const reshade::api::pipeline_subobject* subobjects)
   {
      auto* new_subobjects = new reshade::api::pipeline_subobject[subobject_count];
      memcpy(new_subobjects, subobjects, sizeof(reshade::api::pipeline_subobject) * subobject_count);
      for (uint32_t i = 0; i < subobject_count; ++i)
      {
         const auto& subobject = subobjects[i];
#if _DEBUG && LOG_VERBOSE
         {
            std::stringstream s;
            s << "Shader::ClonePipelineSubobjects(cloning " << subobjects[i].type << "[" << i << "]";
            reshade::log::message(reshade::log::level::debug, s.str().c_str());
         }
#endif
         if (IsClonedShaderSubobject(subobject.type))
         {
            new_subobjects[i].data = malloc(sizeof(reshade::api::shader_desc));
            memcpy(new_subobjects[i].data, subobject.data, sizeof(reshade::api::shader_desc));
            auto* old_desc = static_cast<reshade::api::shader_desc*>(subobject.data);
            auto* new_desc = static_cast<reshade::api::shader_desc*>(new_subobjects[i].data);
            if (old_desc->code_size != 0u)
            {
               void* code_copy = malloc(old_desc->code_size);
               memcpy(code_copy, old_desc->code, old_desc->code_size);
               new_desc->code = code_copy;
            }
            else
            {
               new_desc->code = nullptr; // Never free the game's pointer
            }

#if _DEBUG && LOG_VERBOSE
            std::stringstream s;
            s << "Shader::ClonePipelineSubobjects(cloning ";
            s << subobject.type;
            s << " with " << PRINT_CRC32(compute_crc32(static_cast<const uint8_t*>(old_desc->code), old_desc->code_size));
            s << " => " << PRINT_CRC32(compute_crc32(static_cast<const uint8_t*>(new_desc->code), new_desc->code_size));
            s << " (" << old_desc->code_size << " bytes)";
            s << " from " << old_desc->code;
            s << " to " << new_desc->code;
            s << ")";
            reshade::log::message(reshade::log::level::debug, s.str().c_str());
#endif
            continue;
         }
         switch (subobject.type)
         {
#if defined(DX12) && DX12
         case reshade::api::pipeline_subobject_type::input_layout:
         new_subobjects[i].data = malloc(sizeof(reshade::api::input_element) * subobject.count);
         memcpy(new_subobjects[i].data, subobject.data, sizeof(reshade::api::input_element) * subobject.count);
         for (uint32_t j = 0; j < subobject.count; j++)
         {
            auto* old_input_elements = static_cast<reshade::api::input_element*>(subobject.data);
            auto* new_input_elements = static_cast<reshade::api::input_element*>(new_subobjects[i].data);
            new_input_elements[j].semantic = old_input_elements[j].semantic ? _strdup(old_input_elements[j].semantic) : nullptr;
         }
         break;
         case reshade::api::pipeline_subobject_type::stream_output_state:
         new_subobjects[i].data = malloc(sizeof(reshade::api::stream_output_desc) * subobject.count);
         memcpy(new_subobjects[i].data, subobject.data, sizeof(reshade::api::stream_output_desc) * subobject.count);
         break;
         case reshade::api::pipeline_subobject_type::blend_state:
         new_subobjects[i].data = malloc(sizeof(reshade::api::blend_desc) * subobject.count);
         memcpy(new_subobjects[i].data, subobject.data, sizeof(reshade::api::blend_desc) * subobject.count);
         break;
         case reshade::api::pipeline_subobject_type::rasterizer_state:
         new_subobjects[i].data = malloc(sizeof(reshade::api::rasterizer_desc) * subobject.count);
         memcpy(new_subobjects[i].data, subobject.data, sizeof(reshade::api::rasterizer_desc) * subobject.count);
         break;
         case reshade::api::pipeline_subobject_type::depth_stencil_state:
         new_subobjects[i].data = malloc(sizeof(reshade::api::depth_stencil_desc) * subobject.count);
         memcpy(new_subobjects[i].data, subobject.data, sizeof(reshade::api::depth_stencil_desc) * subobject.count);
         break;
         case reshade::api::pipeline_subobject_type::primitive_topology:
         new_subobjects[i].data = malloc(sizeof(reshade::api::primitive_topology) * subobject.count);
         memcpy(new_subobjects[i].data, subobject.data, sizeof(reshade::api::primitive_topology) * subobject.count);
         break;
         case reshade::api::pipeline_subobject_type::depth_stencil_format:
         case reshade::api::pipeline_subobject_type::render_target_formats:
         new_subobjects[i].data = malloc(sizeof(reshade::api::format) * subobject.count);
         memcpy(new_subobjects[i].data, subobject.data, sizeof(reshade::api::format) * subobject.count);
         break;
         case reshade::api::pipeline_subobject_type::sample_mask:
         case reshade::api::pipeline_subobject_type::sample_count:
         case reshade::api::pipeline_subobject_type::viewport_count:
         case reshade::api::pipeline_subobject_type::max_vertex_count:
         case reshade::api::pipeline_subobject_type::max_payload_size:
         case reshade::api::pipeline_subobject_type::max_attribute_size:
         case reshade::api::pipeline_subobject_type::max_recursion_depth:
         new_subobjects[i].data = malloc(sizeof(uint32_t) * subobject.count);
         memcpy(new_subobjects[i].data, subobject.data, sizeof(uint32_t) * subobject.count);
         break;
         case reshade::api::pipeline_subobject_type::dynamic_pipeline_states:
         new_subobjects[i].data = malloc(sizeof(reshade::api::dynamic_state) * subobject.count);
         memcpy(new_subobjects[i].data, subobject.data, sizeof(reshade::api::dynamic_state) * subobject.count);
         break;
         case reshade::api::pipeline_subobject_type::libraries:
         case reshade::api::pipeline_subobject_type::shader_groups:
         new_subobjects[i].data = malloc(sizeof(reshade::api::shader_group) * subobject.count);
         memcpy(new_subobjects[i].data, subobject.data, sizeof(reshade::api::shader_group) * subobject.count);
         break;
         case reshade::api::pipeline_subobject_type::flags:
         new_subobjects[i].data = malloc(sizeof(reshade::api::pipeline_flags) * subobject.count);
         memcpy(new_subobjects[i].data, subobject.data, sizeof(reshade::api::pipeline_flags) * subobject.count);
         break;
#endif
         default:
         // Not copied: drop the game's pointer, so it's never used or freed after the original pipeline is gone
         new_subobjects[i].data = nullptr;
         break;
         }
      }

      return new_subobjects;
   }

   void DestroyPipelineSubojects(reshade::api::pipeline_subobject* subojects, uint32_t subobject_count)
   {
      for (uint32_t i = 0; i < subobject_count; ++i)
      {
         auto& suboject = subojects[i];

         if (IsClonedShaderSubobject(suboject.type))
         {
            auto* desc = static_cast<reshade::api::shader_desc*>(suboject.data);
            free(const_cast<void*>(desc->code));
            desc->code = nullptr;
         }
#if defined(DX12) && DX12
         else if (suboject.type == reshade::api::pipeline_subobject_type::input_layout && suboject.data)
         {
            for (uint32_t j = 0; j < suboject.count; j++)
            {
               free(const_cast<char*>(static_cast<reshade::api::input_element*>(suboject.data)[j].semantic));
            }
         }
#endif

         free(suboject.data);
         suboject.data = nullptr;
      }
      delete[] subojects;
   }
}
