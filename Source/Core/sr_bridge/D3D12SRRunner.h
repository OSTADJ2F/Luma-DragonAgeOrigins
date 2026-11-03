#pragma once

// Runs Core's D3D12 super resolution implementations (DLSS, FSR, XeSS) on their own D3D12 device and queue, on textures and fences a D3D11 game
// shares by NT handle. Used by the x64 "SR Bridge Helper" (the 32-bit games' SR bridge, a separate process) and by an x64 D3D11 mod's
// in-process host (same process, handles from the game's own devices). Per frame: the queue waits for "in", moves the textures from COMMON
// to the "DrawData" states, draws, moves them back and signals "out".
// Include it from a D3D12 translation unit (the helper, "unity_build_d3d12.cpp"): it needs d3d12.h and the D3D12 implementations' types.
#include "SRBridgeProtocol.h"

#include "../includes/super_resolution.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <share.h>
#include <string>
#include <utility>
#include <vector>

namespace SRRunner
{
   using Microsoft::WRL::ComPtr;
   using Impl = SR::D3D12::SuperResolutionImpl;

   // A device and its direct queue. Command lists are recorded one at a time on an allocator ring, each guarded by the own fence.
   struct Gpu
   {
      // Frames in flight times the profile's extra list: with 3, "Open()" waited for the GPU and queued each frame late (DLSS 7.6 -> 13.3 ms
      // a frame in "_tools/sr_bridge_perf" with load)
      static constexpr int allocator_count = 8;

      ComPtr<ID3D12Device> device;
      ComPtr<ID3D12CommandQueue> queue;
      ComPtr<ID3D12Fence> fence;
      ComPtr<ID3D12CommandAllocator> allocators[allocator_count];
      uint64_t allocator_values[allocator_count] = {};
      ComPtr<ID3D12GraphicsCommandList> list;
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      uint64_t value = 0;
      uint64_t lists = 0;

      Gpu() = default;
      Gpu(const Gpu&) = delete;
      Gpu& operator=(const Gpu&) = delete;
      ~Gpu()
      {
         if (event)
         {
            CloseHandle(event);
         }
      }

      bool Init(IDXGIAdapter1* adapter)
      {
#pragma comment(lib, "dxguid.lib") // CLSID_D3D12DeviceFactory
         // Through the device factory: ReShade (in a game's process) wraps what "D3D12CreateDevice()" returns (NGX's DLSS-G then reported
         // itself unavailable on it); the factory isn't hooked. Not stored as the adapter's singleton, so the game or
         // another library asking for a D3D12 device doesn't get this native one. "D3D12CreateDevice()" where the runtime has no factory.
         ComPtr<ID3D12DeviceFactory> device_factory;
         if (SUCCEEDED(D3D12GetInterface(CLSID_D3D12DeviceFactory, IID_PPV_ARGS(&device_factory))) && SUCCEEDED(device_factory->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON)))
         {
            device_factory->CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
         }
         const D3D12_COMMAND_QUEUE_DESC queue_desc = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
         if ((!device && FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) || FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
             FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
         {
            return false;
         }
         for (ComPtr<ID3D12CommandAllocator>& allocator : allocators)
         {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
            {
               return false;
            }
         }
         if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list))))
         {
            return false;
         }
         list->Close();
         return true;
      }

      // Returns whether it completed in time
      bool Wait(uint64_t target, DWORD timeout = INFINITE) const
      {
         if (fence->GetCompletedValue() >= target)
         {
            return true;
         }
         fence->SetEventOnCompletion(target, event);
         return WaitForSingleObject(event, timeout) == WAIT_OBJECT_0;
      }

      // The list to record into, once the GPU is done with its allocator
      ID3D12GraphicsCommandList* Open()
      {
         const int i = int(lists++ % allocator_count);
         Wait(allocator_values[i]);
         allocators[i]->Reset();
         list->Reset(allocators[i].Get(), nullptr);
         return list.Get();
      }

      // Executes the open list, returns the value the own fence reaches once it's done
      uint64_t Execute()
      {
         list->Close();
         ID3D12CommandList* const submitted = list.Get();
         queue->ExecuteCommandLists(1, &submitted);
         queue->Signal(fence.Get(), ++value);
         allocator_values[(lists - 1) % allocator_count] = value;
         return value;
      }

      // Bounded, as the game's fence "in" can stay unsignaled forever if it died mid frame
      void WaitIdle()
      {
         queue->Signal(fence.Get(), ++value);
         Wait(value, 3000);
      }
   };

   // "LUMA_UPSCALER_PROFILE=<csv path>" (offline, "_tools/sr_bridge_perf", and a game's Development "Performance Test"): a row
   // per frame of CPU times (QPC, the same in every process) and GPU timestamps, readable while written
   struct Profile
   {
      struct Slot
      {
         uint64_t n = 0;
         int64_t cpu[4] = {};      // Line read, evaluation called, evaluation returned, flushed
         uint64_t fence_value = 0; // Of the own fence, once its timestamps are resolved
      };
      static constexpr int lag = 8;        // Frames before a slot is read back (long done by then)
      static constexpr int timestamps = 3; // Before the wait for "in", after it, after the evaluation

      Gpu* gpu;
      FILE* csv = nullptr;
      Slot slots[lag];
      uint64_t count = 0;
      uint64_t frequency = 0;
      ComPtr<ID3D12QueryHeap> heap;
      ComPtr<ID3D12Resource> readback;

      static int64_t Now()
      {
         LARGE_INTEGER time;
         QueryPerformanceCounter(&time);
         return time.QuadPart;
      }

      Profile(Gpu* gpu_, const char* path) : gpu(gpu_)
      {
         const D3D12_QUERY_HEAP_DESC heap_desc = {.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = lag * timestamps};
         const D3D12_HEAP_PROPERTIES heap_properties = {.Type = D3D12_HEAP_TYPE_READBACK};
         const D3D12_RESOURCE_DESC buffer = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = lag * timestamps * sizeof(uint64_t),
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc = {.Count = 1},
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
         if (FAILED(gpu->device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&heap))) ||
             FAILED(gpu->device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
             FAILED(gpu->queue->GetTimestampFrequency(&frequency)))
         {
            return;
         }
         csv = _fsopen(path, "w", _SH_DENYNO);
         if (csv)
         {
            fprintf(csv, "n,line,evaluate_call,evaluate_return,flushed,gpu_queued,gpu_waited,gpu_evaluated,gpu_frequency,disjoint\n");
         }
      }

      void Read(const Slot& slot, int index) const
      {
         gpu->Wait(slot.fence_value);
         uint64_t gpu_times[timestamps] = {};
         const D3D12_RANGE range = {size_t(index) * sizeof(gpu_times), size_t(index + 1) * sizeof(gpu_times)};
         const D3D12_RANGE written = {};
         void* mapped = nullptr;
         if (SUCCEEDED(readback->Map(0, &range, &mapped)))
         {
            memcpy(gpu_times, static_cast<const char*>(mapped) + range.Begin, sizeof(gpu_times));
            readback->Unmap(0, &written);
         }
         // No disjoint flag on D3D12: 0
         fprintf(csv, "%llu,%lld,%lld,%lld,%lld,%llu,%llu,%llu,%llu,%d\n", (unsigned long long)slot.n, slot.cpu[0], slot.cpu[1], slot.cpu[2], slot.cpu[3],
            (unsigned long long)gpu_times[0], (unsigned long long)gpu_times[1], (unsigned long long)gpu_times[2], (unsigned long long)frequency, 0);
         fflush(csv);
      }

      void Begin(ID3D12GraphicsCommandList* list, uint64_t n)
      {
         const int index = int(count % lag);
         if (count >= lag)
         {
            Read(slots[index], index);
         }
         slots[index].n = n;
         slots[index].cpu[0] = Now();
         list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * timestamps);
      }

      // 1: after the wait for "in", 2: after the evaluation (resolves the slot)
      void Mark(ID3D12GraphicsCommandList* list, int step)
      {
         const int index = int(count % lag);
         list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * timestamps + step);
         if (step == 2)
         {
            list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * timestamps, timestamps, readback.Get(), index * timestamps * sizeof(uint64_t));
         }
         slots[index].cpu[step] = Now();
      }

      // "fence_value": of the own fence, once the frame's list is done
      void End(uint64_t fence_value)
      {
         slots[count % lag].fence_value = fence_value;
         slots[count++ % lag].cpu[3] = Now();
      }

      void Finish()
      {
         for (uint64_t i = count > lag ? count - lag : 0; i < count; i++)
         {
            Read(slots[i % lag], int(i % lag));
         }
         fclose(csv);
      }
   };

   inline ComPtr<IDXGIAdapter1> FindAdapter(const LUID& luid)
   {
      ComPtr<IDXGIFactory1> factory;
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
      {
         return nullptr;
      }
      ComPtr<IDXGIAdapter1> adapter;
      for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
      {
         DXGI_ADAPTER_DESC1 desc;
         if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart)
         {
            return adapter;
         }
      }
      return nullptr;
   }

   using Frame = SRBridgeProtocol::Frame;

   class Runner
   {
   public:
      // The in-process host shares one device and queue among the runners of a process (an upscaler that is picked again), the helper has its own
      explicit Runner(std::shared_ptr<Gpu> shared_gpu = std::make_shared<Gpu>()) : gpu_shared(std::move(shared_gpu)), gpu(*gpu_shared)
      {
      }
      std::shared_ptr<Gpu> gpu_shared;
      Gpu& gpu;
      std::unique_ptr<Impl> impl;
      SR::InstanceData* data = nullptr;
      std::unique_ptr<Profile> profile;

      // Opens the game's shared textures ("SRBridgeProtocol::Resource" order, 0: none) and its fences "in" and "out" on this device. Returns the error, empty if fine.
      std::string OpenInputs(const uint64_t (&handles)[SRBridgeProtocol::kCount], const uint64_t (&fence_handles)[2])
      {
         using namespace SRBridgeProtocol;
         for (int i = 0; i < kCount; i++)
         {
            if (handles[i] && FAILED(gpu.device->OpenSharedHandle(HANDLE(uintptr_t(handles[i])), IID_PPV_ARGS(&textures[i]))))
            {
               return "open texture " + std::to_string(i);
            }
         }
         if (!textures[kColor] || !textures[kMotion] || !textures[kDepth] || !textures[kOutput] ||
             FAILED(gpu.device->OpenSharedHandle(HANDLE(uintptr_t(fence_handles[0])), IID_PPV_ARGS(&fence_in))) ||
             FAILED(gpu.device->OpenSharedHandle(HANDLE(uintptr_t(fence_handles[1])), IID_PPV_ARGS(&fence_out))))
         {
            return "inputs or fences";
         }

         // The textures are in COMMON between frames (shared across APIs), and the upscalers expect the "DrawData" states
         for (int i = 0; i < kCount; i++)
         {
            if (!textures[i])
            {
               continue;
            }
            const D3D12_RESOURCE_STATES state =
               i == kOutput ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            D3D12_RESOURCE_BARRIER barrier = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
               .Transition = {.pResource = textures[i].Get(), .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, .StateBefore = D3D12_RESOURCE_STATE_COMMON, .StateAfter = state}};
            barriers_before.push_back(barrier);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            barriers_after.push_back(barrier);
         }
         draw = {
            .output_color = textures[kOutput].Get(),
            .source_color = textures[kColor].Get(),
            .motion_vectors = textures[kMotion].Get(),
            .depth_buffer = textures[kDepth].Get(),
            .exposure = textures[kExposure].Get(),
            .bias_mask = textures[kReactive].Get(),
            .transparency_alpha = textures[kTransparency].Get(),
         };
         return {};
      }

      // Init and the first "UpdateSettings()" of the implementation (DLSS records its feature creation), then signals "out" at "kReady".
      // Even if it fails, "Quit()" releases what it did.
      bool Create(std::unique_ptr<Impl> new_impl, IDXGIAdapter1* adapter, const SR::SettingsData& settings)
      {
         impl = std::move(new_impl);
         if (!impl->Init(data, gpu.device.Get(), adapter) || !UpdateSettings(settings))
         {
            return false;
         }
         gpu.queue->Signal(fence_out.Get(), SRBridgeProtocol::kReady);
         return true;
      }

      // Creates the upscaler's objects for the settings
      bool UpdateSettings(const SR::SettingsData& settings)
      {
         const bool updated = impl->UpdateSettings(data, gpu.Open(), settings);
         gpu.Wait(gpu.Execute());
         return updated;
      }

      // Returns the path's error, false if its csv couldn't be created
      bool StartProfile(const char* path)
      {
         profile = std::make_unique<Profile>(&gpu, path);
         if (!profile->csv)
         {
            profile.reset();
         }
         return profile != nullptr;
      }

      // Waits for "in" n, upscales and signals "out" n. Returns whether the upscaler drew.
      bool RunFrame(const Frame& frame)
      {
         if (profile)
         {
            profile->Begin(gpu.Open(), frame.n);
            gpu.Execute();
         }
         gpu.queue->Wait(fence_in.Get(), frame.n);
         ID3D12GraphicsCommandList* const list = gpu.Open();
         if (profile)
         {
            profile->Mark(list, 1);
         }
         list->ResourceBarrier(UINT(barriers_before.size()), barriers_before.data());
         draw.reset = frame.reset != 0;
         draw.render_width = frame.render_width;
         draw.render_height = frame.render_height;
         draw.pre_exposure = frame.pre_exposure;
         draw.jitter_x = frame.jitter_x;
         draw.jitter_y = frame.jitter_y;
         draw.vert_fov = frame.vert_fov;
         draw.near_plane = frame.near_plane;
         draw.far_plane = frame.far_plane;
         draw.frame_index = frame.n;
         draw.user_sharpness = frame.sharpness;
         const bool drawn = impl->Draw(data, list, draw);
         list->ResourceBarrier(UINT(barriers_after.size()), barriers_after.data());
         if (profile)
         {
            profile->Mark(list, 2);
         }
         const uint64_t submitted = gpu.Execute();
         gpu.queue->Signal(fence_out.Get(), frame.n);
         impl->ReleaseRetired(data);
         if (profile)
         {
            profile->End(submitted);
         }
         return drawn;
      }

      // An exit after "kReady" first sets "out" past every frame: the game's GPU may already wait for the next frame, and only a killed
      // process's fences jump there by themselves. The bridge reads it as an exited helper.
      void ReleaseGame()
      {
         gpu.queue->Signal(fence_out.Get(), UINT64_MAX);
      }

      void FinishProfile()
      {
         if (profile)
         {
            profile->Finish();
         }
      }

      // Waits for the GPU, then releases the upscaler (it expects that also when its init failed)
      void Quit()
      {
         gpu.WaitIdle();
         if (impl)
         {
            impl->Deinit(data, gpu.device.Get());
         }
      }

   private:
      ComPtr<ID3D12Resource> textures[SRBridgeProtocol::kCount];
      ComPtr<ID3D12Fence> fence_in, fence_out;
      std::vector<D3D12_RESOURCE_BARRIER> barriers_before, barriers_after;
      Impl::DrawData draw;
   };
} // namespace SRRunner
