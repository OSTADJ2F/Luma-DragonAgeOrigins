#pragma once

#include <d3d11.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <deps/imgui/imgui.h>
#include <source/com_ptr.hpp>

// "Performance Test" (DEVELOPMENT): GPU timestamps per frame in a ring read back a few frames later without waiting (see
// docs/GPU-Perf-Measurement-Immediate-Deferred.md), the CPU time in a game's hooks, log windows of 120 frames after a settle, and
// sweeps that run modes in turn over several rounds (interleaved, so the scene's drift averages out), then a median per mode.
// The game owns its modes (what each sets, and restoring the user's settings), where it marks timestamps and its log lines.
namespace Perf
{
   inline int g_test = 0;            // The running mode, an index into the game's modes (0: off)
   inline bool g_hook_timers = true; // The hooks' CPU time (two clock reads per hooked draw, themselves a cost to measure)

   // Adds the scope's CPU time to "total_ns", while a test runs with "g_hook_timers"
   struct HookTimer
   {
      std::atomic<int64_t>* total_ns;
      const bool enabled = g_test != 0 && g_hook_timers;
      const std::chrono::steady_clock::time_point start = (enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{});
      ~HookTimer()
      {
         if (enabled)
         {
            *total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
         }
      }
   };

   struct Stat
   {
      double total_ms = 0.0;
      double max_ms = 0.0;
      uint32_t samples = 0;

      void Add(double ms)
      {
         total_ms += ms;
         max_ms = (std::max)(max_ms, ms);
         samples++;
      }
      double Average() const
      {
         return samples != 0 ? total_ms / samples : 0.0;
      }
   };

   // Shader creations, from any thread: the game's (its "Create*Shader" between ReShade's create and init pipeline events, Luma's work
   // on the bytecode included) and Luma's patched variants (timed by the game mod). Present logs each frame that created any, with the
   // frame's CPU time: a driver that compiles asynchronously (NVIDIA) returns from the create early and stalls at the shader's first
   // draw, which only the frame time shows. The totals are the MCP counters "core.shader_creates_*".
   struct ShaderCreates
   {
      std::shared_mutex mutex;
      Stat game; // Since the last present
      Stat luma;
      Stat game_total;
      Stat luma_total;
      std::chrono::steady_clock::time_point last_present = {};

      // Published from the totals at present
      uint32_t game_count = 0;
      uint32_t game_ms = 0;
      uint32_t game_max_us = 0;
      uint32_t luma_count = 0;
      uint32_t luma_ms = 0;

      void AddGame(std::chrono::steady_clock::duration duration)
      {
         const std::unique_lock lock(mutex);
         game.Add(std::chrono::duration<double, std::milli>(duration).count());
      }
      void AddLuma(std::chrono::steady_clock::duration duration)
      {
         const std::unique_lock lock(mutex);
         luma.Add(std::chrono::duration<double, std::milli>(duration).count());
      }
   };
   inline ShaderCreates g_shader_creates;

   // The median of "values" (0 without any); NaN is a window that couldn't measure a column and is left out
   inline double Median(std::vector<double> values)
   {
      std::erase_if(values, [](double value)
         { return std::isnan(value); });
      if (values.empty())
         return 0.0;
      std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
      return values[values.size() / 2];
   }

   // Every set opens with "FRAME_START" at a present and closes with "FRAME_END" at the next; a game numbers its own stamps from
   // "FIRST_GAME_STAMP"
   constexpr size_t FRAME_START = 0;
   constexpr size_t FRAME_END = 1;
   constexpr size_t FIRST_GAME_STAMP = 2;

   template <size_t Stamps>
   struct TimestampSet
   {
      static_assert(Stamps <= 32);
      com_ptr<ID3D11Query> disjoint;
      std::array<com_ptr<ID3D11Query>, Stamps> stamps;
      uint32_t marked = 0; // Issued this frame, by stamp bit
      bool pending = false;

      bool Marked(size_t stamp) const
      {
         return (marked & (1u << stamp)) != 0;
      }
      // The first mark in the frame holds
      void Mark(ID3D11DeviceContext* native_device_context, size_t stamp)
      {
         if (Marked(stamp))
            return;
         native_device_context->End(stamps[stamp].get());
         marked |= 1u << stamp;
      }
   };

   // A finished set's stamps (those marked and read back)
   template <size_t Stamps>
   struct TimestampReadback
   {
      UINT64 frequency = 0;
      std::array<UINT64, Stamps> ticks = {};
      uint32_t valid = 0;

      bool Has(size_t stamp) const
      {
         return (valid & (1u << stamp)) != 0;
      }
      double Ms(size_t start, size_t end) const
      {
         return 1000.0 * double(ticks[end] - ticks[start]) / double(frequency);
      }
   };

   // All on the immediate context
   template <size_t Stamps>
   struct TimestampRing
   {
      std::array<TimestampSet<Stamps>, 8> sets;
      size_t next = 0;
      TimestampSet<Stamps>* frame = nullptr; // This frame's, from present to present

      void Close(ID3D11DeviceContext* native_device_context)
      {
         auto* const set = std::exchange(frame, nullptr);
         if (!set)
            return;
         set->Mark(native_device_context, FRAME_END);
         native_device_context->End(set->disjoint.get());
         set->pending = true;
      }

      void Open(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context)
      {
         auto& set = sets[next];
         if (set.pending)
            return;
         if (!set.disjoint)
         {
            const D3D11_QUERY_DESC disjoint_desc = {D3D11_QUERY_TIMESTAMP_DISJOINT}, timestamp_desc = {D3D11_QUERY_TIMESTAMP};
            native_device->CreateQuery(&disjoint_desc, &set.disjoint);
            for (auto& stamp : set.stamps)
               native_device->CreateQuery(&timestamp_desc, &stamp);
         }
         if (!set.disjoint || std::any_of(set.stamps.begin(), set.stamps.end(), [](const com_ptr<ID3D11Query>& stamp)
                                 { return !stamp; }))
            return;
         native_device_context->Begin(set.disjoint.get());
         set.marked = 0;
         set.Mark(native_device_context, FRAME_START);
         frame = &set;
         next = (next + 1) % sets.size();
      }

      // Calls "on_frame" with each finished set that has its frame stamps, while "measuring" (else they're dropped). Returns the
      // disjoint sets' count.
      template <typename OnFrame>
      uint32_t Collect(ID3D11DeviceContext* native_device_context, bool measuring, OnFrame&& on_frame)
      {
         uint32_t disjoint_count = 0;
         for (auto& set : sets)
         {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
            if (!set.pending || native_device_context->GetData(set.disjoint.get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
               continue;
            set.pending = false;
            // Read even the dropped stamps: an "End" on a query whose results were never read warns
            // (QUERY_END_ABANDONING_PREVIOUS_RESULTS)
            TimestampReadback<Stamps> readback = {.frequency = disjoint.Frequency};
            for (size_t stamp = 0; stamp < Stamps; stamp++)
            {
               if (set.Marked(stamp) && native_device_context->GetData(set.stamps[stamp].get(), &readback.ticks[stamp], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
               {
                  readback.valid |= 1u << stamp;
               }
            }
            if (!measuring)
               continue;
            if (disjoint.Disjoint || disjoint.Frequency == 0)
            {
               disjoint_count++;
               continue;
            }
            if (readback.Has(FRAME_START) && readback.Has(FRAME_END))
            {
               on_frame(readback);
            }
         }
         return disjoint_count;
      }
   };

   // A log window: settles (skips frames) after a settings change or a pause, then measures 120 frames
   template <typename Stats>
   struct Window
   {
      Stats stats; // This log window's
      uint32_t frames = 0;
      uint32_t disjoint = 0;
      double cpu_frame_ms = 0.0;
      std::atomic<int64_t> hook_ns = 0; // This log window's, from "HookTimer"
      int settle_frames = 0;            // Frames left to skip (targets rebuilt, history reset)
      std::string settings;             // The measured settings, to restart the settle on a change
      std::chrono::steady_clock::time_point last_present;
      std::chrono::steady_clock::time_point present;

      // At every present while a test runs: whether this frame is measured. Also settles after a pause (the game stops presenting
      // while unfocused: the upscaler history and the clocks restart) and on "settle".
      bool Present(std::string_view current_settings, int frames_to_settle, bool settle = false)
      {
         present = std::chrono::steady_clock::now();
         const bool changed = (settings != current_settings);
         if (changed)
         {
            settings = current_settings; // Reuses the string's capacity
         }
         if (changed || settle || present - last_present > std::chrono::milliseconds(250))
         {
            settle_frames = frames_to_settle;
         }
         return settle_frames <= 0;
      }

      // After the readback: calls "on_window" when the window is complete, then starts the next
      template <typename OnWindow>
      void Finish(bool measuring, OnWindow&& on_window)
      {
         if (measuring)
         {
            cpu_frame_ms += std::chrono::duration<double, std::milli>(present - last_present).count();
            if (++frames >= 120)
            {
               on_window();
               Reset();
            }
         }
         else
         {
            settle_frames--;
            Reset();
         }
         last_present = present;
      }

      void Reset()
      {
         stats = {};
         frames = 0;
         disjoint = 0;
         cpu_frame_ms = 0.0;
         hook_ns = 0;
      }

      double CpuFrameMs() const
      {
         return cpu_frame_ms / frames;
      }
      // The hooks' CPU time per frame
      double HookMs() const
      {
         return double(hook_ns.load()) / 1e6 / frames;
      }
   };

   struct SweepDef
   {
      const char* name;
      std::span<const int> modes; // The last is the baseline
   };

   // Runs a "SweepDef"'s modes in turn, "windows" log windows each, for "rounds", keeping each window's "Columns" values per mode
   // (column 0: the frame time, the baseline's)
   template <size_t Columns>
   struct Sweep
   {
      using Row = std::array<double, Columns>;

      std::span<const SweepDef> defs;
      int rounds = 3;
      int windows = 1; // Per mode and round
      int running = 0; // "defs" index + 1 (0: none)
      int step = 0;    // Over all rounds
      int windows_done = 0;
      std::vector<std::vector<Row>> results; // By mode index

      std::span<const int> Modes() const
      {
         return defs[running - 1].modes;
      }
      int Total() const
      {
         return rounds * int(Modes().size());
      }
      // Returns the first mode, for the caller to apply
      int Start(int index)
      {
         running = index + 1;
         step = 0;
         windows_done = 0;
         results.assign(size_t(*std::max_element(Modes().begin(), Modes().end())) + 1, {});
         return Modes().front();
      }
      void Stop()
      {
         running = 0;
      }

      // A finished log window of "mode": while a sweep runs, applies its next mode; after the last, calls
      // "log_mode(mode, baseline_mode, baseline_frame_ms)" for each (the baseline is the last mode, column 0), then stops and applies
      // mode 0
      template <typename Apply, typename LogMode>
      void OnWindow(int mode, const Row& row, Apply&& apply, LogMode&& log_mode)
      {
         if (running == 0)
            return;
         // The mode changed without "Stop" (the window isn't the sweep's): the sweep's results no longer hold
         if (mode != Modes()[step % Modes().size()])
         {
            Stop();
            return;
         }
         results[mode].push_back(row);
         if (++windows_done < windows)
            return;
         windows_done = 0;
         if (++step < Total())
         {
            apply(Modes()[step % Modes().size()]);
            return;
         }
         const int baseline_mode = Modes().back();
         const double baseline = Median(baseline_mode, 0);
         for (const int sweep_mode : Modes())
            log_mode(sweep_mode, baseline_mode, baseline);
         Stop();
         apply(0);
      }

      double Median(int mode, size_t column) const
      {
         std::vector<double> values;
         for (const Row& row : results[mode])
            values.push_back(row[column]);
         return Perf::Median(std::move(values));
      }
      std::pair<double, double> MinMax(int mode, size_t column) const
      {
         const auto& rows = results[mode];
         if (rows.empty())
            return {0.0, 0.0};
         const auto [min_row, max_row] = std::minmax_element(rows.begin(), rows.end(), [column](const Row& a, const Row& b)
            { return a[column] < b[column]; });
         return {(*min_row)[column], (*max_row)[column]};
      }
   };

   struct AlwaysAvailable
   {
      bool operator()(const auto&) const
      {
         return true;
      }
   };

   // The "Performance Test" combo: "modes" (each with a "name") then the sweeps, greyed unless "is_available(mode)" (for all of a
   // sweep's). "apply(mode_index)" switches modes; a mode stops a running sweep, a sweep starts from its first mode.
   template <typename Mode, size_t ModeCount, size_t Columns, typename Apply, typename IsAvailable = AlwaysAvailable>
   void DrawCombo(const Mode (&modes)[ModeCount], Sweep<Columns>* sweep, Apply&& apply, IsAvailable&& is_available = {})
   {
      const std::string preview = (sweep->running != 0 ? std::format("{} ({}/{})", sweep->defs[sweep->running - 1].name, sweep->step + 1, sweep->Total()) : std::string(modes[g_test].name));
      if (!ImGui::BeginCombo("Performance Test", preview.c_str()))
         return;
      for (int i = 0; i < int(ModeCount); i++)
      {
         const bool selected = (sweep->running == 0 && g_test == i);
         ImGui::BeginDisabled(!is_available(modes[i]));
         if (ImGui::Selectable(modes[i].name, selected) && !selected)
         {
            sweep->Stop();
            apply(i);
         }
         ImGui::EndDisabled();
      }
      for (int i = 0; i < int(sweep->defs.size()); i++)
      {
         const std::span<const int> sweep_modes = sweep->defs[i].modes;
         ImGui::BeginDisabled(!std::all_of(sweep_modes.begin(), sweep_modes.end(), [&](int mode)
            { return is_available(modes[mode]); }));
         if (ImGui::Selectable(sweep->defs[i].name, sweep->running == i + 1) && sweep->running == 0)
         {
            apply(sweep->Start(i));
         }
         ImGui::EndDisabled();
      }
      ImGui::EndCombo();
   }
} // namespace Perf
