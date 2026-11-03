#pragma once

#include "..\..\Core\includes\motion_vector_patch.h"
#include "..\..\Core\includes\scene_motion_vectors.h"

// The game's slots for Core's DXBC motion vector patch (described in "motion_vector_patch.h"; dgVoodoo's registers in
// "MotionVectorPatch::DgVoodoo"). The census is in "MotionVectorCensus.md" (offline disassembly of the dump + luma_read_cbuffer).
namespace MotionVectorPatches
{
   using MotionVectorPatch::DgVoodoo::jitter_slot;
   using MotionVectorPatch::DgVoodoo::object_row_offset;
   using MotionVectorPatch::DgVoodoo::object_slot;
   using MotionVectorPatch::DgVoodoo::previous_slots;
   using MotionVectorPatch::DgVoodoo::target_slot;
   // The reactive masks, written by the scene's alpha blended draws (their pixel shaders patched with "PatchPixelShaderReactive")
   constexpr uint32_t reactive_slot = target_slot + 1;

   // c<N> at row N + 20. Column vector math, one row per clip space component.
   // c0: static meshes' world view projection (VS 318D7C41 and 10 more; its translation is row 23)
   constexpr uint32_t world_view_projection_row = object_row_offset + 0;
   // c201: the skinned/instanced meshes' view projection (21E45F9B, 6EC2ECFF...). Their bones (c0..: first bone at row 20) carry the world.
   constexpr uint32_t skinned_view_projection_row = 221;
   // c218: the frame's view projection, in every scene draw (identical across them in a frame, y scale 1 / tan(30 degrees))
   constexpr uint32_t frame_view_projection_row = 238;
   // The tie-break transform of the draws sharing a key: the first bone of the skinned meshes' palette, else the static WVP's rows
   constexpr uint32_t first_bone_row = object_row_offset + 0;

   constexpr MotionVectorPatch::Layout layout = MotionVectorPatch::DgVoodoo::MakeLayout();

   // Camera kinds in priority order (the first one a vertex shader reads wins): the object ones, then the frame's view projection.
   constexpr std::array<SceneMotionVectors::Camera, 3> cameras = {{
      {.type = SceneMotionVectors::CameraType::VIEW_PROJECTION, .row = skinned_view_projection_row},
      {.type = SceneMotionVectors::CameraType::WORLD_VIEW_PROJECTION, .row = world_view_projection_row},
      {.type = SceneMotionVectors::CameraType::VIEW_PROJECTION, .row = frame_view_projection_row},
   }};
   constexpr std::array<SceneMotionVectors::TieBreakRow, 1> tie_break_rows = {{
      {.row = first_bone_row, .relative = true},
   }};
   constexpr SceneMotionVectors::Camera frame_camera = cameras[2];
} // namespace MotionVectorPatches
