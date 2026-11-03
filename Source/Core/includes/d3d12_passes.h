#pragma once

#if DX12

// Luma's own GPU work at present on D3D12 (the display composition, later the passes of phase 5b): a command list of Luma's, run by
// "ExecuteCommandLists" on the native present queue inside ReShade's present event. ReShade's immediate command list can't take it: it only
// executes what ReShade's own API recorded, so native commands recorded on it run at some later flush. Native D3D12 only (no ReShade
// calls): the caller passes the native device and queue.
//
// Nothing waits for the GPU (the game can have the queue wait on a fence it signals after present). A "Context" has a ring of up to
// MAX_FRAMES frames, each with an allocator, descriptors and the value of the context's own fence its list signals. "Begin()" takes the
// first frame the fence passed, adds one when none is free, and returns nothing when all MAX_FRAMES are still on the GPU: the caller skips
// its passes for that present. Only "Destroy()" waits, for at most DESTROY_WAIT_MS.
//
// A frame's descriptors are its own until it is reused: SRV_DESCRIPTORS_PER_FRAME CBV_SRV_UAV descriptors of a shader visible heap and one
// RTV of a CPU heap, written by the caller (CreateShaderResourceView and the like) at the handles "Begin()" returns. The heap starts
// uninitialized: write every descriptor a root signature's table declares (a null view where the pass doesn't read it). The list comes
// with that heap set and no other state: root signature, pipeline, viewport, scissor, render target, topology and root arguments are the
// pass's. What the caller replaces while the GPU may still use it goes to "Retire()".
//
// Threading: one present thread per context, and the caller serializes every call. The queue is the same one every time (the context's
// fence is signaled on it, in order).
//
// Call order: "Create()" per device; at each present "Begin()", the recording on its list, "Submit()" ("Retire()" anywhere in between);
// "Destroy()" at device teardown.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <Windows.h>
#include <d3d12.h>
#include <source/com_ptr.hpp>

namespace D3D12Passes
{
   constexpr uint32_t MAX_FRAMES = 16;
   // ponytail: room for one pass's table (the composition's t0-t8) and a few more. A recording of several passes with tables of their
   // own needs more: raise it, the heap is MAX_FRAMES times this.
   constexpr uint32_t SRV_DESCRIPTORS_PER_FRAME = 16;
   constexpr DWORD DESTROY_WAIT_MS = 1000;

   struct Frame
   {
      com_ptr<ID3D12CommandAllocator> allocator;
      uint64_t fence_value = 0;                  // The context's fence reaches it once the GPU ran the frame's list (0: never submitted)
      std::vector<com_ptr<IUnknown>> keep_alive; // Dropped when the frame is reused
   };

   // ponytail: frames are never freed, so a context stays at the most presents the GPU ever lagged behind, with allocators at their peak
   // size. Free the idle ones if a game's lag spikes.
   struct Context
   {
      com_ptr<ID3D12Device> device; // The native one
      com_ptr<ID3D12Fence> fence;
      uint64_t last_signaled = 0;
      std::vector<Frame> frames;               // Frame i owns the SRV descriptors from i * SRV_DESCRIPTORS_PER_FRAME of "srv_heap" and RTV i of "rtv_heap"
      std::optional<size_t> current;           // The frame "Begin()" handed out, until "Submit()"
      com_ptr<ID3D12GraphicsCommandList> list; // Made with the first frame
      com_ptr<ID3D12DescriptorHeap> srv_heap;  // Shader visible
      com_ptr<ID3D12DescriptorHeap> rtv_heap;
      uint32_t srv_increment = 0;
      uint32_t rtv_increment = 0;
      std::vector<com_ptr<IUnknown>> retired; // See "Retire()"
   };

   // What "Begin()" hands out, until "Submit()"
   struct Recording
   {
      ID3D12GraphicsCommandList* list = nullptr;
      D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = {}; // The first of the frame's SRV_DESCRIPTORS_PER_FRAME, "srv_increment" apart
      D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = {};
      uint32_t srv_increment = 0;
      D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu = {};
   };

   // Makes the fence and heaps. False, with "context" left as it was, if the device refuses one.
   inline bool Create(Context* context, ID3D12Device* device)
   {
      const D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, .NumDescriptors = MAX_FRAMES * SRV_DESCRIPTORS_PER_FRAME, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
      const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = MAX_FRAMES};
      Context created = {.device = device};
      if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&created.fence))) || FAILED(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&created.srv_heap))) || FAILED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&created.rtv_heap))))
      {
         return false;
      }
      created.srv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      created.rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
      *context = std::move(created);
      return true;
   }

   // The next recording, on a frame the GPU is done with: its allocator and the list reset, the heap set. Nothing if all MAX_FRAMES are on
   // the GPU or the device couldn't make a frame, and for a context that wasn't created. A recording that never reached "Submit()" is
   // dropped. Never waits.
   inline std::optional<Recording> Begin(Context* context)
   {
      if (!context->fence)
      {
         return std::nullopt;
      }
      if (context->current)
      {
         context->list->Close();
         context->current.reset();
      }
      const uint64_t completed = context->fence->GetCompletedValue();
      size_t index = 0;
      while (index < context->frames.size() && completed < context->frames[index].fence_value)
      {
         ++index;
      }
      if (index == context->frames.size())
      {
         com_ptr<ID3D12CommandAllocator> allocator;
         if (context->frames.size() >= MAX_FRAMES || FAILED(context->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
         {
            return std::nullopt;
         }
         if (!context->list)
         {
            if (FAILED(context->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(&context->list))))
            {
               return std::nullopt;
            }
            context->list->Close(); // Made recording, and every recording starts with a reset
         }
         context->frames.push_back({.allocator = std::move(allocator)});
      }
      Frame& frame = context->frames[index];
      frame.keep_alive.clear();
      if (FAILED(frame.allocator->Reset()) || FAILED(context->list->Reset(frame.allocator.get(), nullptr)))
      {
         return std::nullopt;
      }
      context->current = index;
      ID3D12DescriptorHeap* const heaps[] = {context->srv_heap.get()};
      context->list->SetDescriptorHeaps(1, heaps);
      const size_t srv_offset = index * SRV_DESCRIPTORS_PER_FRAME * context->srv_increment;
      return Recording{
         .list = context->list.get(),
         .srv_cpu = {.ptr = context->srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + srv_offset},
         .srv_gpu = {.ptr = context->srv_heap->GetGPUDescriptorHandleForHeapStart().ptr + srv_offset},
         .srv_increment = context->srv_increment,
         .rtv_cpu = {.ptr = context->rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + index * context->rtv_increment},
      };
   }

   // "object" (if any) may still be used by the lists already submitted: the next frame "Submit()" takes holds a reference to it until it is
   // reused, which is after the GPU ran that frame's list. After "Create()".
   inline void Retire(Context* context, IUnknown* object)
   {
      if (object)
      {
         context->retired.emplace_back(object);
      }
   }

   // Closes the recording, executes it on "queue" and signals the context's fence behind it. False if there was none or its list failed to
   // close (a command was invalid): nothing runs, and the frame is free again once the GPU ran the lists submitted before it. Never waits.
   inline bool Submit(Context* context, ID3D12CommandQueue* queue)
   {
      if (!context->current)
      {
         return false;
      }
      Frame& frame = context->frames[*std::exchange(context->current, std::nullopt)];
      frame.keep_alive = std::exchange(context->retired, {});
      if (FAILED(context->list->Close()))
      {
         // What it keeps alive (what was retired from earlier frames) may still be used by the lists already submitted
         frame.fence_value = context->last_signaled;
         return false;
      }
      ID3D12CommandList* const lists[] = {context->list.get()};
      queue->ExecuteCommandLists(1, lists);
      queue->Signal(context->fence.get(), ++context->last_signaled);
      frame.fence_value = context->last_signaled;
      return true;
   }

   // At device teardown: waits for the GPU to finish the submitted lists, for at most DESTROY_WAIT_MS, then releases everything.
   // ponytail: a GPU that is still running them by then (a hang) gets its objects released anyway.
   inline void Destroy(Context* context)
   {
      if (context->fence && context->fence->GetCompletedValue() < context->last_signaled)
      {
         const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
         if (event && SUCCEEDED(context->fence->SetEventOnCompletion(context->last_signaled, event)))
         {
            WaitForSingleObject(event, DESTROY_WAIT_MS);
         }
         CloseHandle(event);
      }
      *context = {};
   }

   // A root signature on the native device, so ReShade's proxy and the hook that appends Luma's CBVs to the game's root signatures
   // ("d3d12_root_signatures.h") never see it. Null if "desc" doesn't serialize or the device refuses it.
   inline com_ptr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc)
   {
      // An export of the runtime, which Luma doesn't link
      const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
      const auto serialize = (d3d12 ? reinterpret_cast<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature")) : nullptr);
      com_ptr<ID3DBlob> blob;
      com_ptr<ID3D12RootSignature> root_signature;
      if (serialize && SUCCEEDED(serialize(&desc, &blob, nullptr)))
      {
         device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature));
      }
      return root_signature;
   }

   // The blend of "CreateFullscreenPipeline()": none
   constexpr D3D12_RENDER_TARGET_BLEND_DESC OPAQUE_BLEND = {.SrcBlend = D3D12_BLEND_ONE, .DestBlend = D3D12_BLEND_ZERO, .BlendOp = D3D12_BLEND_OP_ADD, .SrcBlendAlpha = D3D12_BLEND_ONE, .DestBlendAlpha = D3D12_BLEND_ZERO, .BlendOpAlpha = D3D12_BLEND_OP_ADD, .LogicOp = D3D12_LOGIC_OP_NOOP, .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL};

   // A pipeline for a pass that draws a triangle strip of 4 vertices (or a triangle list of 3) from SV_VertexID (no input layout) over its one render
   // target: no culling, depth, stencil or multisampling, "blend" (none by default). The caller sets the topology. Null if the device refuses it.
   inline com_ptr<ID3D12PipelineState> CreateFullscreenPipeline(ID3D12Device* device, ID3D12RootSignature* root_signature, D3D12_SHADER_BYTECODE vertex_shader, D3D12_SHADER_BYTECODE pixel_shader, DXGI_FORMAT render_target_format, const D3D12_RENDER_TARGET_BLEND_DESC& blend = OPAQUE_BLEND)
   {
      const D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {
         .pRootSignature = root_signature,
         .VS = vertex_shader,
         .PS = pixel_shader,
         .BlendState = {.RenderTarget = {blend}},
         .SampleMask = D3D12_DEFAULT_SAMPLE_MASK,
         .RasterizerState = {.FillMode = D3D12_FILL_MODE_SOLID, .CullMode = D3D12_CULL_MODE_NONE, .DepthClipEnable = TRUE},
         .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
         .NumRenderTargets = 1,
         .RTVFormats = {render_target_format},
         .SampleDesc = {.Count = 1},
      };
      com_ptr<ID3D12PipelineState> pipeline;
      device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
      return pipeline;
   }

   // Null if the device refuses it
   inline com_ptr<ID3D12PipelineState> CreateComputePipeline(ID3D12Device* device, ID3D12RootSignature* root_signature, D3D12_SHADER_BYTECODE compute_shader)
   {
      const D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {.pRootSignature = root_signature, .CS = compute_shader};
      com_ptr<ID3D12PipelineState> pipeline;
      device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
      return pipeline;
   }

   // "subresource" (all by default) of "resource" from "before" to "after" (legacy barrier). Records nothing for equal states: such a barrier makes the
   // list fail to close.
   inline void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
   {
      if (before == after)
      {
         return;
      }
      const D3D12_RESOURCE_BARRIER barrier = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, .Transition = {.pResource = resource, .Subresource = subresource, .StateBefore = before, .StateAfter = after}};
      list->ResourceBarrier(1, &barrier);
   }
} // namespace D3D12Passes

#endif // DX12
