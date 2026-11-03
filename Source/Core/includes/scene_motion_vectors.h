#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "motion_vector_patch.h"
#include "patched_draws.h"

// A scene's motion vectors from the game's own mesh draws (dgVoodoo D3D9 games): each draw's vertex shader runs a second time with the
// previous frame's vc4 of the same object ("motion_vector_patch.h"), found by its draw key and tie-break transform ("patched_draws.h").
// The state machine written for Bayonetta: the CPU copies of vc4, the patched shader caches, the draw key and previous frame match, the
// camera-only fallback, the motion vector target and the reactive mask target, the jitter. A game decides when its scene opens and
// ends (OpenScene's refusals, the fill, the upscaler); it calls "BeginFrame", "DrawWithMotionVectors" and "DrawWithJitter".
// Include after "core.hpp", with the game defining LUMA_PATCH_BYTECODE_SYNC (the shaders are read from the patch context). A game provides a "Game" type (template argument) with:
//   static constexpr const char* log_tag;
//   static constexpr MotionVectorPatch::Layout layout;
//   static constexpr std::array<Camera, N> cameras;           // Ordered: the first one a vertex shader reads wins
//   static constexpr std::array<TieBreakRow, N> tie_break_rows; // Ordered likewise
//   static constexpr Camera frame_camera;                      // VIEW_PROJECTION or VIEW_AND_PROJECTION
//   static constexpr const char* patch_names[size_t(PixelShaderPatch::COUNT)];
//   static bool AcceptSceneColor(const D3D11_RENDER_TARGET_VIEW_DESC& rtv_desc, const D3D11_TEXTURE2D_DESC& color_desc);
//   static bool IsExcludedPixelShader(uint32_t pixel_shader_hash);   // Alpha blended draws with it keep what's behind them
//   static bool ReactiveMaskUsed(const DeviceData& device_data);     // The upscaler reads the reactive masks
//   static bool CanFill(DeviceData& device_data);                    // The fill can run this frame (its depth and shader exist)
//   static std::vector<uint8_t> PatchPixelShader(PixelShaderPatch patch, const uint8_t* code, size_t code_size, std::string* error);
//   optional: static void FillMarkersBeforeBlend(ID3D11Device*, ID3D11DeviceContext*, DeviceData&);
//   optional: static constexpr bool second_scene_color = true;       // A second color target "AcceptSceneColor" passes (the first one
//                                                                     // accepted stays "scene_color"), with the same scene depth
//   optional: static constexpr PixelShaderPatch blended_mask_patch;  // "BLENDED" plus the reactive mask, for the unmatched alpha blended
//   optional: static bool MaskUnmatchedBlended(DeviceData&);   // draws into "reactive_target" (their own motion is unknown)
namespace SceneMotionVectors
{
   namespace Dg = MotionVectorPatch::DgVoodoo;

   constexpr uint32_t object_slot = Dg::object_slot;
   constexpr uint32_t jitter_slot = Dg::jitter_slot;
   constexpr uint32_t target_slot = Dg::target_slot;
   // The reactive masks, written by the scene's alpha blended draws (their pixel shaders patched with "PatchPixelShaderReactive")
   constexpr uint32_t reactive_slot = target_slot + 1;
   // A camera matrix' 4 rows in vc4
   constexpr UINT CAMERA_SIZE = 4 * 16;
   static_assert(sizeof(PatchedDraws::ObjectTransform) == 3 * 16);

   // Column vector matrices in vc4 (one row per clip space component). Rows are absolute (include "Dg::object_row_offset").
   enum class CameraType : uint8_t
   {
      VIEW_PROJECTION,           // "row": the product
      WORLD_VIEW_PROJECTION,     // "row": the product
      VIEW_AND_PROJECTION,       // "row": the view (3x4, or 4x4), "projection_row"
      WORLD_VIEW_AND_PROJECTION, // "row": the world view, "projection_row"
   };
   struct Camera
   {
      CameraType type;
      uint32_t row;
      uint32_t projection_row = 0;
      // How many rows of each matrix a vertex shader must read to be of this kind (a position uses all 4 of a product, a view may
      // have 3)
      uint32_t rows_read = 4;
      constexpr bool operator==(const Camera&) const = default;
   };
   // The tie-break transform between the draws sharing a key: the 3x4 at "row" (a "relative" index: a bone palette's first bone)
   struct TieBreakRow
   {
      uint32_t row;
      bool relative = false;
   };

   // "DrawWithMotionVectors" refusals, counted per frame in DEVELOPMENT ("mv.rejected.*")
   enum MotionVectorReject
   {
      REJECT_EXTRA_TARGET,
      REJECT_NO_SCENE,
      REJECT_DEPTH_TEST,
      REJECT_BLEND,
      REJECT_OTHER_DEPTH_COLOR,
      REJECT_FORMAT,
      REJECT_SIZE,
      REJECT_CREATE,
      REJECT_SHADERS,
      REJECT_NO_CAMERA,
      REJECT_COUNT
   };
#if DEVELOPMENT
   constexpr const char* MOTION_VECTOR_REJECT_NAMES[REJECT_COUNT] = {"extra_target", "no_scene", "depth_test", "blend", "other_depth_color", "format", "size", "create", "shaders", "no_camera"};
#endif

   // A draw's blend for the reactive masks (see "ClassifyBoundBlend")
   enum class ReactiveBlend : uint8_t
   {
      NONE,
      ALPHA,
      ADDITIVE,
   };

   // A pixel shader's patch: the motion vector target, the mask target of an alpha blended or additive draw, or an alpha blended
   // draw's own motion vectors; the game's own patches (its "PatchPixelShader") follow
   enum class PixelShaderPatch : uint8_t
   {
      MOTION_VECTORS,
      MASK_ALPHA,
      MASK_ADDITIVE,
      BLENDED,
      GAME_0,
      GAME_1,
      GAME_2,
      GAME_3,
      COUNT
   };

   using ConstantsCopy = PatchedDraws::ObjectConstants::Copy;
   using BoundStates = PatchedDraws::BoundStates<object_slot, jitter_slot>;

   // A game shader's patched version (null if refused), patched on first use, by its hash; a vertex shader's with the bytes of vc4 it
   // reads ("DXBC::ConstantBufferBytes": the previous frame's copy uploads only those), its camera kind and the vc4 byte offsets of
   // its matrices (0 for none: no motion vectors) and of its tie-break transform (0 for none)
   template <typename T>
   struct PatchedShader
   {
      com_ptr<T> shader;
      UINT read_size = 0;
      UINT camera_offset = 0;
      UINT projection_offset = 0; // The second matrix of the two matrix kinds
      UINT camera_end = 0;        // The bytes of vc4 the camera needs
      UINT translation_offset = 0;
      CameraType camera_type = CameraType::VIEW_PROJECTION;
      bool frame_camera = false; // Of the game's "frame_camera" kind
   };

   // Motion vector draws by draw key (shaders, buffers, arguments), with their tie-break transform and vc4. A draw takes the previous
   // frame's vc4 of its key's nearest draw (same object, a frame earlier), its camera included.
   struct MotionVectorObject
   {
      PatchedDraws::ObjectTransform transform;
      ConstantsCopy constants;
   };

#if DEVELOPMENT
   // Per frame counts for the MCP (the last complete frame's)
   struct MotionVectorStats
   {
      uint32_t motion_vector_draws = 0, jitter_draws = 0, reactive_draws = 0, matched = 0, camera_only = 0, other_camera = 0, uncopied = 0, maps = 0, updates = 0, other_maps = 0, sr_draws = 0;
      uint32_t tiebreak_collisions = 0; // Objects sharing a key and a transform with other constants ("PatchedDraws::CountTieBreakCollisions")
      uint32_t untransformed = 0;       // Motion vector draws of vertex shaders without a tie-break transform (camera motion only)
      uint32_t bound_state_mismatches = 0;
      uint32_t constants_pool = 0;
      uint32_t prepass_drawn = 0;
      uint32_t ended_by = 0; // The ending pass's PS hash
      uint32_t rejected[REJECT_COUNT] = {};
   };
#endif

   struct State
   {
      // Motion vectors and jitter this frame: an upscaler or a development toggle (set at present by the game)
      std::atomic<bool> active = false;
      std::shared_mutex mutex;
      std::unordered_map<uint32_t, PatchedShader<ID3D11VertexShader>> vertex_shaders;
      // By "PixelShaderPatch"
      std::unordered_map<uint32_t, PatchedShader<ID3D11PixelShader>> patched_pixel_shaders[size_t(PixelShaderPatch::COUNT)];
      // The motion vector target (sized like the scene; the alpha blending states blend it like their color, the others write it
      // unblended, see "PatchedDraws::OnCreateBlendState")
      com_ptr<ID3D11Texture2D> texture;
      com_ptr<ID3D11RenderTargetView> rtv;
      com_ptr<ID3D11ShaderResourceView> srv;
      com_ptr<ID3D11UnorderedAccessView> uav; // Null without typed UAV loads of its format (then no upscaler)
      // The upscaler's depth, built by the game's fill
      com_ptr<ID3D11Texture2D> device_depth;
      com_ptr<ID3D11UnorderedAccessView> device_depth_uav;
      // The reactive and transparency & composition masks (FSR, Luma TAA), written by the fill from "reactive_target"
      com_ptr<ID3D11Texture2D> reactive;
      com_ptr<ID3D11UnorderedAccessView> reactive_uav;
      com_ptr<ID3D11Texture2D> transparency;
      com_ptr<ID3D11UnorderedAccessView> transparency_uav;
      // The additive draws' mask of the previous frame, which the fill reads and replaces: a laser that went out wrote nothing this
      // frame but is still in the upscaler's history (R32_FLOAT: typed UAV loads of other formats are optional in D3D11)
      com_ptr<ID3D11Texture2D> reactive_previous;
      com_ptr<ID3D11UnorderedAccessView> reactive_previous_uav;
      // What the alpha blended draws wrote (x reactive, y transparency & composition; max blended), read by the fill. Created when a
      // mask user first runs, gone after the game's idle frames.
      com_ptr<ID3D11Texture2D> reactive_target;
      com_ptr<ID3D11RenderTargetView> reactive_target_rtv;
      com_ptr<ID3D11ShaderResourceView> reactive_target_srv;
      uint32_t sr_reactive_frame = 0;
      // A frame opens at its first mesh draw into the scene, starts at its first motion vector draw (the target is cleared) and
      // ends at the game's first post pass, once per present.
      bool scene_open = false;
      bool scene_done = false;
      bool frame_ended = true;
      bool fill_pending = false;
      // The scene's size, the one of its depth. Everything the upscaler reads is this size.
      uint2 scene_size = {};
      com_ptr<ID3D11Resource> depth; // The scene depth (the depth view's resource)
      // The scene color the motion vector draws write, and the game's view of it
      com_ptr<ID3D11Resource> scene_color;
      com_ptr<ID3D11RenderTargetView> scene_rtv;
      // The game's "second_scene_color" (e.g. the translucents' color target when "scene_color" is a depth prepass' target)
      com_ptr<ID3D11Resource> second_scene_color;
      com_ptr<ID3D11RenderTargetView> second_scene_rtv;
      com_ptr<ID3D11Buffer> fill_buffer;
      // The projection jitter (pixels, +y down), chosen when the scene opens; its NDC offset is at the VS "jitter_slot" of every
      // mesh draw depth tested against the scene
      std::array<float, 2> jitter = {};
      std::array<float, 2> jitter_ndc = {}; // The same offset in NDC (y up), as the jitter buffer holds it
      com_ptr<ID3D11Buffer> jitter_buffer;
      // Per-draw lookups kept for the next draw (reset when the scene opens): the jitter path's last depth view and whether it's the
      // scene depth, the motion vector path's last accepted targets and the last blend state's opacity, the last vertex and pixel
      // shader's patched versions (owned by the maps above, never erased)
      ID3D11DepthStencilView* jitter_dsv = nullptr;
      bool jitter_dsv_scene = false;
      ID3D11DepthStencilView* accepted_dsv = nullptr;
      ID3D11BlendState* blend_state = nullptr; // See "ClassifyBoundBlend"
      bool blend_opaque = true;
      bool alpha_blended = false;
      ReactiveBlend reactive_blend = ReactiveBlend::NONE;
      uint32_t last_vertex_shader_hash = 0;
      const PatchedShader<ID3D11VertexShader>* last_vertex_shader = nullptr;
      uint32_t last_pixel_shader_hash = 0;
      ID3D11PixelShader* last_pixel_shader = nullptr;
      PatchedDraws::BoundShader<ID3D11VertexShader> bound_vertex_shader;
      PatchedDraws::BoundShader<ID3D11PixelShader> bound_pixel_shader;
      // The scene's targets refused by "DrawWithMotionVectors" (other depth or color, format, size) and the reason; reset when the
      // scene opens. The shadow maps and the prepass' targets are refused once per frame, not per draw.
      ID3D11RenderTargetView* refused_rtv = nullptr;
      ID3D11DepthStencilView* refused_dsv = nullptr;
      MotionVectorReject refused_reason = REJECT_COUNT;
      // The marker texels got their camera motion before the frame's first alpha blended motion vector draw
      bool markers_filled = false;
      // The immediate context's bindings as the game last bound them (see "PatchedDraws::BoundStates"); the jitter buffer's at
      // "jitter_slot" ("BindJitter")
      BoundStates bound_states;
      // CPU copies of the vc4 buffers the motion vector draws bind (see "PatchedDraws::ObjectConstants"). Not always from one thread
      // (a D3D9 Reset maps from the game's window thread). Restarted at present when motion vectors restart.
      PatchedDraws::ObjectConstants constants;
      // Previous frame constants of the motion vector draws (see "PatchedDraws::PreviousConstants")
      PatchedDraws::PreviousConstants previous_constants;
      PatchedDraws::FrameObjects<MotionVectorObject, PatchedDraws::KeyHash> objects;
      // The frame's camera (vc4 of its first scene depth writing draw of the "frame_camera" kind) and the previous frame's
      ConstantsCopy camera;
      ConstantsCopy previous_camera;
      // Current clip space to the previous frame's (column vectors), identity without a previous camera: the fill's, and the world view
      // projection of a draw without a previous one
      Math::Matrix44D reprojection;
      uint32_t frame_index = 0;              // The Luma frame index of the last motion vector frame
      std::vector<uint8_t> camera_only_copy; // An unmatched draw's constants with last frame's camera (reused)
      // Off (a development A/B): the draws keep their own constants with last frame's camera, no object motion
      bool object_motion = true;
#if DEVELOPMENT
      MotionVectorStats stats, last_stats;
      MotionVectorReject draw_reject = REJECT_COUNT; // The current draw's refusal ("REJECT_COUNT" for none), for the MCP trace note
#endif

      // The motion vector target and the device depth the fill writes, recreated at their next use
      void ReleaseMotionVectorTargets()
      {
         texture.reset();
         rtv.reset();
         srv.reset();
         uav.reset();
         device_depth.reset();
         device_depth_uav.reset();
         ReleaseReactiveMasks();
      }

      // The reactive masks and what the draws wrote for them, recreated at their next use
      void ReleaseReactiveMasks()
      {
         reactive.reset();
         reactive_uav.reset();
         transparency.reset();
         transparency_uav.reset();
         reactive_previous.reset();
         reactive_previous_uav.reset();
         reactive_target.reset();
         reactive_target_rtv.reset();
         reactive_target_srv.reset();
      }
   };

   // The tracked bindings the "PatchedDraws::BoundStates" events update: the immediate context's. "get" gives the game's state of a
   // device (null if it has none yet).
   template <State* (*get)(reshade::api::device*)>
   BoundStates* GetBoundStates(reshade::api::command_list* cmd_list)
   {
      State* const state = get(cmd_list->get_device());
      const CommandListData* const cmd_list_data = cmd_list->get_private_data<CommandListData>();
      return ((state && cmd_list_data && cmd_list_data->is_primary) ? &state->bound_states : nullptr);
   }

   // Motion vectors: a registered vc4 buffer mapped for a whole rewrite, remembered until its Unmap
   template <State* (*get)(reshade::api::device*)>
   void OnMapBufferRegion(reshade::api::device* device, reshade::api::resource resource, uint64_t offset, uint64_t size, reshade::api::map_access access, void** data)
   {
      State* const state = get(device);
      if (!state || !state->active || !data || !*data)
         return;
      const PatchedDraws::ObjectConstants::Entry* const registered = state->constants.OnMap(resource.handle, offset, access, *data);
#if DEVELOPMENT
      if (registered && !registered->mapped)
      {
         state->stats.other_maps++; // A partial or appending write: the copies would miss it
      }
#endif
   }

   // Motion vectors: the CPU copy of a vc4 buffer, before its Unmap (the game has written it). Reads the mapped memory back.
   template <State* (*get)(reshade::api::device*)>
   void OnUnmapBufferRegion(reshade::api::device* device, reshade::api::resource resource)
   {
      State* const state = get(device);
      if (!state || !state->active)
         return;
      if (state->constants.OnUnmap(resource.handle))
      {
#if DEVELOPMENT
         state->stats.maps++;
#endif
      }
   }

   // Motion vectors: the CPU copy of a vc4 buffer from an UpdateSubresource (before it runs). Needs the unsigned "full add-on support"
   // ReShade (the signed build doesn't raise the event).
   template <State* (*get)(reshade::api::device*)>
   bool OnUpdateBufferRegion(reshade::api::device* device, const void* data, reshade::api::resource resource, uint64_t offset, uint64_t size)
   {
      State* const state = get(device);
      if (!state || !state->active || !data)
         return false;
      if (state->constants.OnUpdate(resource.handle, data, offset, size))
      {
#if DEVELOPMENT
         state->stats.updates++;
#endif
      }
      return false;
   }

   template <State* (*get)(reshade::api::device*)>
   void SetEvents(bool enable)
   {
      const auto set = [enable]<reshade::addon_event event>(typename reshade::addon_event_traits<event>::decl callback)
      {
         if (enable)
         {
            reshade::register_event<event>(callback);
         }
         else
         {
            reshade::unregister_event<event>(callback);
         }
      };
      // The motion vector target in every blend state (and dgVoodoo's impossible per-target blends repaired), the tracked bindings
      // (always current, so the first motion vector frame finds them) and the vc4 copies
      set.template operator()<reshade::addon_event::create_pipeline>(PatchedDraws::OnCreateBlendState<target_slot, reactive_slot, true, true>);
      set.template operator()<reshade::addon_event::bind_pipeline>(PatchedDraws::OnBindPipeline<GetBoundStates<get>>);
      set.template operator()<reshade::addon_event::push_descriptors>(PatchedDraws::OnPushConstantBuffers<GetBoundStates<get>>);
      set.template operator()<reshade::addon_event::bind_vertex_buffers>(PatchedDraws::OnBindVertexBuffers<GetBoundStates<get>>);
      set.template operator()<reshade::addon_event::bind_index_buffer>(PatchedDraws::OnBindIndexBuffer<GetBoundStates<get>>);
      // The vc4 copies' events stay registered too: ReShade changes its callback lists without a lock, and dgVoodoo presents from
      // another thread than the frame's (registering them at present would race the frame's events). They return at once while
      // "State::active" is off.
      set.template operator()<reshade::addon_event::map_buffer_region>(OnMapBufferRegion<get>);
      set.template operator()<reshade::addon_event::unmap_buffer_region>(OnUnmapBufferRegion<get>);
      set.template operator()<reshade::addon_event::update_buffer_region>(OnUpdateBufferRegion<get>);
   }

   template <State* (*get)(reshade::api::device*)>
   void RegisterEvents()
   {
      SetEvents<get>(true);
   }

   template <State* (*get)(reshade::api::device*)>
   void UnregisterEvents()
   {
      SetEvents<get>(false);
   }

   // A refused "DrawWithMotionVectors": always false
   inline bool Refuse([[maybe_unused]] State* state, [[maybe_unused]] MotionVectorReject reason)
   {
#if DEVELOPMENT
      state->stats.rejected[reason]++;
      state->draw_reject = reason;
#endif
      return false;
   }

   // A matrix of 4 rows (16 floats) from constants
   inline Math::Matrix44D ReadMatrix(const std::vector<uint8_t>& constants, UINT offset)
   {
      Math::Matrix44D matrix;
      std::copy_n(reinterpret_cast<const float*>(constants.data() + offset), 16, matrix.GetData());
      return matrix;
   }

   inline void WriteMatrix(std::vector<uint8_t>* constants, UINT offset, const Math::Matrix44D& matrix)
   {
      float* const destination = reinterpret_cast<float*>(constants->data() + offset);
      for (int i = 0; i < 16; i++)
      {
         destination[i] = float(matrix.GetData()[i]);
      }
   }

   // A view matrix' 4th row isn't read from the constants: affine
   inline Math::Matrix44D ReadAffineMatrix(const std::vector<uint8_t>& constants, UINT offset)
   {
      Math::Matrix44D matrix = ReadMatrix(constants, offset);
      matrix.GetData()[12] = matrix.GetData()[13] = matrix.GetData()[14] = 0.0;
      matrix.GetData()[15] = 1.0;
      return matrix;
   }

   // The frame camera's clip space transform: its product, or the projection of its view
   template <typename Game>
   Math::Matrix44D ReadFrameViewProjection(const std::vector<uint8_t>& constants)
   {
      static_assert(Game::frame_camera.type == CameraType::VIEW_PROJECTION || Game::frame_camera.type == CameraType::VIEW_AND_PROJECTION);
      if constexpr (Game::frame_camera.type == CameraType::VIEW_PROJECTION)
         return ReadMatrix(constants, Game::frame_camera.row * 16);
      else
         return ReadMatrix(constants, Game::frame_camera.projection_row * 16) * ReadAffineMatrix(constants, Game::frame_camera.row * 16);
   }

   // A game pixel shader's or the bound vertex shader's patched version (null if refused), patched from Core's bytecode copy on first
   // use (null if it can't be, e.g. a vertex shader that doesn't place vertices with a camera). The maps' nodes are never erased.
   template <typename Game, typename T>
   const PatchedShader<T>& GetPatchedShader(State* state, ID3D11Device* native_device, DeviceData& device_data, uint32_t hash, reshade::api::pipeline pipeline,
      PixelShaderPatch patch = PixelShaderPatch::MOTION_VECTORS)
   {
      constexpr bool vertex = std::is_same_v<T, ID3D11VertexShader>;
      auto* const shaders = [&]
      {
         if constexpr (vertex)
            return &state->vertex_shaders;
         else
            return &state->patched_pixel_shaders[size_t(patch)];
      }();
      {
         const std::shared_lock lock(state->mutex);
         if (const auto it = shaders->find(hash); it != shaders->end())
            return it->second;
      }
#if DEVELOPMENT
      const auto create_start = std::chrono::steady_clock::now();
#endif
      std::vector<uint8_t> patched;
      std::string error; // Written by a refusal only
      PatchedShader<T> record;
      {
         const std::shared_lock lock(s_mutex_generic);
         if (const auto it = device_data.pipeline_cache_by_pipeline_handle.find(pipeline.handle); it != device_data.pipeline_cache_by_pipeline_handle.end() && it->second->subobjects_cache)
         {
            const auto* desc = static_cast<const reshade::api::shader_desc*>(it->second->subobjects_cache[0].data);
            // On top of the shader as created (the game's "PatchShaderBytecodeSync" clamps; the pipeline cache keeps the original bytecode)
            const auto sync_patched = device_data.patch_context.GetShaderData(hash);
            const auto* code = (sync_patched ? sync_patched->code.data() : static_cast<const uint8_t*>(desc->code));
            const size_t code_size = (sync_patched ? sync_patched->code.size() : desc->code_size);
            if constexpr (vertex)
            {
               patched = MotionVectorPatch::PatchVertexShader(code, code_size, Game::layout, &error);
               if (!patched.empty())
               {
                  record.read_size = DXBC::ConstantBufferBytes(code, code_size, object_slot);
                  const auto reads = [&](uint32_t row, bool relative_base = false)
                  {
                     return DXBC::ReadsConstantRow(code, code_size, object_slot, row, relative_base);
                  };
                  const auto reads_matrix = [&](uint32_t row, uint32_t rows_read)
                  {
                     for (uint32_t i = 0; i < rows_read; i++)
                     {
                        if (!reads(row + i))
                           return false;
                     }
                     return true;
                  };
                  // The first kind in the game's order that the shader reads
                  for (const Camera& camera : Game::cameras)
                  {
                     const bool two_matrices = camera.type == CameraType::VIEW_AND_PROJECTION || camera.type == CameraType::WORLD_VIEW_AND_PROJECTION;
                     if (!reads_matrix(camera.row, camera.rows_read) || (two_matrices && !reads_matrix(camera.projection_row, camera.rows_read)))
                        continue;
                     record.camera_type = camera.type;
                     record.camera_offset = camera.row * 16;
                     record.projection_offset = (two_matrices ? camera.projection_row * 16 : 0);
                     record.camera_end = (std::max)(record.camera_offset, record.projection_offset) + CAMERA_SIZE;
                     record.frame_camera = camera == Game::frame_camera;
                     break;
                  }
                  // The tie-break transform: the first row the shader reads (a bone palette's first bone indexed relatively, a
                  // world matrix)
                  for (const TieBreakRow& tie_break : Game::tie_break_rows)
                  {
                     if (reads(tie_break.row, tie_break.relative))
                     {
                        record.translation_offset = tie_break.row * 16;
                        break;
                     }
                  }
               }
            }
            else
            {
               patched = Game::PatchPixelShader(patch, code, code_size, &error);
            }
         }
         else
         {
            error = "no bytecode";
         }
      }
      com_ptr<T> shader;
      if (!patched.empty())
      {
         const HRESULT hr = [&]
         {
            if constexpr (vertex)
               return native_device->CreateVertexShader(patched.data(), patched.size(), nullptr, &shader);
            else
               return native_device->CreatePixelShader(patched.data(), patched.size(), nullptr, &shader);
         }();
         if (FAILED(hr))
         {
            error = std::format("create 0x{:08X}", uint32_t(hr));
         }
#if DEVELOPMENT
         else
         {
            Perf::g_shader_creates.AddLuma(std::chrono::steady_clock::now() - create_start);
         }
#endif
      }
      // Every build logs the unexpected failures (bug reports). The expected refusals are a screen space vertex shader and a pixel
      // shader without a clip offered to the coverage patch. Development also logs every patched shader and the other patches' refusals
      // (the game's own patches are expected to refuse).
      const bool expected = error.starts_with("screen space") || error.starts_with("no clip");
      if ((DEVELOPMENT && (shader || patch < PixelShaderPatch::GAME_0)) || (!shader && !expected))
      {
         // A patched shader's note (the coverage's "inward only") follows "patched"
         std::string status = error;
         if (shader)
         {
            status = (error.empty() ? std::string("patched") : "patched, " + error);
         }
         reshade::log::message(((shader || expected) ? reshade::log::level::info : reshade::log::level::warning),
            std::format("[{}] {} 0x{:08X} {}", Game::log_tag, (vertex ? "MV VS" : Game::patch_names[size_t(patch)]), hash, status).c_str());
      }
      record.shader = shader;
      const std::unique_lock lock(state->mutex);
      const auto [entry, inserted] = shaders->try_emplace(hash, std::move(record));
      return entry->second;
   }

   // The bound vertex shader's patched version (null shader if refused), looked up again only when the game's shader changes
   template <typename Game>
   const PatchedShader<ID3D11VertexShader>& GetPatchedVertexShader(State* state, ID3D11Device* native_device, const CommandListData& cmd_list_data, DeviceData& device_data, uint32_t hash)
   {
      if (hash != state->last_vertex_shader_hash || !state->last_vertex_shader)
      {
         state->last_vertex_shader = &GetPatchedShader<Game, ID3D11VertexShader>(state, native_device, device_data, hash, cmd_list_data.pipeline_state_original_vertex_shader);
         state->last_vertex_shader_hash = hash;
      }
      return *state->last_vertex_shader;
   }

   // Classifies the bound blend state (cached in "blend_state"): "blend_opaque" for the motion vectors (additive lights, decals and
   // translucents keep the motion vectors of what's behind them, and so do colorless draws: the occlusion query bounding boxes), and
   // "reactive_blend": NONE not alpha blended (opaque, lights ONE/ONE, shadows DEST_COLOR), ALPHA (SRC_ALPHA / INV_SRC_ALPHA:
   // smoke, glass, water; reactive and transparency & composition), ADDITIVE (SRC_ALPHA / ONE: sparks, glows; reactive)
   inline void ClassifyBoundBlend(ID3D11DeviceContext* native_device_context, State* state)
   {
      ID3D11BlendState* const blend_state = state->bound_states.BlendState(native_device_context);
      if (blend_state == state->blend_state)
         return;
      D3D11_BLEND_DESC blend_desc = CD3D11_BLEND_DESC(D3D11_DEFAULT);
      if (blend_state)
      {
         blend_state->GetDesc(&blend_desc);
      }
      const D3D11_RENDER_TARGET_BLEND_DESC& rt0 = blend_desc.RenderTarget[0];
      state->blend_opaque = rt0.RenderTargetWriteMask != 0 && (!rt0.BlendEnable || (rt0.SrcBlend == D3D11_BLEND_ONE && rt0.DestBlend == D3D11_BLEND_ZERO && rt0.BlendOp == D3D11_BLEND_OP_ADD));
      state->reactive_blend = ((!rt0.BlendEnable || rt0.SrcBlend != D3D11_BLEND_SRC_ALPHA) ? ReactiveBlend::NONE : (rt0.DestBlend == D3D11_BLEND_ONE ? ReactiveBlend::ADDITIVE : ReactiveBlend::ALPHA));
      // Exactly the states whose motion vector target "PatchedDraws::OnCreateBlendState" blends ("blend_alpha_blended_target")
      state->alpha_blended = rt0.BlendEnable && rt0.SrcBlend == D3D11_BLEND_SRC_ALPHA && rt0.DestBlend == D3D11_BLEND_INV_SRC_ALPHA && rt0.BlendOp == D3D11_BLEND_OP_ADD;
      state->blend_state = blend_state;
   }

   // The common half of a game's scene opening, once the game accepted the scene's targets: the scene's depth and size, last frame's
   // camera and objects rolled over (unless frames without a scene came between) and the jitter ("jitter_px", +y down) the whole scene
   // draws with, written to the jitter buffer. False if that failed: no jitter (the game resets what depends on it).
   inline bool BeginFrame(State* state, ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, ID3D11DepthStencilView* dsv, const uint2& depth_size, const std::array<float, 2>& jitter_px)
   {
      state->scene_open = true;
      const bool same_size = state->scene_size == depth_size;
      state->scene_size = depth_size;
      state->depth.reset();
      dsv->GetResource(&state->depth);
      state->scene_color.reset();
      state->scene_rtv.reset();
      state->second_scene_color.reset();
      state->second_scene_rtv.reset();
      state->jitter_dsv = nullptr;
      state->bound_states.ResetDepthTest();
      state->accepted_dsv = nullptr;
      state->refused_rtv = nullptr;
      state->refused_dsv = nullptr;
      state->bound_states.jitter_bound = false;
      state->blend_state = nullptr;
      state->blend_opaque = true;
      state->alpha_blended = false;
      state->reactive_blend = ReactiveBlend::NONE;
      // Last frame's camera and objects are the previous ones, unless frames without a scene (menus, videos) came between
      const bool previous_valid = state->camera && same_size && cb_luma_global_settings.FrameIndex - state->frame_index <= 1;
      state->previous_camera = (previous_valid ? state->camera : nullptr);
      state->camera = nullptr;
      state->frame_index = cb_luma_global_settings.FrameIndex;
#if DEVELOPMENT
      // Objects without a transform match by their draw key alone: any twin has the same vertices
      state->stats.tiebreak_collisions = PatchedDraws::CountTieBreakCollisions(state->objects.Current(), [](const auto& a, const auto& b)
         { return a.transform == PatchedDraws::ObjectTransform{} || PatchedDraws::SameBytes(a.constants, b.constants); });
#endif
      state->objects.EndFrame(previous_valid);
      // Pixels to NDC (y up)
      state->jitter = jitter_px;
      state->jitter_ndc = {state->jitter[0] * 2.f / float(depth_size.x), state->jitter[1] * -2.f / float(depth_size.y)};
      const float ndc_jitter[4] = {state->jitter_ndc[0], state->jitter_ndc[1], 0.f, 0.f};
      if (!PatchedDraws::WriteDynamicConstants(native_device, native_device_context, std::addressof(state->jitter_buffer), ndc_jitter, sizeof(ndc_jitter)))
      {
         // No stale jitter on the scene draws either: no motion vectors this frame
         state->jitter = {};
         state->jitter_ndc = {};
         state->jitter_buffer.reset();
         return false;
      }
      return true;
   }

   // The jitter buffer, bound once per scene (no translated shader reads its slot, Luma draws nothing during the scene; a game bind over
   // it resets "jitter_bound", see "PatchedDraws::OnPushConstantBuffers")
   inline void BindJitter(ID3D11DeviceContext* native_device_context, State* state)
   {
      if (std::exchange(state->bound_states.jitter_bound, true))
         return;
      ID3D11Buffer* const jitter = state->jitter_buffer.get();
      native_device_context->VSSetConstantBuffers(jitter_slot, 1, &jitter);
   }

   // What the next frame keeps of a draw's vc4: only the bytes its vertex shader reads (at least its camera and tie-break transform),
   // copied from the buffer's latest upload. A draw reading all of it (skinned) shares the entry's one copy. Null if the upload doesn't
   // hold the camera yet.
   inline ConstantsCopy SnapshotConstants(State* state, PatchedDraws::ObjectConstants::Entry* entry, const PatchedShader<ID3D11VertexShader>& vertex_shader)
   {
      if (entry->latest.size() < vertex_shader.camera_end)
         return nullptr;
      const size_t min_size = (std::max)(size_t(vertex_shader.camera_end), vertex_shader.translation_offset != 0 ? vertex_shader.translation_offset + sizeof(PatchedDraws::ObjectTransform) : 0);
      return state->constants.Snapshot(*entry, vertex_shader.read_size, min_size);
   }

   // A scene draw's vc4 buffer, registered for copies at every Map and Update, and its entry (null without an upload holding the
   // draw's camera), for a vertex shader with a camera. The frame's camera and the reprojection are taken here, at the first scene
   // draw of the "frame_camera" kind.
   struct SceneDrawConstants
   {
      ID3D11Buffer* buffer = nullptr;
      PatchedDraws::ObjectConstants::Entry* entry = nullptr;
   };
   template <typename Game>
   SceneDrawConstants ReadSceneDrawConstants(ID3D11DeviceContext* native_device_context, State* state, const PatchedShader<ID3D11VertexShader>& vertex_shader)
   {
      SceneDrawConstants result;
      result.buffer = state->bound_states.ObjectBuffer(native_device_context);
      if (!result.buffer)
         return result;
      PatchedDraws::ObjectConstants::Entry& entry = state->constants.Register(result.buffer);
      // An upload from before the hooks paused is stale (the draw stays uncopied until the next one)
      if (const auto* const latest = state->constants.Latest(entry); !latest || latest->size() < vertex_shader.camera_end)
         return result;
      result.entry = &entry;
      // Any scene draw of the frame camera's kind: the prepass and the base passes share the camera, and the prepass' draws reach here
      // only once they are jittered (after the upscaler's first frame, which needs this camera for its fill)
      // A world view and projection shader takes it too when the frame camera is a view and projection: its draw holds the view in its
      // vc4 rows as well, and the statics' prepass draws may come before the first skinned one
      bool takes_frame_camera = vertex_shader.frame_camera;
      if constexpr (Game::frame_camera.type == CameraType::VIEW_AND_PROJECTION)
      {
         takes_frame_camera |= vertex_shader.camera_type == CameraType::WORLD_VIEW_AND_PROJECTION && vertex_shader.projection_offset == Game::frame_camera.projection_row * 16;
      }
      if (!state->camera && takes_frame_camera)
      {
         state->camera = SnapshotConstants(state, &entry, vertex_shader);
         // Previous clip space from the current: previous * inverse(current), in double (absolute world translation)
         state->reprojection.SetIdentity();
         if (state->previous_camera)
         {
            Math::Matrix44D current = ReadFrameViewProjection<Game>(*state->camera);
            const Math::Matrix44D previous = ReadFrameViewProjection<Game>(*state->previous_camera);
            current.Invert();
            state->reprojection = previous * current;
         }
      }
      return result;
   }

   // Draws an opaque or alpha blended draw into the scene color (alone, with the scene depth) with the patched shaders, adding the
   // motion vector target ("target_slot", past the game's) and the previous frame's vc4 ("previous_slots"). False if it can't (the
   // draw then goes to "DrawWithJitter").
   template <typename Game>
   bool DrawWithMotionVectors(State* state, ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, const CommandListData& cmd_list_data, DeviceData& device_data,
      const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes, const std::function<void()>& draw, const com_ptr<ID3D11RenderTargetView> (&rtvs)[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT], ID3D11DepthStencilView* dsv)
   {
      // The scene target alone, plus the motion vector and mask targets the last patched draws left bound
      for (UINT slot = 1; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; slot++)
      {
         if (rtvs[slot] && (slot != target_slot || rtvs[slot] != state->rtv) && (slot != reactive_slot || rtvs[slot] != state->reactive_target_rtv))
            return Refuse(state, REJECT_EXTRA_TARGET);
      }
      if (!rtvs[0] || !dsv || !state->scene_open)
         return Refuse(state, REJECT_NO_SCENE);
      if (!state->bound_states.DepthTested(native_device_context))
         return Refuse(state, REJECT_DEPTH_TEST);
      ClassifyBoundBlend(native_device_context, state);
      // Alpha blended draws (SRC_ALPHA / INV_SRC_ALPHA) take their own motion vectors too, blended by their alpha: the particles are
      // such draws. Additive ones and the other blends keep what's behind them.
      const bool blended = state->alpha_blended;
      if ((!state->blend_opaque && !blended) || (blended && Game::IsExcludedPixelShader(uint32_t(original_shader_hashes.pixel_shaders[0]))))
         return Refuse(state, REJECT_BLEND);
      // Known targets: checked, and the motion vector target built for them (the depth prepass' R32F targets are refused here)
      constexpr bool second_scene_color = []
      {
         if constexpr (requires { Game::second_scene_color; })
            return Game::second_scene_color;
         else
            return false;
      }();
      if ((rtvs[0] != state->scene_rtv && (!second_scene_color || rtvs[0] != state->second_scene_rtv)) || dsv != state->accepted_dsv)
      {
         if (rtvs[0].get() == state->refused_rtv && dsv == state->refused_dsv)
            return Refuse(state, state->refused_reason);
         const auto refuse = [&](MotionVectorReject reason)
         {
            state->refused_rtv = rtvs[0].get();
            state->refused_dsv = dsv;
            state->refused_reason = reason;
            return Refuse(state, reason);
         };
         com_ptr<ID3D11Resource> depth;
         dsv->GetResource(&depth);
         com_ptr<ID3D11Resource> color;
         rtvs[0]->GetResource(&color);
         const bool second = state->scene_color && color != state->scene_color;
         if (depth != state->depth || !color || (second && (!second_scene_color || (state->second_scene_color && color != state->second_scene_color))))
            return refuse(REJECT_OTHER_DEPTH_COLOR);
         D3D11_RENDER_TARGET_VIEW_DESC rtv_desc;
         rtvs[0]->GetDesc(&rtv_desc);
         D3D11_TEXTURE2D_DESC color_desc = {};
         if (com_ptr<ID3D11Texture2D> color_texture; SUCCEEDED(color->QueryInterface(&color_texture)))
         {
            color_texture->GetDesc(&color_desc);
         }
         // The upscaler reads the scene: the game's test includes a shader resource view. Filtered by the resource, not the view
         // (dgVoodoo binds single-slice array views). A multisampled scene is refused here only on MSAA's first frame (the game's
         // MSAA flag then stops this path).
         if (!Game::AcceptSceneColor(rtv_desc, color_desc))
            return refuse(REJECT_FORMAT);
         const uint2 size = {color_desc.Width, color_desc.Height};
         if (size != state->scene_size)
            return refuse(REJECT_SIZE);
         const std::unique_lock lock(state->mutex);
         D3D11_TEXTURE2D_DESC desc = {};
         if (state->texture)
         {
            state->texture->GetDesc(&desc);
         }
         // R16G16_FLOAT: FSR keeps 16 bits internally; the error is under 0.1% of the motion (BL GOTY)
         constexpr DXGI_FORMAT format = DXGI_FORMAT_R16G16_FLOAT;
         if (desc.Width != size.x || desc.Height != size.y)
         {
            state->ReleaseMotionVectorTargets();
            // The fill reads the target back through its UAV
            D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support = {format};
            const bool typed_uav_load = SUCCEEDED(native_device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support))) && (support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
            desc = CD3D11_TEXTURE2D_DESC(format, size.x, size.y, 1, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | (typed_uav_load ? D3D11_BIND_UNORDERED_ACCESS : 0u));
            if (FAILED(SRBridge::CreateSharableTexture(native_device, desc, &state->texture)) || FAILED(native_device->CreateRenderTargetView(state->texture.get(), nullptr, &state->rtv)))
            {
               state->texture.reset();
               state->rtv.reset();
               return Refuse(state, REJECT_CREATE);
            }
            if (typed_uav_load)
            {
               native_device->CreateUnorderedAccessView(state->texture.get(), nullptr, &state->uav);
            }
            native_device->CreateShaderResourceView(state->texture.get(), nullptr, &state->srv);
            const CD3D11_TEXTURE2D_DESC depth_desc(DXGI_FORMAT_R32_FLOAT, size.x, size.y, 1, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
            if (SUCCEEDED(SRBridge::CreateSharableTexture(native_device, depth_desc, &state->device_depth)))
            {
               native_device->CreateUnorderedAccessView(state->device_depth.get(), nullptr, &state->device_depth_uav);
            }
            state->frame_ended = true;
         }
         if (second)
         {
            state->second_scene_color = color;
            state->second_scene_rtv = rtvs[0];
         }
         else
         {
            state->scene_color = color;
            state->scene_rtv = rtvs[0];
         }
         state->accepted_dsv = dsv;
      }

      const auto& vertex_shader = GetPatchedVertexShader<Game>(state, native_device, cmd_list_data, device_data, original_shader_hashes.vertex_shaders[0]);
      // The alpha blended draws write no masks: they write their own motion vectors, and FSR's reactive and transparency & composition
      // masks are for content without them (AMD's FSR manual)
      ID3D11PixelShader* pixel_shader = nullptr;
      if (blended)
      {
         pixel_shader = GetPatchedShader<Game, ID3D11PixelShader>(state, native_device, device_data, original_shader_hashes.pixel_shaders[0], cmd_list_data.pipeline_state_original_pixel_shader, PixelShaderPatch::BLENDED).shader.get();
      }
      else
      {
         if (const uint32_t pixel_shader_hash = original_shader_hashes.pixel_shaders[0]; pixel_shader_hash != state->last_pixel_shader_hash)
         {
            state->last_pixel_shader = GetPatchedShader<Game, ID3D11PixelShader>(state, native_device, device_data, pixel_shader_hash, cmd_list_data.pipeline_state_original_pixel_shader).shader.get();
            state->last_pixel_shader_hash = pixel_shader_hash;
         }
         pixel_shader = state->last_pixel_shader;
      }
      if (!vertex_shader.shader || !pixel_shader || !state->jitter_buffer)
         return Refuse(state, REJECT_SHADERS);
      // A shader without a camera has no motion to rebuild
      if (vertex_shader.camera_offset == 0)
         return Refuse(state, REJECT_NO_CAMERA);
      // The game's vc4 (object, camera and bones in one). The slots added past it stay bound after the draw: no translated shader reads
      // a constant buffer past b4.
      const SceneDrawConstants scene_constants = ReadSceneDrawConstants<Game>(native_device_context, state, vertex_shader);
      if (std::exchange(state->frame_ended, false))
      {
         state->markers_filled = false;
         state->fill_pending = state->uav && state->device_depth_uav && Game::CanFill(device_data);
         // The fill's marker: the largest float16 (a larger clear value is stored as it in R16G16_FLOAT)
         const FLOAT clear_value = (state->fill_pending ? 65504.f : 0.f);
         const FLOAT clear[4] = {clear_value, clear_value, 0.f, 0.f};
         native_device_context->ClearRenderTargetView(state->rtv.get(), clear);
         // The reactive masks: what the alpha blended draws write, from zero
         if (Game::ReactiveMaskUsed(device_data) && !state->reactive_target)
         {
            D3D11_TEXTURE2D_DESC mv_desc;
            state->texture->GetDesc(&mv_desc);
            const CD3D11_TEXTURE2D_DESC reactive_desc(DXGI_FORMAT_R8G8_UNORM, mv_desc.Width, mv_desc.Height, 1, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            const std::unique_lock lock(state->mutex);
            if (SUCCEEDED(native_device->CreateTexture2D(&reactive_desc, nullptr, &state->reactive_target)) && SUCCEEDED(native_device->CreateRenderTargetView(state->reactive_target.get(), nullptr, &state->reactive_target_rtv)))
            {
               native_device->CreateShaderResourceView(state->reactive_target.get(), nullptr, &state->reactive_target_srv);
            }
            state->sr_reactive_frame = cb_luma_global_settings.FrameIndex;
         }
         if (state->reactive_target_rtv)
         {
            const FLOAT zero[4] = {};
            native_device_context->ClearRenderTargetView(state->reactive_target_rtv.get(), zero);
         }
      }

      // The buffer's copy as of this draw (null until its first upload)
      ConstantsCopy constants;
      if (scene_constants.entry)
      {
         constants = SnapshotConstants(state, scene_constants.entry, vertex_shader);
      }
      // The previous frame's vc4: the same object's from last frame, else this draw's with last frame's camera (no object motion).
      // None (no CPU copy yet, another camera): the current one (zero motion).
      const std::vector<uint8_t>* upload = nullptr;
      bool object_matched = false;
      if (constants)
      {
         // Draw key: same mesh, same shaders, no instance count. The offsets go with the buffers: queried with them, else tracked
         // with them.
         UINT vertex_offset, index_offset;
         ID3D11Buffer* const vertex_buffer = state->bound_states.VertexBuffer(native_device_context, &vertex_offset);
         ID3D11Buffer* const index_buffer = state->bound_states.IndexBuffer(native_device_context, &index_offset);
#if DEVELOPMENT
         if (state->bound_states.check && state->bound_states.tracked && (vertex_offset != state->bound_states.vertex_offset || index_offset != state->bound_states.index_offset))
         {
            state->stats.bound_state_mismatches++;
         }
#endif
         const DrawDispatchData& draw_data = last_draw_dispatch_data;
         uint64_t key = 0;
         for (const uint64_t value : {uint64_t(original_shader_hashes.vertex_shaders[0]), uint64_t(original_shader_hashes.pixel_shaders[0]), reinterpret_cast<uint64_t>(vertex_buffer), uint64_t(vertex_offset),
                 reinterpret_cast<uint64_t>(index_buffer), uint64_t(index_offset), uint64_t(draw_data.index_count), uint64_t(draw_data.first_index), uint64_t(uint32_t(draw_data.vertex_offset)),
                 uint64_t(draw_data.vertex_count), uint64_t(draw_data.first_vertex)})
         {
            HashCombine(key, value);
         }
         // The tie-break between the objects sharing a key (props, characters): the 3x4 transform at the shader's translation row (the
         // first bone, the world matrix; see "PatchedDraws::ObjectTransform"), copied as it is. The effects without a world matrix have
         // none: their draws keep their own constants with last frame's camera (static).
         PatchedDraws::ObjectTransform transform = {};
         const bool untransformed = vertex_shader.translation_offset == 0;
         if (!untransformed)
         {
            std::memcpy(transform.data(), constants->data() + vertex_shader.translation_offset, sizeof(transform));
         }
#if DEVELOPMENT
         state->stats.untransformed += untransformed;
#endif

         // The same object at the same transform by its draw order or the exact index, else a linear search among the key's candidates
         const MotionVectorObject* match = nullptr;
         if (!untransformed)
         {
            match = state->objects.FindNearest(key, transform, [&](const auto& candidate)
               { return candidate.constants->size() == constants->size(); });
         }
         constexpr UINT frame_matrix_offset = Game::frame_camera.row * 16;
         constexpr UINT frame_projection_offset = Game::frame_camera.projection_row * 16;
         const auto same_rows = [&](UINT offset)
         { return std::memcmp(constants->data() + offset, state->camera->data() + offset, CAMERA_SIZE) == 0; };
         // Drawn with the frame's camera (a world view projection shader's can't be told)
         bool frame_camera = false;
         if (state->camera)
         {
            switch (vertex_shader.camera_type)
            {
            case CameraType::VIEW_PROJECTION:
               frame_camera = vertex_shader.frame_camera && same_rows(vertex_shader.camera_offset);
               break;
            case CameraType::WORLD_VIEW_PROJECTION:
               frame_camera = true;
               break;
            case CameraType::VIEW_AND_PROJECTION:
               frame_camera = vertex_shader.frame_camera && same_rows(vertex_shader.camera_offset) && same_rows(vertex_shader.projection_offset);
               break;
            case CameraType::WORLD_VIEW_AND_PROJECTION:
               // Its world view is rebuilt from the frame camera's view: the frame camera has one, and the projection is its
               frame_camera = Game::frame_camera.type == CameraType::VIEW_AND_PROJECTION && vertex_shader.projection_offset == frame_projection_offset && same_rows(vertex_shader.projection_offset);
               break;
            }
         }
         if (match && state->object_motion)
         {
            // Last frame's list outlives the draw (the previous objects only change at the next frame start)
            upload = &*match->constants;
            object_matched = true;
#if DEVELOPMENT
            state->stats.matched++;
#endif
         }
         else if (state->previous_camera && frame_camera)
         {
            // Not found, drawn with the frame's camera: its own constants with last frame's camera (camera motion only). A view
            // projection takes the previous one; a world view projection (a static object's) previous * inverse(current) * itself;
            // a view and a projection take the previous ones; a world view takes previous view * inverse(current view) * itself,
            // and the previous projection.
            state->camera_only_copy.assign(constants->begin(), constants->end());
            switch (vertex_shader.camera_type)
            {
            case CameraType::VIEW_PROJECTION:
               std::memcpy(state->camera_only_copy.data() + vertex_shader.camera_offset, state->previous_camera->data() + vertex_shader.camera_offset, CAMERA_SIZE);
               break;
            case CameraType::WORLD_VIEW_PROJECTION:
               WriteMatrix(&state->camera_only_copy, vertex_shader.camera_offset, state->reprojection * ReadMatrix(*constants, vertex_shader.camera_offset));
               break;
            case CameraType::VIEW_AND_PROJECTION:
               std::memcpy(state->camera_only_copy.data() + vertex_shader.camera_offset, state->previous_camera->data() + vertex_shader.camera_offset, CAMERA_SIZE);
               std::memcpy(state->camera_only_copy.data() + vertex_shader.projection_offset, state->previous_camera->data() + vertex_shader.projection_offset, CAMERA_SIZE);
               break;
            case CameraType::WORLD_VIEW_AND_PROJECTION:
               if constexpr (Game::frame_camera.type == CameraType::VIEW_AND_PROJECTION)
               {
                  Math::Matrix44D current_view = ReadAffineMatrix(*state->camera, frame_matrix_offset);
                  current_view.Invert();
                  const Math::Matrix44D previous_view = ReadAffineMatrix(*state->previous_camera, frame_matrix_offset);
                  WriteMatrix(&state->camera_only_copy, vertex_shader.camera_offset, previous_view * current_view * ReadAffineMatrix(*constants, vertex_shader.camera_offset));
                  std::memcpy(state->camera_only_copy.data() + vertex_shader.projection_offset, state->previous_camera->data() + vertex_shader.projection_offset, CAMERA_SIZE);
               }
               break;
            }
            upload = &state->camera_only_copy;
#if DEVELOPMENT
            state->stats.camera_only++;
#endif
         }
#if DEVELOPMENT
         else
         {
            state->stats.other_camera++;
         }
#endif
         // Kept as drawn for the next frame
         // Untransformed draws are never looked up
         state->objects.Push(key, {.transform = transform, .constants = std::move(constants)}, !untransformed);
      }
#if DEVELOPMENT
      else
      {
         state->stats.uncopied++;
      }
#endif
      // An alpha blended draw blends its motion vector with what's under it: texels no opaque motion vector draw wrote still hold the
      // fill's marker (65504), which the blend would turn into a huge motion. The game's fill gives them their camera motion first.
      if constexpr (requires { Game::FillMarkersBeforeBlend(native_device, native_device_context, device_data); })
      {
         if (blended && state->fill_pending && !std::exchange(state->markers_filled, true))
         {
            DrawStateStack<DrawStateStackType::FullGraphics> graphics_state;
            DrawStateStack<DrawStateStackType::Compute> compute_state;
            graphics_state.Cache(native_device_context, device_data.uav_max_count);
            compute_state.Cache(native_device_context, device_data.uav_max_count);
            Game::FillMarkersBeforeBlend(native_device, native_device_context, device_data);
            compute_state.Restore(native_device_context);
            graphics_state.Restore(native_device_context);
         }
      }
      ID3D11Buffer* const previous_current[] = {scene_constants.buffer};
      state->previous_constants.Bind(native_device, native_device_context, Dg::previous_slots, {&upload, 1}, previous_current, Game::log_tag, {&vertex_shader.read_size, 1});
      BindJitter(native_device_context, state);
      // Left bound after the draw (set directly, bypassing Core's state tracking): the game's next draws either bind their own targets
      // and shaders, or are motion vector draws too. The game's pixel shaders write o0 only, so the motion vector target keeps its
      // contents; a mask target left by an earlier draw goes (its max blend would take an undefined value).
      if (rtvs[target_slot] != state->rtv || rtvs[reactive_slot] != nullptr)
      {
         ID3D11RenderTargetView* targets[target_slot + 1] = {rtvs[0].get()};
         targets[target_slot] = state->rtv.get();
         native_device_context->OMSetRenderTargets(target_slot + 1, targets, dsv);
      }
      // An unmatched alpha blended draw (particles: their vertices are rewritten every frame, so no draw key repeats) has the camera
      // motion only, not its own: it also writes the reactive mask, as its motion vectors are wrong (AMD's FSR manual). Matched ones
      // (hair) move right and stay unmasked.
      ID3D11RenderTargetView* mask_targets[reactive_slot + 1] = {};
      if constexpr (requires { Game::blended_mask_patch; Game::MaskUnmatchedBlended(device_data); })
      {
         if (blended && !object_matched && state->reactive_target_rtv && Game::MaskUnmatchedBlended(device_data))
         {
            if (ID3D11PixelShader* const mask_shader = GetPatchedShader<Game, ID3D11PixelShader>(state, native_device, device_data, original_shader_hashes.pixel_shaders[0], cmd_list_data.pipeline_state_original_pixel_shader, Game::blended_mask_patch).shader.get())
            {
               pixel_shader = mask_shader;
               mask_targets[0] = rtvs[0].get();
               mask_targets[target_slot] = state->rtv.get();
               mask_targets[reactive_slot] = state->reactive_target_rtv.get();
               native_device_context->OMSetRenderTargets(reactive_slot + 1, mask_targets, dsv);
            }
         }
      }
      PatchedDraws::BindPatchedShader(native_device_context, vertex_shader.shader.get(), &state->bound_vertex_shader);
      PatchedDraws::BindPatchedShader(native_device_context, pixel_shader, &state->bound_pixel_shader);

      draw();
      if (mask_targets[reactive_slot])
      {
         // No mask bound for the next draws, which don't write it
         native_device_context->OMSetRenderTargets(target_slot + 1, mask_targets, dsv);
      }
#if DEVELOPMENT
      state->stats.motion_vector_draws++;
      state->stats.reactive_draws += mask_targets[reactive_slot] != nullptr;
#endif
      return true;
   }

   // Jitter for the scene's mesh draws without motion vectors (patched vertex shader, game pixel shader): the depth prepass, lights,
   // shadow projections, decals, translucents. Every draw depth tested against the scene takes the same jitter, or jittered and
   // unjittered depths of the same surface fail each other's test. False if it can't (the draw runs untouched).
   template <typename Game>
   bool DrawWithJitter(State* state, ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, const CommandListData& cmd_list_data, DeviceData& device_data,
      const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes, const std::function<void()>& draw, const com_ptr<ID3D11RenderTargetView> (&rtvs)[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT], ID3D11DepthStencilView* dsv)
   {
      if (!state->scene_open || state->jitter == std::array<float, 2>{} || !state->jitter_buffer)
         return false;
      // Meshes only (full screen passes have no vertex buffer or no depth test), into the scene depth (not shadows)
      if (!dsv)
         return false;
      if (dsv != state->jitter_dsv)
      {
         com_ptr<ID3D11Resource> depth;
         dsv->GetResource(&depth);
         state->jitter_dsv = dsv;
         state->jitter_dsv_scene = depth == state->depth;
      }
      if (!state->jitter_dsv_scene || !state->bound_states.DepthTested(native_device_context))
         return false;
      const ID3D11Buffer* const vertex_buffer = state->bound_states.VertexBuffer(native_device_context);
      if (!vertex_buffer)
         return false;
      const auto& vertex_shader = GetPatchedVertexShader<Game>(state, native_device, cmd_list_data, device_data, original_shader_hashes.vertex_shaders[0]);
      if (!vertex_shader.shader)
         return false;
      if (vertex_shader.camera_offset != 0)
      {
         // Takes the frame's camera from the prepass' draws, which come through here
         ReadSceneDrawConstants<Game>(native_device_context, state, vertex_shader);
      }

      // An alpha blended draw into the scene writes its mask, reactive or transparency & composition (its pixel shader patched, the mask
      // target added past the motion vector one)
      ID3D11PixelShader* reactive_shader = nullptr;
      if (Game::ReactiveMaskUsed(device_data) && state->reactive_target_rtv && rtvs[0] && rtvs[0] == state->scene_rtv)
      {
         ClassifyBoundBlend(native_device_context, state);
         if (const ReactiveBlend blend = state->reactive_blend; blend != ReactiveBlend::NONE)
         {
            reactive_shader = GetPatchedShader<Game, ID3D11PixelShader>(state, native_device, device_data, original_shader_hashes.pixel_shaders[0], cmd_list_data.pipeline_state_original_pixel_shader, (blend == ReactiveBlend::ADDITIVE ? PixelShaderPatch::MASK_ADDITIVE : PixelShaderPatch::MASK_ALPHA)).shader.get();
         }
      }

      // The patched vertex shader and the jitter stay bound after the draw (see "DrawWithMotionVectors"), with the game's pixel shader
      // (a motion vector draw's is put back) or its mask version, and the mask target
      PatchedDraws::BindPatchedShader(native_device_context, vertex_shader.shader.get(), &state->bound_vertex_shader);
      BindJitter(native_device_context, state);
      // A mask target left by an earlier draw goes when this one doesn't write it
      if (ID3D11RenderTargetView* const mask_rtv = (reactive_shader ? state->reactive_target_rtv.get() : nullptr); rtvs[reactive_slot].get() != mask_rtv)
      {
         ID3D11RenderTargetView* targets[reactive_slot + 1] = {};
         for (uint32_t slot = 0; slot < reactive_slot; slot++)
         {
            targets[slot] = rtvs[slot].get();
         }
         targets[reactive_slot] = mask_rtv;
         native_device_context->OMSetRenderTargets(reactive_slot + 1, targets, dsv);
      }
      if (reactive_shader)
      {
         PatchedDraws::BindPatchedShader(native_device_context, reactive_shader, &state->bound_pixel_shader);
#if DEVELOPMENT
         state->stats.reactive_draws++;
#endif
      }
      else
      {
         PatchedDraws::RestoreGameShader(native_device_context, &state->bound_pixel_shader);
      }
      draw();
#if DEVELOPMENT
      state->stats.jitter_draws++;
#endif
      return true;
   }

#if DEVELOPMENT
   // The last complete frame's counts for the MCP ("luma_dev_values"), as "<prefix>draws", "<prefix>rejected.<reason>" and so on
   inline void RegisterDevValues(const State& state, const void* owner, const std::string& prefix = "mv.")
   {
      const MotionVectorStats& stats = state.last_stats;
      const std::pair<const char*, const uint32_t*> counters[] = {{"draws", &stats.motion_vector_draws}, {"jitter_draws", &stats.jitter_draws}, {"reactive_draws", &stats.reactive_draws}, {"matched", &stats.matched},
         {"camera_only", &stats.camera_only}, {"other_camera", &stats.other_camera}, {"uncopied", &stats.uncopied}, {"maps", &stats.maps}, {"updates", &stats.updates},
         {"other_maps", &stats.other_maps}, {"sr_draws", &stats.sr_draws}, {"tiebreak_collisions", &stats.tiebreak_collisions}, {"untransformed", &stats.untransformed},
         {"bound_state_mismatches", &stats.bound_state_mismatches}, {"constants_pool", &stats.constants_pool}, {"prepass_drawn", &stats.prepass_drawn}, {"ended_by_hash", &stats.ended_by}};
      for (const auto& [name, value] : counters)
      {
         Mcp::RegisterCounter(prefix + name, value, owner);
      }
      for (size_t i = 0; i < std::size(MOTION_VECTOR_REJECT_NAMES); i++)
      {
         Mcp::RegisterCounter(prefix + "rejected." + MOTION_VECTOR_REJECT_NAMES[i], &stats.rejected[i], owner);
      }
   }
#endif
} // namespace SceneMotionVectors
