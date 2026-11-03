#pragma once

// D3D12 readbacks for the dev MCP ("luma_read_resource", "luma_read_cbuffer"). A readback is a GPU copy
// recorded into the game's command list at the target command, into a READBACK buffer that is read once that list ran. The target is a draw
// or dispatch, or a copy, resolve or clear: the command is re-issued natively right before the copy, and the original call is skipped.
// D3D12 has no state queries and Core doesn't track resource states (ReShade's "barrier" event drops subresources), so only what is
// certain is copied: render targets and unordered access views right after the draw, a depth or stencil plane after a draw whose
// pipeline writes it, what a copy left in COPY_DEST, a resolve in RESOLVE_DEST, a render target clear in RENDER_TARGET and a depth stencil
// clear in DEPTH_WRITE, constant buffers in UPLOAD heaps (always GENERIC_READ), an ExecuteIndirect()'s arguments right before it (INDIRECT_ARGUMENT
// outside UPLOAD heaps). The barriers are legacy ones: textures a game manages
// with enhanced barrier layouts are outside their interop rules.
// Completion never waits for a game queue inside present (the game can have it wait on a fence it signals after present): a copy gets its own
// fence, signaled on its queue at a present, and its buffer is read at a later present once that fence passed ("Advance()"). Only the MCP's
// reads of the back buffer and of registered textures, recorded on a list of Luma's own at present, wait for it (bounded, see "RunReadResource()").

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <d3d12.h>
#include <include/reshade.hpp>

#include <source/com_ptr.hpp>

#include "d3d12_passes.h"

namespace D3D12Readback
{
   enum class Kind : uint8_t
   {
      RenderTarget,
      UnorderedAccess,
      Depth,
      Stencil,
      ConstantBuffer,
      IndirectArguments, // The commands an ExecuteIndirect() reads from its argument buffer (see "D3D12CommandSignatures")
   };

   // What re-issuable copies, resolves and clears do to a resource. Unordered access view clears and uploads aren't here: ReShade passes no
   // GPU descriptor handle or row pitch for them, so they can't be re-issued.
   enum class WriteKind : uint8_t
   {
      CopyResource,
      CopyRegion,
      Resolve,
      ClearRenderTarget,
      ClearDepthStencil,
   };

   // A copy, resolve or clear command as its ReShade event gives it, and as its frame capture entry records it: a target and an event are
   // the same command when they are equal
   struct Write
   {
      uint64_t resource = 0; // What it wrote, null for a draw target
      uint64_t source = 0;   // What a copy or resolve read
      WriteKind kind = WriteKind::CopyResource;
      uint32_t subresource = 0; // Region copies and resolves: the destination subresource
      uint32_t level = 0;       // Clears: the cleared view's mip and first layer
      uint32_t layer = 0;
      uint8_t planes = 0;                               // Depth stencil clears: the cleared planes (bit 0 depth, bit 1 stencil)
      DXGI_FORMAT resolve_format = DXGI_FORMAT_UNKNOWN; // Resolves
      bool operator==(const Write&) const = default;
   };

   // What tells the draws of a pipeline apart across frames and threads (a job system records the same pass on another thread each frame):
   // its outputs and arguments ("D3D12Trace::MakeDrawSignature()"). Swapchain back buffers rotate, so they count as none.
   struct DrawSignature
   {
      uint64_t rtv_resource = 0; // The resources of render target 0 and of the depth stencil, none for compute
      uint64_t dsv_resource = 0;
      bool compute = false;
      std::array<uint32_t, 9> arguments = {};
      bool operator==(const DrawSignature&) const = default;
   };

   // The command a readback copies the output of, recorded on "thread" (any if none): the "instance"th draw since the last present with one of
   // "pipelines" (and the "signature", if set), or, with a "write.resource", the "instance"th copy, resolve or clear equal to "write". Those
   // have no view: "kind" only picks the plane of a depth resource (stencil, else depth).
   struct Target
   {
      std::vector<uint64_t> pipelines;
      std::thread::id thread;
      std::optional<DrawSignature> signature;
      int32_t instance = 0;
      Kind kind = Kind::RenderTarget;
      uint32_t slot = 0; // The render target slot, or the packed UAV or constant buffer entry (as the frame capture lists them)
      // Constant buffers: the "b" register (in "cb_space") whose packed entry is read, in place of "slot"
      std::optional<uint32_t> cb_register;
      uint32_t cb_space = 0;
      Write write;
      uint32_t mip = 0;                         // Resource copies: the mip to read, they wrote all of them
      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // Copies and resolves: how to read the data of a color texture (an SRV's view format), its own if unknown
   };

   struct Copy
   {
      enum class Status : uint8_t
      {
         Recorded,  // In a command list not executed yet
         Executed,  // Its list went to "queue". ReShade's event comes before the native call, "Advance()" checks the token.
         Discarded, // Its list was reset or destroyed unexecuted, the GPU never ran it
         Ready,     // Root constants, read on the CPU
         Failed,    // The target command can't be copied, see "error"
      };
      Status status = Status::Recorded;
      std::string error;
      const reshade::api::command_list* cmd_list = nullptr;
      // The copies recorded on a native list that its recorder has no "reshade::api::command_list" of (the super resolution tap, called with the
      // native list of the game's): its events carry the list, "get_native()" is that list
      uint64_t native_list = 0;
      com_ptr<ID3D12CommandQueue> queue; // Where its list went (kept alive for the fence, games can release their queues)

      com_ptr<ID3D12Resource> buffer;   // READBACK, in COPY_DEST for its whole life
      com_ptr<ID3D12Resource> resolved; // Multisampled render targets are resolved into it first
      uint64_t token = 0;               // The GPU copies it into the buffer's last 8 bytes, proof that the list ran
      uint64_t token_offset = 0;

      // Completion, see "Advance()": "fence" is signaled on "queue" up to "fence_value", "signals" times
      com_ptr<ID3D12Fence> fence;
      uint64_t fence_value = 0;
      uint32_t signals = 0;

      // Textures: one mip of the layers the view covers, each subresource laid out by its footprint
      D3D12_RESOURCE_DESC desc = {}; // The source texture's (the data of multisampled ones is resolved)
      uint64_t texture = 0;          // The source texture's "ID3D12Resource" (a "reshade::api::resource" handle), for the MCP's report of how it was created
      uint32_t mip = 0;
      uint32_t first_layer = 0;
      DXGI_FORMAT data_format = DXGI_FORMAT_UNKNOWN; // How to read the data: the view's format, or the depth stencil plane's
      std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
      uint32_t rows = 0; // Of each subresource (block rows for block compressed formats)

      // The debug draw's copy: the texture one mip of the view's layers is copied into (the buffer above only holds its token), "data_format" is how
      // the display composition samples it
      com_ptr<ID3D12Resource> debug_texture;

      // Constant buffers: "size" bytes at "buffer_offset" of the bound buffer, or the root constants
      std::string resource; // The frame capture's hash of the buffer
      uint64_t buffer_offset = 0;
      uint64_t size = 0;
      std::vector<uint32_t> root_constants;
      uint32_t register_index = 0;
      uint32_t register_space = 0;
      // Indirect arguments: "indirect_commands" commands of "indirect_stride" bytes from "buffer_offset", laid out as "indirect_arguments" says
      uint32_t indirect_stride = 0;
      uint32_t indirect_commands = 0;
      std::vector<D3D12_INDIRECT_ARGUMENT_DESC> indirect_arguments;
   };

   // One texture of the dev MCP's "sr_capture": the super resolution tap copies each one the draw has, in the draw's command list
   struct Capture
   {
      const char* name = nullptr;
      Copy copy;
      std::optional<std::vector<char>> bytes; // Once it ran
   };

   // One per device
   struct State
   {
      std::shared_mutex mutex;        // Guards everything below
      std::optional<Target> target;   // Armed by an MCP job, taken by the target command
      int32_t instances = 0;          // Commands matching "target" since the last present
      std::optional<Copy> copy;       // The MCP job's
      std::vector<Capture> captures;  // The "sr_capture" job's
      std::vector<Copy> retired;      // Copies a job gave up on, whose buffer the GPU may still write
      com_ptr<ID3D12Resource> tokens; // UPLOAD, mapped for its whole life
      uint64_t* mapped_tokens = nullptr;
      uint64_t next_token = 1;

      // The overlay's debug draw: its target stays armed (matched like "target", counted by its own "debug_instances"), and its copy is
      // made again at each match into one texture of Luma's, which the display composition draws at present once its list ran
      std::optional<Target> debug_target;
      int32_t debug_instances = 0;
      std::optional<Copy> debug_copy;
      // Why the last match wasn't copied, empty if it was
      std::string debug_error;
   };

   // Checked without a lock by every draw, copy, resolve and clear: a "State::target" is armed (rechecked under its lock)
   inline std::atomic<bool> armed = false;
   // Checked without a lock by every command list execution, reset and destruction: copies in "Status::Recorded"
   inline std::atomic<uint32_t> recorded_copies = 0;

   // After a target was taken or dropped: the debug draw's stays armed. Expects "state.mutex".
   inline void UpdateArmed(const State& state)
   {
      armed = state.target.has_value() || state.debug_target.has_value();
   }

   // Each copy takes its token from its own slot, the GPU reads it when the list runs.
   // ponytail: a ring, so a copy whose list runs after 64 later copies were recorded reads a newer token and counts as not run (a false
   // negative, never wrong bytes); grow it if a job ever records that many copies before their lists run.
   constexpr uint32_t token_slots = 64;

   // How a plane of a depth stencil resource copies: "format" is the resource's or a view's. The planes of the combined formats copy as
   // textures of their own, the depth of D24 with its top 8 bits zeroed and the stencil as R8. The typeless R32 and R16 only come here as depth.
   inline DXGI_FORMAT PlaneFormat(DXGI_FORMAT format, bool is_stencil)
   {
      switch (format)
      {
      case DXGI_FORMAT_D24_UNORM_S8_UINT:
      case DXGI_FORMAT_R24G8_TYPELESS:
         return (is_stencil ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
      case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      case DXGI_FORMAT_R32G8X24_TYPELESS:
         return (is_stencil ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R32_FLOAT);
      case DXGI_FORMAT_D32_FLOAT:
      case DXGI_FORMAT_R32_TYPELESS:
         return DXGI_FORMAT_R32_FLOAT;
      case DXGI_FORMAT_D16_UNORM:
      case DXGI_FORMAT_R16_TYPELESS:
         return DXGI_FORMAT_R16_UNORM;
      default:
         return format;
      }
   }

   // Moves "copy" to "state->retired" if the GPU may still use its resources (a recorded one keeps counting in "recorded_copies"). Expects
   // "state->mutex" (exclusive).
   inline void RetireInFlight(State* state, Copy* copy)
   {
      if (copy->status == Copy::Status::Recorded || copy->status == Copy::Status::Executed)
      {
         state->retired.push_back(std::move(*copy));
      }
   }

   // UPLOAD buffers start (and stay) in GENERIC_READ, READBACK ones in COPY_DEST
   inline com_ptr<ID3D12Resource> CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heap_type, uint64_t size)
   {
      const D3D12_HEAP_PROPERTIES heap = {.Type = heap_type};
      const D3D12_RESOURCE_DESC desc = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = size, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {.Count = 1}, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
      const D3D12_RESOURCE_STATES state = (heap_type == D3D12_HEAP_TYPE_UPLOAD) ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
      com_ptr<ID3D12Resource> buffer;
      device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
      return buffer;
   }

   // Expects "state->mutex" (exclusive)
   inline bool CreateTokens(State* state, ID3D12Device* device)
   {
      if (state->tokens)
         return true;
      state->tokens = CreateBuffer(device, D3D12_HEAP_TYPE_UPLOAD, token_slots * sizeof(uint64_t));
      const D3D12_RANGE nothing_read = {};
      if (state->tokens && SUCCEEDED(state->tokens->Map(0, &nothing_read, reinterpret_cast<void**>(&state->mapped_tokens))))
         return true;
      state->tokens.reset();
      return false;
   }

   // Expects "state->mutex" (exclusive) and "CreateTokens()"
   inline void RecordToken(State* state, ID3D12GraphicsCommandList* native_list, Copy* copy)
   {
      copy->token = state->next_token++;
      const uint32_t slot = uint32_t(copy->token % token_slots);
      state->mapped_tokens[slot] = copy->token;
      native_list->CopyBufferRegion(copy->buffer.get(), copy->token_offset, state->tokens.get(), slot * sizeof(uint64_t), sizeof(uint64_t));
   }

   // Nothing for equal states, like "D3D12Passes::Transition()": such a barrier makes the list fail to close
   inline void PushTransition(std::vector<D3D12_RESOURCE_BARRIER>* barriers, ID3D12Resource* resource, uint32_t subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
   {
      if (before == after)
         return;
      barriers->push_back({.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, .Transition = {.pResource = resource, .Subresource = subresource, .StateBefore = before, .StateAfter = after}});
   }

   // Records a copy of "subresources" of "resource" into "native_list", each in "draw_state" (the state the target draw needs, "COMMON" for the
   // back buffer at present). Multisampled render targets are resolved with "resolve_format" first. The resources are created before any
   // command is recorded: "reissue" re-issues the target draw only once nothing can fail anymore. Expects "state->mutex" (exclusive).
   template <typename Reissue>
   bool RecordTextureCopy(State* state, ID3D12Device* device, ID3D12GraphicsCommandList* native_list, ID3D12Resource* resource, D3D12_RESOURCE_STATES draw_state, std::span<const uint32_t> subresources, DXGI_FORMAT resolve_format, Copy* copy, Reissue&& reissue)
   {
      const D3D12_RESOURCE_DESC source_desc = resource->GetDesc();
      const bool resolve = source_desc.SampleDesc.Count > 1;
      ID3D12Resource* copy_source = resource;
      std::vector<uint32_t> copy_subresources(subresources.begin(), subresources.end());
      if (resolve)
      {
         // Copies need matching sample counts. Multisampled textures have one mip.
         D3D12_RESOURCE_DESC resolved_desc = source_desc;
         resolved_desc.Alignment = 0;
         resolved_desc.DepthOrArraySize = UINT16(subresources.size());
         resolved_desc.MipLevels = 1;
         resolved_desc.Format = resolve_format;
         resolved_desc.SampleDesc = {.Count = 1};
         resolved_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
         resolved_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
         const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
         if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resolved_desc, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&copy->resolved))))
            return false;
         copy_source = copy->resolved.get();
         for (uint32_t i = 0; i < copy_subresources.size(); i++)
            copy_subresources[i] = i;
      }

      copy->desc = source_desc;
      copy->texture = reinterpret_cast<uint64_t>(resource);
      const D3D12_RESOURCE_DESC copied_desc = copy_source->GetDesc();
      copy->footprints.resize(copy_subresources.size());
      uint64_t offset = 0;
      for (size_t i = 0; i < copy_subresources.size(); i++)
      {
         UINT rows = 0;
         UINT64 row_size = 0;
         device->GetCopyableFootprints(&copied_desc, copy_subresources[i], 1, offset, &copy->footprints[i], &rows, &row_size, nullptr);
         copy->rows = rows;
         const D3D12_SUBRESOURCE_FOOTPRINT& footprint = copy->footprints[i].Footprint;
         offset = (copy->footprints[i].Offset + uint64_t(footprint.RowPitch) * rows * footprint.Depth + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
      }
      copy->token_offset = offset;
      copy->buffer = CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, offset + sizeof(uint64_t));
      if (!copy->buffer || !CreateTokens(state, device))
         return false;

      reissue();

      // Unresolved sources are copied while in COPY_SOURCE, resolved ones once the resolve target moved there
      const auto copy_to_buffer = [&]
      {
         for (size_t i = 0; i < copy_subresources.size(); i++)
         {
            const D3D12_TEXTURE_COPY_LOCATION destination = {.pResource = copy->buffer.get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = copy->footprints[i]};
            const D3D12_TEXTURE_COPY_LOCATION source = {.pResource = copy_source, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = copy_subresources[i]};
            native_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr); // Depth stencil sources need whole subresources
         }
      };
      // Only the copied subresources change state, the others of the resource can be in any
      std::vector<D3D12_RESOURCE_BARRIER> barriers;
      const D3D12_RESOURCE_STATES source_state = resolve ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE;
      for (const uint32_t subresource : subresources)
         PushTransition(&barriers, resource, subresource, draw_state, source_state);
      native_list->ResourceBarrier(UINT(barriers.size()), barriers.data());
      barriers.clear();
      if (resolve)
      {
         for (uint32_t i = 0; i < subresources.size(); i++)
            native_list->ResolveSubresource(copy_source, i, resource, subresources[i], resolve_format);
         PushTransition(&barriers, copy_source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      }
      else
      {
         copy_to_buffer();
      }
      for (const uint32_t subresource : subresources)
         PushTransition(&barriers, resource, subresource, source_state, draw_state);
      native_list->ResourceBarrier(UINT(barriers.size()), barriers.data());
      if (resolve)
      {
         copy_to_buffer();
      }
      RecordToken(state, native_list, copy);
      return true;
   }

   // The format a depth stencil format is copied in (its typeless family, so the copy can be sampled) and the one its depth plane is sampled in
   struct DepthSampleFormats
   {
      DXGI_FORMAT texture = DXGI_FORMAT_UNKNOWN;
      DXGI_FORMAT srv = DXGI_FORMAT_UNKNOWN;
   };

   inline DepthSampleFormats DepthSampleFormatsOf(DXGI_FORMAT format)
   {
      switch (format)
      {
      case DXGI_FORMAT_D24_UNORM_S8_UINT:
      case DXGI_FORMAT_R24G8_TYPELESS:
         return {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS};
      case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      case DXGI_FORMAT_R32G8X24_TYPELESS:
         return {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS};
      case DXGI_FORMAT_D32_FLOAT:
      case DXGI_FORMAT_R32_TYPELESS:
         return {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT};
      case DXGI_FORMAT_D16_UNORM:
      case DXGI_FORMAT_R16_TYPELESS:
         return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM};
      default:
         return {};
      }
   }

   // Records the debug draw's copy of "mip" of "layers" layers of "resource" (in "draw_state", the state the target draw needs) into
   // "state->debug_copy"'s texture of "texture_format" ("plane" of a depth stencil resource: 1 is its stencil), a 2D texture of one mip that rests in PIXEL_SHADER_RESOURCE and is sampled in "srv_format"
   // (multisampled color is resolved into it). The texture is kept while its description matches, a replaced one is retired with the copy that
   // used it. The copy's buffer only holds its 8 byte token, which "Advance()" needs to retire it. "reissue" re-issues the target draw once
   // nothing can fail anymore. Expects "state->mutex" (exclusive).
   template <typename Reissue>
   bool RecordDebugCopy(State* state, ID3D12Device* device, ID3D12GraphicsCommandList* native_list, const reshade::api::command_list* cmd_list, ID3D12Resource* resource, D3D12_RESOURCE_STATES draw_state, uint32_t mip, uint32_t first_layer, uint32_t layers, uint32_t plane, DXGI_FORMAT texture_format, DXGI_FORMAT srv_format, Reissue&& reissue)
   {
      const D3D12_RESOURCE_DESC source_desc = resource->GetDesc();
      const bool resolve = source_desc.SampleDesc.Count > 1;
      const D3D12_RESOURCE_DESC desc = {.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = (std::max)(source_desc.Width >> mip, 1ull), .Height = (std::max)(source_desc.Height >> mip, 1u), .DepthOrArraySize = UINT16(layers), .MipLevels = 1, .Format = texture_format, .SampleDesc = {.Count = 1}, .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN};
      if (!state->debug_copy)
      {
         state->debug_copy.emplace().status = Copy::Status::Failed; // Nothing recorded yet
      }
      Copy* copy = &*state->debug_copy;
      if (copy->debug_texture)
      {
         const D3D12_RESOURCE_DESC current = copy->debug_texture->GetDesc();
         if (current.Width != desc.Width || current.Height != desc.Height || current.DepthOrArraySize != desc.DepthOrArraySize || current.Format != desc.Format)
         {
            RetireInFlight(state, copy); // The GPU can still write or sample it
            *copy = {};
            copy->status = Copy::Status::Failed;
         }
      }
      if (!copy->debug_texture)
      {
         const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
         if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&copy->debug_texture))))
            return false;
      }
      if (!copy->buffer)
      {
         copy->buffer = CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(uint64_t));
      }
      if (!copy->buffer || !CreateTokens(state, device))
         return false;

      reissue();

      std::vector<D3D12_RESOURCE_BARRIER> barriers;
      ID3D12Resource* const destination = copy->debug_texture.get();
      const D3D12_RESOURCE_STATES source_state = (resolve ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE);
      const D3D12_RESOURCE_STATES destination_state = (resolve ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST);
      const auto source_subresource = [&](uint32_t layer)
      { return mip + (first_layer + layer) * source_desc.MipLevels + plane * source_desc.MipLevels * source_desc.DepthOrArraySize; };
      for (uint32_t layer = 0; layer < layers; layer++)
         PushTransition(&barriers, resource, source_subresource(layer), draw_state, source_state);
      PushTransition(&barriers, destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, destination_state);
      native_list->ResourceBarrier(UINT(barriers.size()), barriers.data());
      for (uint32_t layer = 0; layer < layers; layer++)
      {
         if (resolve)
         {
            native_list->ResolveSubresource(destination, layer, resource, source_subresource(layer), texture_format);
         }
         else
         {
            const D3D12_TEXTURE_COPY_LOCATION copy_destination = {.pResource = destination, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = layer};
            const D3D12_TEXTURE_COPY_LOCATION copy_source = {.pResource = resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = source_subresource(layer)};
            native_list->CopyTextureRegion(&copy_destination, 0, 0, 0, &copy_source, nullptr); // Depth stencil sources need whole subresources
         }
      }
      barriers.clear();
      for (uint32_t layer = 0; layer < layers; layer++)
         PushTransition(&barriers, resource, source_subresource(layer), source_state, draw_state);
      PushTransition(&barriers, destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, destination_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      native_list->ResourceBarrier(UINT(barriers.size()), barriers.data());
      RecordToken(state, native_list, copy);

      if (copy->status != Copy::Status::Recorded)
      {
         recorded_copies++;
      }
      // A copy still in a list that wasn't executed is replaced: the list of this one is the one whose execution counts
      copy->status = Copy::Status::Recorded;
      copy->error.clear();
      copy->cmd_list = cmd_list;
      copy->native_list = 0;
      copy->queue.reset();
      copy->fence.reset();
      copy->fence_value = 0;
      copy->signals = 0;
      copy->desc = source_desc;
      copy->texture = reinterpret_cast<uint64_t>(resource);
      copy->mip = mip;
      copy->first_layer = first_layer;
      copy->data_format = srv_format;
      return true;
   }

   // Records a copy of "size" bytes at "offset" of a buffer: in an UPLOAD heap by default (always GENERIC_READ, which includes COPY_SOURCE, so no
   // barrier), else in "buffer_state", which it leaves it in. Expects "state->mutex" (exclusive).
   inline bool RecordBufferCopy(State* state, ID3D12Device* device, ID3D12GraphicsCommandList* native_list, ID3D12Resource* resource, uint64_t offset, uint64_t size, Copy* copy, D3D12_RESOURCE_STATES buffer_state = D3D12_RESOURCE_STATE_COPY_SOURCE)
   {
      copy->buffer_offset = offset;
      copy->size = size;
      copy->token_offset = (size + sizeof(uint64_t) - 1) & ~uint64_t(sizeof(uint64_t) - 1);
      copy->buffer = CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, copy->token_offset + sizeof(uint64_t));
      if (!copy->buffer || !CreateTokens(state, device))
         return false;
      D3D12Passes::Transition(native_list, resource, buffer_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      native_list->CopyBufferRegion(copy->buffer.get(), 0, resource, offset, size);
      D3D12Passes::Transition(native_list, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, buffer_state);
      RecordToken(state, native_list, copy);
      return true;
   }

   // The copied bytes once the GPU ran the copy (a fence of its queue passed first, see "Advance()"), nothing if it didn't: the list wasn't
   // natively submitted yet, or the game's predication skipped the copy. Textures are their subresources' rows of "RowPitch" bytes, ordered
   // [layer][z][row].
   inline std::optional<std::vector<char>> TryRead(const Copy& copy)
   {
      void* data = nullptr;
      if (FAILED(copy.buffer->Map(0, nullptr, &data)))
         return std::nullopt;
      const char* bytes = static_cast<const char*>(data);
      uint64_t token = 0;
      std::memcpy(&token, bytes + copy.token_offset, sizeof(token));
      std::optional<std::vector<char>> result;
      if (token == copy.token)
      {
         result.emplace();
         if (copy.footprints.empty())
         {
            result->assign(bytes, bytes + copy.size);
         }
         for (const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint : copy.footprints)
         {
            const size_t size = size_t(footprint.Footprint.RowPitch) * copy.rows * footprint.Footprint.Depth;
            result->insert(result->end(), bytes + footprint.Offset, bytes + footprint.Offset + size);
         }
      }
      const D3D12_RANGE nothing_written = {};
      copy.buffer->Unmap(0, &nothing_written);
      return result;
   }

   enum class Completion : uint8_t
   {
      Pending, // The last fence didn't pass yet
      Ran,     // The token is there
      NotRun,  // Still no token after two passed fences: the game's predication skipped the copy, or its list was never submitted
   };

   constexpr uint32_t max_signals = 2;

   // Advances an executed copy at a present, without waiting for its queue (it can wait on something the game signals after present): the
   // copy's fence is signaled on the queue behind the lists submitted to it so far, and the token is read once that passed. ReShade's
   // execute event comes before the native submission, so that first signal can pass before the copy's list was submitted: signal once
   // more, a token still missing then isn't coming. "bytes" gets the copied bytes when it ran. Expects "state->mutex" (exclusive).
   inline Completion Advance(Copy* copy, std::vector<char>* bytes)
   {
      if (copy->signals != 0)
      {
         if (copy->fence->GetCompletedValue() < copy->fence_value)
            return Completion::Pending;
         if (std::optional<std::vector<char>> data = TryRead(*copy))
         {
            *bytes = std::move(*data);
            return Completion::Ran;
         }
         if (copy->signals == max_signals)
            return Completion::NotRun;
      }
      if (!copy->fence)
      {
         com_ptr<ID3D12Device> device;
         if (FAILED(copy->queue->GetDevice(IID_PPV_ARGS(&device))) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy->fence))))
            return Completion::NotRun;
      }
      if (FAILED(copy->queue->Signal(copy->fence.get(), copy->fence_value + 1)))
         return Completion::NotRun;
      copy->fence_value++;
      copy->signals++;
      return Completion::Pending;
   }

   template <typename Function>
   void ForEachCopy(State* state, Function&& function)
   {
      if (state->copy)
      {
         function(&*state->copy);
      }
      if (state->debug_copy)
      {
         function(&*state->debug_copy);
      }
      for (Capture& capture : state->captures)
      {
         function(&capture.copy);
      }
      for (Copy& copy : state->retired)
      {
         function(&copy);
      }
   }

   // A copy whose list hasn't executed yet, recorded by "cmd_list" or on its native list
   inline bool IsRecordedIn(const Copy& copy, const reshade::api::command_list* cmd_list)
   {
      return copy.status == Copy::Status::Recorded && (copy.cmd_list == cmd_list || (copy.native_list != 0 && copy.native_list == cmd_list->get_native()));
   }

   // Called before "cmd_list" goes to "queue"
   inline void OnExecute(State* state, const reshade::api::command_list* cmd_list, reshade::api::command_queue* queue)
   {
      const std::unique_lock lock(state->mutex);
      ForEachCopy(state, [&](Copy* copy)
         {
            if (IsRecordedIn(*copy, cmd_list))
            {
               copy->status = Copy::Status::Executed;
               copy->queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
               recorded_copies--;
            } });
   }

   // Called when "cmd_list" is reset or destroyed: the copies it didn't execute never run
   inline void OnReset(State* state, const reshade::api::command_list* cmd_list)
   {
      const std::unique_lock lock(state->mutex);
      ForEachCopy(state, [&](Copy* copy)
         {
            if (IsRecordedIn(*copy, cmd_list))
            {
               copy->status = Copy::Status::Discarded;
               recorded_copies--;
            } });
      std::erase_if(state->retired, [](const Copy& copy)
         { return copy.status == Copy::Status::Discarded; });
   }

   // Moves the job's copy and captures to "retired" if the GPU may still write their buffer. Expects "state->mutex" (exclusive).
   inline void Retire(State* state)
   {
      if (state->copy)
      {
         RetireInFlight(state, &*state->copy);
      }
      state->copy.reset();
      for (Capture& capture : state->captures)
      {
         RetireInFlight(state, &capture.copy);
      }
      state->captures.clear();
   }

   // Frees the retired copies the GPU is done with (rare: a job gave up on them): executed ones once "Advance()" has no more to wait for,
   // those in lists not executed or reset yet stay
   inline void CollectRetired(State* state)
   {
      {
         const std::shared_lock lock(state->mutex);
         if (state->retired.empty())
            return;
      }
      const std::unique_lock lock(state->mutex);
      std::vector<char> unused; // The bytes of a copy nobody waits for
      for (auto it = state->retired.begin(); it != state->retired.end();)
      {
         if (it->status == Copy::Status::Executed && Advance(&*it, &unused) != Completion::Pending)
         {
            it = state->retired.erase(it);
         }
         else
         {
            ++it;
         }
      }
   }
} // namespace D3D12Readback
