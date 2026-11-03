#pragma once

#include "SRBridgeProtocol.h"
#include "DepthCopyCS.h"
#include "../includes/super_resolution.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>
#include <iterator>
#include <string>

// Profiling of a frame's steps: defined by Mass Effect's Development builds (force included "BridgeProfile.h") and by
// "_tools/sr_bridge_perf" and "_tools/sr_local_test" (before including this file)
#ifndef SR_BRIDGE_PROFILE
#define SR_BRIDGE_PROFILE(command_list, step)
#endif

namespace SRBridge
{
   // The game's (D3D11) side of an SR bridge, shared by the 32-bit helper bridge ("SRBridge.h") and the in-process one ("D3D12SRLocal.h"):
   // the upscaler works on NT-handle shared textures, the game's own when they are shared already (zero copy), else copies it makes each
   // frame, between two shared fences: "in" (a frame's inputs copied) and "out" (its output written, first "SRBridgeProtocol::kReady").
   struct Link
   {
      using DrawData = SR::SuperResolutionImplT<SR::D3D11Api>::DrawData;
      static constexpr int kCount = SRBridgeProtocol::kCount;
      static constexpr const char* names[kCount] = {"color", "motion vectors", "depth", "bias mask", "transparency", "exposure", "output"};

      Microsoft::WRL::ComPtr<ID3D11Device5> device;
      Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
      Microsoft::WRL::ComPtr<ID3D11Fence> fences[2];
      // The game's resources it was opened for (compared only). Referenced, so a recreated one (e.g. at a new size) can't reuse the address
      Microsoft::WRL::ComPtr<ID3D11Resource> sources[kCount];
      Microsoft::WRL::ComPtr<ID3D11Texture2D> shared[kCount]; // What the upscaler opens: the game's own when NT-handle shared already, else copies
      bool copied[kCount] = {};
      DXGI_FORMAT source_formats[kCount] = {}; // Of the game's textures the copies are of
      uint64_t frame = 0;                      // 0 until the upscaler is ready, then the last frame sent (from "kReady")

      // Depth formats can't be NT-handle shared: such a depth is copied into an R32_FLOAT texture by "kDepthCopyCS" (a SRV of the game's
      // texture with the depth's read format, cached for the last source)
      Microsoft::WRL::ComPtr<ID3D11ComputeShader> depth_copy_cs;
      Microsoft::WRL::ComPtr<ID3D11Buffer> depth_copy_size;
      Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> depth_copy_uav;
      Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth_copy_srv;
      ID3D11Resource* depth_copy_srv_source = nullptr; // Compared only
      DXGI_FORMAT depth_read_format = DXGI_FORMAT_UNKNOWN;

      // The format a depth texture is read with, UNKNOWN if it isn't a depth format
      static DXGI_FORMAT GetDepthReadFormat(DXGI_FORMAT format)
      {
         switch (format)
         {
         case DXGI_FORMAT_R24G8_TYPELESS:
         case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
         case DXGI_FORMAT_R32G8X24_TYPELESS:
         case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
         case DXGI_FORMAT_R32_TYPELESS:
         case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
         case DXGI_FORMAT_R16_TYPELESS:
         case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_UNORM;
         default:
            return DXGI_FORMAT_UNKNOWN;
         }
      }

      void Reset()
      {
         for (auto& fence : fences)
         {
            fence.Reset();
         }
         for (int i = 0; i < kCount; i++)
         {
            sources[i].Reset();
            shared[i].Reset();
            copied[i] = false;
            source_formats[i] = DXGI_FORMAT_UNKNOWN;
         }
         depth_copy_uav.Reset();
         depth_copy_srv.Reset();
         depth_copy_srv_source = nullptr;
         depth_read_format = DXGI_FORMAT_UNKNOWN;
         frame = 0;
      }

      // Shares the textures (or copies of them) and creates the fences. "handles": ours, the textures' (0: none) then the fences'; the caller
      // closes them, also after an error. Returns the error, empty if fine.
      std::string Open(ID3D11DeviceContext* command_list, ID3D11Resource* const* resources, HANDLE (&handles)[kCount + 2])
      {
         if (FAILED(command_list->QueryInterface(IID_PPV_ARGS(&context))))
         {
            return "no D3D11 fences on this device context";
         }
         if (!resources[SRBridgeProtocol::kColor] || !resources[SRBridgeProtocol::kMotion] || !resources[SRBridgeProtocol::kDepth] || !resources[SRBridgeProtocol::kOutput])
         {
            return "missing color, motion vectors, depth or output";
         }
         for (int i = 0; i < kCount; i++)
         {
            sources[i] = resources[i];
            if (!resources[i])
            {
               continue;
            }
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (FAILED(resources[i]->QueryInterface(IID_PPV_ARGS(&texture))))
            {
               return std::format("resource {} isn't a 2D texture", i);
            }
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);
            copied[i] = (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) == 0;
            source_formats[i] = desc.Format;
            if (copied[i])
            {
               const bool depth_copy = i == SRBridgeProtocol::kDepth && GetDepthReadFormat(desc.Format) != DXGI_FORMAT_UNKNOWN;
               desc.Usage = D3D11_USAGE_DEFAULT;
               desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | ((i == SRBridgeProtocol::kOutput || depth_copy) ? D3D11_BIND_UNORDERED_ACCESS : 0);
               desc.CPUAccessFlags = 0;
               desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
               if (depth_copy)
               {
                  depth_read_format = GetDepthReadFormat(desc.Format);
                  desc.Format = DXGI_FORMAT_R32_FLOAT;
                  desc.MipLevels = 1;
                  desc.ArraySize = 1;
               }
               if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)))
               {
                  return std::format("resource {} (format {}) can't be shared", i, int(desc.Format));
               }
               if (depth_copy)
               {
                  const D3D11_BUFFER_DESC size_desc = {.ByteWidth = 16, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_CONSTANT_BUFFER};
                  if ((!depth_copy_cs && FAILED(device->CreateComputeShader(kDepthCopyCS, sizeof(kDepthCopyCS), nullptr, &depth_copy_cs))) ||
                      (!depth_copy_size && FAILED(device->CreateBuffer(&size_desc, nullptr, &depth_copy_size))) ||
                      FAILED(device->CreateUnorderedAccessView(texture.Get(), nullptr, &depth_copy_uav)))
                  {
                     return std::format("the depth (format {}) can't be copied", int(source_formats[i]));
                  }
               }
            }
            shared[i] = texture;
            Microsoft::WRL::ComPtr<IDXGIResource1> dxgi_resource;
            if (FAILED(texture.As(&dxgi_resource)) || FAILED(dxgi_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handles[i])))
            {
               return std::format("resource {} has no shared handle", i);
            }
         }
         for (int i = 0; i < 2; i++)
         {
            if (FAILED(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fences[i]))) || FAILED(fences[i]->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handles[kCount + i])))
            {
               return "no shared fences";
            }
         }
         return {};
      }

      // Whether the upscaler signaled "out" at "kReady" (or ended: a dead one's fence reads UINT64_MAX)
      bool IsReadySignaled() const
      {
         return fences[1]->GetCompletedValue() >= SRBridgeProtocol::kReady;
      }

      // The index of the first input that is another texture than the one opened, -1 if none. An input the bridge copies each frame can come
      // from another texture of the same description (a game that alternates two targets for one pass): only the source changes, the upscaler
      // keeps its shared copy.
      int FindNewInput(ID3D11Resource* const* resources)
      {
         for (int i = 0; i < kCount; i++)
         {
            if (i == SRBridgeProtocol::kOutput || !copied[i] || !resources[i] || resources[i] == sources[i].Get())
            {
               continue;
            }
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (FAILED(resources[i]->QueryInterface(IID_PPV_ARGS(&texture))))
            {
               continue;
            }
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);
            D3D11_TEXTURE2D_DESC shared_desc;
            shared[i]->GetDesc(&shared_desc);
            if (desc.Format == source_formats[i] && desc.Width == shared_desc.Width && desc.Height == shared_desc.Height && desc.ArraySize == shared_desc.ArraySize &&
                desc.MipLevels == shared_desc.MipLevels && desc.SampleDesc.Count == shared_desc.SampleDesc.Count)
            {
               sources[i] = resources[i];
            }
         }
         const auto changed = std::mismatch(resources, resources + kCount, std::begin(sources), [](ID3D11Resource* resource, const Microsoft::WRL::ComPtr<ID3D11Resource>& source)
            { return resource == source.Get(); });
         return changed.first == resources + kCount ? -1 : int(changed.first - resources);
      }

      // The color as it is (at the top left, if it's smaller), while the upscaler is still starting
      void CopyColorToOutput(ID3D11DeviceContext* command_list, const DrawData& draw_data) const
      {
         Microsoft::WRL::ComPtr<ID3D11Texture2D> source, output;
         if (SUCCEEDED(draw_data.source_color->QueryInterface(IID_PPV_ARGS(&source))) && SUCCEEDED(draw_data.output_color->QueryInterface(IID_PPV_ARGS(&output))))
         {
            D3D11_TEXTURE2D_DESC source_desc, output_desc;
            source->GetDesc(&source_desc);
            output->GetDesc(&output_desc);
            const D3D11_BOX box = {0, 0, 0, (std::min)(source_desc.Width, output_desc.Width), (std::min)(source_desc.Height, output_desc.Height), 1};
            if (source_desc.Format == output_desc.Format)
            {
               command_list->CopySubresourceRegion(output.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
            }
         }
      }

      // Copies the inputs the bridge copies, then signals "in" with the next frame number (returned) and flushes
      uint64_t Submit(ID3D11DeviceContext* command_list, ID3D11Resource* const* resources, UINT render_width, UINT render_height)
      {
         SR_BRIDGE_PROFILE(command_list, 0);
         for (int i = 0; i < kCount; i++)
         {
            if (i == SRBridgeProtocol::kOutput || !copied[i])
            {
               continue;
            }
            if (i == SRBridgeProtocol::kExposure)
            {
               command_list->CopyResource(shared[i].Get(), resources[i]);
               continue;
            }
            // The upscaler reads the render resolution inputs only within the render size (a render scale draws into the top left of larger targets)
            D3D11_TEXTURE2D_DESC desc;
            shared[i]->GetDesc(&desc);
            const UINT width = (std::min)(render_width, desc.Width), height = (std::min)(render_height, desc.Height);
            if (i == SRBridgeProtocol::kDepth && depth_copy_uav)
            {
               CopyDepth(command_list, resources[i], width, height);
               continue;
            }
            const D3D11_BOX render_area = {0, 0, 0, width, height, 1};
            command_list->CopySubresourceRegion(shared[i].Get(), 0, 0, 0, 0, resources[i], 0, &render_area);
         }
         SR_BRIDGE_PROFILE(command_list, 1);
         const uint64_t n = ++frame;
         context->Signal(fences[0].Get(), n);
         command_list->Flush();
         SR_BRIDGE_PROFILE(command_list, 2);
         return n;
      }

      // A depth format's render area into the shared R32_FLOAT texture, restoring the compute state it touches
      void CopyDepth(ID3D11DeviceContext* command_list, ID3D11Resource* source, UINT width, UINT height)
      {
         if (depth_copy_srv_source != source)
         {
            const D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {.Format = depth_read_format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1}};
            depth_copy_srv.Reset();
            if (FAILED(device->CreateShaderResourceView(source, &srv_desc, &depth_copy_srv)))
            {
               return;
            }
            depth_copy_srv_source = source;
         }
         const uint32_t size[4] = {width, height, 0, 0};
         command_list->UpdateSubresource(depth_copy_size.Get(), 0, nullptr, size, 0, 0);

         Microsoft::WRL::ComPtr<ID3D11ComputeShader> previous_cs;
         Microsoft::WRL::ComPtr<ID3D11ClassInstance> previous_instances[D3D11_SHADER_MAX_INTERFACES];
         UINT previous_instance_count = D3D11_SHADER_MAX_INTERFACES;
         command_list->CSGetShader(&previous_cs, previous_instances[0].GetAddressOf(), &previous_instance_count);
         Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> previous_srv;
         command_list->CSGetShaderResources(0, 1, &previous_srv);
         Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> previous_uav;
         command_list->CSGetUnorderedAccessViews(0, 1, &previous_uav);
         Microsoft::WRL::ComPtr<ID3D11Buffer> previous_cb;
         command_list->CSGetConstantBuffers(0, 1, &previous_cb);

         command_list->CSSetShader(depth_copy_cs.Get(), nullptr, 0);
         ID3D11ShaderResourceView* const srv = depth_copy_srv.Get();
         command_list->CSSetShaderResources(0, 1, &srv);
         ID3D11UnorderedAccessView* const uav = depth_copy_uav.Get();
         command_list->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
         ID3D11Buffer* const cb = depth_copy_size.Get();
         command_list->CSSetConstantBuffers(0, 1, &cb);
         command_list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

         ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES] = {};
         for (UINT i = 0; i < previous_instance_count; i++)
         {
            instances[i] = previous_instances[i].Get();
         }
         command_list->CSSetShader(previous_cs.Get(), instances, previous_instance_count);
         ID3D11ShaderResourceView* const restored_srv = previous_srv.Get();
         command_list->CSSetShaderResources(0, 1, &restored_srv);
         ID3D11UnorderedAccessView* const restored_uav = previous_uav.Get();
         command_list->CSSetUnorderedAccessViews(0, 1, &restored_uav, nullptr);
         ID3D11Buffer* const restored_cb = previous_cb.Get();
         command_list->CSSetConstantBuffers(0, 1, &restored_cb);
      }

      // After the upscaler was told of frame n: the game's GPU waits for its output, which is copied back if the upscaler wrote a copy
      void WaitOutput(ID3D11DeviceContext* command_list, ID3D11Resource* output_color, uint64_t n)
      {
         context->Wait(fences[1].Get(), n);
         SR_BRIDGE_PROFILE(command_list, 4);
         if (copied[SRBridgeProtocol::kOutput])
         {
            command_list->CopyResource(output_color, shared[SRBridgeProtocol::kOutput].Get());
         }
         SR_BRIDGE_PROFILE(command_list, 5);
      }
   };
} // namespace SRBridge
