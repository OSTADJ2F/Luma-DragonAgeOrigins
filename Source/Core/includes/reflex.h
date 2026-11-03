#pragma once

// NVIDIA Reflex injected at present, for games without their own: "NvAPI_D3D_Sleep()" right after the game's present returns
// (where most engines start simulating the next frame) keeps the render queue empty when GPU bound.
// The markers are only an approximation of the game's frame (simulation and render submission span the whole CPU frame),
// so they aren't used for driver optimizations ("bUseMarkersToOptimize").
#if ENABLE_REFLEX
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <thread>
#include <utility>

#include <evntrace.h>
#include <TraceLoggingProvider.h>

#pragma comment(lib, "Advapi32.lib") // ETW, for PCLStats

#include "nvapi.h"

#include "minhook.h"

namespace Reflex
{
   enum class Mode : int
   {
      Off,
      On,
      Boost, // Also keeps GPU clocks at their maximum
   };

   // The states after "Running" are final for the device
   enum class State
   {
      Inactive,
      Running,
      Unsupported, // Not an NVIDIA GPU or driver
      Other,       // The game or another tool (e.g. Display Commander) already runs Reflex
      // NVIDIA Smooth Motion (the driver's frame generation) is on: it turns on the driver's low latency mode itself, and Luma's
      // Reflex on top gained nothing in a test
      SmoothMotion,
   };
   constexpr const char* state_names[] = {"Inactive", "Running", "Unsupported", "Other", "Smooth Motion"};

   // The driver reports another caller's sleep ("bUseGameSleep") only a few presents after it started, so Luma watches this
   // many before taking over
   constexpr uint32_t detection_presents = 60;
   // Luma's frame IDs start far from a game's own (which count from about 0), so a latency report of another frame ID means the
   // game or a tool started its own Reflex later (e.g. turned on from a menu)
   constexpr uint64_t first_frame_id = 1ull << 40;
   // Presents between the checks for another owner (and the DEV stats)
   constexpr uint32_t check_interval = 30;
   // The driver's sleep mode off (or no sleep seen) this long while Luma's is on means another owner: longer than its status lag
   // (~340 presents without markers) plus Display Commander's 500 frame delay before it passes Luma's mode on
   constexpr uint32_t owner_presents = 600;

   // PC Latency stats ("PCLStats"): Luma's markers again as ETW events, plus a ping message posted to the game's window that the
   // frame handling it marks. FrameView and the NVIDIA App overlay need both to measure the PC latency, the driver doesn't pass
   // the NVAPI markers on. Provider, event and field names and the ping as in NVIDIA's "pclstats.h" (Reflex SDK, Streamline).
   // Running only while Luma owns Reflex: a game or tool with its own Reflex has its own PCLStats, two would mix their events.
   namespace PCL
   {
      TRACELOGGING_DEFINE_PROVIDER(provider, "PCLStatsTraceLoggingProvider", (0x0d216f06, 0x82a6, 0x4d49, 0xbc, 0x4f, 0x8f, 0x38, 0xae, 0x56, 0xef, 0xab));

      std::atomic<bool> ping_pending = false; // Taken from the game's message queue, marked on the frame's next present
      UINT ping_message = 0;

      // Whether an ETW session (FrameView, the NVIDIA App) enabled the provider, which wakes the ping thread
      struct Listener
      {
         std::shared_mutex mutex;
         bool enabled = false;
         std::condition_variable_any changed;
      };
      Listener listener;

      // Shared by the devices whose frames Luma marks, stopped with the last one
      struct Session
      {
         std::shared_mutex mutex;
         uint32_t users = 0;
         HHOOK message_hook = nullptr;
         std::jthread ping_thread;
      };
      Session session;

      void NTAPI OnProviderControl(LPCGUID, ULONG control_code, UCHAR, ULONGLONG, ULONGLONG, PEVENT_FILTER_DESCRIPTOR, PVOID)
      {
         switch (control_code)
         {
         case EVENT_CONTROL_CODE_ENABLE_PROVIDER:
         case EVENT_CONTROL_CODE_DISABLE_PROVIDER:
         {
            {
               const std::unique_lock lock(listener.mutex);
               listener.enabled = control_code == EVENT_CONTROL_CODE_ENABLE_PROVIDER;
            }
            listener.changed.notify_all();
            break;
         }
         // No "PCLSTATS_NO_PRESENT_MARKERS": Luma sets the present markers
         case EVENT_CONTROL_CODE_CAPTURE_STATE:
            TraceLoggingWrite(provider, "PCLStatsFlags", TraceLoggingUInt32(0u, "Flags"));
            break;
         default:
            break;
         }
      }

      // On the window's thread, for the messages its loop takes from the queue
      LRESULT CALLBACK OnGetMessage(int code, WPARAM wparam, LPARAM lparam)
      {
         if (code == HC_ACTION && wparam == PM_REMOVE && reinterpret_cast<const MSG*>(lparam)->message == ping_message)
         {
            ping_pending = true;
         }
         return CallNextHookEx(nullptr, code, wparam, lparam);
      }

      // Pings every 100 to 300 ms (random, as NVIDIA's) while listened to and the game is in the foreground, asleep otherwise
      void PingLoop(std::stop_token stop, HWND window)
      {
         std::minstd_rand random(GetCurrentThreadId());
         std::unique_lock lock(listener.mutex);
         while (!stop.stop_requested())
         {
            // Asleep until a session listens ("wait" also returns on a stop, which ends the loop)
            if (!listener.changed.wait(lock, stop, []
                   { return listener.enabled; }))
               continue;
            // Then a random wait, cut short by a stop or the session ending
            if (listener.changed.wait_for(lock, stop, std::chrono::milliseconds(100 + random() % 200), []
                   { return !listener.enabled; }) ||
                stop.stop_requested())
               continue;
            DWORD foreground_process = 0;
            if (!GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process) || foreground_process != GetCurrentProcessId())
               continue;
            TraceLoggingWrite(provider, "PCLStatsInput", TraceLoggingUInt32(ping_message, "MsgId"));
            PostMessageW(window, ping_message, 0, 0);
         }
      }

      void Start(HWND window)
      {
         const std::unique_lock lock(session.mutex);
         if (session.users++ != 0)
            return;
         if (!ping_message)
         {
            ping_message = RegisterWindowMessageW(L"PC_Latency_Stats_Ping");
         }
         ping_pending = false;
         TraceLoggingRegisterEx(provider, OnProviderControl, nullptr);
         TraceLoggingWrite(provider, "PCLStatsInit");
         session.message_hook = SetWindowsHookExW(WH_GETMESSAGE, OnGetMessage, nullptr, GetWindowThreadProcessId(window, nullptr));
         session.ping_thread = std::jthread(PingLoop, window);
      }

      // For each "Start()", also before the addon unloads: the hook and the thread run its code
      void Stop()
      {
         const std::unique_lock lock(session.mutex);
         if (--session.users != 0)
            return;
         session.ping_thread = {}; // Stops and joins it
         UnhookWindowsHookEx(std::exchange(session.message_hook, nullptr));
         TraceLoggingWrite(provider, "PCLStatsShutdown");
         TraceLoggingUnregister(provider);
         // Unregistering sends no disable: a later start without a session must not ping
         const std::unique_lock listener_lock(listener.mutex);
         listener.enabled = false;
      }
   } // namespace PCL

   // Another caller's "NvAPI_D3D_Sleep()": the game's or a tool's Reflex, found on its first call, also one without markers that
   // starts after the detection presents, which the driver's status can't tell from Luma's own sleep. Hooked on the driver's
   // function, the address "nvapi_QueryInterface()" gives every caller (Display Commander hooks the same one, chained either way).
   // A tool that sleeps through its own hook's trampoline (Display Commander's own sleep, only done while nobody else sleeps)
   // bypasses it.
   namespace SleepHook
   {
      using SleepFunction = NvAPI_Status(__cdecl*)(IUnknown*);
      constexpr NvU32 sleep_interface_id = 0x852CD1D2; // "NvAPI_D3D_Sleep" in "nvapi_interface.h"

      // Shared by the devices that run Luma's Reflex, removed with the last one
      struct Hook
      {
         std::shared_mutex mutex;
         uint32_t users = 0;
         MinHook::HookedFunction<SleepFunction> sleep;
      };
      Hook hook;
      thread_local bool luma_sleeping = false;
      // Counts every other caller's sleep: each device compares it with the count from when it started watching, so none clears
      // another's finding
      std::atomic<uint32_t> other_sleeps = 0;

      NvAPI_Status __cdecl Detour(IUnknown* device)
      {
         if (!luma_sleeping)
         {
            other_sleeps++;
         }
         return hook.sleep.original(device);
      }

      // True if the hook is in, the caller then being one of its users
      bool Install()
      {
         const std::lock_guard lock(hook.mutex);
         if (!hook.sleep.target)
         {
            using QueryInterfaceFunction = void*(__cdecl*)(NvU32);
            const HMODULE nvapi = GetModuleHandleW(sizeof(void*) == 8 ? L"nvapi64.dll" : L"nvapi.dll");
            const auto query_interface = nvapi ? reinterpret_cast<QueryInterfaceFunction>(GetProcAddress(nvapi, "nvapi_QueryInterface")) : nullptr;
            void* const sleep = query_interface ? query_interface(sleep_interface_id) : nullptr;
            if (!sleep || !MinHook::Attach(&hook.sleep, sleep, &Detour))
               return false;
         }
         hook.users++;
         return true;
      }

      // For each successful "Install()". False if the last user's hook had to stay (see "MinHook::Detach()").
      bool Remove()
      {
         const std::lock_guard lock(hook.mutex);
         return --hook.users != 0 || MinHook::Detach(&hook.sleep);
      }
   } // namespace SleepHook

#if DEVELOPMENT
   // The driver's last frame reports (whoever set their markers), averaged for the DEV panel
   struct LatencyStats
   {
      double latency_ms = 0.0;    // Frame start (after the sleep, or the previous present) to the GPU's end of the frame
      double gpu_ms = 0.0;        // The GPU's active time on the frame
      double gpu_frame_ms = 0.0;  // GPU end to GPU end
      uint32_t frames = 0;        // Reports averaged (0: none, nobody sets markers)
      bool other_markers = false; // Some were the game's or a tool's
      double sleep_ms = 0.0;      // Luma's time in "NvAPI_D3D_Sleep()" per frame, since the last update
      // The driver's sleep status ("bLowLatencyMode", "bUseGameSleep")
      bool driver_low_latency = false, driver_game_sleep = false;
   };
#endif

   struct DeviceData
   {
      State state = State::Inactive;
      Mode applied_mode = Mode::Off;
      uint64_t frame_id = first_frame_id;
      uint32_t presents = 0;              // Watched for another Reflex owner, up to "detection_presents"
      bool markers = false;               // The frame "frame_id" has Luma's markers
      uint32_t mode_presents = 0;         // Since Luma last set the sleep mode, or since the driver last agreed with it
      const char* other_reason = nullptr; // Why the state became "Other"
      bool pcl = false;                   // One of "PCL"'s users (while its frames are marked)
      bool sleep_hook = false;            // One of "SleepHook"'s users
      uint32_t other_sleeps_seen = 0;     // "SleepHook::other_sleeps" when this device started watching
      // The device's first swapchain, the only one followed (only compared)
      std::atomic<void*> swapchain = nullptr;
#if DEVELOPMENT
      LatencyStats latency_stats;
      uint32_t latency_stats_presents = 0;
      int64_t sleep_ns = 0; // Since the last update
      uint32_t sleeps = 0;
      bool log_due = false; // Every 10 updates, for the log
#endif
   };

   // The driver's last 64 frame reports, null if it has none
   const NV_LATENCY_RESULT_PARAMS* GetLatencyReports(IUnknown* device)
   {
      thread_local NV_LATENCY_RESULT_PARAMS latency; // ~15 KB
      latency = {};
      latency.version = NV_LATENCY_RESULT_PARAMS_VER;
      return NvAPI_D3D_GetLatency(device, &latency) == NVAPI_OK ? &latency : nullptr;
   }

   bool IsOtherFrame(uint64_t report_frame_id, uint64_t frame_id)
   {
      return report_frame_id != 0 && (report_frame_id > frame_id || report_frame_id < first_frame_id);
   }

   // Whether the driver's last 64 frame reports hold markers Luma didn't set
   bool HasOtherMarkers(IUnknown* device, uint64_t frame_id)
   {
      const NV_LATENCY_RESULT_PARAMS* const latency = GetLatencyReports(device);
      return latency && std::ranges::any_of(latency->frameReport, [&](const auto& frame)
                           { return IsOtherFrame(frame.frameID, frame_id); });
   }

#if DEVELOPMENT
   void UpdateLatencyStats(IUnknown* device, DeviceData* data)
   {
      LatencyStats stats;
      if (const NV_LATENCY_RESULT_PARAMS* const latency = GetLatencyReports(device))
      {
         for (const auto& frame : latency->frameReport)
         {
            if (frame.frameID == 0 || frame.simStartTime == 0 || frame.gpuRenderEndTime <= frame.simStartTime)
               continue;
            stats.latency_ms += double(frame.gpuRenderEndTime - frame.simStartTime);
            stats.gpu_ms += frame.gpuActiveRenderTimeUs;
            stats.gpu_frame_ms += frame.gpuFrameTimeUs;
            stats.frames++;
            stats.other_markers |= IsOtherFrame(frame.frameID, data->frame_id);
         }
         // From microseconds
         if (stats.frames != 0)
         {
            stats.latency_ms /= stats.frames * 1000.0;
            stats.gpu_ms /= stats.frames * 1000.0;
            stats.gpu_frame_ms /= stats.frames * 1000.0;
         }
      }
      NV_GET_SLEEP_STATUS_PARAMS status = {NV_GET_SLEEP_STATUS_PARAMS_VER};
      if (NvAPI_D3D_GetSleepStatus(device, &status) == NVAPI_OK)
      {
         stats.driver_low_latency = status.bLowLatencyMode;
         stats.driver_game_sleep = status.bUseGameSleep;
      }
      stats.sleep_ms = data->sleeps != 0 ? double(data->sleep_ns) / data->sleeps / 1e6 : 0.0;
      data->sleep_ns = 0;
      data->sleeps = 0;
      data->latency_stats = stats;
      data->log_due = data->latency_stats_presents % (check_interval * 10) == 0;
   }
#endif

   void SetMarker(IUnknown* device, uint64_t frame_id, NV_LATENCY_MARKER_TYPE type)
   {
      NV_LATENCY_MARKER_PARAMS params = {NV_LATENCY_MARKER_PARAMS_VER};
      params.frameID = frame_id;
      params.markerType = type;
      NvAPI_D3D_SetLatencyMarker(device, &params);
      // PCLStats' marker values are NVAPI's. Dropped while "PCL" isn't registered.
      TraceLoggingWrite(PCL::provider, "PCLStatsEvent", TraceLoggingUInt32(uint32_t(type), "Marker"), TraceLoggingUInt64(frame_id, "FrameID"));
   }

   // Before the game's present
   void OnPresent(IUnknown* device, const DeviceData& data)
   {
      if (!data.markers)
         return;
      // The ping arrived during this frame's simulation (the game's message loop)
      if (PCL::ping_pending.exchange(false))
      {
         SetMarker(device, data.frame_id, PC_LATENCY_PING);
      }
      SetMarker(device, data.frame_id, SIMULATION_END);
      SetMarker(device, data.frame_id, RENDERSUBMIT_END);
      SetMarker(device, data.frame_id, PRESENT_START);
   }

   // After the game's present returned: the frame ends, and the next one starts after the sleep
   void OnFinishPresent(IUnknown* device, HWND window, DeviceData* data, Mode mode)
   {
#if DEVELOPMENT
      if (data->state != State::Unsupported && ++data->latency_stats_presents % check_interval == 0)
      {
         UpdateLatencyStats(device, data);
      }
#endif
      // Every return but the last leaves the next frame unmarked. PCLStats runs while Luma marks the frames, so it stops a present
      // after the state became final, Off (non-DEV) or detecting.
      const bool had_markers = std::exchange(data->markers, false);
      if (!had_markers && std::exchange(data->pcl, false))
      {
         PCL::Stop();
      }
      if (data->state > State::Running)
         return;
      if (data->sleep_hook && SleepHook::other_sleeps != data->other_sleeps_seen)
      {
         data->state = State::Other;
         data->other_reason = "other sleep call";
         return;
      }
      if (data->presents < detection_presents)
      {
         NV_GET_SLEEP_STATUS_PARAMS status = {NV_GET_SLEEP_STATUS_PARAMS_VER};
         if (NvAPI_D3D_GetSleepStatus(device, &status) != NVAPI_OK) // Also when NVAPI didn't initialize
         {
            data->state = State::Unsupported;
         }
         // Its present layer, loaded with the device
         else if (GetModuleHandleW(sizeof(void*) == 8 ? L"NvPresent64.dll" : L"NvPresent.dll"))
         {
            data->state = State::SmoothMotion;
         }
         // Not "bLowLatencyMode" alone: the control panel's "Low Latency Mode" can set it (the game's own sleep can't)
         else if (status.bUseGameSleep)
         {
            data->state = State::Other;
            data->other_reason = "sleep status at start";
         }
         // Watches other callers' sleeps from here on
         else if (data->presents == 0)
         {
            data->other_sleeps_seen = SleepHook::other_sleeps;
            data->sleep_hook = SleepHook::Install();
         }
         data->presents++;
         return;
      }

      // Another Reflex (the game's or a tool's) started later: leave it to that (and its sleep mode)
      if (((had_markers && data->frame_id % check_interval == 0) || (data->applied_mode == Mode::Off && mode != Mode::Off)) && HasOtherMarkers(device, data->frame_id))
      {
         data->state = State::Other;
         data->other_reason = "other markers";
         return;
      }

      if (data->applied_mode != mode)
      {
         NV_SET_SLEEP_MODE_PARAMS params = {NV_SET_SLEEP_MODE_PARAMS_VER};
         params.bLowLatencyMode = mode != Mode::Off;
         params.bLowLatencyBoost = mode == Mode::Boost;
         if (NvAPI_D3D_SetSleepMode(device, &params) != NVAPI_OK)
         {
            data->state = State::Unsupported;
            return;
         }
         data->applied_mode = mode;
         data->mode_presents = 0;
      }
      // Another owner of the sleep mode without markers of its own, or started later: while Luma's is on, the driver's stays off
      // or never sees Luma's sleep (e.g. Display Commander, which takes Luma's calls for the game's and applies its own mode).
      // The driver's on while Luma's is off isn't one: the control panel's "Low Latency Mode" does that too.
      else if (mode != Mode::Off && ++data->mode_presents >= detection_presents && data->mode_presents % check_interval == 0)
      {
         NV_GET_SLEEP_STATUS_PARAMS status = {NV_GET_SLEEP_STATUS_PARAMS_VER};
         if (NvAPI_D3D_GetSleepStatus(device, &status) != NVAPI_OK || (status.bLowLatencyMode && status.bUseGameSleep))
         {
            data->mode_presents = detection_presents;
         }
         else if (data->mode_presents >= detection_presents + owner_presents)
         {
            data->state = State::Other;
            data->other_reason = !status.bLowLatencyMode ? "driver low latency off, Luma on" : "driver sees no sleep, Luma on";
            return;
         }
      }
      if (had_markers)
      {
         SetMarker(device, data->frame_id, PRESENT_END);
      }
      data->state = mode == Mode::Off ? State::Inactive : State::Running;
      // DEV also marks the frames with Reflex off (no sleep), to compare their latency
      if (mode == Mode::Off && !DEVELOPMENT)
         return;
      if (mode != Mode::Off)
      {
#if DEVELOPMENT
         const auto sleep_start = std::chrono::steady_clock::now();
#endif
         SleepHook::luma_sleeping = true;
         NvAPI_D3D_Sleep(device);
         SleepHook::luma_sleeping = false;
#if DEVELOPMENT
         data->sleep_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - sleep_start).count();
         data->sleeps++;
#endif
      }
      // Before the frame's first markers, so the trace has all of them
      if (!std::exchange(data->pcl, true))
      {
         PCL::Start(window);
      }
      data->frame_id++;
      data->markers = true;
      SetMarker(device, data->frame_id, SIMULATION_START);
      SetMarker(device, data->frame_id, RENDERSUBMIT_START);
   }

   // When the device is destroyed: its ping thread, message hook and sleep hook run the addon's code. False if the sleep hook had
   // to stay ("SleepHook::Remove()").
   bool OnDestroyDevice(DeviceData* data)
   {
      if (std::exchange(data->pcl, false))
      {
         PCL::Stop();
      }
      return !std::exchange(data->sleep_hook, false) || SleepHook::Remove();
   }
} // namespace Reflex
#endif // ENABLE_REFLEX
