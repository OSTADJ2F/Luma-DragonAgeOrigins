// Unit test of the CSMT command ring (csmt_queue.h): order, payload copies, wrap-around padding, back pressure
// on a full ring, Drain, idle polling and stop. Exit code 0 = pass.
//   csmt_queue_test.exe
#include "../csmt_queue.h"
#include <cstdio>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(condition, ...)                        \
   do                                                \
   {                                                 \
      if (!(condition))                              \
      {                                              \
         g_failures++;                               \
         printf("FAIL %s:%d: ", __FILE__, __LINE__); \
         printf(__VA_ARGS__);                        \
         printf("\n");                               \
      }                                              \
   } while (0)

static uint32_t Byte(uint32_t sequence, uint32_t i)
{
   return (sequence * 2654435761u + i * 40503u) >> 13;
}

// `count` commands with payloads of 0..max_payload bytes through a ring of `capacity`, consumer checks order and
// content. Drains every `drain_every` pushes (0 = never).
static void OrderAndPayloads(size_t capacity, uint32_t count, uint32_t max_payload, uint32_t drain_every)
{
   Csmt::CommandQueue queue(capacity);
   uint32_t expected = 0;
   uint64_t bad_payloads = 0;
   std::thread consumer([&]
      { queue.Run([]
           { return DWORD(INFINITE); }); });
   std::vector<uint8_t> payload;
   for (uint32_t sequence = 0; sequence < count; sequence++)
   {
      const uint32_t bytes = (max_payload ? (Byte(sequence, 7) % (max_payload + 1)) : 0);
      payload.resize(bytes);
      for (uint32_t i = 0; i < bytes; i++)
      {
         payload[i] = uint8_t(Byte(sequence, i));
      }
      queue.Push(
         [&, sequence, bytes](char* data)
         {
            if (sequence != expected)
            {
               printf("  order: got %u expected %u\n", sequence, expected);
            }
            expected = sequence + 1;
            for (uint32_t i = 0; i < bytes; i++)
            {
               bad_payloads += (uint8_t(data[i]) != uint8_t(Byte(sequence, i)));
            }
         },
         payload.data(), bytes);
      if (drain_every && sequence % drain_every == drain_every - 1)
      {
         queue.Drain();
         CHECK(expected == sequence + 1, "drain returned before command %u ran (at %u)", sequence, expected);
      }
   }
   queue.Drain();
   CHECK(expected == count, "ran %u of %u", expected, count);
   CHECK(bad_payloads == 0, "%llu payload bytes differ", bad_payloads);
   CHECK(queue.Consumed() == queue.Pushed(), "consumed %u pushed %u", queue.Consumed(), queue.Pushed());
   queue.Push([&](char*)
      { queue.RequestStop(); });
   consumer.join();
   printf("order/payloads: capacity %zu, %u commands, payload <= %u, drain every %u: %s\n", capacity, count,
      max_payload, drain_every, g_failures ? "FAILED" : "ok");
}

// A payload filled through the returned pointer, after Push, is not allowed: Push publishes at once. The closure must
// see the payload copied at Push time even when the source changes right after.
static void PayloadIsCopied()
{
   Csmt::CommandQueue queue(1 << 16);
   int seen = -1;
   std::thread consumer([&]
      { queue.Run([]
           { return DWORD(INFINITE); }); });
   int value = 41;
   queue.Push([&](char* data)
      { Sleep(5), memcpy(&seen, data, sizeof(seen)); }, &value, sizeof(value));
   value = 99;
   queue.Drain();
   CHECK(seen == 41, "payload not copied at Push: %d", seen);
   queue.Push([&](char*)
      { queue.RequestStop(); });
   consumer.join();
   printf("payload copied at push: %s\n", seen == 41 ? "ok" : "FAILED");
}

// Idle runs when the ring is empty; its sleep bound lets the consumer poll without new commands.
static void IdlePolling()
{
   Csmt::CommandQueue queue(1 << 16);
   std::atomic<int> idles = 0;
   std::thread consumer([&]
      { queue.Run([&]
           { return idles++ < 20 ? DWORD(1) : DWORD(INFINITE); }); });
   const DWORD start = GetTickCount();
   while (idles < 20 && GetTickCount() - start < 2000)
   {
      Sleep(1);
   }
   CHECK(idles >= 20, "idle polled %d times in 2 s", idles.load());
   queue.Push([&](char*)
      { queue.RequestStop(); });
   consumer.join();
   printf("idle polling: %d polls\n", idles.load());
}

// A consumer that SendMessages to the producer's window while the producer is in Drain must not deadlock.
static void DrainDispatchesSentMessages()
{
   WNDCLASSA wc = {.lpfnWndProc = DefWindowProcA, .hInstance = GetModuleHandleA(nullptr), .lpszClassName = "csmtq"};
   RegisterClassA(&wc);
   HWND hwnd = CreateWindowA("csmtq", "csmtq", 0, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
   Csmt::CommandQueue queue(1 << 16);
   std::thread consumer([&]
      { queue.Run([]
           { return DWORD(INFINITE); }); });
   LRESULT result = -1;
   queue.Push([&](char*)
      { result = SendMessageA(hwnd, WM_NULL, 0, 0); });
   queue.Drain();
   CHECK(result == 0, "SendMessage from the consumer returned %ld", long(result));
   queue.Push([&](char*)
      { queue.RequestStop(); });
   consumer.join();
   DestroyWindow(hwnd);
   printf("drain dispatches sent messages: %s\n", result == 0 ? "ok" : "FAILED");
}

// Throughput of empty commands, for the per-call overhead the layer adds on the game thread.
static void Throughput()
{
   Csmt::CommandQueue queue(32 << 20);
   std::thread consumer([&]
      { queue.Run([]
           { return DWORD(INFINITE); }); });
   uint64_t sum = 0;
   LARGE_INTEGER frequency, begin, end;
   QueryPerformanceFrequency(&frequency);
   QueryPerformanceCounter(&begin);
   constexpr int COUNT = 2000000;
   for (int i = 0; i < COUNT; i++)
   {
      queue.Push([&sum, i](char*)
         { sum += i; });
   }
   LARGE_INTEGER pushed;
   QueryPerformanceCounter(&pushed);
   queue.Drain();
   QueryPerformanceCounter(&end);
   CHECK(sum == uint64_t(COUNT) * (COUNT - 1) / 2, "sum %llu", sum);
   queue.Push([&](char*)
      { queue.RequestStop(); });
   consumer.join();
   printf("throughput: push %.1f ns/command, end to end %.1f ns/command\n",
      (pushed.QuadPart - begin.QuadPart) * 1e9 / frequency.QuadPart / COUNT,
      (end.QuadPart - begin.QuadPart) * 1e9 / frequency.QuadPart / COUNT);
}

int main()
{
   OrderAndPayloads(1 << 20, 200000, 0, 0);
   OrderAndPayloads(4096, 100000, 900, 0); // constant wrap-around and back pressure
   OrderAndPayloads(4096, 20000, 960, 7);  // payloads at the size limit, frequent drains
   OrderAndPayloads(1 << 16, 50000, 3000, 1000);
   PayloadIsCopied();
   IdlePolling();
   DrainDispatchesSentMessages();
   Throughput();
   printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
   return g_failures ? 1 : 0;
}
