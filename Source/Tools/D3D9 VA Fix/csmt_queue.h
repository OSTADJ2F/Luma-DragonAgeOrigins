// Single-producer, single-consumer command ring for the CSMT layer: the game thread records D3D9 calls as closures
// (plus copied payloads), one worker thread replays them in order. The producer side is not thread-safe; the CSMT
// layer serializes producers with its own lock.
//
// Cost model: Push only writes producer-owned memory. Commands are published in batches (or at once while the consumer
// is idle), and the consumer reports progress every few commands, so the shared cache lines move per batch, not per
// call. Positions are 32-bit (64-bit atomics are cmpxchg8b on x86) and grow modulo 2^32 over a power-of-two ring.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <malloc.h>
#include <new>
#include <utility>
#include <windows.h>

namespace Csmt
{
   class CommandQueue
   {
   public:
      // `capacity` is rounded up to a power of two; one command plus its payload must fit in MaxPayload.
      explicit CommandQueue(size_t capacity)
          : capacity_(RoundUpToPowerOfTwo(capacity)),
            buffer_((char*)_aligned_malloc(capacity_, 64)),
            consumer_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
            producer_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr))
      {
      }

      ~CommandQueue()
      {
         _aligned_free(buffer_);
         CloseHandle(consumer_event_);
         CloseHandle(producer_event_);
      }

      CommandQueue(const CommandQueue&) = delete;
      CommandQueue& operator=(const CommandQueue&) = delete;

      size_t MaxPayload() const
      {
         return capacity_ / 4 - 64;
      }

      // `command(char* payload)` runs on the consumer; `payload` is a copy of `payload_bytes` bytes from `payload`,
      // or, when `payload` is null, whatever the caller writes through the returned pointer before the next Push,
      // Flush or Drain.
      template <typename F>
      char* Push(F&& command, const void* payload = nullptr, size_t payload_bytes = 0)
      {
         using C = Closure<std::decay_t<F>>;
         const uint32_t bytes = uint32_t((sizeof(C) + payload_bytes + 15) & ~size_t(15));
         char* at = Reserve(bytes);
         C* closure = new (at) C(std::forward<F>(command));
         closure->run = &C::Run;
         closure->bytes = bytes;
         char* payload_copy = at + sizeof(C);
         if (payload)
         {
            memcpy(payload_copy, payload, payload_bytes);
         }
         if (write_ - published_local_ >= PUBLISH_BYTES || consumer_idle_.load(std::memory_order_relaxed))
         {
            Publish();
         }
         return payload_copy;
      }

      // Producer: makes every pushed command visible to the consumer.
      void Flush()
      {
         if (published_local_ != write_)
         {
            Publish();
         }
      }

      // Producer: returns once every command pushed so far has run.
      void Drain()
      {
         Flush();
         WaitForProducer([&]
            { return consumed_.load(std::memory_order_seq_cst) == write_; });
      }

      // Producer: waits until `ready()` holds, woken by the consumer as it makes progress. Sent window messages are
      // dispatched meanwhile, so a consumer blocked in SendMessage to this thread (DXGI) cannot deadlock it.
      template <typename Ready>
      void WaitForProducer(Ready&& ready)
      {
         for (int spin = 0; spin < 4000; spin++)
         {
            if (ready())
               return;
            YieldProcessor();
         }
         for (;;)
         {
            producer_waiting_.store(true, std::memory_order_seq_cst);
            if (ready())
               break;
            if (MsgWaitForMultipleObjects(1, &producer_event_, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1)
            {
               MSG msg;
               PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            }
         }
         producer_waiting_.store(false, std::memory_order_relaxed);
      }

      // Consumer: runs commands as they arrive until a command calls RequestStop. `idle()` runs whenever the queue is
      // empty and returns how long the consumer may sleep (INFINITE when it has nothing to poll); `busy()` runs every
      // BUSY_EVERY commands while it isn't.
      template <typename Idle>
      void Run(Idle&& idle)
      {
         Run(std::forward<Idle>(idle), [] {});
      }

      static constexpr int BUSY_EVERY = 128;

      template <typename Idle, typename Busy>
      void Run(Idle&& idle, Busy&& busy)
      {
         while (!stop_)
         {
            uint32_t published = published_.load(std::memory_order_acquire);
            if (read_ == published)
            {
               consumer_idle_.store(true, std::memory_order_relaxed);
               const DWORD sleep_ms = idle();
               for (int spin = 0; spin < 2000 && read_ == published; spin++)
               {
                  YieldProcessor();
                  published = published_.load(std::memory_order_acquire);
               }
               if (read_ == published)
               {
                  consumer_waiting_.store(true, std::memory_order_seq_cst);
                  if (published_.load(std::memory_order_seq_cst) == read_)
                  {
                     WaitForSingleObject(consumer_event_, sleep_ms);
                  }
                  consumer_waiting_.store(false, std::memory_order_relaxed);
               }
               consumer_idle_.store(false, std::memory_order_relaxed);
               continue;
            }
            for (int done = 1; read_ != published && !stop_; done++)
            {
               auto* record = (Record*)(buffer_ + (read_ & (capacity_ - 1)));
               const uint32_t bytes = record->bytes;
               if (record->run)
               {
                  record->run(record);
               }
               read_ += bytes;
               if (done % 32 == 0)
               {
                  ReportProgress();
               }
               if (done % BUSY_EVERY == 0)
               {
                  busy();
               }
            }
            ReportProgress();
         }
      }

      // Called from a command: Run returns after it.
      void RequestStop()
      {
         stop_ = true;
      }

      uint32_t Pushed() const
      {
         return write_;
      }

      uint32_t Consumed() const
      {
         return consumed_.load(std::memory_order_acquire);
      }

   private:
      static constexpr uint32_t PUBLISH_BYTES = 4096;

      struct Record
      {
         void (*run)(Record*); // null: padding up to the end of the ring
         uint32_t bytes;
      };

      template <typename F>
      struct Closure : Record
      {
         explicit Closure(F&& f) : f(std::move(f))
         {
         }

         explicit Closure(const F& f) : f(f)
         {
         }

         F f;

         static void Run(Record* record)
         {
            auto* self = (Closure*)record;
            self->f((char*)self + sizeof(Closure));
            self->~Closure();
         }
      };

      static size_t RoundUpToPowerOfTwo(size_t value)
      {
         size_t result = 4096;
         while (result < value)
         {
            result *= 2;
         }
         return result;
      }

      void ReportProgress()
      {
         consumed_.store(read_, std::memory_order_seq_cst);
         if (producer_waiting_.load(std::memory_order_seq_cst))
         {
            SetEvent(producer_event_);
         }
      }

      // Space for `bytes` at the write position, padding to the start of the ring first if they don't fit before
      // its end. Waits for the consumer when the ring is full.
      char* Reserve(uint32_t bytes)
      {
         const uint32_t offset = write_ & uint32_t(capacity_ - 1);
         const uint32_t tail = uint32_t(capacity_) - offset;
         const uint32_t needed = (tail < bytes) ? (tail + bytes) : bytes;
         if (write_ + needed - consumed_cache_ > capacity_)
         {
            consumed_cache_ = consumed_.load(std::memory_order_acquire);
            if (write_ + needed - consumed_cache_ > capacity_)
            {
               Publish();
               WaitForProducer([&]
                  {
                     consumed_cache_ = consumed_.load(std::memory_order_seq_cst);
                     return write_ + needed - consumed_cache_ <= capacity_; });
            }
         }
         if (tail < bytes)
         {
            auto* padding = (Record*)(buffer_ + offset);
            padding->run = nullptr;
            padding->bytes = tail;
            write_ += tail;
         }
         char* at = buffer_ + (write_ & uint32_t(capacity_ - 1));
         write_ += bytes;
         return at;
      }

      void Publish()
      {
         published_local_ = write_;
         published_.store(write_, std::memory_order_seq_cst);
         if (consumer_waiting_.load(std::memory_order_seq_cst))
         {
            SetEvent(consumer_event_);
         }
      }

      const size_t capacity_;
      char* const buffer_;
      const HANDLE consumer_event_;
      const HANDLE producer_event_;
      alignas(64) uint32_t write_ = 0; // producer only
      uint32_t published_local_ = 0;   // producer only
      uint32_t consumed_cache_ = 0;    // producer only
      alignas(64) std::atomic<uint32_t> published_{0};
      std::atomic<bool> producer_waiting_{false};
      alignas(64) std::atomic<bool> consumer_idle_{false};
      std::atomic<bool> consumer_waiting_{false};
      alignas(64) uint32_t read_ = 0; // consumer only
      bool stop_ = false;             // consumer only
      alignas(64) std::atomic<uint32_t> consumed_{0};
   };
} // namespace Csmt
