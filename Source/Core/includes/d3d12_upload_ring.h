#pragma once

#if DX12

// Memory for the root constant buffer views Luma appends to the game's root signatures ("LumaSettings", "LumaData"). A root CBV is a GPU
// address recorded into a command list, so what it points at must keep its bytes until the GPU ran every list that recorded it. The memory
// is persistently mapped UPLOAD buffers ("Pool"), handed out as 256 byte aligned allocations of 64 KiB pages. The ring never waits for the
// GPU: it grows when no page is free, and takes pages back by the fences of the queues that ran them. Native D3D12 only (no ReShade
// calls): the caller passes the native device (growing creates a buffer under the pool's lock, a ReShade proxy would fire events), the
// native queues and its device's present count, and holds the pool in a "std::shared_ptr" (a command list's data can outlive its
// device's, ReShade's command list proxies don't reference the device).
//
// A command list allocates from pages of its own ("Pages", used by one recording thread at a time, no lock). When the list is reset or
// destroyed ("Retire()") its pages are free at once if it never executed, else once every queue that executed it ("OnExecute()") passed
// the value "Present()" signals there, "EXECUTE_MARGIN" presents after the execution. Pages every list may point at (the device's
// copies of the settings) are a "Pages::shared" that retires its page against all queues when it fills.
//
// Call order: "RegisterQueue()" for each DIRECT and COMPUTE queue (the already existing ones too, when the pool is made late),
// "Allocate()" while recording, "OnExecute()" at "execute_command_list", "Retire()" at "reset_command_list" and "destroy_command_list",
// "UnregisterQueue()" at "destroy_command_queue", and "Present()" at each present with the count it makes. A list executed at count N is
// only certain to be done once the fences passed N + 2, see "EXECUTE_MARGIN".

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include <Windows.h>
#include <d3d12.h>
#include <source/com_ptr.hpp>

namespace D3D12UploadRing
{
   constexpr uint32_t PAGE_SIZE = 64 * 1024;
   constexpr uint32_t PAGES_PER_BLOCK = 16; // A block is one UPLOAD buffer, mapped for its whole life
   constexpr uint32_t BLOCK_SIZE = PAGES_PER_BLOCK * PAGE_SIZE;
   constexpr uint32_t ALIGNMENT = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

   // ReShade fires "execute_command_list" before the native "ExecuteCommandLists", which another thread may still be inside when the next
   // present signals the queues: a list executed at count N is only certain to be submitted before the signal of N + 2.
   // ponytail: a direct command list executed again more than a frame after its last reset reads pages the ring already reused (valid
   // memory, wrong values), as the shared pages it points at are only held for this margin. If a game does that, keep the shared pages
   // a list recorded against until it is reset.
   constexpr uint64_t EXECUTE_MARGIN = 2;

   // Memory a CBV can point at. "cpu" is write combined: write it, don't read it.
   struct Allocation
   {
      void* cpu = nullptr; // Null if the pool couldn't grow
      D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
   };

   // The GPU may read pages until "queue", which ran them, passed "value" of its fence
   struct Wait
   {
      const ID3D12CommandQueue* queue = nullptr; // Compared only
      uint64_t value = 0;
   };

   // The pages a command list allocated from since its last "Retire()"
   struct Pages
   {
      std::vector<Allocation> used; // The current page last
      uint32_t offset = 0;          // Bytes allocated from the current page
      std::vector<Wait> waits;      // Per queue that executed "used", see "OnExecute()"
      // For the pages every command list may point at, whichever list allocated them: a list records the address of the newest allocation
      // until the next upload replaces it. "Allocate()" retires the page when it fills, to wait for every registered queue; there is no
      // "OnExecute()" or "Retire()". The caller serializes the uses of one shared "Pages".
      bool shared = false;
   };

   struct Queue
   {
      ID3D12CommandQueue* queue = nullptr; // No reference, it is unregistered before it is destroyed
      com_ptr<ID3D12Fence> fence;          // Signaled on "queue" by "Present()"
      uint64_t completed = 0;              // The fence's value at the last "Reclaim()"
   };

   // The pages of one "Retire()", free once all its waits passed
   struct Retired
   {
      std::vector<Allocation> pages;
      std::vector<Wait> waits;
   };

   // ponytail: blocks are never freed or capped, so the pool stays at its peak (the pages in flight over the margin) and a game that
   // records lists without presenting grows it without bound. Free idle blocks, or cap it, if a game does that.
   struct Pool
   {
      std::shared_mutex mutex; // Guards everything below
      com_ptr<ID3D12Device> device;
      std::vector<com_ptr<ID3D12Resource>> blocks; // UPLOAD buffers, mapped
      std::vector<Allocation> free_pages;
      std::vector<Retired> retiring;
      std::vector<Queue> queues;

      explicit Pool(ID3D12Device* native_device)
          : device(native_device)
      {
      }
   };

   // Expects "pool->mutex" (exclusive). False if the device couldn't make the buffer.
   inline bool AddBlock(Pool* pool)
   {
      const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_UPLOAD};
      const D3D12_RESOURCE_DESC desc = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = BLOCK_SIZE, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {.Count = 1}, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
      com_ptr<ID3D12Resource> block;
      std::byte* cpu = nullptr;
      const D3D12_RANGE nothing_read = {};
      if (FAILED(pool->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&block))) || FAILED(block->Map(0, &nothing_read, reinterpret_cast<void**>(&cpu))))
      {
         return false;
      }
      const D3D12_GPU_VIRTUAL_ADDRESS gpu = block->GetGPUVirtualAddress();
      for (uint32_t page = 0; page < PAGES_PER_BLOCK; ++page)
      {
         pool->free_pages.push_back({.cpu = cpu + page * PAGE_SIZE, .gpu = gpu + page * PAGE_SIZE});
      }
      pool->blocks.push_back(std::move(block));
      return true;
   }

   // Expects "pool->mutex" (exclusive). Pages nothing waits for are free at once.
   inline void Release(Pool* pool, std::vector<Allocation> pages, std::vector<Wait> waits)
   {
      if (waits.empty())
      {
         pool->free_pages.insert(pool->free_pages.end(), pages.begin(), pages.end());
      }
      else
      {
         pool->retiring.push_back({.pages = std::move(pages), .waits = std::move(waits)});
      }
   }

   // "size" bytes for a CBV (at most PAGE_SIZE), from the list's current page or a new one if it doesn't fit. "present_count" is the
   // device's now, only a shared "Pages" uses it. Null "cpu" if the pool couldn't grow.
   inline Allocation Allocate(Pool* pool, Pages* pages, uint32_t size, uint64_t present_count)
   {
      assert(size <= PAGE_SIZE);
      const uint32_t aligned_size = (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
      if (pages->used.empty() || pages->offset + aligned_size > PAGE_SIZE)
      {
         const std::unique_lock lock(pool->mutex);
         if (pages->shared && !pages->used.empty())
         {
            std::vector<Wait> waits;
            for (const Queue& queue : pool->queues)
            {
               waits.push_back({.queue = queue.queue, .value = present_count + EXECUTE_MARGIN});
            }
            Release(pool, std::exchange(pages->used, {}), std::move(waits));
         }
         if (pool->free_pages.empty() && !AddBlock(pool))
         {
            return {};
         }
         pages->used.push_back(pool->free_pages.back());
         pool->free_pages.pop_back();
         pages->offset = 0;
      }
      const Allocation& page = pages->used.back();
      const uint32_t offset = pages->offset;
      pages->offset += aligned_size;
      return {.cpu = static_cast<std::byte*>(page.cpu) + offset, .gpu = page.gpu + offset};
   }

   // The list's pages ran on "queue" (the native one, registered), called at "execute_command_list" with the device's present count
   inline void OnExecute(Pages* pages, const ID3D12CommandQueue* queue, uint64_t present_count)
   {
      if (pages->used.empty())
      {
         return;
      }
      const uint64_t value = present_count + EXECUTE_MARGIN;
      const auto wait = std::ranges::find(pages->waits, queue, &Wait::queue);
      if (wait == pages->waits.end())
      {
         pages->waits.push_back({.queue = queue, .value = value});
      }
      else
      {
         wait->value = (std::max)(wait->value, value);
      }
   }

   // At the list's reset or destruction. For lists, not for a shared "Pages" (the pool's destruction releases those).
   inline void Retire(Pool* pool, Pages* pages)
   {
      assert(!pages->shared);
      if (pages->used.empty())
      {
         return;
      }
      const std::unique_lock lock(pool->mutex);
      Release(pool, std::exchange(pages->used, {}), std::exchange(pages->waits, {}));
   }

   // Expects "pool->mutex" (exclusive). Frees the retired pages whose queues all passed their wait, or are gone: a destroyed queue
   // finished its work.
   inline void Reclaim(Pool* pool)
   {
      for (Queue& queue : pool->queues)
      {
         queue.completed = queue.fence->GetCompletedValue();
      }
      const auto passed = [pool](const Wait& wait)
      {
         const auto queue = std::ranges::find(pool->queues, wait.queue, &Queue::queue);
         return queue == pool->queues.end() || queue->completed >= wait.value;
      };
      std::erase_if(pool->retiring, [pool, &passed](const Retired& retired)
         {
            if (!std::ranges::all_of(retired.waits, passed))
               return false;
            pool->free_pages.insert(pool->free_pages.end(), retired.pages.begin(), retired.pages.end());
            return true; });
   }

   // Only DIRECT and COMPUTE queues can run lists that set root CBVs: false for the others, or if the fence couldn't be made. The pool
   // makes the queue's fence, so a queue the pool never saw has no waits (those count as passed).
   inline bool RegisterQueue(Pool* pool, ID3D12CommandQueue* queue)
   {
      const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
      com_ptr<ID3D12Fence> fence;
      if ((type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) || FAILED(pool->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
      {
         return false;
      }
      const std::unique_lock lock(pool->mutex);
      if (std::ranges::find(pool->queues, queue, &Queue::queue) == pool->queues.end())
      {
         pool->queues.push_back({.queue = queue, .fence = std::move(fence)});
      }
      return true;
   }

   // Drops the waits on a destroyed queue, which finished its work: what only it held is free now
   inline void UnregisterQueue(Pool* pool, const ID3D12CommandQueue* queue)
   {
      const std::unique_lock lock(pool->mutex);
      std::erase_if(pool->queues, [queue](const Queue& registered)
         { return registered.queue == queue; });
      Reclaim(pool);
   }

   // At each present, with the count it makes: signals the count on every queue, then frees what they passed so far (a signal completes
   // after the call, so the pages lag a present). Never waits for the GPU.
   inline void Present(Pool* pool, uint64_t present_count)
   {
      const std::unique_lock lock(pool->mutex);
      for (const Queue& queue : pool->queues)
      {
         queue.queue->Signal(queue.fence.get(), present_count);
      }
      Reclaim(pool);
   }
} // namespace D3D12UploadRing

#endif // DX12
