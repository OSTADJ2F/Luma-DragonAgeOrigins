#pragma once

#if DX12 && ENABLE_MINHOOK && DEVELOPMENT

// The game's command signatures and the ExecuteIndirect() calls that use them, for the frame capture and the dev MCP: ReShade's
// "draw_or_dispatch_indirect" event has the argument buffer and the most commands, not the signature, which says what each command sets
// (draw or dispatch arguments, root constants, root views, vertex and index buffers). Development only.

#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <d3d12.h>

#include "d3d12_resource_creation.h"

namespace D3D12CommandSignatures
{
   struct Signature
   {
      UINT stride = 0;
      std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments;
   };

   // By the signature's "ID3D12CommandSignature" pointer, which ReShade doesn't wrap.
   // ponytail: a released signature stays until a new one gets its address
   inline std::shared_mutex signatures_mutex;
   inline std::unordered_map<uint64_t, Signature> signatures;

   // The ExecuteIndirect() call that ReShade's event fires inside, on this thread
   struct Call
   {
      std::optional<Signature> signature; // None if it was created before the hook
      ID3D12Resource* argument_buffer = nullptr;
      uint64_t argument_offset = 0;
      UINT max_count = 0;
      ID3D12Resource* count_buffer = nullptr;
   };
   inline thread_local std::optional<Call> current_call;

   // The name of an argument type for the capture and the MCP
   inline const char* ArgumentName(D3D12_INDIRECT_ARGUMENT_TYPE type)
   {
      switch (type)
      {
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
         return "draw";
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
         return "draw_indexed";
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
         return "dispatch";
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
         return "vertex_buffer";
      case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
         return "index_buffer";
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
         return "constants";
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
         return "cbv";
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
         return "srv";
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
         return "uav";
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:
         return "dispatch_rays";
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
         return "dispatch_mesh";
      default:
         return "unknown";
      }
   }

   // The bytes an argument takes in a command of the argument buffer, 0 for a type this doesn't know
   inline size_t ArgumentSize(const D3D12_INDIRECT_ARGUMENT_DESC& argument)
   {
      switch (argument.Type)
      {
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
         return sizeof(D3D12_DRAW_ARGUMENTS);
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
         return sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
         return sizeof(D3D12_DISPATCH_ARGUMENTS);
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
         return sizeof(D3D12_VERTEX_BUFFER_VIEW);
      case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
         return sizeof(D3D12_INDEX_BUFFER_VIEW);
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
         return sizeof(uint32_t) * argument.Constant.Num32BitValuesToSet;
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
         return sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:
         return sizeof(D3D12_DISPATCH_RAYS_DESC);
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
         return sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
      default:
         return 0;
      }
   }

   // The slots of "ID3D12Device" and "ID3D12GraphicsCommandList", counting each method once (see "D3D12Clones")
   constexpr size_t CREATE_COMMAND_SIGNATURE_SLOT = 41; // After "SetStablePowerState"
   constexpr size_t EXECUTE_INDIRECT_SLOT = 59;         // After "EndEvent"

   inline D3D12ProxyHook::HookedFunction<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**)> create_command_signature;
   inline D3D12ProxyHook::HookedFunction<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64)> execute_indirect;
   inline std::mutex hooks_mutex; // For "Install()", "InstallListHook()" and "Remove()"

   inline HRESULT STDMETHODCALLTYPE CreateCommandSignatureDetour(ID3D12Device* device, const D3D12_COMMAND_SIGNATURE_DESC* desc, ID3D12RootSignature* root_signature, REFIID riid, void** command_signature)
   {
      const HRESULT result = create_command_signature.original(device, desc, root_signature, riid, command_signature);
      if (SUCCEEDED(result) && desc && command_signature && *command_signature)
      {
         const std::unique_lock lock(signatures_mutex);
         signatures[reinterpret_cast<uint64_t>(*command_signature)] = {.stride = desc->ByteStride, .arguments = {desc->pArgumentDescs, desc->pArgumentDescs + desc->NumArgumentDescs}};
      }
      return result;
   }

   inline void STDMETHODCALLTYPE ExecuteIndirectDetour(ID3D12GraphicsCommandList* list, ID3D12CommandSignature* signature, UINT max_count, ID3D12Resource* argument_buffer, UINT64 argument_offset, ID3D12Resource* count_buffer, UINT64 count_offset)
   {
      Call call = {.argument_buffer = argument_buffer, .argument_offset = argument_offset, .max_count = max_count, .count_buffer = count_buffer};
      {
         const std::shared_lock lock(signatures_mutex);
         if (const auto it = signatures.find(reinterpret_cast<uint64_t>(signature)); it != signatures.end())
         {
            call.signature = it->second;
         }
      }
      current_call = std::move(call);
      execute_indirect.original(list, signature, max_count, argument_buffer, argument_offset, count_buffer, count_offset);
      current_call.reset();
   }

   // The device proxy's command signature creation, once per process. False if there is no ReShade proxy, or MinHook failed.
   inline bool Install(ID3D12Device* native_device)
   {
      void** const proxy_vtable = D3D12ProxyHook::FindProxyVtable(native_device);
      if (!proxy_vtable)
      {
         return false;
      }
      const std::lock_guard lock(hooks_mutex);
      return create_command_signature.target || D3D12ProxyHook::Attach(&create_command_signature, proxy_vtable[CREATE_COMMAND_SIGNATURE_SLOT], &CreateCommandSignatureDetour);
   }

   // The command list proxy's ExecuteIndirect(), from the vtable of the game's first list (see "D3D12Clones::OnCommandListCreated()")
   inline bool InstallListHook(void** list_vtable)
   {
      const std::lock_guard lock(hooks_mutex);
      return execute_indirect.target || D3D12ProxyHook::Attach(&execute_indirect, list_vtable[EXECUTE_INDIRECT_SLOT], &ExecuteIndirectDetour);
   }

   // See "D3D12ProxyHook::Detach()"
   inline bool Remove()
   {
      const std::lock_guard lock(hooks_mutex);
      const bool removed_create = D3D12ProxyHook::Detach(&create_command_signature);
      return D3D12ProxyHook::Detach(&execute_indirect) && removed_create;
   }
} // namespace D3D12CommandSignatures

#endif // DX12 && ENABLE_MINHOOK && DEVELOPMENT
