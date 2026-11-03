// CSMT ("command stream multithreading", Wine's term): D3D9 calls that only record work are queued and replayed on
// one worker thread, so the translation layer below (dgVoodoo -> D3D11 -> driver) runs in parallel with the game's
// render thread instead of inside it. DXVK gets most of its CPU win on old D3D9 games the same way.
//
// How calls are routed (every vtable slot of every D3D9 object the game can reach is patched):
// - deferred: state setters (repeats of the last value dropped), draws, clears, copies, Present, AddRef/Release of
//   resources, Issue, and Lock/Unlock of DYNAMIC buffers with DISCARD or NOOVERWRITE (the game writes into a shadow
//   copy, the worker copies the locked range in). They return D3D_OK (Present: the worker's result of the previous
//   frame; AddRef/Release: the public count kept on the game side);
// - answered on the game side: GetData from results the worker polled (S_FALSE until then), GetDesc/GetLevelDesc
//   from descriptions asked once per object, TestCooperativeLevel from the last Present;
// - everything else is synchronous: the worker runs it after the queued calls while the caller waits.
// Calls the worker makes (dgVoodoo calling its own objects through the vtable) go straight down.
//
// The worker is the only thread that ever calls the layer below, so the device is created without
// D3DCREATE_MULTITHREADED (dgVoodoo misrendered now and then when calls came from several threads, one at a time).
// Producers (game threads) are serialized by one lock. The game may run at most MAX_QUEUED_FRAMES frames ahead of
// the worker. Only for dgVoodoo: NVIDIA's native D3D9 driver hangs on work recorded by one thread and waited for by
// another (d3d9_memlog.cpp refuses it).
#pragma once
#include "csmt_methods.h"
#include "csmt_queue.h"
#include "vertex_constants.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <d3d9.h>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace Csmt
{
   enum Kind
   {
      DEVICE,
      SWAP_CHAIN,
      STATE_BLOCK,
      VERTEX_DECLARATION,
      VERTEX_SHADER,
      PIXEL_SHADER,
      TEXTURE,
      VOLUME_TEXTURE,
      CUBE_TEXTURE,
      VERTEX_BUFFER,
      INDEX_BUFFER,
      SURFACE,
      VOLUME,
      QUERY,
      KIND_COUNT,
      BASE_TEXTURE, // an IDirect3DBaseTexture9 out parameter: resolved through GetType
   };

   constexpr int MAX_SLOTS = CSMT_DEVICE_SLOTS;
   constexpr uint32_t MAX_QUEUED_FRAMES = 1;
   constexpr size_t QUEUE_BYTES = 32 << 20;
   constexpr DWORD MAX_QUERY_RESULT_BYTES = 16; // larger results (pipeline statistics) are always synchronous

   // ---- vtables -------------------------------------------------------------------------------------------------

   struct PatchedVtable
   {
      void** vtable;
      int kind;
      int slots;
      void* originals[MAX_SLOTS];
   };

   // Append-only, so lookups need no lock.
   inline PatchedVtable g_vtables[128];
   inline std::atomic<int> g_vtable_count{0};
   inline std::mutex g_patch_mutex;

   inline PatchedVtable* FindVtable(void** vtable)
   {
      const int count = g_vtable_count.load(std::memory_order_acquire);
      for (int i = 0; i < count; i++)
      {
         if (g_vtables[i].vtable == vtable)
            return &g_vtables[i];
      }
      return nullptr;
   }

   inline void* OriginalFor(void* object, int slot)
   {
      void** vtable = *(void***)object;
      if (PatchedVtable* patched = FindVtable(vtable))
         return patched->originals[slot];
      return vtable[slot];
   }

   // A hook installed later on a slot this layer owns goes below it: the layer calls `hook`, which must call the
   // returned function. Null when the layer doesn't own the slot.
   inline void* InsertBelow(void* object, int slot, void* hook)
   {
      PatchedVtable* patched = FindVtable(*(void***)object);
      if (!patched || slot >= patched->slots)
         return nullptr;
      std::lock_guard lock(g_patch_mutex);
      void* below = patched->originals[slot];
      patched->originals[slot] = hook;
      return below;
   }

   // ---- shared state --------------------------------------------------------------------------------------------

   struct QueryState
   {
      IDirect3DQuery9* query = nullptr;
      DWORD size = 0;
      uint32_t issued = 0;               // producer: Issue(END) calls pushed
      std::atomic<uint32_t> executed{0}; // worker: Issue(END) calls run
      std::atomic<uint32_t> ready{0};    // the issue whose result is in `data`
      std::atomic<HRESULT> result{S_OK};
      std::atomic<bool> wanted{false}; // the game asked and got S_FALSE: polled between commands too
      bool polled = false;             // worker: in the poll list
      bool dead = false;               // worker: released
      bool building = false;           // worker: between Issue(BEGIN) and Issue(END); its data belongs to no issue yet
      alignas(8) uint8_t data[MAX_QUERY_RESULT_BYTES] = {};
   };

   struct DescEntry
   {
      uint32_t key; // slot << 16 | level
      uint8_t bytes[32];
   };

   struct PendingLock
   {
      UINT offset;
      UINT size;
      DWORD flags;
   };

   // The device state as last pushed, so a setter that repeats it isn't queued (UE3 sets every sampler state of
   // every draw). Unknown entries are always pushed. Forgotten when the device state changes behind the setters
   // (Reset, StateBlock::Apply) and ignored while a state block records.
   struct StateShadow
   {
      static constexpr int SAMPLERS = 21; // 16 pixel + displacement map + 4 vertex samplers
      DWORD render[256];
      DWORD sampler[SAMPLERS][16];
      DWORD stage[8][33];
      void* texture[SAMPLERS];
      void* shader[2]; // vertex, pixel
      struct Stream
      {
         void* buffer;
         UINT offset, stride;
         bool operator==(const Stream&) const = default;
      };
      Stream stream[16];
      UINT stream_frequency[16];
      void* indices;
      void* declaration;
      bool render_known[256];
      bool sampler_known[SAMPLERS][16];
      bool stage_known[8][33];
      bool texture_known[SAMPLERS];
      bool shader_known[2];
      bool stream_known[16];
      bool stream_frequency_known[16];
      bool indices_known;
      bool declaration_known;
      bool recording; // between BeginStateBlock and EndStateBlock

      void Forget()
      {
         *this = {};
      }

      // Pixel samplers 0-15, D3DDMAPSAMPLER (256) and D3DVERTEXTEXTURESAMPLER0-3 (257-260); -1 otherwise.
      static int SamplerIndex(DWORD sampler)
      {
         if (sampler < 16)
            return int(sampler);
         if (sampler >= D3DDMAPSAMPLER && sampler <= D3DVERTEXTEXTURESAMPLER3)
            return int(sampler - D3DDMAPSAMPLER + 16);
         return -1;
      }
   };

   struct Stats
   {
      uint64_t commands = 0;
      uint64_t payload_bytes = 0;
      uint64_t deferred_locks = 0;
      uint64_t sync_locks = 0;
      uint64_t presents = 0;
      double present_wait_ms = 0;
      uint64_t query_hits = 0;
      uint64_t query_misses = 0;
      uint64_t redundant = 0;
      uint32_t syncs[KIND_COUNT][MAX_SLOTS] = {};
   };

   // Taken on every call, so it must not be an OS lock: on Windows 11 26300 each SRWLOCK/CRITICAL_SECTION acquire runs
   // the RtlAb* lock tracking, and the game thread spent half its time there. Uncontended this is one interlocked
   // exchange; on its own cache line so the other thread's writes don't evict it.
   struct alignas(64) SpinLock
   {
      std::atomic<LONG> locked{0};

      bool TryLock()
      {
         return locked.load(std::memory_order_relaxed) == 0 && locked.exchange(1, std::memory_order_acquire) == 0;
      }

      void Lock()
      {
         for (int spin = 0; !TryLock(); spin++)
         {
            if (spin < 1000)
            {
               YieldProcessor();
            }
            else
            {
               SwitchToThread();
            }
         }
      }

      void Unlock()
      {
         locked.store(0, std::memory_order_release);
      }
   };

   struct Shared
   {
      SpinLock producer; // game threads
      alignas(64) std::atomic<bool> active{false};
      CommandQueue* queue = nullptr;
      HANDLE thread = nullptr;
      void* device = nullptr;
      uint32_t presents_pushed = 0; // producer
      alignas(64) std::atomic<uint32_t> presents_done{0};
      std::atomic<HRESULT> last_present{S_OK};
      std::unordered_map<void*, std::vector<PendingLock>> locks; // producer
      // DYNAMIC buffer contents as the game last wrote them: a lock hands out this memory, so bytes of the locked range
      // the game doesn't write keep their value when the range is copied in (Wine and DXVK map persistent memory too).
      std::unordered_map<void*, std::vector<char>> buffer_shadows; // producer
      // Usage and size of a buffer, asked once (synchronously): even a getter isn't safe beside the worker in dgVoodoo.
      std::unordered_map<void*, std::pair<DWORD, UINT>> buffer_descs; // producer
      // GetDesc/GetLevelDesc results per object: fixed while it lives, forgotten whenever an object is handed to the
      // game (Create*, Get*), which is how a new object at a dead one's address arrives.
      std::unordered_map<void*, std::vector<DescEntry>> descs; // producer
      std::unordered_map<void*, ULONG> refs;                   // producer: public counts of objects handed out
      StateShadow state;                                       // producer
      std::unordered_map<void*, QueryState*> queries;          // producer
      std::vector<QueryState*> polled_queries;                 // worker
      Stats stats;                                             // producer
      std::atomic<uint64_t> worker_polls{0};
      LARGE_INTEGER last_poll = {}; // worker
      LARGE_INTEGER poll_interval = {};
   };

   inline Shared g;
   inline thread_local int t_producer_depth = 0;
   inline thread_local bool t_worker = false;

   // "VertexConstantMirror" (exported for Luma), written by the thread that calls the layer below, after each call: the worker,
   // or the game thread while the layer passes calls through
   inline VertexConstantMirror g_vertex_constants;
   inline uint16_t g_vertex_bools_known = 0;         // b0-b15: a bool row is known once its four are
   inline bool g_vertex_constants_recording = false; // Between BeginStateBlock and EndStateBlock constants are recorded, not set

   inline void ForgetVertexConstants()
   {
      std::memset(g_vertex_constants.rows, 0, sizeof(g_vertex_constants.rows));
      std::memset(g_vertex_constants.known, 0, sizeof(g_vertex_constants.known));
      g_vertex_constants.unknown_rows = VertexConstantMirror::ROWS;
      g_vertex_constants.generation++;
      g_vertex_bools_known = 0;
   }

   // A Set{Vertex,Pixel}ShaderConstant{F,I,B} the layer below accepted: the vertex shader ones go to the mirror
   template <int SLOT, typename T>
   void MirrorConstants(UINT start, const T* data, UINT count)
   {
      constexpr bool is_float = SLOT == CSMT_SLOT_DEVICE_SetVertexShaderConstantF;
      constexpr bool is_int = SLOT == CSMT_SLOT_DEVICE_SetVertexShaderConstantI;
      constexpr bool is_bool = SLOT == CSMT_SLOT_DEVICE_SetVertexShaderConstantB;
      if constexpr (is_float || is_int || is_bool)
      {
         if (g_vertex_constants_recording)
            return;
         VertexConstantMirror& mirror = g_vertex_constants;
         const auto mark_known = [&](uint32_t row)
         {
            if (!mirror.known[row])
            {
               mirror.known[row] = 1;
               mirror.unknown_rows--;
            }
         };
         if constexpr (is_bool)
         {
            for (UINT i = 0; i < count && start + i < 16; i++)
            {
               const UINT reg = start + i;
               mirror.rows[VertexConstantMirror::BOOL_ROW + reg / 4][reg % 4] = uint32_t(data[i]);
               g_vertex_bools_known |= uint16_t(1u << reg);
               if (((g_vertex_bools_known >> (reg & ~3u)) & 0xF) == 0xF)
               {
                  mark_known(VertexConstantMirror::BOOL_ROW + reg / 4);
               }
            }
         }
         else
         {
            constexpr UINT first_row = (is_float ? VertexConstantMirror::FLOAT_ROW : VertexConstantMirror::INT_ROW);
            constexpr UINT registers = (is_float ? 256 : 16);
            for (UINT i = 0; i < count && start + i < registers; i++)
            {
               std::memcpy(mirror.rows[first_row + start + i], data + size_t(i) * 4, 16);
               mark_known(first_row + start + i);
            }
         }
         mirror.generation++;
      }
   }

   inline bool PassThrough()
   {
      return t_worker || !g.active.load(std::memory_order_relaxed);
   }

   // Serializes game threads. Recursive per thread: a sent message dispatched while waiting can re-enter the layer.
   struct ProducerScope
   {
      ProducerScope()
      {
         if (t_producer_depth++ == 0)
         {
            g.producer.Lock();
         }
      }

      ~ProducerScope()
      {
         if (--t_producer_depth == 0)
         {
            g.producer.Unlock();
         }
      }

      ProducerScope(const ProducerScope&) = delete;
      ProducerScope& operator=(const ProducerScope&) = delete;
   };

   // A synchronous call: the worker runs `call` after everything queued before it, while the caller waits for the
   // result. The worker is then the only thread that ever calls the layer below: dgVoodoo without
   // D3DCREATE_MULTITHREADED misrendered now and then when calls came from several threads, even one at a time.
   // `call` may update producer state; the caller holds the producer lock until it returns.
   template <typename F>
   auto OnWorker(int kind, int slot, F&& call)
   {
      ProducerScope scope;
      g.stats.syncs[kind][slot]++;
      using R = decltype(call());
      if constexpr (std::is_void_v<R>)
      {
         g.queue->Push([&](char*)
            { call(); });
         g.queue->Drain();
      }
      else
      {
         R result = {};
         g.queue->Push([&](char*)
            { result = call(); });
         g.queue->Drain();
         return result;
      }
   }

   // Commands pushed per call site (lambda type), for the report. Producer only.
   struct CallSite
   {
      const char* name;
      uint64_t count;
   };
   inline CallSite g_call_sites[256];
   inline int g_call_site_count = 0;

   inline uint64_t* RegisterCallSite(const char* name)
   {
      static uint64_t overflow = 0;
      if (g_call_site_count == int(std::size(g_call_sites)))
         return &overflow;
      g_call_sites[g_call_site_count] = {name, 0};
      return &g_call_sites[g_call_site_count++].count;
   }

   // Caller holds a ProducerScope.
   template <typename F>
   void Push(F&& command, const void* payload = nullptr, size_t payload_bytes = 0)
   {
      static uint64_t* const call_site = RegisterCallSite(typeid(std::decay_t<F>).name());
      (*call_site)++;
      g.queue->Push(std::forward<F>(command), payload, payload_bytes);
      g.stats.commands++;
      g.stats.payload_bytes += payload_bytes;
   }

   void PatchObject(void* object, int kind);
   // Defined with the buffer hooks, which share its state.
   void ForgetObject(void* object, int kind);

   // The public reference count of an object just handed to the game, read by the worker in a synchronous call:
   // AddRef/Release then answer from it instead of the queued real calls.
   inline void LearnRefs(void* object)
   {
      using RefFn = ULONG(STDMETHODCALLTYPE*)(void*);
      ((RefFn)OriginalFor(object, 1))(object);
      g.refs[object] = ((RefFn)OriginalFor(object, 2))(object);
   }

   // ---- generic hooks -------------------------------------------------------------------------------------------

   template <typename I, typename R, typename... A>
   struct MethodHooks
   {
      using Fn = R(STDMETHODCALLTYPE*)(I*, A...);

      template <int K, int S>
      static R STDMETHODCALLTYPE Sync(I* self, A... args)
      {
         const auto original = (Fn)OriginalFor(self, S);
         if (PassThrough())
            return original(self, args...);
         return OnWorker(K, S, [&]
            { return original(self, args...); });
      }

      // Synchronous, then patches the object returned through argument `OUT_INDEX` (a T**) as `OUT_KIND`.
      template <int K, int S, int OUT_INDEX, int OUT_KIND>
      static R STDMETHODCALLTYPE SyncOut(I* self, A... args)
      {
         const auto original = (Fn)OriginalFor(self, S);
         if (PassThrough())
            return original(self, args...);
         return OnWorker(K, S,
            [&]
            {
               const R result = original(self, args...);
               auto out = std::get<OUT_INDEX>(std::forward_as_tuple(args...));
               if (SUCCEEDED(result) && out && *out)
               {
                  PatchObject(*out, OUT_KIND);
                  ForgetObject(*out, OUT_KIND);
                  LearnRefs(*out);
               }
               return result;
            });
      }

      // Only for methods whose arguments are all values or object pointers.
      template <int K, int S>
      static R STDMETHODCALLTYPE Defer(I* self, A... args)
      {
         const auto original = (Fn)OriginalFor(self, S);
         if (PassThrough())
            return original(self, args...);
         ProducerScope scope;
         Push([=](char*)
            { original(self, args...); });
         return R(D3D_OK);
      }
   };

   // Hooks for the method behind a pointer-to-member type. COM methods are __declspec(nothrow), which MSVC makes part
   // of the type as noexcept.
   template <typename T>
   struct Method;

   template <typename I, typename R, typename... A>
   struct Method<R (STDMETHODCALLTYPE I::*)(A...)> : MethodHooks<I, R, A...>
   {
   };

   template <typename I, typename R, typename... A>
   struct Method<R (STDMETHODCALLTYPE I::*)(A...) noexcept> : MethodHooks<I, R, A...>
   {
   };

#define CSMT_SYNC_HOOK(KIND, I, M, S) (void*)&Method<decltype(&I::M)>::template Sync<KIND, S>,
#define CSMT_METHOD_NAME(KIND, I, M, S) #M,
   inline void* g_hooks_device[] = {CSMT_DEVICE(CSMT_SYNC_HOOK)};
   inline void* g_hooks_swap_chain[] = {CSMT_SWAP_CHAIN(CSMT_SYNC_HOOK)};
   inline void* g_hooks_state_block[] = {CSMT_STATE_BLOCK(CSMT_SYNC_HOOK)};
   inline void* g_hooks_vertex_declaration[] = {CSMT_VERTEX_DECLARATION(CSMT_SYNC_HOOK)};
   inline void* g_hooks_vertex_shader[] = {CSMT_VERTEX_SHADER(CSMT_SYNC_HOOK)};
   inline void* g_hooks_pixel_shader[] = {CSMT_PIXEL_SHADER(CSMT_SYNC_HOOK)};
   inline void* g_hooks_texture[] = {CSMT_TEXTURE(CSMT_SYNC_HOOK)};
   inline void* g_hooks_volume_texture[] = {CSMT_VOLUME_TEXTURE(CSMT_SYNC_HOOK)};
   inline void* g_hooks_cube_texture[] = {CSMT_CUBE_TEXTURE(CSMT_SYNC_HOOK)};
   inline void* g_hooks_vertex_buffer[] = {CSMT_VERTEX_BUFFER(CSMT_SYNC_HOOK)};
   inline void* g_hooks_index_buffer[] = {CSMT_INDEX_BUFFER(CSMT_SYNC_HOOK)};
   inline void* g_hooks_surface[] = {CSMT_SURFACE(CSMT_SYNC_HOOK)};
   inline void* g_hooks_volume[] = {CSMT_VOLUME(CSMT_SYNC_HOOK)};
   inline void* g_hooks_query[] = {CSMT_QUERY(CSMT_SYNC_HOOK)};
   inline void** const g_hooks[KIND_COUNT] = {g_hooks_device, g_hooks_swap_chain, g_hooks_state_block,
      g_hooks_vertex_declaration, g_hooks_vertex_shader, g_hooks_pixel_shader, g_hooks_texture, g_hooks_volume_texture,
      g_hooks_cube_texture, g_hooks_vertex_buffer, g_hooks_index_buffer, g_hooks_surface, g_hooks_volume, g_hooks_query};

   inline const char* const g_names_device[] = {CSMT_DEVICE(CSMT_METHOD_NAME)};
   inline const char* const g_names_swap_chain[] = {CSMT_SWAP_CHAIN(CSMT_METHOD_NAME)};
   inline const char* const g_names_state_block[] = {CSMT_STATE_BLOCK(CSMT_METHOD_NAME)};
   inline const char* const g_names_vertex_declaration[] = {CSMT_VERTEX_DECLARATION(CSMT_METHOD_NAME)};
   inline const char* const g_names_vertex_shader[] = {CSMT_VERTEX_SHADER(CSMT_METHOD_NAME)};
   inline const char* const g_names_pixel_shader[] = {CSMT_PIXEL_SHADER(CSMT_METHOD_NAME)};
   inline const char* const g_names_texture[] = {CSMT_TEXTURE(CSMT_METHOD_NAME)};
   inline const char* const g_names_volume_texture[] = {CSMT_VOLUME_TEXTURE(CSMT_METHOD_NAME)};
   inline const char* const g_names_cube_texture[] = {CSMT_CUBE_TEXTURE(CSMT_METHOD_NAME)};
   inline const char* const g_names_vertex_buffer[] = {CSMT_VERTEX_BUFFER(CSMT_METHOD_NAME)};
   inline const char* const g_names_index_buffer[] = {CSMT_INDEX_BUFFER(CSMT_METHOD_NAME)};
   inline const char* const g_names_surface[] = {CSMT_SURFACE(CSMT_METHOD_NAME)};
   inline const char* const g_names_volume[] = {CSMT_VOLUME(CSMT_METHOD_NAME)};
   inline const char* const g_names_query[] = {CSMT_QUERY(CSMT_METHOD_NAME)};
   inline const char* const* const g_names[KIND_COUNT] = {g_names_device, g_names_swap_chain, g_names_state_block,
      g_names_vertex_declaration, g_names_vertex_shader, g_names_pixel_shader, g_names_texture, g_names_volume_texture,
      g_names_cube_texture, g_names_vertex_buffer, g_names_index_buffer, g_names_surface, g_names_volume,
      g_names_query};
   inline const char* const g_kind_names[KIND_COUNT] = {"Device", "SwapChain", "StateBlock", "VertexDeclaration",
      "VertexShader", "PixelShader", "Texture", "VolumeTexture", "CubeTexture", "VertexBuffer", "IndexBuffer",
      "Surface", "Volume", "Query"};
   inline const int g_slot_counts[KIND_COUNT] = {CSMT_DEVICE_SLOTS, CSMT_SWAP_CHAIN_SLOTS, CSMT_STATE_BLOCK_SLOTS,
      CSMT_VERTEX_DECLARATION_SLOTS, CSMT_VERTEX_SHADER_SLOTS, CSMT_PIXEL_SHADER_SLOTS, CSMT_TEXTURE_SLOTS,
      CSMT_VOLUME_TEXTURE_SLOTS, CSMT_CUBE_TEXTURE_SLOTS, CSMT_VERTEX_BUFFER_SLOTS, CSMT_INDEX_BUFFER_SLOTS,
      CSMT_SURFACE_SLOTS, CSMT_VOLUME_SLOTS, CSMT_QUERY_SLOTS};
#undef CSMT_SYNC_HOOK
#undef CSMT_METHOD_NAME

   // ---- patching ------------------------------------------------------------------------------------------------

   // The Ex interfaces have more slots; their vtables are only that long when the object implements them.
   inline int SlotCount(void* object, int kind)
   {
      const IID* ex_iid = (kind == DEVICE ? &IID_IDirect3DDevice9Ex : kind == SWAP_CHAIN ? &IID_IDirect3DSwapChain9Ex
                                                                                         : nullptr);
      if (!ex_iid)
         return g_slot_counts[kind];
      using QueryInterfaceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
      using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(void*);
      void* ex = nullptr;
      if (SUCCEEDED(((QueryInterfaceFn)(*(void***)object)[0])(object, *ex_iid, &ex)) && ex)
      {
         ((ReleaseFn)(*(void***)object)[2])(object);
         return g_slot_counts[kind];
      }
      return (kind == DEVICE ? CSMT_SLOT_DEVICE_SetConvolutionMonoKernel : CSMT_SLOT_SWAP_CHAIN_GetLastPresentCount);
   }

   // Runs on the worker in a synchronous call, or before the worker exists: the vtable is rewritten while nobody calls
   // it.
   inline void PatchObject(void* object, int kind)
   {
      if (kind == BASE_TEXTURE)
      {
         switch (((IDirect3DBaseTexture9*)object)->GetType())
         {
         case D3DRTYPE_TEXTURE:
            kind = TEXTURE;
            break;
         case D3DRTYPE_VOLUMETEXTURE:
            kind = VOLUME_TEXTURE;
            break;
         case D3DRTYPE_CUBETEXTURE:
            kind = CUBE_TEXTURE;
            break;
         default:
            return;
         }
      }
      void** vtable = *(void***)object;
      if (FindVtable(vtable))
         return;
      const int slots = SlotCount(object, kind);
      std::lock_guard lock(g_patch_mutex);
      const int count = g_vtable_count.load(std::memory_order_relaxed);
      if (FindVtable(vtable) || count == int(std::size(g_vtables)))
         return;
      PatchedVtable& patched = g_vtables[count];
      patched.vtable = vtable;
      patched.kind = kind;
      patched.slots = slots;
      std::copy_n(vtable, slots, patched.originals);
      g_vtable_count.store(count + 1, std::memory_order_release);
      DWORD old_protect;
      VirtualProtect(vtable, slots * sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect);
      std::copy_n(g_hooks[kind], slots, vtable);
      VirtualProtect(vtable, slots * sizeof(void*), old_protect, &old_protect);
      FlushInstructionCache(GetCurrentProcess(), vtable, slots * sizeof(void*));
   }

   // ---- device hooks with copied arguments ---------------------------------------------------------------------

   template <typename T>
   struct Copy
   {
      bool present = false;
      T value = {};

      explicit Copy(const T* source)
      {
         if (source)
         {
            present = true;
            value = *source;
         }
      }

      const T* Get() const
      {
         return present ? &value : nullptr;
      }
   };

   struct Plane
   {
      float coefficients[4];
   };

#define CSMT_ORIGINAL(KIND, NAME)                                                                                     \
   using Fn = decltype(&Method<decltype(&IDirect3DDevice9Ex::NAME)>::template Sync<KIND, CSMT_SLOT_##KIND##_##NAME>); \
   const auto original = (Fn)OriginalFor(self, CSMT_SLOT_##KIND##_##NAME)

   inline HRESULT STDMETHODCALLTYPE Clear(IDirect3DDevice9Ex* self, DWORD count, const D3DRECT* rects, DWORD flags,
      D3DCOLOR color, float z, DWORD stencil)
   {
      CSMT_ORIGINAL(DEVICE, Clear);
      if (PassThrough())
         return original(self, count, rects, flags, color, z, stencil);
      ProducerScope scope;
      const DWORD rect_count = (rects ? count : 0);
      Push([=](char* payload)
         { original(self, rect_count, rect_count ? (const D3DRECT*)payload : nullptr, flags, color, z, stencil); },
         rects, rect_count * sizeof(D3DRECT));
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetTransform(IDirect3DDevice9Ex* self, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix)
   {
      CSMT_ORIGINAL(DEVICE, SetTransform);
      if (PassThrough())
         return original(self, state, matrix);
      ProducerScope scope;
      Push([=, copy = Copy(matrix)](char*)
         { original(self, state, copy.Get()); });
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE MultiplyTransform(IDirect3DDevice9Ex* self, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix)
   {
      CSMT_ORIGINAL(DEVICE, MultiplyTransform);
      if (PassThrough())
         return original(self, state, matrix);
      ProducerScope scope;
      Push([=, copy = Copy(matrix)](char*)
         { original(self, state, copy.Get()); });
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetViewport(IDirect3DDevice9Ex* self, const D3DVIEWPORT9* viewport)
   {
      CSMT_ORIGINAL(DEVICE, SetViewport);
      if (PassThrough())
         return original(self, viewport);
      ProducerScope scope;
      Push([=, copy = Copy(viewport)](char*)
         { original(self, copy.Get()); });
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetMaterial(IDirect3DDevice9Ex* self, const D3DMATERIAL9* material)
   {
      CSMT_ORIGINAL(DEVICE, SetMaterial);
      if (PassThrough())
         return original(self, material);
      ProducerScope scope;
      Push([=, copy = Copy(material)](char*)
         { original(self, copy.Get()); });
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetLight(IDirect3DDevice9Ex* self, DWORD index, const D3DLIGHT9* light)
   {
      CSMT_ORIGINAL(DEVICE, SetLight);
      if (PassThrough())
         return original(self, index, light);
      ProducerScope scope;
      Push([=, copy = Copy(light)](char*)
         { original(self, index, copy.Get()); });
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetClipPlane(IDirect3DDevice9Ex* self, DWORD index, const float* plane)
   {
      CSMT_ORIGINAL(DEVICE, SetClipPlane);
      if (PassThrough())
         return original(self, index, plane);
      ProducerScope scope;
      Push([=, copy = Copy((const Plane*)plane)](char*)
         { original(self, index, (const float*)copy.Get()); });
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetScissorRect(IDirect3DDevice9Ex* self, const RECT* rect)
   {
      CSMT_ORIGINAL(DEVICE, SetScissorRect);
      if (PassThrough())
         return original(self, rect);
      ProducerScope scope;
      Push([=, copy = Copy(rect)](char*)
         { original(self, copy.Get()); });
      return D3D_OK;
   }

   // Set{Vertex,Pixel}ShaderConstant{F,I,B}: `count` registers of `register_bytes`.
   template <int SLOT, typename T, size_t REGISTER_BYTES>
   HRESULT STDMETHODCALLTYPE SetConstants(IDirect3DDevice9Ex* self, UINT start, const T* data, UINT count)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, UINT, const T*, UINT);
      const auto original = (Fn)OriginalFor(self, SLOT);
      const size_t bytes = size_t(count) * REGISTER_BYTES;
      const auto call = [=](const T* values)
      {
         const HRESULT hr = original(self, start, values, count);
         if (SUCCEEDED(hr) && values)
         {
            MirrorConstants<SLOT>(start, values, count);
         }
         return hr;
      };
      if (PassThrough() || !data)
         return call(data);
      if (bytes > g.queue->MaxPayload())
         return OnWorker(DEVICE, SLOT, [&]
            { return call(data); });
      ProducerScope scope;
      Push([=](char* payload)
         { call((const T*)payload); }, data, bytes);
      return D3D_OK;
   }

   inline UINT VerticesFor(D3DPRIMITIVETYPE type, UINT primitives)
   {
      switch (type)
      {
      case D3DPT_POINTLIST:
         return primitives;
      case D3DPT_LINELIST:
         return primitives * 2;
      case D3DPT_LINESTRIP:
         return primitives + 1;
      case D3DPT_TRIANGLELIST:
         return primitives * 3;
      case D3DPT_TRIANGLESTRIP:
      case D3DPT_TRIANGLEFAN:
         return primitives + 2;
      default:
         return 0;
      }
   }

   inline HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(IDirect3DDevice9Ex* self, D3DPRIMITIVETYPE type, UINT primitives,
      const void* vertices, UINT stride)
   {
      CSMT_ORIGINAL(DEVICE, DrawPrimitiveUP);
      const size_t bytes = size_t(VerticesFor(type, primitives)) * stride;
      if (PassThrough() || !vertices || bytes == 0)
         return original(self, type, primitives, vertices, stride);
      if (bytes > g.queue->MaxPayload())
         return OnWorker(DEVICE, CSMT_SLOT_DEVICE_DrawPrimitiveUP, [&]
            { return original(self, type, primitives, vertices, stride); });
      ProducerScope scope;
      g.state.stream_known[0] = false; // the runtime unbinds stream 0 after an UP draw
      Push([=](char* payload)
         { original(self, type, primitives, payload, stride); }, vertices, bytes);
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(IDirect3DDevice9Ex* self, D3DPRIMITIVETYPE type,
      UINT min_vertex, UINT vertex_count, UINT primitives, const void* indices, D3DFORMAT index_format,
      const void* vertices, UINT stride)
   {
      CSMT_ORIGINAL(DEVICE, DrawIndexedPrimitiveUP);
      const size_t index_bytes = size_t(VerticesFor(type, primitives)) * (index_format == D3DFMT_INDEX32 ? 4 : 2);
      const size_t index_bytes_aligned = (index_bytes + 15) & ~size_t(15);
      // Indices address vertices from the start of the array; the used ones are [min_vertex, min_vertex + count).
      const size_t vertex_bytes = size_t(min_vertex + vertex_count) * stride;
      const size_t bytes = index_bytes_aligned + vertex_bytes;
      if (PassThrough() || !indices || !vertices || index_bytes == 0)
         return original(self, type, min_vertex, vertex_count, primitives, indices, index_format, vertices, stride);
      if (bytes > g.queue->MaxPayload())
      {
         return OnWorker(DEVICE, CSMT_SLOT_DEVICE_DrawIndexedPrimitiveUP,
            [&]
            { return original(self, type, min_vertex, vertex_count, primitives, indices, index_format, vertices, stride); });
      }
      ProducerScope scope;
      g.state.stream_known[0] = false; // the runtime unbinds stream 0 and the indices after an UP draw
      g.state.indices_known = false;
      char* payload = g.queue->Push(
         [=](char* data)
         { original(self, type, min_vertex, vertex_count, primitives, data, index_format, data + index_bytes_aligned, stride); },
         nullptr, bytes);
      memcpy(payload, indices, index_bytes);
      memcpy(payload + index_bytes_aligned, vertices, vertex_bytes);
      g.stats.commands++;
      g.stats.payload_bytes += bytes;
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DDevice9Ex* self, IDirect3DSurface9* source, const RECT* source_rect,
      IDirect3DSurface9* destination, const RECT* destination_rect, D3DTEXTUREFILTERTYPE filter)
   {
      CSMT_ORIGINAL(DEVICE, StretchRect);
      if (PassThrough())
         return original(self, source, source_rect, destination, destination_rect, filter);
      ProducerScope scope;
      Push([=, from = Copy(source_rect), to = Copy(destination_rect)](char*)
         { original(self, source, from.Get(), destination, to.Get(), filter); });
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DDevice9Ex* self, IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color)
   {
      CSMT_ORIGINAL(DEVICE, ColorFill);
      if (PassThrough())
         return original(self, surface, rect, color);
      ProducerScope scope;
      Push([=, copy = Copy(rect)](char*)
         { original(self, surface, copy.Get(), color); });
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DDevice9Ex* self, IDirect3DSurface9* source, const RECT* rect,
      IDirect3DSurface9* destination, const POINT* point)
   {
      CSMT_ORIGINAL(DEVICE, UpdateSurface);
      if (PassThrough())
         return original(self, source, rect, destination, point);
      ProducerScope scope;
      Push([=, from = Copy(rect), at = Copy(point)](char*)
         { original(self, source, from.Get(), destination, at.Get()); });
      return D3D_OK;
   }

   // Queries the worker issued and whose result hasn't been read yet. Worker only.
   // `wanted_only`: just the queries the game is waiting for (cheap enough to run every few commands).
   inline void PollQueries(DWORD flags, bool wanted_only = false)
   {
      using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DQuery9*, void*, DWORD, DWORD);
      auto& polled = g.polled_queries;
      for (size_t i = 0; i < polled.size();)
      {
         QueryState* state = polled[i];
         HRESULT hr = S_FALSE;
         if (wanted_only && !state->wanted.load(std::memory_order_relaxed))
         {
            i++;
            continue;
         }
         if (!state->dead && !state->building)
         {
            const auto get_data = (GetDataFn)OriginalFor(state->query, CSMT_SLOT_QUERY_GetData);
            hr = get_data(state->query, state->data, state->size, flags);
            g.worker_polls.fetch_add(1, std::memory_order_relaxed);
         }
         if (hr == S_FALSE)
         {
            i++;
            continue;
         }
         if (!state->dead)
         {
            state->result.store(hr, std::memory_order_relaxed);
            state->ready.store(state->executed.load(std::memory_order_relaxed), std::memory_order_release);
            state->wanted.store(false, std::memory_order_relaxed);
         }
         state->polled = false;
         polled[i] = polled.back();
         polled.pop_back();
      }
   }

   inline HRESULT STDMETHODCALLTYPE Present(IDirect3DDevice9Ex* self, const RECT* source_rect, const RECT* destination_rect,
      HWND window, const RGNDATA* dirty_region)
   {
      CSMT_ORIGINAL(DEVICE, Present);
      if (PassThrough())
         return original(self, source_rect, destination_rect, window, dirty_region);
      if (dirty_region)
         return OnWorker(DEVICE, CSMT_SLOT_DEVICE_Present, [&]
            { return original(self, source_rect, destination_rect, window, dirty_region); });
      ProducerScope scope;
      Push([=, from = Copy(source_rect), to = Copy(destination_rect)](char*)
         {
            g.last_present.store(original(self, from.Get(), to.Get(), window, nullptr), std::memory_order_relaxed);
            PollQueries(D3DGETDATA_FLUSH);
            g.presents_done.fetch_add(1, std::memory_order_release); });
      g.presents_pushed++;
      g.stats.presents++;
      g.queue->Flush();
      if (g.presents_pushed - g.presents_done.load(std::memory_order_acquire) > MAX_QUEUED_FRAMES)
      {
         LARGE_INTEGER frequency, begin, end;
         QueryPerformanceFrequency(&frequency);
         QueryPerformanceCounter(&begin);
         g.queue->WaitForProducer([&]
            { return g.presents_pushed - g.presents_done.load(std::memory_order_seq_cst) <= MAX_QUEUED_FRAMES; });
         QueryPerformanceCounter(&end);
         g.stats.present_wait_ms += (end.QuadPart - begin.QuadPart) * 1000.0 / frequency.QuadPart;
      }
      return g.last_present.load(std::memory_order_relaxed);
   }

   void Stop();

   inline ULONG STDMETHODCALLTYPE DeviceRelease(IDirect3DDevice9Ex* self)
   {
      CSMT_ORIGINAL(DEVICE, Release);
      if (PassThrough())
         return original(self);
      const ULONG refs = OnWorker(DEVICE, CSMT_SLOT_DEVICE_Release, [&]
         { return original(self); });
      if (refs == 0 && self == g.device)
      {
         Stop();
      }
      return refs;
   }

   inline HRESULT STDMETHODCALLTYPE CreateQuery(IDirect3DDevice9Ex* self, D3DQUERYTYPE type, IDirect3DQuery9** query)
   {
      CSMT_ORIGINAL(DEVICE, CreateQuery);
      if (PassThrough() || !query) // null: the caller only asks whether the type is supported
         return original(self, type, query);
      return OnWorker(DEVICE, CSMT_SLOT_DEVICE_CreateQuery,
         [&]
         {
            const HRESULT hr = original(self, type, query);
            if (FAILED(hr) || !*query)
               return hr;
            PatchObject(*query, QUERY);
            LearnRefs(*query);
            auto* state = new QueryState(); // never freed: a few hundred per game, and the worker may still poll it
            state->query = *query;
            state->size = (*query)->GetDataSize();
            g.queries[*query] = state;
            return hr;
         });
   }

   // ---- resource hooks ------------------------------------------------------------------------------------------

   // AddRef/Release of everything but the device: in order with the commands that use the object, so a resource the
   // game releases stays alive until its last queued use ran. The returned counts are not the real ones.
   template <int K>
   ULONG STDMETHODCALLTYPE ResourceAddRef(IUnknown* self)
   {
      using Fn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
      const auto original = (Fn)OriginalFor(self, 1);
      if (PassThrough())
         return original(self);
      ProducerScope scope;
      Push([=](char*)
         { original(self); });
      auto it = g.refs.find(self);
      return (it != g.refs.end() ? ++it->second : 2);
   }

   template <int K>
   ULONG STDMETHODCALLTYPE ResourceRelease(IUnknown* self)
   {
      using Fn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
      const auto original = (Fn)OriginalFor(self, 2);
      if (PassThrough())
         return original(self);
      ProducerScope scope;
      QueryState* state = nullptr;
      if (K == QUERY)
      {
         if (auto it = g.queries.find(self); it != g.queries.end())
         {
            state = it->second;
         }
      }
      Push([=](char*)
         {
            if (original(self) == 0 && state)
            {
               state->dead = true; // PollQueries drops it
            } });
      auto it = g.refs.find(self);
      if (it == g.refs.end())
         return 1;
      const ULONG refs = --it->second;
      if (refs == 0)
      {
         g.refs.erase(it);
      }
      return refs;
   }

   // Lock/Unlock of vertex and index buffers (same slots, same leading desc fields).
   struct BufferDesc
   {
      D3DFORMAT format;
      D3DRESOURCETYPE type;
      DWORD usage;
      D3DPOOL pool;
      UINT size;
      DWORD fvf; // vertex buffers only
   };

   template <int K>
   HRESULT STDMETHODCALLTYPE BufferLock(IUnknown* self, UINT offset, UINT size, void** data, DWORD flags)
   {
      using LockFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, UINT, UINT, void**, DWORD);
      using DescFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, BufferDesc*);
      const auto original = (LockFn)OriginalFor(self, CSMT_SLOT_VERTEX_BUFFER_Lock);
      if (PassThrough())
         return original(self, offset, size, data, flags);
      ProducerScope scope;
      auto known = g.buffer_descs.find(self);
      if (known == g.buffer_descs.end())
      {
         BufferDesc desc = {};
         if (FAILED(OnWorker(K, CSMT_SLOT_VERTEX_BUFFER_GetDesc,
                [&]
                { return ((DescFn)OriginalFor(self, CSMT_SLOT_VERTEX_BUFFER_GetDesc))(self, &desc); })))
            return OnWorker(K, CSMT_SLOT_VERTEX_BUFFER_Lock, [&]
               { return original(self, offset, size, data, flags); });
         known = g.buffer_descs.emplace(self, std::pair{desc.usage, desc.size}).first;
      }
      const auto [usage, buffer_size] = known->second;
      const bool write_only_rename = (flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) && !(flags & D3DLOCK_READONLY) &&
                                     data && (usage & D3DUSAGE_DYNAMIC) && offset < buffer_size;
      if (!write_only_rename)
      {
         g.stats.sync_locks++;
         return OnWorker(K, CSMT_SLOT_VERTEX_BUFFER_Lock, [&]
            { return original(self, offset, size, data, flags); });
      }
      const UINT bytes = ((size == 0 || offset + size > buffer_size) ? (buffer_size - offset) : size);
      std::vector<char>& shadow = g.buffer_shadows[self];
      if (shadow.size() != buffer_size)
      {
         shadow.assign(buffer_size, 0);
      }
      *data = shadow.data() + offset;
      g.locks[self].push_back({.offset = offset, .size = bytes, .flags = flags});
      g.stats.deferred_locks++;
      return D3D_OK;
   }

   template <int K>
   HRESULT STDMETHODCALLTYPE BufferUnlock(IUnknown* self)
   {
      using LockFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, UINT, UINT, void**, DWORD);
      using UnlockFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*);
      const auto unlock = (UnlockFn)OriginalFor(self, CSMT_SLOT_VERTEX_BUFFER_Unlock);
      if (PassThrough())
         return unlock(self);
      const auto lock = (LockFn)OriginalFor(self, CSMT_SLOT_VERTEX_BUFFER_Lock);
      ProducerScope scope;
      auto it = g.locks.find(self);
      if (it == g.locks.end() || it->second.empty())
         return OnWorker(K, CSMT_SLOT_VERTEX_BUFFER_Unlock, [&]
            { return unlock(self); });
      const PendingLock pending = it->second.back();
      it->second.pop_back();
      if (it->second.empty())
      {
         g.locks.erase(it);
      }
      const UINT offset = pending.offset, size = pending.size;
      const DWORD flags = pending.flags;
      auto write = [=](const char* source)
      {
         void* destination = nullptr;
         if (SUCCEEDED(lock(self, offset, size, &destination, flags)) && destination)
         {
            memcpy(destination, source, size);
            unlock(self);
         }
      };
      const char* written = g.buffer_shadows[self].data() + offset;
      if (size > g.queue->MaxPayload())
      {
         OnWorker(K, CSMT_SLOT_VERTEX_BUFFER_Unlock, [&]
            { write(written); });
      }
      else
      {
         Push([=](char* payload)
            { write(payload); }, written, size);
      }
      return D3D_OK;
   }

   // Runs on the worker in a synchronous call; `object` was just handed to the game and patched.
   // The description a game usually asks for right after getting an object is stored at once (it runs on the worker
   // anyway), so that GetDesc doesn't need a synchronous call of its own.
   inline void ForgetObject(void* object, int kind)
   {
      g.descs.erase(object);
      auto store = [&](int slot, UINT level, auto desc, HRESULT hr)
      {
         if (SUCCEEDED(hr))
         {
            DescEntry entry = {.key = (uint32_t(slot) << 16) | level};
            memcpy(entry.bytes, &desc, sizeof(desc));
            g.descs[object].push_back(entry);
         }
      };
      switch (kind)
      {
      case SURFACE:
      {
         D3DSURFACE_DESC desc = {};
         using Fn = HRESULT(STDMETHODCALLTYPE*)(void*, D3DSURFACE_DESC*);
         store(CSMT_SLOT_SURFACE_GetDesc, 0, desc, ((Fn)OriginalFor(object, CSMT_SLOT_SURFACE_GetDesc))(object, &desc));
         break;
      }
      case TEXTURE:
      case CUBE_TEXTURE:
      {
         D3DSURFACE_DESC desc = {};
         using Fn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, D3DSURFACE_DESC*);
         store(CSMT_SLOT_TEXTURE_GetLevelDesc, 0, desc, ((Fn)OriginalFor(object, CSMT_SLOT_TEXTURE_GetLevelDesc))(object, 0, &desc));
         break;
      }
      case VERTEX_BUFFER:
      case INDEX_BUFFER:
      {
         g.buffer_shadows.erase(object);
         g.buffer_descs.erase(object);
         BufferDesc desc = {}; // a vertex buffer desc; an index buffer desc is its first five fields
         using Fn = HRESULT(STDMETHODCALLTYPE*)(void*, BufferDesc*);
         const HRESULT hr = ((Fn)OriginalFor(object, CSMT_SLOT_VERTEX_BUFFER_GetDesc))(object, &desc);
         if (SUCCEEDED(hr))
         {
            g.buffer_descs[object] = {desc.usage, desc.size};
         }
         store(CSMT_SLOT_VERTEX_BUFFER_GetDesc, 0, desc, hr);
         break;
      }
      default:
         break;
      }
   }

   // GetDesc (LEVELED = false) and GetLevelDesc: synchronous once per object and level, then from g.descs.
   template <int K, int SLOT, typename Desc, bool LEVELED>
   HRESULT CachedDesc(IUnknown* self, UINT level, Desc* desc)
   {
      static_assert(sizeof(Desc) <= sizeof(DescEntry::bytes));
      using DescFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, Desc*);
      using LevelDescFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, UINT, Desc*);
      auto fetch = [&]
      {
         void* original = OriginalFor(self, SLOT);
         return (LEVELED ? ((LevelDescFn)original)(self, level, desc) : ((DescFn)original)(self, desc));
      };
      if (PassThrough() || !desc)
         return fetch();
      ProducerScope scope;
      const uint32_t key = (uint32_t(SLOT) << 16) | level;
      std::vector<DescEntry>& entries = g.descs[self];
      for (const DescEntry& entry : entries)
      {
         if (entry.key == key)
         {
            memcpy(desc, entry.bytes, sizeof(Desc));
            return D3D_OK;
         }
      }
      const HRESULT hr = OnWorker(K, SLOT, fetch);
      if (SUCCEEDED(hr))
      {
         DescEntry entry = {.key = key};
         memcpy(entry.bytes, desc, sizeof(Desc));
         entries.push_back(entry);
      }
      return hr;
   }

   template <int K, int SLOT, typename Desc>
   HRESULT STDMETHODCALLTYPE GetDesc(IUnknown* self, Desc* desc)
   {
      return CachedDesc<K, SLOT, Desc, false>(self, 0, desc);
   }

   template <int K, int SLOT, typename Desc>
   HRESULT STDMETHODCALLTYPE GetLevelDesc(IUnknown* self, UINT level, Desc* desc)
   {
      return CachedDesc<K, SLOT, Desc, true>(self, level, desc);
   }

   inline HRESULT STDMETHODCALLTYPE QueryIssue(IDirect3DQuery9* self, DWORD flags)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DQuery9*, DWORD);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_QUERY_Issue);
      if (PassThrough())
         return original(self, flags);
      ProducerScope scope;
      QueryState* state = nullptr;
      if (auto it = g.queries.find(self); it != g.queries.end() && it->second->size <= MAX_QUERY_RESULT_BYTES)
      {
         state = it->second;
      }
      if (!state)
      {
         Push([=](char*)
            { original(self, flags); });
         return D3D_OK;
      }
      if (!(flags & D3DISSUE_END))
      {
         Push([=](char*)
            {
               original(self, flags);
               state->building = true; });
         return D3D_OK;
      }
      const uint32_t issue = ++state->issued;
      Push([=](char*)
         {
            original(self, flags);
            state->building = false;
            state->executed.store(issue, std::memory_order_release);
            if (!state->polled)
            {
               state->polled = true;
               g.polled_queries.push_back(state);
            } });
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE QueryGetData(IDirect3DQuery9* self, void* data, DWORD size, DWORD flags)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DQuery9*, void*, DWORD, DWORD);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_QUERY_GetData);
      if (PassThrough())
         return original(self, data, size, flags);
      ProducerScope scope;
      auto it = g.queries.find(self);
      QueryState* state = (it != g.queries.end() && it->second->size <= MAX_QUERY_RESULT_BYTES && it->second->issued)
                             ? it->second
                             : nullptr;
      if (!state)
         return OnWorker(QUERY, CSMT_SLOT_QUERY_GetData, [&]
            { return original(self, data, size, flags); });
      if (state->ready.load(std::memory_order_acquire) == state->issued)
      {
         g.stats.query_hits++;
         const HRESULT hr = state->result.load(std::memory_order_relaxed);
         if (data && size && hr == S_OK)
         {
            memcpy(data, state->data, (std::min)(size, state->size));
         }
         return hr;
      }
      g.stats.query_misses++;
      state->wanted.store(true, std::memory_order_relaxed); // the worker polls it at once
      g.queue->Flush();
      return S_FALSE;
   }

   // Records `value` in `slot` unless it is already known there; true = push the call. Caller holds a ProducerScope.
   template <typename T>
   bool Changes(T* slot, bool* known, T value)
   {
      if (g.state.recording)
         return true; // any thread: the shadow can't tell recorded calls from applied ones
      if (*known && *slot == value)
      {
         g.stats.redundant++;
         return false;
      }
      *slot = value;
      *known = true;
      return true;
   }

   inline HRESULT STDMETHODCALLTYPE SetRenderState(IDirect3DDevice9Ex* self, D3DRENDERSTATETYPE state, DWORD value)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DRENDERSTATETYPE, DWORD);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_DEVICE_SetRenderState);
      if (PassThrough())
         return original(self, state, value);
      ProducerScope scope;
      if (DWORD(state) >= 256 || Changes(&g.state.render[state], &g.state.render_known[state], value))
      {
         Push([=](char*)
            { original(self, state, value); });
      }
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetSamplerState(IDirect3DDevice9Ex* self, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_DEVICE_SetSamplerState);
      if (PassThrough())
         return original(self, sampler, type, value);
      ProducerScope scope;
      const int index = StateShadow::SamplerIndex(sampler);
      if (index < 0 || DWORD(type) >= 16 ||
          Changes(&g.state.sampler[index][type], &g.state.sampler_known[index][type], value))
      {
         Push([=](char*)
            { original(self, sampler, type, value); });
      }
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetTextureStageState(IDirect3DDevice9Ex* self, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_DEVICE_SetTextureStageState);
      if (PassThrough())
         return original(self, stage, type, value);
      ProducerScope scope;
      if (stage >= 8 || DWORD(type) >= 33 || Changes(&g.state.stage[stage][type], &g.state.stage_known[stage][type], value))
      {
         Push([=](char*)
            { original(self, stage, type, value); });
      }
      return D3D_OK;
   }

   // A bound texture or shader can't die while bound, so an equal pointer is the same object.
   inline HRESULT STDMETHODCALLTYPE SetTexture(IDirect3DDevice9Ex* self, DWORD sampler, IDirect3DBaseTexture9* texture)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, DWORD, IDirect3DBaseTexture9*);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_DEVICE_SetTexture);
      if (PassThrough())
         return original(self, sampler, texture);
      ProducerScope scope;
      const int index = StateShadow::SamplerIndex(sampler);
      if (index < 0 || Changes(&g.state.texture[index], &g.state.texture_known[index], (void*)texture))
      {
         Push([=](char*)
            { original(self, sampler, texture); });
      }
      return D3D_OK;
   }

   template <int SLOT, typename Shader>
   HRESULT STDMETHODCALLTYPE SetShader(IDirect3DDevice9Ex* self, Shader* shader)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, Shader*);
      const auto original = (Fn)OriginalFor(self, SLOT);
      if (PassThrough())
         return original(self, shader);
      ProducerScope scope;
      constexpr int STAGE = (SLOT == CSMT_SLOT_DEVICE_SetVertexShader ? 0 : 1);
      if (Changes(&g.state.shader[STAGE], &g.state.shader_known[STAGE], (void*)shader))
      {
         Push([=](char*)
            { original(self, shader); });
      }
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetStreamSource(IDirect3DDevice9Ex* self, UINT stream, IDirect3DVertexBuffer9* buffer,
      UINT offset, UINT stride)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_DEVICE_SetStreamSource);
      if (PassThrough())
         return original(self, stream, buffer, offset, stride);
      ProducerScope scope;
      if (stream >= 16 || Changes(&g.state.stream[stream], &g.state.stream_known[stream],
                             StateShadow::Stream{.buffer = buffer, .offset = offset, .stride = stride}))
      {
         Push([=](char*)
            { original(self, stream, buffer, offset, stride); });
      }
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(IDirect3DDevice9Ex* self, UINT stream, UINT setting)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, UINT, UINT);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_DEVICE_SetStreamSourceFreq);
      if (PassThrough())
         return original(self, stream, setting);
      ProducerScope scope;
      if (stream >= 16 || Changes(&g.state.stream_frequency[stream], &g.state.stream_frequency_known[stream], setting))
      {
         Push([=](char*)
            { original(self, stream, setting); });
      }
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DDevice9Ex* self, IDirect3DIndexBuffer9* indices)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, IDirect3DIndexBuffer9*);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_DEVICE_SetIndices);
      if (PassThrough())
         return original(self, indices);
      ProducerScope scope;
      if (Changes(&g.state.indices, &g.state.indices_known, (void*)indices))
      {
         Push([=](char*)
            { original(self, indices); });
      }
      return D3D_OK;
   }

   // SetFVF replaces the declaration, so the declaration shadow is forgotten there.
   inline HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DDevice9Ex* self, IDirect3DVertexDeclaration9* declaration)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, IDirect3DVertexDeclaration9*);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_DEVICE_SetVertexDeclaration);
      if (PassThrough())
         return original(self, declaration);
      ProducerScope scope;
      if (Changes(&g.state.declaration, &g.state.declaration_known, (void*)declaration))
      {
         Push([=](char*)
            { original(self, declaration); });
      }
      return D3D_OK;
   }

   inline HRESULT STDMETHODCALLTYPE SetFVF(IDirect3DDevice9Ex* self, DWORD fvf)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, DWORD);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_DEVICE_SetFVF);
      if (PassThrough())
         return original(self, fvf);
      ProducerScope scope;
      g.state.declaration_known = false;
      Push([=](char*)
         { original(self, fvf); });
      return D3D_OK;
   }

   // Synchronous calls that change the device state behind the shadow (and the vertex constant mirror, on the calling side of
   // the layer below)
   template <int K, int SLOT, typename... A>
   HRESULT STDMETHODCALLTYPE ForgetState(IUnknown* self, A... args)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, A...);
      const auto original = (Fn)OriginalFor(self, SLOT);
      const auto call = [&]
      {
         const HRESULT hr = original(self, args...);
         if constexpr (K == DEVICE && SLOT == CSMT_SLOT_DEVICE_BeginStateBlock)
         {
            g_vertex_constants_recording = SUCCEEDED(hr);
         }
         else
         {
            ForgetVertexConstants(); // Reset, ResetEx, a state block's Apply
         }
         return hr;
      };
      if (PassThrough())
         return call();
      g.state.Forget();
      if constexpr (K == DEVICE && SLOT == CSMT_SLOT_DEVICE_BeginStateBlock)
      {
         g.state.recording = true;
      }
      return OnWorker(K, SLOT, call);
   }

   // UE3 asks every few draws. Answered from the last Present while it succeeded (a D3D9Ex device always gets S_OK
   // from the runtime anyway); a lost or removed device gets the real, synchronous answer.
   inline HRESULT STDMETHODCALLTYPE TestCooperativeLevel(IDirect3DDevice9Ex* self)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_DEVICE_TestCooperativeLevel);
      if (PassThrough())
         return original(self);
      if (SUCCEEDED(g.last_present.load(std::memory_order_relaxed)))
         return D3D_OK;
      return OnWorker(DEVICE, CSMT_SLOT_DEVICE_TestCooperativeLevel, [&]
         { return original(self); });
   }

   inline HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DDevice9Ex* self, IDirect3DStateBlock9** block)
   {
      using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, IDirect3DStateBlock9**);
      const auto original = (Fn)OriginalFor(self, CSMT_SLOT_DEVICE_EndStateBlock);
      if (PassThrough())
      {
         g_vertex_constants_recording = false;
         return original(self, block);
      }
      g.state.Forget(); // recording off
      return OnWorker(DEVICE, CSMT_SLOT_DEVICE_EndStateBlock,
         [&]
         {
            g_vertex_constants_recording = false;
            const HRESULT hr = original(self, block);
            if (SUCCEEDED(hr) && block && *block)
            {
               PatchObject(*block, STATE_BLOCK);
               LearnRefs(*block);
            }
            return hr;
         });
   }

   // ---- tables, lifetime, report ----------------------------------------------------------------------------------

#define CSMT_SET(KIND, SLOT, HOOK) g_hooks[KIND][SLOT] = (void*)(HOOK)
#define CSMT_DEFER(KIND, NAME)               \
   CSMT_SET(KIND, CSMT_SLOT_##KIND##_##NAME, \
      (&Method<decltype(&IDirect3DDevice9Ex::NAME)>::template Defer<KIND, CSMT_SLOT_##KIND##_##NAME>))
#define CSMT_OUT(KIND, I, NAME, OUT_INDEX, OUT_KIND) \
   CSMT_SET(KIND, CSMT_SLOT_##KIND##_##NAME,         \
      (&Method<decltype(&I::NAME)>::template SyncOut<KIND, CSMT_SLOT_##KIND##_##NAME, OUT_INDEX, OUT_KIND>))

   inline void InitHookTables()
   {
      static bool done = false;
      if (done)
         return;
      done = true;
      // Deferred, value arguments only.
      CSMT_DEFER(DEVICE, SetRenderTarget);
      CSMT_DEFER(DEVICE, SetDepthStencilSurface);
      CSMT_DEFER(DEVICE, BeginScene);
      CSMT_DEFER(DEVICE, EndScene);
      CSMT_DEFER(DEVICE, LightEnable);
      // Deferred, repeats dropped.
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetRenderState, &SetRenderState);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetTexture, &SetTexture);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetTextureStageState, &SetTextureStageState);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetSamplerState, &SetSamplerState);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetVertexShader, (&SetShader<CSMT_SLOT_DEVICE_SetVertexShader, IDirect3DVertexShader9>));
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetPixelShader, (&SetShader<CSMT_SLOT_DEVICE_SetPixelShader, IDirect3DPixelShader9>));
      // Synchronous, and the shadow state is forgotten.
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_Reset, (&ForgetState<DEVICE, CSMT_SLOT_DEVICE_Reset, D3DPRESENT_PARAMETERS*>));
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_ResetEx,
         (&ForgetState<DEVICE, CSMT_SLOT_DEVICE_ResetEx, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*>));
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_BeginStateBlock, (&ForgetState<DEVICE, CSMT_SLOT_DEVICE_BeginStateBlock>));
      CSMT_SET(STATE_BLOCK, CSMT_SLOT_STATE_BLOCK_Apply, (&ForgetState<STATE_BLOCK, CSMT_SLOT_STATE_BLOCK_Apply>));
      CSMT_DEFER(DEVICE, DrawPrimitive);
      CSMT_DEFER(DEVICE, DrawIndexedPrimitive);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetVertexDeclaration, &SetVertexDeclaration);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetFVF, &SetFVF);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetStreamSource, &SetStreamSource);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetStreamSourceFreq, &SetStreamSourceFreq);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetIndices, &SetIndices);
      CSMT_DEFER(DEVICE, UpdateTexture);
      // Deferred with copied arguments.
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_Clear, &Clear);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetTransform, &SetTransform);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_MultiplyTransform, &MultiplyTransform);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetViewport, &SetViewport);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetMaterial, &SetMaterial);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetLight, &SetLight);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetClipPlane, &SetClipPlane);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetScissorRect, &SetScissorRect);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetVertexShaderConstantF, (&SetConstants<CSMT_SLOT_DEVICE_SetVertexShaderConstantF, float, 16>));
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetVertexShaderConstantI, (&SetConstants<CSMT_SLOT_DEVICE_SetVertexShaderConstantI, int, 16>));
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetVertexShaderConstantB, (&SetConstants<CSMT_SLOT_DEVICE_SetVertexShaderConstantB, BOOL, 4>));
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetPixelShaderConstantF, (&SetConstants<CSMT_SLOT_DEVICE_SetPixelShaderConstantF, float, 16>));
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetPixelShaderConstantI, (&SetConstants<CSMT_SLOT_DEVICE_SetPixelShaderConstantI, int, 16>));
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_SetPixelShaderConstantB, (&SetConstants<CSMT_SLOT_DEVICE_SetPixelShaderConstantB, BOOL, 4>));
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_DrawPrimitiveUP, &DrawPrimitiveUP);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_DrawIndexedPrimitiveUP, &DrawIndexedPrimitiveUP);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_StretchRect, &StretchRect);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_ColorFill, &ColorFill);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_UpdateSurface, &UpdateSurface);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_Present, &Present);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_Release, &DeviceRelease);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_CreateQuery, &CreateQuery);
      // Synchronous, and the returned object gets patched.
      using D = IDirect3DDevice9Ex;
      CSMT_OUT(DEVICE, D, CreateAdditionalSwapChain, 1, SWAP_CHAIN);
      CSMT_OUT(DEVICE, D, GetSwapChain, 1, SWAP_CHAIN);
      CSMT_OUT(DEVICE, D, GetBackBuffer, 3, SURFACE);
      CSMT_OUT(DEVICE, D, CreateTexture, 6, TEXTURE);
      CSMT_OUT(DEVICE, D, CreateVolumeTexture, 7, VOLUME_TEXTURE);
      CSMT_OUT(DEVICE, D, CreateCubeTexture, 5, CUBE_TEXTURE);
      CSMT_OUT(DEVICE, D, CreateVertexBuffer, 4, VERTEX_BUFFER);
      CSMT_OUT(DEVICE, D, CreateIndexBuffer, 4, INDEX_BUFFER);
      CSMT_OUT(DEVICE, D, CreateRenderTarget, 6, SURFACE);
      CSMT_OUT(DEVICE, D, CreateDepthStencilSurface, 6, SURFACE);
      CSMT_OUT(DEVICE, D, CreateOffscreenPlainSurface, 4, SURFACE);
      CSMT_OUT(DEVICE, D, GetRenderTarget, 1, SURFACE);
      CSMT_OUT(DEVICE, D, GetDepthStencilSurface, 0, SURFACE);
      CSMT_OUT(DEVICE, D, CreateStateBlock, 1, STATE_BLOCK);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_EndStateBlock, &EndStateBlock);
      CSMT_OUT(DEVICE, D, GetTexture, 1, BASE_TEXTURE);
      CSMT_OUT(DEVICE, D, CreateVertexDeclaration, 1, VERTEX_DECLARATION);
      CSMT_OUT(DEVICE, D, GetVertexDeclaration, 0, VERTEX_DECLARATION);
      CSMT_OUT(DEVICE, D, CreateVertexShader, 1, VERTEX_SHADER);
      CSMT_OUT(DEVICE, D, GetVertexShader, 0, VERTEX_SHADER);
      CSMT_OUT(DEVICE, D, GetStreamSource, 1, VERTEX_BUFFER);
      CSMT_OUT(DEVICE, D, GetIndices, 0, INDEX_BUFFER);
      CSMT_OUT(DEVICE, D, CreatePixelShader, 1, PIXEL_SHADER);
      CSMT_OUT(DEVICE, D, GetPixelShader, 0, PIXEL_SHADER);
      CSMT_OUT(DEVICE, D, CreateRenderTargetEx, 6, SURFACE);
      CSMT_OUT(DEVICE, D, CreateOffscreenPlainSurfaceEx, 4, SURFACE);
      CSMT_OUT(DEVICE, D, CreateDepthStencilSurfaceEx, 6, SURFACE);
      CSMT_OUT(SWAP_CHAIN, IDirect3DSwapChain9Ex, GetBackBuffer, 2, SURFACE);
      // Synchronous: a native runtime hands out a new surface object per call once the old one died, so the result
      // can't be cached (the lifetime stress in d3d9_csmt_test.exe catches a cache).
      CSMT_OUT(TEXTURE, IDirect3DTexture9, GetSurfaceLevel, 1, SURFACE);
      CSMT_OUT(CUBE_TEXTURE, IDirect3DCubeTexture9, GetCubeMapSurface, 2, SURFACE);
      CSMT_SET(DEVICE, CSMT_SLOT_DEVICE_TestCooperativeLevel, &TestCooperativeLevel);
      CSMT_OUT(VOLUME_TEXTURE, IDirect3DVolumeTexture9, GetVolumeLevel, 1, VOLUME);
      // Buffers and queries.
      // Descriptions, cached.
      CSMT_SET(SURFACE, CSMT_SLOT_SURFACE_GetDesc, (&GetDesc<SURFACE, CSMT_SLOT_SURFACE_GetDesc, D3DSURFACE_DESC>));
      CSMT_SET(VOLUME, CSMT_SLOT_VOLUME_GetDesc, (&GetDesc<VOLUME, CSMT_SLOT_VOLUME_GetDesc, D3DVOLUME_DESC>));
      CSMT_SET(VERTEX_BUFFER, CSMT_SLOT_VERTEX_BUFFER_GetDesc,
         (&GetDesc<VERTEX_BUFFER, CSMT_SLOT_VERTEX_BUFFER_GetDesc, D3DVERTEXBUFFER_DESC>));
      CSMT_SET(INDEX_BUFFER, CSMT_SLOT_INDEX_BUFFER_GetDesc,
         (&GetDesc<INDEX_BUFFER, CSMT_SLOT_INDEX_BUFFER_GetDesc, D3DINDEXBUFFER_DESC>));
      CSMT_SET(TEXTURE, CSMT_SLOT_TEXTURE_GetLevelDesc,
         (&GetLevelDesc<TEXTURE, CSMT_SLOT_TEXTURE_GetLevelDesc, D3DSURFACE_DESC>));
      CSMT_SET(CUBE_TEXTURE, CSMT_SLOT_CUBE_TEXTURE_GetLevelDesc,
         (&GetLevelDesc<CUBE_TEXTURE, CSMT_SLOT_CUBE_TEXTURE_GetLevelDesc, D3DSURFACE_DESC>));
      CSMT_SET(VOLUME_TEXTURE, CSMT_SLOT_VOLUME_TEXTURE_GetLevelDesc,
         (&GetLevelDesc<VOLUME_TEXTURE, CSMT_SLOT_VOLUME_TEXTURE_GetLevelDesc, D3DVOLUME_DESC>));
      CSMT_SET(VERTEX_BUFFER, CSMT_SLOT_VERTEX_BUFFER_Lock, &BufferLock<VERTEX_BUFFER>);
      CSMT_SET(VERTEX_BUFFER, CSMT_SLOT_VERTEX_BUFFER_Unlock, &BufferUnlock<VERTEX_BUFFER>);
      CSMT_SET(INDEX_BUFFER, CSMT_SLOT_INDEX_BUFFER_Lock, &BufferLock<INDEX_BUFFER>);
      CSMT_SET(INDEX_BUFFER, CSMT_SLOT_INDEX_BUFFER_Unlock, &BufferUnlock<INDEX_BUFFER>);
      CSMT_SET(QUERY, CSMT_SLOT_QUERY_Issue, &QueryIssue);
      CSMT_SET(QUERY, CSMT_SLOT_QUERY_GetData, &QueryGetData);
      // AddRef/Release of every object but the device.
      CSMT_SET(SWAP_CHAIN, 1, &ResourceAddRef<SWAP_CHAIN>);
      CSMT_SET(SWAP_CHAIN, 2, &ResourceRelease<SWAP_CHAIN>);
      CSMT_SET(STATE_BLOCK, 1, &ResourceAddRef<STATE_BLOCK>);
      CSMT_SET(STATE_BLOCK, 2, &ResourceRelease<STATE_BLOCK>);
      CSMT_SET(VERTEX_DECLARATION, 1, &ResourceAddRef<VERTEX_DECLARATION>);
      CSMT_SET(VERTEX_DECLARATION, 2, &ResourceRelease<VERTEX_DECLARATION>);
      CSMT_SET(VERTEX_SHADER, 1, &ResourceAddRef<VERTEX_SHADER>);
      CSMT_SET(VERTEX_SHADER, 2, &ResourceRelease<VERTEX_SHADER>);
      CSMT_SET(PIXEL_SHADER, 1, &ResourceAddRef<PIXEL_SHADER>);
      CSMT_SET(PIXEL_SHADER, 2, &ResourceRelease<PIXEL_SHADER>);
      CSMT_SET(TEXTURE, 1, &ResourceAddRef<TEXTURE>);
      CSMT_SET(TEXTURE, 2, &ResourceRelease<TEXTURE>);
      CSMT_SET(VOLUME_TEXTURE, 1, &ResourceAddRef<VOLUME_TEXTURE>);
      CSMT_SET(VOLUME_TEXTURE, 2, &ResourceRelease<VOLUME_TEXTURE>);
      CSMT_SET(CUBE_TEXTURE, 1, &ResourceAddRef<CUBE_TEXTURE>);
      CSMT_SET(CUBE_TEXTURE, 2, &ResourceRelease<CUBE_TEXTURE>);
      CSMT_SET(VERTEX_BUFFER, 1, &ResourceAddRef<VERTEX_BUFFER>);
      CSMT_SET(VERTEX_BUFFER, 2, &ResourceRelease<VERTEX_BUFFER>);
      CSMT_SET(INDEX_BUFFER, 1, &ResourceAddRef<INDEX_BUFFER>);
      CSMT_SET(INDEX_BUFFER, 2, &ResourceRelease<INDEX_BUFFER>);
      CSMT_SET(SURFACE, 1, &ResourceAddRef<SURFACE>);
      CSMT_SET(SURFACE, 2, &ResourceRelease<SURFACE>);
      CSMT_SET(VOLUME, 1, &ResourceAddRef<VOLUME>);
      CSMT_SET(VOLUME, 2, &ResourceRelease<VOLUME>);
      CSMT_SET(QUERY, 1, &ResourceAddRef<QUERY>);
      CSMT_SET(QUERY, 2, &ResourceRelease<QUERY>);
   }
#undef CSMT_SET
#undef CSMT_DEFER
#undef CSMT_OUT
#undef CSMT_ORIGINAL

   inline DWORD WINAPI WorkerMain(void*)
   {
      t_worker = true;
      g.queue->Run(
         []
         {
            // Idle: queries the game is waiting for are polled at once and again without sleeping; the rest every
            // poll interval.
            if (g.polled_queries.empty())
               return DWORD(INFINITE);
            const bool waited_for = std::any_of(g.polled_queries.begin(), g.polled_queries.end(),
               [](const QueryState* state)
               { return state->wanted.load(std::memory_order_relaxed); });
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (!waited_for && now.QuadPart - g.last_poll.QuadPart < g.poll_interval.QuadPart)
               return DWORD(1);
            g.last_poll = now;
            PollQueries(D3DGETDATA_FLUSH);
            if (g.polled_queries.empty())
               return DWORD(INFINITE);
            return (waited_for ? DWORD(0) : DWORD(1));
         },
         []
         {
            // Busy: queries the game waits for get results between commands too (Wine polls every 100 commands).
            if (!g.polled_queries.empty())
            {
               PollQueries(0, true); // no flush mid-batch
            }
         });
      return 0;
   }

   // After the device is created and every other layer patched it (they end up below this one). The game thread is
   // the only one that knows the device yet.
   inline bool Start(IDirect3DDevice9* device, FILE* log)
   {
      if (g.active.load())
      {
         fprintf(log, "CSMT: a second device while the first is alive, not threaded\n");
         return false;
      }
      InitHookTables();
      PatchObject(device, DEVICE);
      g.device = device;
      g.state.Forget();
      g.queue = new CommandQueue(QUEUE_BYTES);
      g.presents_pushed = 0;
      g.presents_done = 0;
      LARGE_INTEGER frequency;
      QueryPerformanceFrequency(&frequency);
      g.poll_interval.QuadPart = frequency.QuadPart / 4000; // 0.25 ms between idle polls
      g.last_present = S_OK;
      ForgetVertexConstants();
      g_vertex_constants_recording = false;
      g_vertex_constants.active = 1;
      g.thread = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
      if (!g.thread)
      {
         delete g.queue;
         g.queue = nullptr;
         g_vertex_constants.active = 0;
         fprintf(log, "CSMT: CreateThread failed %lu\n", GetLastError());
         return false;
      }
      SetThreadPriority(g.thread, GetThreadPriority(GetCurrentThread()));
      if (auto set_description = (HRESULT(WINAPI*)(HANDLE, PCWSTR))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))
      {
         set_description(g.thread, L"Luma D3D9 CSMT");
      }
      g.active = true;
      fprintf(log, "CSMT: on, device %p, %d slots\n", (void*)device, FindVtable(*(void***)device)->slots);
      return true;
   }

   // The device's last reference is gone; everything queued has run.
   inline void Stop()
   {
      ProducerScope scope;
      g.queue->Push([](char*)
         { g.queue->RequestStop(); });
      g.queue->Flush();
      WaitForSingleObject(g.thread, INFINITE);
      CloseHandle(g.thread);
      g.thread = nullptr;
      g.active = false;
      g_vertex_constants.active = 0;
      delete g.queue;
      g.queue = nullptr;
      g.device = nullptr;
      g.locks.clear();
      g.queries.clear();
      g.polled_queries.clear();
      g.state.Forget();
      g.buffer_shadows.clear();
      g.buffer_descs.clear();
      g.descs.clear();
      g.refs.clear();
   }

   inline void Report(FILE* log, double seconds)
   {
      if (!g.queue)
         return;
      Stats stats;
      {
         ProducerScope scope;
         stats = g.stats;
         g.stats = {};
      }
      struct Entry
      {
         uint32_t count;
         int kind, slot;
      };
      std::vector<Entry> syncs;
      uint64_t total_syncs = 0;
      for (int kind = 0; kind < KIND_COUNT; kind++)
      {
         for (int slot = 0; slot < g_slot_counts[kind]; slot++)
         {
            if (const uint32_t count = stats.syncs[kind][slot])
            {
               syncs.push_back({count, kind, slot});
               total_syncs += count;
            }
         }
      }
      std::sort(syncs.begin(), syncs.end(), [](const Entry& a, const Entry& b)
         { return a.count > b.count; });
      const double frames = (std::max<double>)(double(stats.presents), 1.0);
      fprintf(log,
         "    CSMT | %.0f fps | per frame: commands %.0f, payload %.1f KB, syncs %.1f, locks deferred %.1f sync %.1f, "
         "query hits %.1f misses %.1f, present wait %.2f ms, repeats dropped %.0f | worker polls %llu | syncs:",
         stats.presents / seconds, stats.commands / frames, stats.payload_bytes / frames / 1024, total_syncs / frames,
         stats.deferred_locks / frames, stats.sync_locks / frames, stats.query_hits / frames, stats.query_misses / frames,
         stats.present_wait_ms / frames, stats.redundant / frames, (unsigned long long)g.worker_polls.exchange(0));
      for (size_t i = 0; i < (std::min<size_t>)(syncs.size(), 12); i++)
      {
         fprintf(log, " %s::%s %.1f", g_kind_names[syncs[i].kind], g_names[syncs[i].kind][syncs[i].slot],
            syncs[i].count / frames);
      }
      std::vector<CallSite> sites;
      {
         ProducerScope scope;
         for (int i = 0; i < g_call_site_count; i++)
         {
            sites.push_back(g_call_sites[i]);
            g_call_sites[i].count = 0;
         }
      }
      std::sort(sites.begin(), sites.end(), [](const CallSite& a, const CallSite& b)
         { return a.count > b.count; });
      fprintf(log, "\n    CSMT commands per frame:");
      for (size_t i = 0; i < (std::min<size_t>)(sites.size(), 12); i++)
      {
         // MSVC names a lambda after its enclosing function: keep that part.
         const char* name = sites[i].name;
         const char* tail = strstr(name, "::<lambda");
         fprintf(log, " [%.*s] %.0f", int(tail ? tail - name : strlen(name)), name, sites[i].count / frames);
      }
      fprintf(log, "\n");
   }
} // namespace Csmt
