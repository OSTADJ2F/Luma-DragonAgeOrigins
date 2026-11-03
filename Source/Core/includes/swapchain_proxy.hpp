#pragma once

// Proxy back buffer for scRGB games, which present HDR10 PQ: the game and Luma (up to its display composition) keep seeing an
// fp16 scRGB back buffer (a plain texture), while the real DXGI buffer is R10G10B10A2_UNORM, written once per frame by the
// swapchain encode pass at the end of Luma's "present" (see "DrawSwapchainEncode()"), before ReShade's effects and overlay.
// D3D12 has one proxy per back buffer.
//
// Only the game sees the proxy: the "GetBuffer()", "GetDesc()" and "GetDesc1()" slots of the vtable of ReShade's wrapper (the
// swapchain the game holds) are replaced. ReShade (it calls the native swapchain), Luma's calls on the native swapchain and
// overlays hooking the native "Present()" get the real buffer, with its real format and color space (HDR10 PQ, or sRGB on SDR).
#if ENABLE_SWAPCHAIN_PROXY
#if DX11
#include <d3d11.h>
#else
#include <d3d12.h>
#include <include/reshade.hpp>
#endif
#include <dxgi1_6.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <include/reshade_api.hpp>
#include <source/com_ptr.hpp>

#include "../utils/system.h"

#pragma comment(lib, "dxgi.lib") // For "CreateDXGIFactory1()" (the display state tracking in Core)

namespace SwapchainProxy
{
   struct Entry
   {
      IDXGISwapChain* native = nullptr;
      // ReShade's wrapper of the native swapchain, what the game calls
      IDXGISwapChain* wrapper = nullptr;

#if DX11
      // Created by "Register()" (null if that failed), lives until "Unregister()"
      com_ptr<ID3D11Texture2D> proxy;
      com_ptr<ID3D11ShaderResourceView> proxy_srv;

      // Only used by the present thread, which ReShade serializes with the swapchain and effect runtime init/destroy events
      com_ptr<ID3D11Texture2D> real_buffer;
      com_ptr<ID3D11RenderTargetView> real_rtv;
#else
      // One per back buffer (committed, in the PRESENT state), created by "Register()" (empty if that failed), lives until "Unregister()".
      // The real buffers aren't kept: "ResizeBuffers()" fails while anything references them.
      std::vector<com_ptr<ID3D12Resource>> proxies;
#endif
      // The display state is stale once it stops being current
      com_ptr<IDXGIFactory1> factory;
      // Whether the window's display is in HDR (PQ output), otherwise it's SDR (sRGB output)
      bool encode_pq = false;
      // ReShade's effect runtime, which draws on the real buffer: it reads the color space when it initializes, then is told the changes
      reshade::api::effect_runtime* runtime = nullptr;
   };

   using GetBufferFunction = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
   using GetDescFunction = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
   using GetDesc1Function = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, DXGI_SWAP_CHAIN_DESC1*);
   using SetColorSpace1Function = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);

   // "IDXGISwapChain" vtable slots (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, then these), "IDXGISwapChain1" to "3" continue it
   constexpr size_t GET_BUFFER_VTABLE_INDEX = 9;
   constexpr size_t GET_DESC_VTABLE_INDEX = 12;
   constexpr size_t GET_DESC1_VTABLE_INDEX = 18;
   constexpr size_t SET_COLOR_SPACE1_VTABLE_INDEX = 38;

   // ReShade keeps a pointer to its wrapper in the native swapchain's private data, under "__uuidof(DXGISwapChain)"
   constexpr GUID RESHADE_WRAPPER_GUID = {0x1f445f9f, 0x9887, 0x4c4c, {0x90, 0x55, 0x4e, 0x3b, 0xad, 0xaf, 0xcc, 0xa8}}; // {1F445F9F-9887-4C4C-9055-4E3BADAFCCA8}

   inline std::shared_mutex mutex;
   // One per swapchain (rarely more than one). Boxed so their address stays valid while the vector grows.
   inline std::vector<std::unique_ptr<Entry>> swapchains;

   inline GetBufferFunction original_get_buffer = nullptr;
   inline GetDescFunction original_get_desc = nullptr;
   inline GetDesc1Function original_get_desc1 = nullptr;
   inline SetColorSpace1Function original_set_color_space1 = nullptr;
   // The vtable of ReShade's wrapper, shared by all its swapchains, once patched
   inline void** wrapper_vtable = nullptr;

   // The caller holds the mutex. "swapchain" is the native swapchain or ReShade's wrapper.
   inline Entry* FindLocked(IDXGISwapChain* swapchain)
   {
      for (const std::unique_ptr<Entry>& entry : swapchains)
      {
         if (entry->native == swapchain || entry->wrapper == swapchain)
            return entry.get();
      }
      return nullptr;
   }

   // Valid until "Unregister()" of that swapchain
   inline Entry* Find(IDXGISwapChain* swapchain)
   {
      const std::shared_lock lock(mutex);
      return FindLocked(swapchain);
   }

   // Before the entry is registered
   inline void CreateProxy(Entry* entry)
   {
      DXGI_SWAP_CHAIN_DESC swapchain_desc;
#if DX11
      com_ptr<ID3D11Device> device;
      if (FAILED(entry->native->GetDesc(&swapchain_desc)) || FAILED(entry->native->GetDevice(IID_PPV_ARGS(&device))))
         return;

      const D3D11_TEXTURE2D_DESC proxy_desc = {
         .Width = swapchain_desc.BufferDesc.Width,
         .Height = swapchain_desc.BufferDesc.Height,
         .MipLevels = 1,
         .ArraySize = 1,
         .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
         .SampleDesc = {.Count = 1},
         .Usage = D3D11_USAGE_DEFAULT,
         .BindFlags = UINT(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | ((swapchain_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) != 0 ? D3D11_BIND_UNORDERED_ACCESS : 0)),
      };
      if (FAILED(device->CreateTexture2D(&proxy_desc, nullptr, &entry->proxy)) || FAILED(device->CreateShaderResourceView(entry->proxy.get(), nullptr, &entry->proxy_srv)))
      {
         entry->proxy = nullptr;
         entry->proxy_srv = nullptr;
      }
#else
      // The proxies are made on the native device, so ReShade's proxy and Luma's creation hooks never see them (as for the real back buffers,
      // which fire no "init_resource" either). A D3D12 swapchain is made on a queue, which its "GetDevice()" may hand out instead of the device.
      const auto fail = [](const char* reason)
      {
         reshade::log::message(reshade::log::level::error, (std::string("[Swapchain proxy] ") + reason + ", so the game draws into the 10 bit buffer and HDR looks wrong").c_str());
      };
      com_ptr<ID3D12Device> device;
      if (FAILED(entry->native->GetDesc(&swapchain_desc)))
         return fail("The swapchain refused its desc");
      if (com_ptr<ID3D12CommandQueue> queue; FAILED(entry->native->GetDevice(IID_PPV_ARGS(&device))) && (FAILED(entry->native->GetDevice(IID_PPV_ARGS(&queue))) || FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))))
         return fail("The swapchain gave out neither its device nor its queue");

      // Unordered access where the device loads fp16 from typed UAVs: the display composition then writes the proxy in place, without a copy of it
      // (see "DrawDisplayCompositionD3D12()")
      D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support = {.Format = DXGI_FORMAT_R16G16B16A16_FLOAT};
      const bool typed_uav_load = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format_support, sizeof(format_support))) && (format_support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
      const D3D12_HEAP_PROPERTIES default_heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC proxy_desc = {
         .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
         .Width = swapchain_desc.BufferDesc.Width,
         .Height = swapchain_desc.BufferDesc.Height,
         .DepthOrArraySize = 1,
         .MipLevels = 1,
         .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
         .SampleDesc = {.Count = 1},
         .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | (((swapchain_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) != 0 || typed_uav_load) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE),
      };
      entry->proxies.resize(swapchain_desc.BufferCount);
      for (com_ptr<ID3D12Resource>& proxy : entry->proxies)
      {
         if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &proxy_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&proxy))))
         {
            entry->proxies.clear();
            return fail("The device refused the proxy back buffers");
         }
         proxy->SetName(L"Luma swapchain proxy");
      }
#endif
   }

   // Empty if the swapchain isn't proxied (or the proxy couldn't be created), the caller then returns the real buffer.
   // D3D11 swapchains only give out buffer 0.
   inline std::optional<HRESULT> QueryProxy(IDXGISwapChain* swapchain, UINT buffer, REFIID riid, void** surface)
   {
      const std::shared_lock lock(mutex);
      const Entry* const entry = FindLocked(swapchain);
#if DX11
      if (!entry || !entry->proxy || buffer != 0)
         return std::nullopt;
      return entry->proxy->QueryInterface(riid, surface);
#else
      if (!entry || buffer >= entry->proxies.size())
         return std::nullopt;
      return entry->proxies[buffer]->QueryInterface(riid, surface);
#endif
   }

   inline HRESULT STDMETHODCALLTYPE GetBufferDetour(IDXGISwapChain* swapchain, UINT buffer, REFIID riid, void** surface)
   {
      if (const std::optional<HRESULT> hr = QueryProxy(swapchain, buffer, riid, surface))
         return *hr;
      return original_get_buffer(swapchain, buffer, riid, surface);
   }

   inline HRESULT STDMETHODCALLTYPE GetDescDetour(IDXGISwapChain* swapchain, DXGI_SWAP_CHAIN_DESC* desc)
   {
      const HRESULT hr = original_get_desc(swapchain, desc);
      if (SUCCEEDED(hr) && desc != nullptr && Find(swapchain))
      {
         desc->BufferDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      }
      return hr;
   }

   inline HRESULT STDMETHODCALLTYPE GetDesc1Detour(IDXGISwapChain1* swapchain, DXGI_SWAP_CHAIN_DESC1* desc)
   {
      const HRESULT hr = original_get_desc1(swapchain, desc);
      if (SUCCEEDED(hr) && desc != nullptr && Find(swapchain))
      {
         desc->Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      }
      return hr;
   }

   // Luma sets the real buffer's color space (see "UpdateSwapchainProxyOutput()"). Forwarding the game's would also make ReShade
   // recreate the swapchain when it differs from the one in the SKID private data, leaving the game on a stale proxy.
   inline HRESULT STDMETHODCALLTYPE SetColorSpace1Detour(IDXGISwapChain3* swapchain, DXGI_COLOR_SPACE_TYPE color_space)
   {
      if (Find(swapchain))
         return S_OK;
      return original_set_color_space1(swapchain, color_space);
   }

   // The vtable is in read only memory
   inline bool PatchVTable(size_t index, void* function)
   {
      return System::PatchMemory(&wrapper_vtable[index], &function, sizeof(function), System::PatchMemoryType::Data);
   }

   // Called when the addon unloads (explicitly, not at process exit)
   inline void RemoveHooks()
   {
      if (!wrapper_vtable)
         return;
      if (original_get_buffer)
         PatchVTable(GET_BUFFER_VTABLE_INDEX, reinterpret_cast<void*>(original_get_buffer));
      if (original_get_desc)
         PatchVTable(GET_DESC_VTABLE_INDEX, reinterpret_cast<void*>(original_get_desc));
      if (original_get_desc1)
         PatchVTable(GET_DESC1_VTABLE_INDEX, reinterpret_cast<void*>(original_get_desc1));
      if (original_set_color_space1)
         PatchVTable(SET_COLOR_SPACE1_VTABLE_INDEX, reinterpret_cast<void*>(original_set_color_space1));
      wrapper_vtable = nullptr;
   }

   // ReShade's wrappers share their vtable, so the hooks install once, from the first swapchain's, before the game gets it
   inline bool InstallHooks(IDXGISwapChain* wrapper)
   {
      static const bool installed = [wrapper]()
      {
         wrapper_vtable = *reinterpret_cast<void***>(wrapper);
         const auto hook = [](size_t index, void* detour, auto* original)
         {
            *original = reinterpret_cast<std::remove_pointer_t<decltype(original)>>(wrapper_vtable[index]);
            if (PatchVTable(index, detour))
               return true;
            *original = nullptr;
            return false;
         };
         const bool hooked =
            hook(GET_BUFFER_VTABLE_INDEX, reinterpret_cast<void*>(&GetBufferDetour), &original_get_buffer) &&
            hook(GET_DESC_VTABLE_INDEX, reinterpret_cast<void*>(&GetDescDetour), &original_get_desc) &&
            hook(GET_DESC1_VTABLE_INDEX, reinterpret_cast<void*>(&GetDesc1Detour), &original_get_desc1) &&
            hook(SET_COLOR_SPACE1_VTABLE_INDEX, reinterpret_cast<void*>(&SetColorSpace1Detour), &original_set_color_space1);
         if (!hooked)
         {
            RemoveHooks();
         }
         return hooked;
      }();
      return installed;
   }

   // Releases the proxy and the views of the real buffer, which must not outlive "ResizeBuffers()"
   inline void Unregister(IDXGISwapChain* native)
   {
      std::unique_ptr<Entry> entry;
      {
         const std::unique_lock lock(mutex);
         const auto it = std::find_if(swapchains.begin(), swapchains.end(), [native](const std::unique_ptr<Entry>& other)
            { return other->native == native; });
         if (it == swapchains.end())
            return;
         entry = std::move(*it);
         swapchains.erase(it);
      }
   }

   // False if the swapchain couldn't be hooked, then it isn't proxied.
   // Call before the game gets the swapchain's back buffer, and "Unregister()" before it's resized or destroyed.
   inline bool Register(IDXGISwapChain* native)
   {
      IDXGISwapChain* wrapper = nullptr;
      UINT size = sizeof(wrapper);
      if (FAILED(native->GetPrivateData(RESHADE_WRAPPER_GUID, &size, &wrapper)) || size != sizeof(wrapper) || !wrapper || !InstallHooks(wrapper))
         return false;

      auto entry = std::make_unique<Entry>();
      entry->native = native;
      entry->wrapper = wrapper;
      CreateProxy(entry.get());
      Unregister(native);
      const std::unique_lock lock(mutex);
      swapchains.push_back(std::move(entry));
      return true;
   }
} // namespace SwapchainProxy
#endif // ENABLE_SWAPCHAIN_PROXY
