#pragma once

// Luma MCP backend (DEVELOPMENT only), included by "core.hpp" inside its anonymous namespace, after the dev globals it drives.
// A named pipe ("\\.\pipe\luma-mcp-<pid>") serves any number of clients, usually "Scripts/luma_mcp.py" (the stdio MCP bridge, one
// per agent session). Their requests share one queue and the game's state (knobs, the last trace).
// Requests run serially on the render thread from "OnPresent" (before display composition), so they share the dev UI's
// trace, debug draw and constant buffer tracking state without new locks in the draw hot paths, and a debug draw copy
// requested here is consumed before it could be composed on screen.
// Wire format, both ways: little endian u32 byte length + UTF-8 payload.
// Request: the tool name line, then "key=value" lines. Response: one JSON object with an "ok" field.
namespace Mcp
{
   class JsonWriter
   {
   public:
      std::string out;

      JsonWriter& BeginObject()
      {
         Separate();
         out += '{';
         comma = false;
         return *this;
      }
      JsonWriter& EndObject()
      {
         out += '}';
         comma = true;
         return *this;
      }
      JsonWriter& BeginArray()
      {
         Separate();
         out += '[';
         comma = false;
         return *this;
      }
      JsonWriter& EndArray()
      {
         out += ']';
         comma = true;
         return *this;
      }
      JsonWriter& Key(std::string_view key)
      {
         Separate();
         WriteString(key);
         out += ':';
         comma = false;
         return *this;
      }
      JsonWriter& Value(std::string_view value)
      {
         Separate();
         WriteString(value);
         comma = true;
         return *this;
      }
      // Otherwise pointers would pick the bool overload
      JsonWriter& Value(const char* value)
      {
         return Value(std::string_view(value ? value : ""));
      }
      JsonWriter& Value(bool value)
      {
         Separate();
         out += value ? "true" : "false";
         comma = true;
         return *this;
      }
      JsonWriter& Null()
      {
         Separate();
         out += "null";
         comma = true;
         return *this;
      }
      JsonWriter& Value(double value)
      {
         Separate();
         // JSON has no NaN/Inf literals
         if (std::isfinite(value))
            std::format_to(std::back_inserter(out), "{}", value);
         else
            std::format_to(std::back_inserter(out), "\"{}\"", value);
         comma = true;
         return *this;
      }
      template <typename T>
         requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)
      JsonWriter& Value(T value)
      {
         Separate();
         out += std::to_string(value);
         comma = true;
         return *this;
      }
      template <typename T>
      JsonWriter& Field(std::string_view key, const T& value)
      {
         Key(key);
         return Value(value);
      }
      JsonWriter& Hash(std::string_view key, uint32_t hash)
      {
         return Field(key, Shader::Hash_NumToStr(hash, true));
      }
      JsonWriter& Format(std::string_view key, DXGI_FORMAT format)
      {
         return Field(key, GetFormatNameSafe(format));
      }
      // As UTF-8: "path::string()" uses the ANSI code page and throws on characters outside it
      JsonWriter& Path(std::string_view key, const std::filesystem::path& path)
      {
         const std::u8string utf8 = path.u8string();
         return Field(key, std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
      }

   private:
      bool comma = false;

      void Separate()
      {
         if (comma)
            out += ',';
      }
      void WriteString(std::string_view value)
      {
         out += '"';
         for (const char c : value)
         {
            switch (c)
            {
            case '"':
               out += "\\\"";
               break;
            case '\\':
               out += "\\\\";
               break;
            case '\n':
               out += "\\n";
               break;
            case '\r':
               out += "\\r";
               break;
            case '\t':
               out += "\\t";
               break;
            default:
               if (static_cast<unsigned char>(c) < 0x20)
                  out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
               else
                  out += c;
            }
         }
         out += '"';
      }
   };

#if DX11 // The D3D12 readbacks are "docs/DX12-Support-Plan.md" A4
   // The dev UI's debug draw pass selection, overwritten by a readback and restored once the job finishes
   struct DebugDrawSelection
   {
      uint64_t pipeline;
      uint32_t shader_hash;
      int32_t target_instance;
      std::thread::id target_thread;
      DebugDrawMode mode;
      int32_t view_index;
      bool freeze_inputs;
      bool disable_blends;
      bool replaced_pass;

      static DebugDrawSelection Current()
      {
         return {debug_draw_pipeline, debug_draw_shader_hash, debug_draw_pipeline_target_instance, debug_draw_pipeline_target_thread, debug_draw_mode, debug_draw_view_index, debug_draw_freeze_inputs, debug_draw_disable_blends, debug_draw_replaced_pass};
      }
      void Apply() const
      {
         debug_draw_pipeline = pipeline;
         debug_draw_shader_hash = shader_hash;
         debug_draw_pipeline_target_instance = target_instance;
         debug_draw_pipeline_target_thread = target_thread;
         debug_draw_mode = mode;
         debug_draw_view_index = view_index;
         debug_draw_freeze_inputs = freeze_inputs;
         debug_draw_disable_blends = disable_blends;
         debug_draw_replaced_pass = replaced_pass;
      }
   };

   // The dev UI's constant buffer tracking selection, same as above
   struct TrackBufferSelection
   {
      uint64_t pipeline;
      int32_t target_instance;
      int32_t index;

      static TrackBufferSelection Current()
      {
         return {track_buffer_pipeline, track_buffer_pipeline_target_instance, track_buffer_index};
      }
      void Apply() const
      {
         track_buffer_pipeline = pipeline;
         track_buffer_pipeline_target_instance = target_instance;
         track_buffer_index = index;
      }
   };
#endif // DX11

   struct Job
   {
      std::string tool;
      std::unordered_map<std::string, std::string> args;
      mutable std::unordered_set<std::string> read_args; // To report the arguments no tool read (typos, renamed keys)
      JsonWriter result;
      std::string error;
      HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      // Readbacks, written to disk by the pipe thread before it answers, so the render thread only pays for the copy
      std::vector<std::pair<std::filesystem::path, std::vector<char>>> files;

      // Multi frame state (under "s_mutex_active_job")
      DeviceData* device_data = nullptr;
      uint32_t phase = 0;
      uint32_t frames = 0; // Captures done, or frames waited for a pass
      // The pass a readback targets (see "ResolveTarget")
      uint64_t pipeline_handle = 0;
      uint32_t shader_hash = 0;
      const char* stage = "";
      int32_t instance = 0;
      std::thread::id thread;
      std::vector<std::string> series; // Per frame results of multi frame reads
      struct HashCounts
      {
         const char* stage;
         std::vector<uint32_t> per_frame;
      };
      std::unordered_map<uint32_t, HashCounts> hash_counts; // Draws per shader hash over consecutive captures
      struct PerfState
      {
#if DX11
         Perf::TimestampRing<2> timestamps;
#else
         // A timestamp per present on the present queue, resolved into "readback", read once "fence" passed it. Recorded on Luma's own
         // command list: ReShade only flushes its immediate one when its own API recorded into it.
         static constexpr size_t slots = 8;
         std::array<com_ptr<ID3D12CommandAllocator>, slots> allocators; // Reset once their slot was read
         com_ptr<ID3D12GraphicsCommandList> list;
         com_ptr<ID3D12QueryHeap> query_heap;
         com_ptr<ID3D12Resource> readback;
         com_ptr<ID3D12Fence> fence;
         uint64_t fence_value = 0;
         uint64_t frequency = 0;
         uint64_t presents = 0;
         std::array<uint64_t, slots> slot_fence_values = {}; // 0 for a free slot
         std::array<uint64_t, slots> slot_presents = {};
         std::optional<std::pair<uint64_t, uint64_t>> last_read; // The present and the ticks of the last read timestamp
#endif
         std::vector<double> cpu_frame_ms;
         std::vector<double> gpu_frame_ms;
         uint32_t disjoint = 0;
         std::chrono::steady_clock::time_point last_present;
      };
      std::unique_ptr<PerfState> perf;
#if DX11
      std::optional<DebugDrawSelection> saved_debug_draw;
      std::optional<TrackBufferSelection> saved_track_buffer;
#else
      std::optional<D3D12Readback::Target> readback_target; // Armed again for "frames" series and for copies whose command list was reset unexecuted
      std::string written_resource;                         // Copy, resolve and clear targets: the frame capture's hash of the resource they wrote
#endif

      Job()
      {
         result.BeginObject().Field("ok", true);
      }
      ~Job()
      {
         CloseHandle(done);
      }

      const std::string* Arg(const char* key) const
      {
         read_args.emplace(key);
         const auto it = args.find(key);
         return it != args.end() && !it->second.empty() ? &it->second : nullptr;
      }
      // D3D12: whether the job reads the back buffer after Luma's display composition ("composed"), which "OnPresent()" runs in its call after the composition
      bool ReadsComposedBackBuffer() const
      {
         if (tool != "read_resource")
            return false;
         const std::string* view = Arg("view");
         return view && *view == "swapchain" && BoolArg("composed", false);
      }
      int64_t IntArg(const char* key, int64_t default_value) const
      {
         const std::string* value = Arg(key);
         return value ? std::strtoll(value->c_str(), nullptr, 0) : default_value;
      }
      int64_t IntArg(const char* key, int64_t default_value, int64_t min, int64_t max) const
      {
         return std::clamp(IntArg(key, default_value), min, max);
      }
      bool BoolArg(const char* key, bool default_value) const
      {
         const std::string* value = Arg(key);
         return value ? (*value == "1" || *value == "true") : default_value;
      }
      // Accepts "0x1234ABCD" or "1234ABCD"
      std::optional<uint32_t> HashArg(const char* key) const
      {
         const std::string* value = Arg(key);
         if (!value)
            return std::nullopt;
         return uint32_t(std::strtoul(value->c_str(), nullptr, 16));
      }
   };

   std::mutex s_mutex_jobs;
   std::vector<std::shared_ptr<Job>> pending_jobs; // Guarded by "s_mutex_jobs"
   std::string active_tool;                        // Guarded by "s_mutex_jobs", the running job's, for the status and timeout replies
   std::atomic<bool> has_pending_jobs = false;     // Set under "s_mutex_jobs", read without it
   // Two swapchains can present on two threads, and "OnDestroyDevice()" ends a job on another one
   std::mutex s_mutex_active_job;
   std::shared_ptr<Job> active_job;            // Guarded by "s_mutex_active_job"
   std::atomic<bool> has_active_job = false;   // Set under "s_mutex_active_job", read without it
   std::thread server_thread;
   std::atomic<bool> server_stop = false;
   std::atomic<bool> server_running = false;
   std::atomic<int> connected_clients = 0; // Skips the per draw bookkeeping nobody would read
   // Written at every present, so "status" can answer from the pipe thread while the game doesn't present
   std::atomic<uint64_t> last_present_ms = 0; // GetTickCount64()
   std::atomic<uint32_t> last_frame_index = 0;
   std::wstring pipe_name;

   // Game dev values (toggles, tweakables, counters and textures) exposed to "luma_dev_values", registered by the game code.
   // Values are read and written on the render thread at present, where games usually swap their per frame counters.
   // Some are persisted user settings: writing them here bypasses the config, fine for A/B tests.
   enum class DevValueKind : uint8_t
   {
      Toggle,
      Float,
      Int,
      Counter, // Read only
      Texture, // Read with "luma_read_resource view=registered:<name>"
   };
   // Called with the validated value (on the render thread) instead of a plain write, for values whose change has side effects (a mirror
   // into the shader settings, a mode switch). Returns an error, or nothing.
   using DevValueSetter = std::function<std::string(DeviceData& device_data, double value)>;
   // Returns a texture, or a view of it, borrowed (null while it isn't allocated), see "MCP_GAME_TEXTURE"
#if DX11
   using DevTextureGetter = ID3D11DeviceChild* (*)(DeviceData & device_data);
#else
   // A D3D12 texture of Luma's, borrowed, with the state it rests in between its passes: what a read at present copies it from
   struct DevTexture
   {
      com_ptr<ID3D12Resource> resource; // A reference: the owner can release it once its lock is gone (the debug draw's retired copies)
      D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
   };
   using DevTextureGetter = DevTexture (*)(DeviceData& device_data);
#endif
   struct DevValue
   {
      std::string name;
      DevValueKind kind;
      void* value;
      const void* owner = nullptr; // Lets per device values be removed together (e.g. "&device_data")
      double min = -DBL_MAX;       // Range of floats and ints (games index arrays with the ints)
      double max = DBL_MAX;
      DevTextureGetter texture = nullptr;
      DevValueSetter set; // Optional
   };
   std::mutex s_mutex_dev_values;
   std::vector<DevValue> dev_values; // Guarded by "s_mutex_dev_values"

   // Needs "s_mutex_dev_values"
   DevValue* FindDevValue(std::string_view name)
   {
      const auto it = std::find_if(dev_values.begin(), dev_values.end(), [&](const DevValue& dev_value)
         { return dev_value.name == name; });
      return it != dev_values.end() ? &*it : nullptr;
   }
   void RegisterDevValue(DevValue dev_value)
   {
      const std::lock_guard lock(s_mutex_dev_values);
      std::erase_if(dev_values, [&](const DevValue& other)
         { return other.name == dev_value.name; });
      dev_values.push_back(std::move(dev_value));
   }
   void RegisterToggles(std::initializer_list<std::pair<const char*, bool*>> values)
   {
      for (const auto& [name, value] : values)
         RegisterDevValue({name, DevValueKind::Toggle, value});
   }
   // A toggle the shaders read as a 0/1 float from the settings buffer too (e.g. "&cb_luma_global_settings.GameSettings.LumaBloomEnable")
   void RegisterMirroredToggle(const char* name, bool* value, float* mirror)
   {
      RegisterDevValue({name, DevValueKind::Toggle, value, nullptr, -DBL_MAX, DBL_MAX, nullptr, [value, mirror](DeviceData&, double new_value)
         {
            *value = new_value != 0.0;
            *mirror = *value ? 1.f : 0.f;
            return std::string(); }});
   }
   struct FloatValue
   {
      const char* name;
      float* value;
      float min = -FLT_MAX;
      float max = FLT_MAX;
   };
   void RegisterValues(std::initializer_list<FloatValue> values)
   {
      for (const auto& [name, value, min, max] : values)
         RegisterDevValue({name, DevValueKind::Float, value, nullptr, min, max});
   }
   struct IntValue
   {
      const char* name;
      int* value;
      int min;
      int max;
      DevValueSetter set = {};
   };
   void RegisterInts(std::initializer_list<IntValue> values)
   {
      for (const auto& [name, value, min, max, set] : values)
         RegisterDevValue({name, DevValueKind::Int, value, nullptr, double(min), double(max), nullptr, set});
   }
   // Prefer the per frame copy of a counter (the value from the last finished frame) over the one being incremented
   void RegisterCounter(std::string name, const uint32_t* value, const void* owner = nullptr)
   {
      RegisterDevValue({std::move(name), DevValueKind::Counter, const_cast<uint32_t*>(value), owner}); // Never written
   }
   void RegisterCounters(std::initializer_list<std::pair<const char*, const uint32_t*>> values, const void* owner = nullptr)
   {
      for (const auto& [name, value] : values)
         RegisterCounter(name, value, owner);
   }
   // Luma's own textures: its passes run natively, outside the ReShade events the pass readback hooks. They are read at present, so they
   // hold their last content (usually this frame's).
   void RegisterTextures(std::initializer_list<std::pair<const char*, DevTextureGetter>> textures)
   {
      for (const auto& [name, get] : textures)
         RegisterDevValue({name, DevValueKind::Texture, nullptr, nullptr, -DBL_MAX, DBL_MAX, get});
   }
// A "RegisterTextures" entry for a member of the game's device data (a texture or a view of it), from where "GetGameDeviceData" is visible
#define MCP_GAME_TEXTURE(name, member) {name, [](DeviceData& device_data) -> ID3D11DeviceChild* { return GetGameDeviceData(device_data).member.get(); }}
#if DX11
   com_ptr<ID3D11Resource> GetTexture(const DevValue& dev_value, DeviceData& device_data)
   {
      com_ptr<ID3D11Resource> resource;
      ID3D11DeviceChild* child = dev_value.texture(device_data);
      if (child && FAILED(child->QueryInterface(&resource)))
      {
         com_ptr<ID3D11View> view;
         if (SUCCEEDED(child->QueryInterface(&view)))
            view->GetResource(&resource);
      }
      return resource;
   }
#endif // DX11
   void Unregister(const void* owner)
   {
      const std::lock_guard lock(s_mutex_dev_values);
      std::erase_if(dev_values, [&](const DevValue& dev_value)
         { return dev_value.owner == owner; });
   }

   // Attaches a note to the draw being processed in the current capture (e.g. the game's motion vector decision), shown by "luma_trace_list".
   // Call it from "Game::OnDrawOrDispatch", where the shader entries of the current draw are the last ones of "cmd_list_data".
   // "value" is appended as "text=value" (formatted only while capturing, so it can be called on every draw).
   void Annotate(CommandListData& cmd_list_data, std::string_view text, std::optional<int64_t> value = std::nullopt)
   {
      if (!trace_running) // Cheap early out for the hot path, rechecked under the lock
         return;
      const std::shared_lock lock_trace(s_mutex_trace);
      if (!trace_running)
         return;
      const std::unique_lock lock_trace_2(cmd_list_data.mutex_trace);
      // Games can push their own custom entries after the shader ones
      for (auto it = cmd_list_data.trace_draw_calls_data.rbegin(); it != cmd_list_data.trace_draw_calls_data.rend(); ++it)
      {
         if (it->type != TraceDrawCallData::TraceDrawCallType::Shader)
            continue;
         if (!it->note.empty())
            it->note += ' ';
         it->note += text;
         if (value)
            std::format_to(std::back_inserter(it->note), "={}", *value);
         return;
      }
   }

   constexpr uint32_t readback_max_frames = 8; // A pass not hit within these frames is reported as missing

   template <size_t N>
   const char* EnumName(const char* const (&names)[N], size_t value)
   {
      return value < N && names[value][0] != '\0' ? names[value] : "invalid";
   }

   // In "TraceDrawCallData::TraceDrawCallType" and "TraceDrawCallData::CopyKind" order
   constexpr const char* trace_type_names[] = {"shader", "copy", "clear", "bind_pipeline", "bind_resource", "cpu_read", "cpu_write", "present", "create_command_list", "append_command_list", "reset_command_list", "flush_command_list", "custom", "begin_render_pass", "end_render_pass"};
   constexpr const char* copy_kind_names[] = {"resource", "region", "resolve", "buffer_region", "buffer_to_texture", "texture_to_buffer"};
   // In "TraceDrawCallData::ClearKind" order (D3D12)
   constexpr const char* clear_kind_names[] = {"render_target", "depth_stencil", "unordered_access"};

   const Shader::CachedPipeline* FindPipeline(const DeviceData& device_data, uint64_t pipeline_handle)
   {
      const auto it = device_data.pipeline_cache_by_pipeline_handle.find(pipeline_handle);
      return it != device_data.pipeline_cache_by_pipeline_handle.end() ? it->second : nullptr;
   }

   // In "Shader::CachedPipeline::Replacement" order
   constexpr const char* replacement_names[] = {"none", "file", "patch_inplace", "patch_sync", "patch_async"};
   // The stage of "shader_hash" in "pipeline" (a D3D12 PSO has several)
   const char* StageName(const Shader::CachedPipeline& pipeline, uint32_t shader_hash)
   {
#if DX12
      if (const auto it = std::ranges::find(pipeline.shader_hashes, shader_hash); it != pipeline.shader_hashes.end())
         return Shader::CachedPipeline::StageName(pipeline.shader_types[it - pipeline.shader_hashes.begin()]);
#endif
      return pipeline.StageName();
   }

   const char* ReplacementName(const Shader::CachedPipeline& pipeline)
   {
      return replacement_names[size_t(pipeline.GetReplacement())];
   }
   bool IsReplaced(const Shader::CachedPipeline& pipeline)
   {
      return pipeline.GetReplacement() != Shader::CachedPipeline::Replacement::None;
   }

   // The custom shader file of a shader, null if it has none (the cache keeps an empty entry for a file that was deleted). Needs "s_mutex_loading" (shared)
   const Shader::CachedCustomShader* FindCustomShader(uint32_t shader_hash)
   {
      const auto it = custom_shaders_cache.find(shader_hash);
      return (it != custom_shaders_cache.end() && it->second != nullptr && !it->second->file_path.empty()) ? it->second : nullptr;
   }

   // The fields of a custom shader object
   void WriteCustomShaderFields(JsonWriter* w, const Shader::CachedCustomShader& custom_shader, bool with_disasm)
   {
      w->Path("file", custom_shader.file_path);
      w->Field("is_hlsl", custom_shader.is_hlsl);
      w->Field("is_luma_native", custom_shader.is_luma_native);
      w->Field("compilation_failed", custom_shader.compilation_error);
      w->Field("compilation_errors", custom_shader.compilation_errors);
      if (with_disasm)
         w->Field("disasm", custom_shader.disasm);
   }

   void WriteCustomShader(JsonWriter* w, const Shader::CachedCustomShader& custom_shader, bool with_disasm)
   {
      w->Key("custom_shader").BeginObject();
      WriteCustomShaderFields(w, custom_shader, with_disasm);
      w->EndObject();
   }

   bool IsDeferred(const TraceDrawCallData& entry)
   {
#if DX11
      return entry.command_list && entry.command_list->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
#else
      return false; // D3D12 entries are already in submission order
#endif
   }

   // Needs "s_mutex_generic" and "s_mutex_loading" (shared)
   void WriteTraceEntrySummary(JsonWriter* w, const DeviceData& device_data, const TraceDrawCallData& entry, size_t index)
   {
      w->Field("index", index);
      w->Field("type", EnumName(trace_type_names, size_t(entry.type)));
      if (IsDeferred(entry))
         w->Field("deferred", true);
      if (entry.type == TraceDrawCallData::TraceDrawCallType::Custom)
         w->Field("name", entry.custom_name);
      if (entry.predicted)
         w->Field("predicted", true);
      if (entry.type == TraceDrawCallData::TraceDrawCallType::Shader)
      {
         if (const auto* pipeline = FindPipeline(device_data, entry.pipeline_handle))
         {
            w->Field("stage", pipeline->StageName());
            w->Hash("hash", pipeline->shader_hashes[0]);
#if DX12 // One entry per draw: every stage of the PSO
            w->Key("shaders").BeginObject();
            for (size_t i = 0; i < pipeline->shader_hashes.size(); i++)
               w->Hash(Shader::CachedPipeline::StageName(pipeline->shader_types[i]), pipeline->shader_hashes[i]);
            w->EndObject();
#endif
            w->Field("replaced", ReplacementName(*pipeline));
#if DX12 // The shaders of a replaced pipeline that are custom ones (the others of its stages are the game's)
            if (IsReplaced(*pipeline))
            {
               w->Key("replaced_shaders").BeginObject();
               for (const uint32_t shader_hash : pipeline->shader_hashes)
               {
                  if (const auto* custom_shader = FindCustomShader(shader_hash); custom_shader && !custom_shader->code.empty())
                     w->Hash(StageName(*pipeline, shader_hash), shader_hash);
               }
               w->EndObject();
            }
#endif
            if (!pipeline->custom_name.empty() && pipeline->custom_name[0] != '\0')
               w->Field("name", pipeline->custom_name.c_str());
            // Any stage of a D3D12 pipeline can have the custom shader
            std::optional<bool> compilation_failed;
            for (const uint32_t shader_hash : pipeline->shader_hashes)
            {
               if (const auto* custom_shader = FindCustomShader(shader_hash); custom_shader && !custom_shader->compilation_errors.empty())
                  compilation_failed = compilation_failed.value_or(false) || custom_shader->compilation_error;
            }
            if (compilation_failed)
               w->Field("compilation_failed", *compilation_failed);
         }
         else
         {
            w->Field("stage", "unknown"); // Destroyed since the capture
         }
         if (entry.clone_variant != Shader::ShaderVariant::Original)
            w->Field("variant", "patched");
      }
      std::string flags;
      if (entry.any_input_resources_format_upgraded)
         flags += '^';
      if (entry.any_input_resources_scaled)
         flags += '\\';
      if (entry.any_output_resources_scaled)
         flags += '/';
      if (entry.any_output_resources_format_upgraded)
         flags += 'v';
      if (!flags.empty())
         w->Field("upgrade_flags", flags);
      if (entry.IsRTVValid(0))
         w->Field("rt0", std::format("{} {}x{}", GetFormatNameSafe(entry.rtv_format[0]), entry.rtv_size[0].x, entry.rtv_size[0].y));
      else if (entry.type != TraceDrawCallData::TraceDrawCallType::Shader && !entry.rt_hash[0].empty()) // A whole resource target, see "WriteTraceEntryDetails"
         w->Field("rt0", std::format("{} {}x{}", GetFormatNameSafe(entry.rt_format[0]), entry.rt_size[0].x, entry.rt_size[0].y));
      if (entry.IsDSVValid() && entry.depth_state != TraceDrawCallData::DepthStateType::Disabled)
         w->Field("depth", EnumName(TraceDrawCallData::depth_state_names, size_t(entry.depth_state)));
      if (entry.type == TraceDrawCallData::TraceDrawCallType::CopyResource && entry.copy_info.kind != TraceDrawCallData::CopyKind::Resource)
         w->Field("copy_kind", EnumName(copy_kind_names, size_t(entry.copy_info.kind)));
      if (!entry.note.empty())
         w->Field("note", entry.note);
   }

   constexpr const char* comparison_func_names[] = {"", "NEVER", "LESS", "EQUAL", "LESS_EQUAL", "GREATER", "NOT_EQUAL", "GREATER_EQUAL", "ALWAYS"};
   constexpr const char* stencil_op_names[] = {"", "KEEP", "ZERO", "REPLACE", "INCR_SAT", "DECR_SAT", "INVERT", "INCR", "DECR"};
   constexpr const char* cull_mode_names[] = {"", "NONE", "FRONT", "BACK"};
   constexpr const char* fill_mode_names[] = {"", "", "WIREFRAME", "SOLID"};

   void WriteStencilFace(JsonWriter* w, const char* key, const D3D11_DEPTH_STENCILOP_DESC& face)
   {
      w->Key(key).BeginObject();
      w->Field("func", EnumName(comparison_func_names, face.StencilFunc)).Field("fail", EnumName(stencil_op_names, face.StencilFailOp));
      w->Field("depth_fail", EnumName(stencil_op_names, face.StencilDepthFailOp)).Field("pass", EnumName(stencil_op_names, face.StencilPassOp));
      w->EndObject();
   }

   // Pixel shader entries only, the trace doesn't record this state for other stages
   void WriteRenderState(JsonWriter* w, const TraceDrawCallData& entry)
   {
#if DX11
      const D3D11_DEPTH_STENCIL_DESC depth_stencil = entry.has_depth_stencil_state ? entry.depth_stencil_desc : CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
#else
      const D3D11_DEPTH_STENCIL_DESC& depth_stencil = entry.depth_stencil_desc; // The D3D12 capture fills the defaults of a pipeline without one
#endif
      w->Key("depth_stencil").BeginObject();
      w->Field("state_bound", entry.has_depth_stencil_state).Field("depth_enable", bool(depth_stencil.DepthEnable));
      w->Field("depth_write", depth_stencil.DepthWriteMask != 0).Field("depth_func", EnumName(comparison_func_names, depth_stencil.DepthFunc));
      w->Field("stencil_enable", bool(depth_stencil.StencilEnable));
      if (depth_stencil.StencilEnable)
      {
         w->Field("stencil_ref", entry.stencil_ref);
         w->Field("read_mask", uint32_t(depth_stencil.StencilReadMask)).Field("write_mask", uint32_t(depth_stencil.StencilWriteMask));
         WriteStencilFace(w, "front", depth_stencil.FrontFace);
         WriteStencilFace(w, "back", depth_stencil.BackFace);
      }
      w->EndObject();

#if DX11
      const D3D11_RASTERIZER_DESC rasterizer = entry.has_rasterizer_state ? entry.rasterizer_desc : CD3D11_RASTERIZER_DESC(D3D11_DEFAULT);
      const bool scissor_enable = rasterizer.ScissorEnable;
#else
      const D3D11_RASTERIZER_DESC& rasterizer = entry.rasterizer_desc;
      const bool scissor_enable = entry.scissor_enable;
#endif
      w->Key("rasterizer").BeginObject();
      w->Field("state_bound", entry.has_rasterizer_state).Field("cull", EnumName(cull_mode_names, rasterizer.CullMode)).Field("fill", EnumName(fill_mode_names, rasterizer.FillMode));
      w->Field("front_ccw", bool(rasterizer.FrontCounterClockwise)).Field("depth_bias", rasterizer.DepthBias).Field("slope_scaled_depth_bias", rasterizer.SlopeScaledDepthBias);
      w->Field("depth_clip", bool(rasterizer.DepthClipEnable)).Field("scissor_enable", scissor_enable).Field("multisample", bool(rasterizer.MultisampleEnable));
      w->EndObject();

#if DX11 || ENABLE_D3D12_PASSES // D3D12 tracks the viewports and scissors with the passes' state only
      w->Field("viewport_count", entry.viewport_count).Key("viewports").BeginArray();
      for (UINT i = 0; i < entry.viewport_count; i++)
      {
         const auto& viewport = entry.viewports[i];
         w->BeginArray().Value(viewport.TopLeftX).Value(viewport.TopLeftY).Value(viewport.Width).Value(viewport.Height).Value(viewport.MinDepth).Value(viewport.MaxDepth).EndArray();
      }
      w->EndArray();
      w->Field("scissor_count", entry.scissor_count).Key("scissor_rects").BeginArray();
      for (UINT i = 0; i < entry.scissor_count; i++)
      {
         const auto& rect = entry.scissor_rects[i];
         w->BeginArray().Value(rect.left).Value(rect.top).Value(rect.right).Value(rect.bottom).EndArray();
      }
      w->EndArray();
#endif
   }

   void WriteViews(JsonWriter* w, const char* key, size_t count, auto is_valid, auto write)
   {
      w->Key(key).BeginArray();
      for (size_t i = 0; i < count; i++)
      {
         if (!is_valid(i))
            continue;
         w->BeginObject().Field("slot", i);
         write(i);
         w->EndObject();
      }
      w->EndArray();
   }

   // Needs "s_mutex_generic" and "s_mutex_loading" (shared)
   void WriteTraceEntryDetails(JsonWriter* w, const DeviceData& device_data, const TraceDrawCallData& entry, size_t index)
   {
      WriteTraceEntrySummary(w, device_data, entry, index);
      w->Field("thread", entry.thread_id._Get_underlying_id());
      w->Field("patch_enabled", entry.patch_enabled);

      const auto* pipeline = entry.type == TraceDrawCallData::TraceDrawCallType::Shader ? FindPipeline(device_data, entry.pipeline_handle) : nullptr;
      if (pipeline)
      {
         w->Field("replace_draw_type", Shader::CachedPipeline::shader_replace_draw_type_names[size_t(pipeline->replace_draw_type)]);
#if DX12 // What the game created the PSO with: a draw into an upgraded target runs a variant with other formats ("rtvs" has the formats of the bound targets)
         w->Key("pipeline_rt_formats").BeginArray();
         for (uint32_t i = 0; i < pipeline->render_target_count; i++)
            w->Value(GetFormatNameSafe(pipeline->render_target_formats[i]));
         w->EndArray();
#endif
#if DX12 // A file per stage
         w->Key("custom_shaders").BeginArray();
         for (const uint32_t shader_hash : pipeline->shader_hashes)
         {
            if (const auto* custom_shader = FindCustomShader(shader_hash))
            {
               w->BeginObject().Field("stage", StageName(*pipeline, shader_hash)).Hash("hash", shader_hash);
               WriteCustomShaderFields(w, *custom_shader, false);
               w->EndObject();
            }
         }
         w->EndArray();
#else
         if (const auto* custom_shader = FindCustomShader(pipeline->shader_hashes[0]))
            WriteCustomShader(w, *custom_shader, false);
#endif
      }

      const auto& dd = entry.draw_dispatch_data;
      w->Key("draw").BeginObject();
      w->Field("vertex_count", dd.vertex_count).Field("instance_count", dd.instance_count).Field("first_vertex", dd.first_vertex).Field("first_instance", dd.first_instance);
      w->Field("index_count", dd.index_count).Field("first_index", dd.first_index).Field("vertex_offset", dd.vertex_offset).Field("indexed", dd.indexed);
      w->Key("dispatch").BeginArray().Value(dd.dispatch_count.x).Value(dd.dispatch_count.y).Value(dd.dispatch_count.z).EndArray();
      w->Field("indirect", dd.indirect);
      w->Field("topology", uint32_t(entry.primitive_topology));
      w->EndObject();
#if DX12
      // ExecuteIndirect(): the signature's arguments, the root ones by register (their bindings are each command's); the values with luma_read_cbuffer indirect
      if (dd.indirect && entry.indirect_buffer != 0)
      {
         w->Key("indirect_command").BeginObject().Field("argument_offset", entry.indirect_offset).Field("max_count", entry.indirect_max_count).Field("count_buffer", entry.indirect_count_buffer);
         w->Field("signature_known", entry.indirect_signature_known);
         if (entry.indirect_signature_known)
         {
            w->Field("stride", entry.indirect_stride).Key("arguments").BeginArray();
            for (const TraceDrawCallData::IndirectArgument& argument : entry.indirect_arguments)
            {
               w->BeginObject().Field("type", D3D12CommandSignatures::ArgumentName(argument.type));
               if (argument.root_parameter != UINT32_MAX)
                  w->Field("root_parameter", argument.root_parameter);
               if (argument.register_type != 0)
                  w->Field("register", std::format("{}{}", argument.register_type, argument.shader_register.index)).Field("space", argument.shader_register.space);
               if (argument.type == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT)
                  w->Field("constant_offset", argument.constant_offset).Field("constant_count", argument.constant_count);
               if (argument.type == D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW)
                  w->Field("slot", argument.slot);
               w->EndObject();
            }
            w->EndArray();
         }
         w->EndObject();
      }
#endif

#if DX11 || ENABLE_D3D12_PASSES
      const D3D11_VIEWPORT& viewport_0 = entry.viewports[0]; // Zeroed if none
      w->Key("viewport0").BeginArray().Value(viewport_0.TopLeftX).Value(viewport_0.TopLeftY).Value(viewport_0.Width).Value(viewport_0.Height).EndArray();
      w->Field("scissors", entry.scissor_count >= 1);
#endif
      w->Field("depth_state", EnumName(TraceDrawCallData::depth_state_names, size_t(entry.depth_state)));
      w->Field("stencil_state", EnumName(TraceDrawCallData::depth_state_names, size_t(entry.stencil_state)));
      if (pipeline && pipeline->HasPixelShader())
         WriteRenderState(w, entry);
#if DX12
      if (entry.uses_descriptor_heap)
         w->Field("uses_descriptor_heap", true).Field("descriptor_heap_note", "Indexes the SM 6.6 descriptor heaps: those resources aren't listed, their indices are usually in the root constants");
      if (entry.truncated)
         w->Field("truncated", true).Field("truncated_note", "Bindless ranges and bindings past the per type limits of an entry (SRV 128, UAV 64, CBV 32) aren't listed");
#endif
      if (entry.type == TraceDrawCallData::TraceDrawCallType::CopyResource)
      {
         const auto& copy = entry.copy_info;
         w->Key("copy").BeginObject().Field("kind", EnumName(copy_kind_names, size_t(copy.kind))).Field("source_subresource", copy.source_subresource).Field("dest_subresource", copy.dest_subresource);
         w->Key("dest_offset").BeginArray().Value(copy.dest_offset.x).Value(copy.dest_offset.y).Value(copy.dest_offset.z).EndArray();
         if (copy.has_box)
            w->Key("source_box").BeginArray().Value(copy.box.left).Value(copy.box.top).Value(copy.box.front).Value(copy.box.right).Value(copy.box.bottom).Value(copy.box.back).EndArray();
         if (copy.kind == TraceDrawCallData::CopyKind::Resolve)
            w->Format("resolve_format", copy.resolve_format);
         w->EndObject();
      }

      const auto write_resource = [&](DXGI_FORMAT resource_format, DXGI_FORMAT view_format, const uint4& size, const uint3& view_size, UINT mip, const std::string& type, const std::string& hash, const std::string& debug_name)
      {
         w->Format("format", resource_format).Format("view_format", view_format);
         w->Key("size").BeginArray().Value(size.x).Value(size.y).Value(size.z).Value(size.w).EndArray();
         w->Key("view_size").BeginArray().Value(view_size.x).Value(view_size.y).Value(view_size.z).EndArray();
         w->Field("mip", mip).Field("type", type).Field("resource", hash);
         if (!debug_name.empty())
            w->Field("debug_name", debug_name);
      };

#if DX12
      // The SRV, UAV and constant buffer entries are packed in binding order (the first "*_count"), with their register. "bound" false
      // is a descriptor the shader references, null or never written.
      const auto write_register = [w](char type, const TraceDrawCallData::ShaderRegister& shader_register)
      {
         w->Field("register", std::format("{}{}", type, shader_register.index)).Field("space", shader_register.space);
      };
#endif

      // Entries without a shader (copies, clears, CPU writes, Luma's passes) record whole resources instead of views: the target in
      // "rt_*[0]" (copy destination) and the source in "sr_*[0]" (copy source, bound buffer)
      if (entry.type != TraceDrawCallData::TraceDrawCallType::Shader)
      {
         const auto write_whole_resource = [&](const char* key, DXGI_FORMAT format, const uint4& size, const std::string& type, const std::string& hash)
         {
            if (hash.empty())
               return;
            w->Key(key).BeginObject().Format("format", format);
            w->Key("size").BeginArray().Value(size.x).Value(size.y).Value(size.z).Value(size.w).EndArray();
            w->Field("type", type).Field("resource", hash).EndObject();
         };
         write_whole_resource("source", entry.sr_format[0], entry.sr_size[0], entry.sr_type_name[0], entry.sr_hash[0]);
         write_whole_resource("target", entry.rt_format[0], entry.rt_size[0], entry.rt_type_name[0], entry.rt_hash[0]);
      }

      WriteViews(w, "rtvs", TraceDrawCallData::rtvs_size, [&](size_t i)
         { return entry.IsRTVValid(i); }, [&](size_t i)
         {
         write_resource(entry.rt_format[i], entry.rtv_format[i], entry.rt_size[i], entry.rtv_size[i], entry.rtv_mip[i], entry.rt_type_name[i], entry.rt_hash[i], entry.rt_debug_name[i]);
         if (entry.rt_is_swapchain[i]) w->Field("swapchain", true);
         const auto& blend = entry.blend_desc.RenderTarget[entry.blend_desc.IndependentBlendEnable ? i : 0];
         w->Field("write_mask", uint32_t(blend.RenderTargetWriteMask));
         if (blend.BlendEnable)
         {
            w->Field("blend", std::format("{} {} {} / alpha {} {} {}", GetBlendName(blend.SrcBlend), GetBlendOpName(blend.BlendOp), GetBlendName(blend.DestBlend), GetBlendName(blend.SrcBlendAlpha), GetBlendOpName(blend.BlendOpAlpha), GetBlendName(blend.DestBlendAlpha)));
         }
         });
      w->Field("alpha_to_coverage", bool(entry.blend_desc.AlphaToCoverageEnable));
#if DX12
      if (entry.type == TraceDrawCallData::TraceDrawCallType::ClearResource)
         w->Field("clear_kind", EnumName(clear_kind_names, size_t(entry.clear_kind)));
#endif
      // Clear entries store the clear values there (depth and stencil in the first two for depth clears)
      w->Key(entry.type == TraceDrawCallData::TraceDrawCallType::ClearResource ? "clear_values" : "blend_factor").BeginArray().Value(entry.blend_factor[0]).Value(entry.blend_factor[1]).Value(entry.blend_factor[2]).Value(entry.blend_factor[3]).EndArray();

      WriteViews(w, "srvs", TraceDrawCallData::srvs_size, [&](size_t i)
         {
#if DX12
         return i < entry.srv_count;
#else
         return entry.IsSRVValid(i);
#endif
         }, [&](size_t i)
         {
#if DX12
         write_register('t', entry.sr_register[i]);
         if (entry.srv_format[i] == DXGI_FORMAT(-1))
         {
            w->Field("bound", false);
            return;
         }
#endif
         write_resource(entry.sr_format[i], entry.srv_format[i], entry.sr_size[i], entry.srv_size[i], entry.srv_mip[i], entry.sr_type_name[i], entry.sr_hash[i], entry.sr_debug_name[i]);
         if (entry.sr_is_rt[i]) w->Field("is_rt", true);
         if (entry.sr_is_ua[i]) w->Field("is_ua", true); });
      WriteViews(w, "uavs", TraceDrawCallData::uavs_size, [&](size_t i)
         {
#if DX12
         return i < entry.uav_count;
#else
         return entry.IsUAVValid(i);
#endif
         }, [&](size_t i)
         {
#if DX12
         write_register('u', entry.ua_register[i]);
         if (entry.uav_format[i] == DXGI_FORMAT(-1))
         {
            w->Field("bound", false);
            return;
         }
#endif
         write_resource(entry.ua_format[i], entry.uav_format[i], entry.ua_size[i], entry.uav_size[i], entry.uav_mip[i], entry.ua_type_name[i], entry.ua_hash[i], entry.ua_debug_name[i]);
         if (entry.ua_is_rt[i]) w->Field("is_rt", true); });
      if (entry.IsDSVValid())
      {
         w->Key("dsv").BeginObject().Format("format", entry.ds_format).Format("view_format", entry.dsv_format);
         w->Key("size").BeginArray().Value(entry.ds_size.x).Value(entry.ds_size.y).EndArray();
         w->Field("resource", entry.ds_hash);
         if (!entry.ds_debug_name.empty())
            w->Field("debug_name", entry.ds_debug_name);
         w->EndObject();
      }
      WriteViews(w, "cbs", TraceDrawCallData::cbs_size, [&](size_t i)
         { return entry.cbs[i]; }, [&](size_t i)
         {
#if DX12 // Root constants have no buffer, a CBV is a range of any buffer (root CBVs have no size)
         write_register('b', entry.cb_register[i]);
         if (const std::vector<uint32_t>& root_constants = entry.cb_root_constants[i]; !root_constants.empty())
         {
            w->Key("root_constants").BeginArray();
            for (const uint32_t value : root_constants)
               w->Value(value);
            w->EndArray().Key("root_constants_float").BeginArray();
            for (const uint32_t value : root_constants)
               w->Value(double(std::bit_cast<float>(value)));
            w->EndArray();
            return;
         }
         if (entry.cb_hash[i].empty())
         {
            w->Field("bound", false);
            return;
         }
         w->Field("resource", entry.cb_hash[i]).Field("offset_bytes", entry.cb_first_constant[i] * 16);
         if (entry.cb_num_constants[i] != 0) w->Field("size_bytes", entry.cb_num_constants[i] * 16);
#else
         w->Field("resource", entry.cb_hash[i]);
         if (entry.cb_num_constants[i] != 0) w->Field("first_constant", entry.cb_first_constant[i]).Field("num_constants", entry.cb_num_constants[i]);
#endif
         });
      WriteViews(w, "samplers", TraceDrawCallData::samplers_size, [&](size_t i)
         {
#if DX12
         return i < entry.sampler_count;
#else
         return entry.samplers_filter[i] != static_cast<D3D11_FILTER>(-1);
#endif
         }, [&](size_t i)
         {
#if DX12 // A sampler descriptor the shader references, never written or created before the capture
         write_register('s', entry.sa_register[i]);
         if (entry.samplers_filter[i] == static_cast<D3D11_FILTER>(-1))
         {
            w->Field("bound", false);
            return;
         }
#endif
         w->Field("filter", GetFilterName(entry.samplers_filter[i])).Field("filter_id", uint32_t(entry.samplers_filter[i]));
         w->Field("address", std::format("{} {} {}", GetTextureAddressModeName(entry.samplers_address_u[i]), GetTextureAddressModeName(entry.samplers_address_v[i]), GetTextureAddressModeName(entry.samplers_address_w[i])));
         w->Key("address_ids").BeginArray().Value(uint32_t(entry.samplers_address_u[i])).Value(uint32_t(entry.samplers_address_v[i])).Value(uint32_t(entry.samplers_address_w[i])).EndArray();
#if DX12
         if (((entry.samplers_filter[i] >> D3D12_FILTER_REDUCTION_TYPE_SHIFT) & D3D12_FILTER_REDUCTION_TYPE_MASK) == D3D12_FILTER_REDUCTION_TYPE_COMPARISON)
            w->Field("comparison", EnumName(comparison_func_names, entry.samplers_comparison_func[i]));
#endif
         w->Field("mip_lod_bias", entry.samplers_mip_lod_bias[i]); });

      if (!entry.input_layout_hash.empty())
      {
         w->Field("input_layout", entry.input_layout_hash);
         w->Key("input_formats").BeginArray();
         for (const DXGI_FORMAT format : entry.input_layouts_formats)
            w->Value(GetFormatNameSafe(format));
         w->EndArray();
      }
      if (!entry.vertex_buffer_hashes.empty())
      {
         w->Key("vertex_buffers").BeginArray();
         for (const std::string& hash : entry.vertex_buffer_hashes)
            w->Value(hash);
         w->EndArray();
      }
      if (!entry.index_buffer_hash.empty())
         w->Field("index_buffer", entry.index_buffer_hash).Format("index_format", entry.index_buffer_format).Field("index_offset", entry.index_buffer_offset);
   }

   // "luma_trace_list" filters, parsed once per request (a capture can have tens of thousands of entries).
   // The defaults hide what the dev UI hides by default: VS entries, bindings and buffer writes.
   struct TraceFilter
   {
      bool include_vs;
      bool include_bindings;
      bool include_buffer_writes;
      bool replaced_only;
      const std::string* type;
      std::optional<uint32_t> hash;
      const std::string* stage;
      const std::string* resource;

      explicit TraceFilter(const Job& job)
          : include_vs(job.BoolArg("include_vs", false)), include_bindings(job.BoolArg("include_bindings", false)), include_buffer_writes(job.BoolArg("include_buffer_writes", false)), replaced_only(job.BoolArg("replaced_only", false)), type(job.Arg("type")), hash(job.HashArg("hash")), stage(job.Arg("stage")), resource(job.Arg("resource"))
      {
      }

      bool Matches(const DeviceData& device_data, const TraceDrawCallData& entry) const
      {
         using Type = TraceDrawCallData::TraceDrawCallType;
         const Shader::CachedPipeline* pipeline = entry.type == Type::Shader ? FindPipeline(device_data, entry.pipeline_handle) : nullptr;
#if DX11 // D3D12 entries are per draw, with their VS
         if (pipeline && pipeline->HasVertexShader() && !include_vs)
            return false;
#endif
         if (((entry.type == Type::BindPipeline || entry.type == Type::BindResource) && !include_bindings) || (entry.type == Type::CPUWrite && entry.rt_type_name[0] == "Buffer" && !include_buffer_writes))
            return false;
         // Any stage of a D3D12 PSO
         const auto has_stage = [&]
         {
#if DX12
            return std::ranges::any_of(pipeline->shader_types, [&](reshade::api::pipeline_subobject_type shader_type)
               { return _stricmp(stage->c_str(), Shader::CachedPipeline::StageName(shader_type)) == 0; });
#else
            return _stricmp(stage->c_str(), pipeline->StageName()) == 0;
#endif
         };
         if ((type && *type != EnumName(trace_type_names, size_t(entry.type))) || (hash && (!pipeline || std::ranges::find(pipeline->shader_hashes, *hash) == pipeline->shader_hashes.end())) || (stage && (!pipeline || !has_stage())) || (replaced_only && (!pipeline || !IsReplaced(*pipeline))))
            return false;
         const auto has_resource = [&](const auto& hashes)
         { return std::ranges::find(hashes, *resource) != std::end(hashes); };
         return !resource || entry.ds_hash == *resource || has_resource(entry.rt_hash) || has_resource(entry.sr_hash) || has_resource(entry.ua_hash) || has_resource(entry.cb_hash);
      }
   };

   // How a texture readback's file is laid out: rows of "row_pitch" bytes, ordered [slice][z][row]
   struct TextureFile
   {
      const char* dimension = "2d";
      uint32_t width = 1;
      uint32_t height = 1;
      uint32_t depth = 1;
      uint32_t slices = 1;
      uint32_t mip = 0;
      uint32_t mips = 1;
      uint32_t samples = 1;
      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
      DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN; // How to interpret the data
      uint32_t row_pitch = 0;
      uint32_t rows = 0;
   };

   bool IsBlockCompressed(DXGI_FORMAT format)
   {
      return (format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC5_SNORM) || (format >= DXGI_FORMAT_BC6H_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB);
   }

   // Describes a texture readback in "job.result", and queues its file for the pipe thread
   void AddTextureFile(Job* job, std::filesystem::path path, const TextureFile& file, std::vector<char> bytes)
   {
      auto& w = job->result;
      w.Path("path", path);
      w.Field("dimension", file.dimension);
      w.Field("width", file.width).Field("height", file.height).Field("depth", file.depth).Field("slices", file.slices);
      w.Field("mip", file.mip).Field("mips", file.mips).Field("samples", file.samples);
      w.Format("format", file.format).Field("format_id", uint32_t(file.format));
      w.Format("view_format", file.view_format).Field("view_format_id", uint32_t(file.view_format));
      w.Field("row_pitch", file.row_pitch).Field("rows", file.rows).Field("block_compressed", IsBlockCompressed(file.format));
      job->files.emplace_back(std::move(path), std::move(bytes));
   }

   // "luma_read_cbuffer"'s "hex_dwords": each little endian dword as 8 hex digits
   std::string HexDwords(const void* data, size_t size)
   {
      std::string hex;
      hex.reserve(size * 2);
      for (size_t offset = 0; offset + sizeof(uint32_t) <= size; offset += sizeof(uint32_t))
      {
         uint32_t dword = 0;
         std::memcpy(&dword, static_cast<const char*>(data) + offset, sizeof(dword));
         std::format_to(std::back_inserter(hex), "{:08X}", dword);
      }
      return hex;
   }

#if DX11
   // Picks the pass a readback targets ("job.pipeline_handle", "shader_hash", "stage", "instance", "thread"), either from the last
   // capture ("index") or by "hash" + "instance" (Nth draw with that shader in a frame). Instances are counted like the runtime counter
   // the debug draw and constant buffer tracking use: across threads, except deferred passes, filtered by their recording thread.
   bool ResolveTarget(Job* job, DeviceData& device_data, CommandListData& cmd_list_data)
   {
      const std::shared_lock lock_generic(s_mutex_generic);
      const Shader::CachedPipeline* pipeline = nullptr;
      if (job->Arg("index"))
      {
         const std::shared_lock lock_trace(cmd_list_data.mutex_trace);
         const size_t index = size_t(job->IntArg("index", 0));
         if (trace_running || index >= cmd_list_data.trace_draw_calls_data.size())
         {
            job->error = std::format("Trace index {} is out of range (captured {}), run luma_trace_capture first", index, cmd_list_data.trace_draw_calls_data.size());
            return false;
         }
         const TraceDrawCallData& entry = cmd_list_data.trace_draw_calls_data[index];
         pipeline = entry.type == TraceDrawCallData::TraceDrawCallType::Shader ? FindPipeline(device_data, entry.pipeline_handle) : nullptr;
         if (!pipeline)
         {
            job->error = std::format("Trace index {} is not a shader pass with a live pipeline", index);
            return false;
         }
         const bool deferred = IsDeferred(entry);
         job->thread = deferred ? entry.thread_id : std::thread::id();
         job->instance = 0;
         for (size_t i = 0; i < index; i++)
         {
            const auto& other = cmd_list_data.trace_draw_calls_data[i];
            if (other.pipeline_handle == entry.pipeline_handle && (!deferred || other.thread_id == entry.thread_id))
               job->instance++;
         }
      }
      else if (const auto hash = job->HashArg("hash"))
      {
         const auto it = device_data.pipeline_caches_by_shader_hash.find(*hash);
         if (it == device_data.pipeline_caches_by_shader_hash.end() || it->second.empty())
         {
            job->error = std::format("Shader {} has no live pipeline", Shader::Hash_NumToStr(*hash, true));
            return false;
         }
         pipeline = *it->second.begin();
         job->instance = int32_t(job->IntArg("instance", 0));
      }
      else
      {
         job->error = "Pass either \"index\" (from luma_trace_list) or \"hash\" (+ optional \"instance\")";
         return false;
      }
      job->pipeline_handle = pipeline->pipeline.handle;
      job->shader_hash = pipeline->shader_hashes[0];
      job->stage = pipeline->StageName();
      return true;
   }

   struct TextureLayout
   {
      UINT width = 1, height = 1, depth = 1, slices = 1, mips = 1;
      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
   };

   // A CPU readable copy of the texture's description (single sampled, no bindings), plus the texture's layout
   template <typename T>
   HRESULT CreateStagingCopy(ID3D11Device* native_device, ID3D11Resource* source, TextureLayout* layout, com_ptr<ID3D11Resource>* staging)
   {
      com_ptr<T> texture;
      source->QueryInterface(&texture);
      D3D11_RESOURCE_DESC<T> desc;
      texture->GetDesc(&desc);
      layout->width = desc.Width;
      layout->mips = desc.MipLevels;
      layout->format = desc.Format;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      if constexpr (std::is_same_v<T, ID3D11Texture3D>)
      {
         layout->height = desc.Height;
         layout->depth = desc.Depth;
         return native_device->CreateTexture3D(&desc, nullptr, reinterpret_cast<ID3D11Texture3D**>(&*staging));
      }
      else if constexpr (std::is_same_v<T, ID3D11Texture2D>)
      {
         layout->height = desc.Height;
         layout->slices = desc.ArraySize;
         desc.SampleDesc = {1, 0};
         return native_device->CreateTexture2D(&desc, nullptr, reinterpret_cast<ID3D11Texture2D**>(&*staging));
      }
      else
      {
         layout->slices = desc.ArraySize;
         return native_device->CreateTexture1D(&desc, nullptr, reinterpret_cast<ID3D11Texture1D**>(&*staging));
      }
   }

   // Reads one mip (all array or depth slices) of "source" back as raw rows of "row_pitch" bytes, ordered [slice][z][row], into
   // "job.files" (the pipe thread writes them to "path"), and describes it in "job.result".
   // "view_format" is how to interpret the data, the resource format if unknown.
   bool ReadTexture(ID3D11DeviceContext* native_device_context, ID3D11Resource* source, DXGI_FORMAT view_format, uint32_t mip, std::filesystem::path path, Job* job, std::string* error)
   {
      com_ptr<ID3D11Device> native_device;
      native_device_context->GetDevice(&native_device);
      D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      source->GetType(&dimension);

      com_ptr<ID3D11Resource> copy_source(source);
      UINT samples = 1;
      if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      {
         com_ptr<ID3D11Texture2D> texture;
         source->QueryInterface(&texture);
         D3D11_TEXTURE2D_DESC desc;
         texture->GetDesc(&desc);
         samples = desc.SampleDesc.Count;
         if (samples > 1)
         {
            // Staging textures can't be multisampled, resolve every slice first
            const DXGI_FORMAT resolve_format = view_format != DXGI_FORMAT_UNKNOWN ? view_format : desc.Format;
            com_ptr<ID3D11Texture2D> resolved = IsTypelessFormat(resolve_format) ? nullptr : CloneTexture<ID3D11Texture2D>(native_device.get(), source, resolve_format, D3D11_BIND_SHADER_RESOURCE, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS, false, false, native_device_context, -1, 1);
            if (!resolved)
            {
               *error = std::format("The multisampled {} texture can't be resolved for readback", GetFormatNameSafe(resolve_format));
               return false;
            }
            for (UINT slice = 0; slice < desc.ArraySize; slice++)
               native_device_context->ResolveSubresource(resolved.get(), slice, source, slice, resolve_format);
            copy_source = nullptr;
            resolved->QueryInterface(&copy_source);
         }
      }
      TextureLayout layout;
      com_ptr<ID3D11Resource> staging;
      HRESULT hr = E_FAIL;
      if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
         hr = CreateStagingCopy<ID3D11Texture2D>(native_device.get(), copy_source.get(), &layout, std::addressof(staging));
      else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
         hr = CreateStagingCopy<ID3D11Texture3D>(native_device.get(), copy_source.get(), &layout, std::addressof(staging));
      else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
         hr = CreateStagingCopy<ID3D11Texture1D>(native_device.get(), copy_source.get(), &layout, std::addressof(staging));
      if (FAILED(hr) || !staging)
      {
         *error = std::format("Failed to create the staging texture for {} (0x{:08X})", GetFormatNameSafe(layout.format), uint32_t(hr));
         return false;
      }
      if (mip >= layout.mips)
      {
         *error = std::format("Mip {} is out of range ({} mips)", mip, layout.mips);
         return false;
      }
      if (view_format == DXGI_FORMAT_UNKNOWN)
         view_format = layout.format;
      native_device_context->CopyResource(staging.get(), copy_source.get());

      const UINT mip_width = (std::max)(layout.width >> mip, 1u);
      const UINT mip_height = (std::max)(layout.height >> mip, 1u);
      const UINT mip_depth = (std::max)(layout.depth >> mip, 1u);
      const UINT rows = IsBlockCompressed(layout.format) ? (mip_height + 3) / 4 : mip_height;

      std::vector<char> bytes;
      UINT row_pitch = 0;
      for (UINT slice = 0; slice < layout.slices; slice++)
      {
         D3D11_MAPPED_SUBRESOURCE mapped = {};
         // Stalls until the GPU caught up, fine for a one off read
         hr = native_device_context->Map(staging.get(), D3D11CalcSubresource(mip, slice, layout.mips), D3D11_MAP_READ, 0, &mapped);
         if (FAILED(hr))
         {
            *error = std::format("Failed to map the staging texture (0x{:08X})", uint32_t(hr));
            return false;
         }
         row_pitch = mapped.RowPitch;
         const size_t plane_bytes = size_t(rows) * row_pitch; // The rows of a plane are contiguous
         bytes.reserve(plane_bytes * mip_depth * layout.slices);
         for (UINT z = 0; z < mip_depth; z++)
         {
            const char* plane = static_cast<const char*>(mapped.pData) + size_t(z) * mapped.DepthPitch;
            bytes.insert(bytes.end(), plane, plane + plane_bytes);
         }
         native_device_context->Unmap(staging.get(), D3D11CalcSubresource(mip, slice, layout.mips));
      }

      const char* dimension_name = ((dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) ? "3d" : ((dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) ? "1d" : "2d"));
      AddTextureFile(job, std::move(path), {.dimension = dimension_name, .width = mip_width, .height = mip_height, .depth = mip_depth, .slices = layout.slices, .mip = mip, .mips = layout.mips, .samples = samples, .format = layout.format, .view_format = view_format, .row_pitch = row_pitch, .rows = rows}, std::move(bytes));
      return true;
   }
#endif // DX11

   // Super resolution tap: wraps every "sr_implementations" entry (see "Init()") to record what games feed the upscalers
   constexpr const char* sr_resource_names[] = {"source_color", "motion_vectors", "depth_buffer", "exposure", "bias_mask", "transparency_alpha", "output_color"};
   constexpr size_t sr_history_size = 256;

   struct SrCapture
   {
#if DX11
      std::vector<std::pair<const char*, com_ptr<ID3D11Resource>>> textures; // GPU copies, read back at present
#else
      DeviceData* device_data = nullptr; // Its "readback.captures" are the GPU copies, read back at present
#endif
      bool draw_succeeded;
   };
   std::atomic<bool> sr_capture_armed = false;
   std::mutex s_mutex_sr_capture;
   std::optional<SrCapture> sr_capture; // Guarded by "s_mutex_sr_capture", set by the draw that found the capture armed

   class SrTap final : public SR::SuperResolutionImpl
   {
   public:
      explicit SrTap(std::unique_ptr<SR::SuperResolutionImpl> wrapped) : inner(std::move(wrapped))
      {
      }

      bool HasInit(const SR::InstanceData* data) const override
      {
         return inner->HasInit(data);
      }
      bool IsSupported(const SR::InstanceData* data) const override
      {
         return inner->IsSupported(data);
      }
      bool Init(SR::InstanceData*& data, SR::Device* device, IDXGIAdapter* adapter) override
      {
         init_count++;
         return inner->Init(data, device, adapter);
      }
      void Deinit(SR::InstanceData*& data, SR::Device* optional_device) override
      {
         deinit_count++;
         inner->Deinit(data, optional_device);
      }
      void ReleaseResources(SR::InstanceData* data) override
      {
         inner->ReleaseResources(data);
      }
      void ReleaseRetired(SR::InstanceData* data) override
      {
         inner->ReleaseRetired(data);
      }
      float GetMipLODBias(const SR::InstanceData* data) const override
      {
         return inner->GetMipLODBias(data);
      }
      bool UpdateSettings(SR::InstanceData* data, SR::CommandList* command_list, const SR::SettingsData& settings_data) override
      {
         const bool succeeded = inner->UpdateSettings(data, command_list, settings_data);
         const std::lock_guard lock(mutex);
         settings = settings_data;
         update_count++;
         last_update_succeeded = succeeded;
         return succeeded;
      }
      bool Draw(const SR::InstanceData* data, SR::CommandList* command_list, const DrawData& draw_data) override
      {
         SR::Resource* const resources[] = {draw_data.source_color, draw_data.motion_vectors, draw_data.depth_buffer, draw_data.exposure, draw_data.bias_mask, draw_data.transparency_alpha, draw_data.output_color};
         const bool capture = sr_capture_armed.load() && sr_capture_armed.exchange(false);
         com_ptr<SR::Device> native_device;
         SrCapture captured;
         // D3D12: recorded in the draw's list, in the state "DrawData" has the texture in (the output is the last one), and read at present once the game executed the list
         const auto copy_texture = [&](size_t i)
         {
#if DX11
            captured.textures.emplace_back(sr_resource_names[i], CloneResource(native_device.get(), command_list, resources[i]));
#else
            if (!captured.device_data)
               return;
            D3D12Readback::State& readback = captured.device_data->readback;
            const std::unique_lock lock(readback.mutex);
            const D3D12_RESOURCE_STATES state = ((i + 1 == std::size(resources)) ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            const DXGI_FORMAT format = D3D12Readback::PlaneFormat(resources[i]->GetDesc().Format, false);
            D3D12Readback::Copy& copy = readback.captures.emplace_back(D3D12Readback::Capture{.name = sr_resource_names[i], .copy = {.native_list = reinterpret_cast<uint64_t>(command_list), .data_format = format}}).copy;
            const uint32_t subresource = 0;
            if (D3D12Readback::RecordTextureCopy(&readback, native_device.get(), command_list, resources[i], state, std::span(&subresource, 1), format, &copy, [] {}))
            {
               D3D12Readback::recorded_copies++;
            }
            else
            {
               copy.status = D3D12Readback::Copy::Status::Failed;
               copy.error = "Failed to create the readback resources";
            }
#endif
         };
         if (capture)
         {
#if DX11
            command_list->GetDevice(&native_device);
#else
            command_list->GetDevice(IID_PPV_ARGS(&native_device));
            const auto found = std::ranges::find_if(global_devices_data, [&](const DeviceData* candidate)
               { return candidate->native_device == native_device.get(); });
            captured.device_data = (found != global_devices_data.end()) ? *found : nullptr;
#endif
            for (size_t i = 0; i < std::size(resources) - 1; i++) // Inputs before the upscaler runs
               if (resources[i])
                  copy_texture(i);
         }
         captured.draw_succeeded = inner->Draw(data, command_list, draw_data);
#if DX11 // D3D12: the SR pass isn't a trace entry yet
         QueueInjectedPassTrace(command_list, "Luma Super Resolution", draw_data.output_color);
#endif
         if (capture && draw_data.output_color)
            copy_texture(std::size(resources) - 1);
         {
            const std::lock_guard lock(mutex);
            last_draw = draw_data;
            // The resource descriptions cost a few queries and allocations per draw, only worth it with a client that can read them
            if (connected_clients > 0)
            {
               for (size_t i = 0; i < std::size(resources); i++)
               {
                  last_resources[i] = {};
                  if (!resources[i])
                     continue;
#if DX11
                  GetResourceInfo(resources[i], last_resources[i].size, last_resources[i].format, nullptr, &last_resources[i].resource);
#else
                  // What the D3D12 "GetResourceInfo()" gives, which wants a ReShade device the tap doesn't have
                  const D3D12_RESOURCE_DESC desc = resources[i]->GetDesc();
                  last_resources[i] = {.size = {uint32_t(desc.Width), desc.Height, desc.DepthOrArraySize, (desc.SampleDesc.Count > 1 ? desc.SampleDesc.Count : desc.MipLevels)}, .format = desc.Format, .resource = ResourceHashString(reinterpret_cast<uint64_t>(resources[i]))};
#endif
               }
            }
            history[draw_count % sr_history_size] = {cb_luma_global_settings.FrameIndex, draw_data.frame_index, draw_data.jitter_x, draw_data.jitter_y, draw_data.pre_exposure, draw_data.vert_fov, draw_data.render_width, draw_data.render_height, draw_data.reset, captured.draw_succeeded};
            draw_count++;
            failures += captured.draw_succeeded ? 0 : 1;
            resets += draw_data.reset ? 1 : 0;
         }
         const bool succeeded = captured.draw_succeeded;
         if (capture)
         {
            const std::lock_guard lock(s_mutex_sr_capture);
            sr_capture = std::move(captured);
         }
         return succeeded;
      }
      int GetJitterPhases(const SR::InstanceData* data) const override
      {
         return inner->GetJitterPhases(data);
      }
      bool IsReady(const SR::InstanceData* data) const override
      {
         return inner->IsReady(data);
      }
      bool NeedsStateRestoration() const override
      {
         return inner->NeedsStateRestoration();
      }

      void Write(JsonWriter* w, size_t history_count) const
      {
         const std::lock_guard lock(mutex);
         w->Field("init_count", init_count.load()).Field("deinit_count", deinit_count.load());
         w->Field("update_settings_count", update_count).Field("last_update_settings_succeeded", last_update_succeeded);
         w->Key("settings").BeginObject();
         w->Key("output").BeginArray().Value(settings.output_width).Value(settings.output_height).EndArray();
         w->Key("render").BeginArray().Value(settings.render_width).Value(settings.render_height).EndArray();
         w->Field("dynamic_resolution", settings.dynamic_resolution).Field("hdr", settings.hdr).Field("inverted_depth", settings.inverted_depth);
         w->Field("mvs_jittered", settings.mvs_jittered).Field("mvs_x_scale", settings.mvs_x_scale).Field("mvs_y_scale", settings.mvs_y_scale);
         w->Field("auto_exposure", settings.auto_exposure).Field("render_preset", settings.render_preset);
         w->EndObject();

         w->Field("draw_count", draw_count).Field("draw_failures", failures).Field("resets", resets);
         if (draw_count == 0)
            return;
         w->Key("last_draw").BeginObject();
         w->Field("reset", last_draw.reset).Key("render").BeginArray().Value(last_draw.render_width).Value(last_draw.render_height).EndArray();
         w->Field("jitter_x", last_draw.jitter_x).Field("jitter_y", last_draw.jitter_y).Field("pre_exposure", last_draw.pre_exposure);
         w->Field("vert_fov", last_draw.vert_fov).Field("near_plane", last_draw.near_plane).Field("far_plane", last_draw.far_plane);
         w->Field("time_delta", last_draw.time_delta).Field("frame_index", last_draw.frame_index).Field("user_sharpness", last_draw.user_sharpness);
         w->Key("resources").BeginObject();
         for (size_t i = 0; i < std::size(sr_resource_names); i++)
         {
            const auto& resource = last_resources[i];
            w->Key(sr_resource_names[i]);
            if (resource.resource.empty())
            {
               w->Value("none"); // Or drawn before a client connected, see "connected_clients"
               continue;
            }
            w->BeginObject().Format("format", resource.format).Key("size").BeginArray().Value(resource.size.x).Value(resource.size.y).Value(resource.size.z).EndArray().Field("resource", resource.resource).EndObject();
         }
         w->EndObject().EndObject();

         // Newest last. Repeated jitters or frames without a draw expose latch and phase bugs.
         const size_t count = (std::min)({history_count, draw_count, sr_history_size});
         std::set<std::pair<float, float>> unique_jitters;
         uint32_t consecutive_repeats = 0;
         w->Key("history").BeginArray();
         for (size_t i = draw_count - count; i < draw_count; i++)
         {
            const auto& record = history[i % sr_history_size];
            if (i > draw_count - count)
            {
               const auto& previous = history[(i - 1) % sr_history_size];
               consecutive_repeats += (previous.jitter_x == record.jitter_x && previous.jitter_y == record.jitter_y) ? 1 : 0;
            }
            unique_jitters.emplace(record.jitter_x, record.jitter_y);
            w->BeginArray().Value(record.luma_frame).Value(record.frame_index).Value(record.jitter_x).Value(record.jitter_y).Value(record.pre_exposure).Value(record.vert_fov);
            w->Value(record.render_width).Value(record.render_height).Value(record.reset).Value(record.succeeded).EndArray();
         }
         w->EndArray();
         w->Field("history_columns", "luma_frame, frame_index, jitter_x, jitter_y, pre_exposure, vert_fov, render_width, render_height, reset, succeeded");
         w->Field("unique_jitters", unique_jitters.size()).Field("consecutive_repeated_jitters", consecutive_repeats);
      }

   private:
      struct ResourceSummary
      {
         uint4 size = {};
         DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
         std::string resource;
      };
      struct DrawRecord
      {
         uint32_t luma_frame = 0;
         unsigned long long frame_index = 0;
         float jitter_x = 0.f;
         float jitter_y = 0.f;
         float pre_exposure = 0.f;
         float vert_fov = 0.f;
         unsigned int render_width = 0;
         unsigned int render_height = 0;
         bool reset = false;
         bool succeeded = false;
      };

      std::unique_ptr<SR::SuperResolutionImpl> inner;
      std::atomic<uint32_t> init_count = 0;
      std::atomic<uint32_t> deinit_count = 0;
      mutable std::mutex mutex; // Guards everything below
      SR::SettingsData settings;
      uint32_t update_count = 0;
      bool last_update_succeeded = false;
      DrawData last_draw;
      ResourceSummary last_resources[std::size(sr_resource_names)];
      std::array<DrawRecord, sr_history_size> history = {};
      size_t draw_count = 0;
      uint32_t failures = 0;
      uint32_t resets = 0;
   };

   // Where readbacks go ("out_dir" can only pick a subfolder): an elevated game must not write where a non elevated client asks.
   // Not under the game's "Luma" folder, a non empty one there would shadow the repository shaders.
   std::filesystem::path ReadbackRoot()
   {
      wchar_t temp_path[MAX_PATH] = {};
      GetTempPathW(MAX_PATH, temp_path);
      return std::filesystem::path(temp_path) / "luma-mcp";
   }

   // Request arguments are UTF-8 (a narrow "path" would read them in the ANSI code page)
   std::filesystem::path Utf8Path(std::string_view utf8)
   {
      return std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
   }

   // "dir" is "ReadbackRoot()" or inside it, ".." and junctions resolved. Never throws (pipe thread).
   // ponytail: checked, then written (a junction swapped in between wins), fine for a DEVELOPMENT only tool
   bool IsInReadbackRoot(const std::filesystem::path& dir)
   {
      std::error_code ec;
      const std::filesystem::path root = std::filesystem::weakly_canonical(ReadbackRoot(), ec);
      if (ec || root.empty())
         return false;
      const std::filesystem::path canonical_dir = std::filesystem::weakly_canonical(dir, ec);
      return !ec && std::mismatch(root.begin(), root.end(), canonical_dir.begin(), canonical_dir.end()).first == root.end();
   }

   // "<out_dir>/<stem>_p<pid>_f<frame>.bin": every Luma process shares the root, and two of them with the same shaders reach the same frame
   std::filesystem::path ReadbackPath(const Job& job, std::string_view stem)
   {
      const std::string file = std::format("{}_p{}_f{}.bin", stem, GetCurrentProcessId(), cb_luma_global_settings.FrameIndex);
      if (const std::string* out_dir = job.Arg("out_dir"))
         return Utf8Path(*out_dir) / file;
      return ReadbackRoot() / file;
   }

   // A readback's pass may not draw in the frame after the request: false (and the error) once its miss budget is spent
   bool KeepWaiting(Job* job, std::string_view view)
   {
      if (++job->frames < readback_max_frames)
         return true;
      job->error = std::format("The pass was not drawn, or had no {} at that slot, in {} frames (check \"instance\" and \"slot\")", view, readback_max_frames);
      return false;
   }

   // Every "Run*" returns true once its job finished (successfully or not), false to run again next present
   bool RunStatus(Job* job, DeviceData& device_data)
   {
      auto& w = job->result;
      w.Field("game", Globals::GAME_NAME).Field("version", Globals::VERSION);
      w.Field("pid", GetCurrentProcessId()).Field("bits", sizeof(void*) * 8);
      w.Path("exe_path", System::GetModulePath()); // ReShade writes its log next to it by default
      w.Field("frame_index", cb_luma_global_settings.FrameIndex).Field("presenting", true);
      w.Key("output_resolution").BeginArray().Value(device_data.output_resolution.x).Value(device_data.output_resolution.y).EndArray();
      w.Key("render_resolution").BeginArray().Value(device_data.render_resolution.x).Value(device_data.render_resolution.y).EndArray();
      w.Field("mod_active", IsModActive(device_data));
      w.Field("file_clones", CountFileClones(device_data));
      {
         const std::shared_lock lock_loading(s_mutex_loading);
         w.Field("has_compilation_errors", shaders_compilation_failed);
      }
      w.Field("trace_count", trace_count);
#if DX12
      {
         // The game's devices: "presenting" is the one this answers from, "captured" the one the frame capture records (see "trace_device_data", "device" of luma_trace_capture)
         const std::shared_lock lock_device(s_mutex_device);
         const std::shared_lock lock_trace(s_mutex_trace);
         w.Key("devices").BeginArray();
         for (size_t i = 0; i < global_devices_data.size(); i++)
         {
            const DeviceData* const candidate = global_devices_data[i];
            w.BeginObject().Field("index", uint32_t(i)).Field("presenting", candidate == &device_data).Field("captured", candidate == (trace_device_data ? trace_device_data : &device_data)).EndObject();
         }
         w.EndArray();
      }
#endif
      w.Field("swapchain_upgrade_type", uint32_t(swapchain_upgrade_type));
      w.Field("swapchain_format_upgrade_type", uint32_t(swapchain_format_upgrade_type));
      w.Field("texture_format_upgrades_type", uint32_t(texture_format_upgrades_type));
      {
         const std::shared_lock lock_device(device_data.mutex);
         w.Key("swapchains").BeginArray();
         for (reshade::api::swapchain* swapchain : device_data.swapchains)
         {
            DXGI_SWAP_CHAIN_DESC desc = {};
            ((IDXGISwapChain*)(swapchain->get_native()))->GetDesc(&desc);
            w.BeginObject().Format("format", desc.BufferDesc.Format).Field("width", desc.BufferDesc.Width).Field("height", desc.BufferDesc.Height).Field("buffers", desc.BufferCount).EndObject();
         }
         w.EndArray();
      }
      MEMORYSTATUSEX memory_status = {sizeof(MEMORYSTATUSEX)};
      if (GlobalMemoryStatusEx(&memory_status))
      {
         // Process address space, what x86 games run out of
         w.Field("virtual_used_mb", (memory_status.ullTotalVirtual - memory_status.ullAvailVirtual) >> 20);
         w.Field("virtual_total_mb", memory_status.ullTotalVirtual >> 20);
         w.Field("system_memory_load_percent", memory_status.dwMemoryLoad);
      }
      return true;
   }

   bool RunTraceCapture(Job* job, DeviceData& device_data, CommandListData& cmd_list_data)
   {
      {
         const std::unique_lock lock_trace(s_mutex_trace);
         if (job->phase == 0)
         {
            if (trace_running || trace_scheduled)
               return false; // Another capture is in flight
            trace_scheduled = true;
            job->phase = 1;
            return false;
         }
         if (job->phase == 1)
         {
            if (trace_running)
               job->phase = 2;
            return false;
         }
         if (trace_running)
            return false;
      }
      job->frames++;
      // Locked after reading the list, "OnReShadePresent" locks the global trace mutex first
      const auto capture_again = [&job]
      {
         const std::unique_lock lock_trace(s_mutex_trace);
         trace_scheduled = true;
         job->phase = 1;
         return false;
      };

      auto& w = job->result;
      if (const auto trigger = job->HashArg("trigger"))
      {
         std::vector<size_t> trigger_indices;
         {
            const std::shared_lock lock_generic(s_mutex_generic);
            const std::shared_lock lock_trace(cmd_list_data.mutex_trace);
            for (size_t i = 0; i < cmd_list_data.trace_draw_calls_data.size() && trigger_indices.size() < 32; i++)
            {
               const auto& entry = cmd_list_data.trace_draw_calls_data[i];
               const auto* pipeline = entry.type == TraceDrawCallData::TraceDrawCallType::Shader ? FindPipeline(device_data, entry.pipeline_handle) : nullptr;
               if (pipeline && std::ranges::find(pipeline->shader_hashes, *trigger) != pipeline->shader_hashes.end())
                  trigger_indices.push_back(i);
            }
         }
         // Keep capturing until the pass shows up
         if (job->frames < uint32_t(job->IntArg("max_frames", 120, 1, 3600)) && trigger_indices.empty()) // Read even when found right away
            return capture_again();
         w.Field("count", trace_count).Field("frames_captured", job->frames).Field("trigger_found", !trigger_indices.empty()).Key("trigger_indices").BeginArray();
         for (const size_t index : trigger_indices)
            w.Value(index);
         w.EndArray();
         return true;
      }

      // Draw counts per shader over consecutive captures (a capture spans two presents, so every other frame)
      const uint32_t frames = uint32_t(job->IntArg("frames", 1, 1, 600));
      if (frames > 1)
      {
         {
            const std::shared_lock lock_generic(s_mutex_generic);
            const std::shared_lock lock_trace(cmd_list_data.mutex_trace);
            for (const auto& entry : cmd_list_data.trace_draw_calls_data)
            {
               const auto* pipeline = entry.type == TraceDrawCallData::TraceDrawCallType::Shader ? FindPipeline(device_data, entry.pipeline_handle) : nullptr;
               if (!pipeline)
                  continue;
               for (const uint32_t shader_hash : pipeline->shader_hashes)
               {
                  auto& counts = job->hash_counts.try_emplace(shader_hash, Job::HashCounts{StageName(*pipeline, shader_hash), {}}).first->second.per_frame;
                  counts.resize(job->frames, 0); // Zeroes for the captures that didn't draw it
                  counts.back()++;
               }
            }
         }
         if (job->frames < frames)
            return capture_again();
         std::vector<std::pair<uint32_t, uint32_t>> totals; // Hash, total draws
         for (auto& [hash, counts] : job->hash_counts)
         {
            counts.per_frame.resize(job->frames, 0);
            uint32_t total = 0;
            for (const uint32_t count : counts.per_frame)
               total += count;
            totals.emplace_back(hash, total);
         }
         std::sort(totals.begin(), totals.end(), [](const auto& a, const auto& b)
            { return a.second > b.second; });
         const size_t top = size_t(job->IntArg("top", 64, 1, 5000));
         // Long windows: per hash aggregates instead of a per frame array
         const bool summary = job->BoolArg("summary", false);
         w.Field("count", trace_count).Field("frames_captured", job->frames).Field("distinct_shaders", totals.size()).Key("hash_counts").BeginArray();
         for (size_t i = 0; i < (std::min)(top, totals.size()); i++)
         {
            const auto& counts = job->hash_counts[totals[i].first];
            w.BeginObject().Hash("hash", totals[i].first).Field("stage", counts.stage).Field("total", totals[i].second);
            if (summary)
            {
               const auto [min, max] = std::minmax_element(counts.per_frame.begin(), counts.per_frame.end());
               w.Field("frames_seen", std::ranges::count_if(counts.per_frame, [](uint32_t count)
                                         { return count != 0; }))
                  .Field("min", *min)
                  .Field("max", *max)
                  .EndObject();
               continue;
            }
            w.Key("per_frame").BeginArray();
            for (const uint32_t count : counts.per_frame)
               w.Value(count);
            w.EndArray().EndObject();
         }
         w.EndArray();
         return true;
      }

      w.Field("count", trace_count).Field("frames_captured", job->frames);
      return true;
   }

   bool RunTraceList(Job* job, DeviceData& device_data, CommandListData& cmd_list_data)
   {
      const size_t offset = size_t((std::max)(job->IntArg("offset", 0), int64_t(0)));
      const size_t limit = size_t(job->IntArg("limit", 300, 1, 5000));
      const std::shared_lock lock_trace(s_mutex_trace);
      if (trace_running)
      {
         job->error = "A capture is in progress";
         return true;
      }
      const std::shared_lock lock_generic(s_mutex_generic);
      const std::shared_lock lock_loading(s_mutex_loading);
      const std::shared_lock lock_trace_2(cmd_list_data.mutex_trace);
      auto& w = job->result;
      w.Field("total", cmd_list_data.trace_draw_calls_data.size());
      w.Key("entries").BeginArray();
      const TraceFilter filter(*job);
      size_t matched = 0;
      for (size_t i = 0; i < cmd_list_data.trace_draw_calls_data.size(); i++)
      {
         const auto& entry = cmd_list_data.trace_draw_calls_data[i];
         if (!filter.Matches(device_data, entry))
            continue;
         if (matched >= offset && matched < offset + limit)
         {
            w.BeginObject();
            WriteTraceEntrySummary(&w, device_data, entry, i);
            w.EndObject();
         }
         matched++;
      }
      w.EndArray();
      w.Field("matched", matched).Field("offset", offset).Field("returned", matched > offset ? (std::min)(matched - offset, limit) : 0);
      return true;
   }

   bool RunTraceGet(Job* job, DeviceData& device_data, CommandListData& cmd_list_data)
   {
      const std::shared_lock lock_trace(s_mutex_trace);
      const std::shared_lock lock_generic(s_mutex_generic);
      const std::shared_lock lock_loading(s_mutex_loading);
      const std::shared_lock lock_trace_2(cmd_list_data.mutex_trace);
      const size_t index = size_t(job->IntArg("index", -1));
      if (trace_running || index >= cmd_list_data.trace_draw_calls_data.size())
      {
         job->error = std::format("Trace index {} is out of range (captured {})", index, cmd_list_data.trace_draw_calls_data.size());
         return true;
      }
      WriteTraceEntryDetails(&job->result, device_data, cmd_list_data.trace_draw_calls_data[index], index);
      return true;
   }

#if DX11
   bool RunReadResource(Job* job, DeviceData& device_data, CommandListData& cmd_list_data, ID3D11DeviceContext* native_device_context, reshade::api::swapchain* swapchain)
   {
      const std::string* view = job->Arg("view");
      if (job->phase == 0)
      {
         if (job->BoolArg("composed", false))
         {
            job->error = "composed=true is D3D12 only: the D3D11 views are read before Luma's display composition";
            return true;
         }
         // Final targets, read right away at present
         if (view && (*view == "swapchain" || *view == "ui"))
         {
            const bool back_buffer = *view == "swapchain";
            // What the game drew into (the proxy of a proxied swapchain)
            const com_ptr<ID3D11Texture2D> texture = (back_buffer ? GetGameBackBuffer((IDXGISwapChain*)(swapchain->get_native())) : device_data.ui_texture);
            ID3D11Resource* resource = texture.get();
            if (!resource)
            {
               job->error = back_buffer ? "The swapchain has no current back buffer" : "There's no separate UI texture (UI separation is off, or the UI hasn't drawn yet)";
               return true;
            }
            if (ReadTexture(native_device_context, resource, DXGI_FORMAT_UNKNOWN, uint32_t(job->IntArg("mip", 0)), ReadbackPath(*job, *view), job, &job->error))
               job->result.Field("view", *view).Field("note", "Read before Luma's display composition: no output encoding, gamma correction, gamut mapping or UI composition yet");
            return true;
         }
         if (view && view->starts_with("registered:"))
         {
            const std::string name = view->substr(11);
            com_ptr<ID3D11Resource> resource;
            {
               const std::lock_guard lock(s_mutex_dev_values);
               const DevValue* dev_value = FindDevValue(name);
               if (!dev_value || dev_value->kind != DevValueKind::Texture)
               {
                  job->error = "Unknown registered texture " + name + " (see luma_dev_values, kind texture)";
                  return true;
               }
               resource = GetTexture(*dev_value, device_data);
            }
            if (!resource)
            {
               job->error = name + " is not allocated (its feature is off, or hasn't run yet)";
               return true;
            }
            if (ReadTexture(native_device_context, resource.get(), DXGI_FORMAT_UNKNOWN, uint32_t(job->IntArg("mip", 0)), ReadbackPath(*job, name), job, &job->error))
               job->result.Field("view", *view).Field("note", "Read at present: the texture's last content, usually this frame's");
            return true;
         }

         static const std::unordered_map<std::string, DebugDrawMode> modes = {
            {"rtv", DebugDrawMode::RenderTarget}, {"srv", DebugDrawMode::ShaderResource}, {"uav", DebugDrawMode::UnorderedAccessView}, {"depth", DebugDrawMode::Depth}, {"stencil", DebugDrawMode::Stencil}};
         const auto mode = modes.find(view ? *view : "rtv");
         if (mode == modes.end())
         {
            job->error = "\"view\" must be one of rtv, srv, uav, depth, stencil, swapchain, ui, registered:<name>";
            return true;
         }
         if (!ResolveTarget(job, device_data, cmd_list_data))
            return true;
         // Debug draw hooks pixel and compute shader draws only
         const bool is_compute = job->stage == std::string_view("CS");
         if (!is_compute && job->stage != std::string_view("PS"))
         {
            job->error = std::format("{} passes can't be read back, use the PS entry of the same draw", job->stage);
            return true;
         }
         if (is_compute && mode->second != DebugDrawMode::ShaderResource && mode->second != DebugDrawMode::UnorderedAccessView)
         {
            job->error = "Compute passes only have srv and uav views";
            return true;
         }

         // Restored in "OnPresent" once the job finishes. Blends stay on and inputs unfrozen, so the pass runs as the game drew it.
         job->saved_debug_draw = DebugDrawSelection::Current();
         DebugDrawSelection{job->pipeline_handle, job->shader_hash, job->instance, job->thread, mode->second, int32_t(job->IntArg("slot", 0)), false, false, job->BoolArg("replaced", true)}.Apply();
         device_data.debug_draw_texture = nullptr;
         job->result.Hash("hash", job->shader_hash).Field("instance", job->instance).Field("view", mode->first).Field("slot", debug_draw_view_index);
         job->phase = 1;
         return false;
      }

      if (!device_data.debug_draw_texture)
         return !KeepWaiting(job, "view");
      const auto path = ReadbackPath(*job, std::format("{:08X}_{}_{}{}", job->shader_hash, job->instance, view ? *view : "rtv", debug_draw_view_index));
      ReadTexture(native_device_context, device_data.debug_draw_texture.get(), device_data.debug_draw_texture_format, uint32_t(job->IntArg("mip", 0)), path, job, &job->error);
      return true;
   }

   bool RunReadConstantBuffer(Job* job, DeviceData& device_data, CommandListData& cmd_list_data, ID3D11DeviceContext* native_device_context)
   {
      if (job->phase == 0)
      {
         if (!ResolveTarget(job, device_data, cmd_list_data))
            return true;
         if (job->stage == std::string_view("GS") || job->stage == std::string_view("XS"))
         {
            job->error = std::format("Constant buffer tracking doesn't support {} passes", job->stage);
            return true;
         }
         const int64_t slot = job->IntArg(job->Arg("register") ? "register" : "slot", 0); // D3D11 slots are the registers
         if (slot < 0 || slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
         {
            job->error = "\"slot\" is out of range";
            return true;
         }
         // Restored in "OnPresent" once the job finishes. The tracking has no thread filter.
         job->saved_track_buffer = TrackBufferSelection::Current();
         TrackBufferSelection{job->pipeline_handle, job->instance, int32_t(slot)}.Apply();
         device_data.track_buffer_data.Clear();
         job->result.Hash("hash", job->shader_hash).Field("stage", job->stage).Field("instance", job->instance).Field("slot", slot);
         job->phase = 1;
         return false;
      }

      auto& data = device_data.track_buffer_data;
      // Buffers bound in deferred contexts were only copied, map them on the immediate one
      if (!data.data_valid && data.cb)
      {
         D3D11_BUFFER_DESC desc = {};
         data.cb->GetDesc(&desc);
         data.data_valid = MapBufferData(data.cb, native_device_context, data.data, desc.ByteWidth);
      }
      if (!data.data_valid)
         return !KeepWaiting(job, "constant buffer");
      const std::string hex = HexDwords(data.data.data(), data.data.size() * sizeof(float));
      auto& w = job->result;
      // Multi frame series: keep the tracking armed and collect one read per frame
      const uint32_t frames = uint32_t(job->IntArg("frames", 1, 1, 600));
      if (frames > 1)
      {
         job->series.push_back(std::format("{}:{}", cb_luma_global_settings.FrameIndex, hex));
         job->frames = 0; // The miss budget is per read
         if (job->series.size() < frames)
         {
            data.Clear();
            return false;
         }
         w.Field("resource", data.hash).Key("series").BeginArray();
         for (const std::string& entry : job->series)
            w.Value(entry);
         w.EndArray().Field("series_format", "luma_frame:hex_dwords");
      }
      else
      {
         w.Field("resource", data.hash).Field("bytes", data.data.size() * sizeof(float));
         if (data.num_constants != 0)
            w.Field("first_constant", data.first_constant).Field("num_constants", data.num_constants);
         w.Field("hex_dwords", hex);
      }
      data.Clear();
      return true;
   }
#else
   // Why a copy or clear entry can't be read back (it goes after "Trace entry <index> "), null if it can
   const char* UnreadableWrite(const TraceDrawCallData& entry)
   {
      if (entry.rt_type_name[0] == "Buffer")
         return "wrote a buffer, and buffers aren't read back (like on D3D11), only textures";
      if (entry.copy_info.kind == TraceDrawCallData::CopyKind::BufferToTexture)
         return "is an upload, and uploads can't be read back: ReShade doesn't pass their row pitch, so they can't be re-issued";
      if (entry.clear_kind == TraceDrawCallData::ClearKind::UnorderedAccess)
         return "is an unordered access view clear, and those can't be read back: ReShade passes no GPU descriptor handle, so they can't be re-issued";
      return nullptr;
   }

   // The last entry of "entries" before "end" whose output holds what a read of "resource" wants (a read of mip "srv_mip", any if it has none,
   // in "srv_format", which picks a depth stencil plane): a draw's render target, unordered access view or depth stencil target, or, unless
   // "draws_only", a copy, resolve or clear. Sets the view of "target" to read it, and returns its index, or nothing. "refusal" gets why the one
   // found can't be read (the reasons of "UnreadableWrite()").
   std::optional<size_t> FindProducer(const std::vector<TraceDrawCallData>& entries, size_t end, const std::string& resource, std::optional<uint32_t> srv_mip, DXGI_FORMAT srv_format, bool draws_only, D3D12Readback::Target* target, std::string* refusal)
   {
      using Kind = D3D12Readback::Kind;
      const bool reads_stencil = srv_format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || srv_format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
      const auto reads_mip = [&](const D3D12State::ViewInfo& view)
      { return !srv_mip || view.first_level == *srv_mip; };
      std::optional<size_t> producer;
      for (size_t i = end; i-- > 0 && !producer;)
      {
         const TraceDrawCallData& entry = entries[i];
         if (entry.type == TraceDrawCallData::TraceDrawCallType::CopyResource || entry.type == TraceDrawCallData::TraceDrawCallType::ClearResource)
         {
            if (draws_only)
               continue;
            // The planes it wrote (bit 0 depth, bit 1 stencil): a resource copy both and a depth stencil clear the ones it cleared, the
            // others the plane of their destination subresource (the index is mip + layer * mips + plane * mips * layers)
            const bool is_clear = entry.type == TraceDrawCallData::TraceDrawCallType::ClearResource;
            const bool is_resource_copy = !is_clear && entry.copy_info.kind == TraceDrawCallData::CopyKind::Resource;
            uint32_t planes = 1;
            if (is_resource_copy)
            {
               planes = 3;
            }
            else if (is_clear && entry.clear_kind == TraceDrawCallData::ClearKind::DepthStencil)
            {
               planes = ((entry.depth_state == TraceDrawCallData::DepthStateType::WriteOnly ? 1 : 0) | (entry.stencil_state == TraceDrawCallData::DepthStateType::WriteOnly ? 2 : 0));
            }
            else if (!is_clear)
            {
               const uint32_t mips = (entry.rt_type_name[0].find("MS") != std::string::npos ? 1 : (std::max)(entry.rt_size[0].w, 1u));
               const uint32_t layers = (entry.rt_type_name[0] == "Texture 3D" ? 1 : (std::max)(entry.rt_size[0].z, 1u));
               planes = 1u << (entry.copy_info.dest_subresource / (mips * layers));
            }
            if (entry.rt_hash[0] == resource && (is_resource_copy || reads_mip(entry.rt_view[0])) && (planes & (reads_stencil ? 2u : 1u)) != 0)
            {
               if (const char* unreadable = UnreadableWrite(entry))
               {
                  *refusal = std::format("Trace entry {} {}", i, unreadable);
                  return std::nullopt;
               }
               producer = i;
               target->kind = (reads_stencil ? Kind::Stencil : Kind::RenderTarget); // Depth for the depth stencil resources
               target->slot = 0;
               target->mip = srv_mip.value_or(0);
               target->format = srv_format;
            }
            continue;
         }
         for (uint32_t slot = 0; slot < TraceDrawCallData::rtvs_size && !producer; slot++)
         {
            if (entry.IsRTVValid(slot) && entry.rt_hash[slot] == resource && reads_mip(entry.rt_view[slot]))
            {
               producer = i;
               target->kind = Kind::RenderTarget;
               target->slot = slot;
            }
         }
         for (uint32_t slot = 0; slot < entry.uav_count && !producer; slot++)
         {
            if (entry.ua_hash[slot] == resource && reads_mip(entry.ua_view[slot]))
            {
               producer = i;
               target->kind = Kind::UnorderedAccess;
               target->slot = slot;
            }
         }
         using DepthStateType = TraceDrawCallData::DepthStateType;
         const DepthStateType plane_state = (reads_stencil ? entry.stencil_state : entry.depth_state);
         if (!producer && entry.IsDSVValid() && entry.ds_hash == resource && reads_mip(entry.ds_view) && (plane_state == DepthStateType::TestAndWrite || plane_state == DepthStateType::WriteOnly))
         {
            producer = i;
            target->kind = (reads_stencil ? Kind::Stencil : Kind::Depth);
            target->slot = 0;
         }
      }
      return producer;
   }

   // The last entry before "consumer" that wrote what its SRV "slot" reads ("FindProducer()": the mip it reads, and for a depth stencil SRV
   // the plane). Sets "target" to read it; nothing, with "error" saying why.
   std::optional<size_t> FindSRVProducer(const std::vector<TraceDrawCallData>& entries, size_t consumer, uint32_t slot, D3D12Readback::Target* target, std::string* error)
   {
      const TraceDrawCallData& entry = entries[consumer];
      if (slot >= entry.srv_count || entry.sr_hash[slot].empty())
      {
         *error = std::format("Trace entry {} has no bound SRV at slot {}", consumer, slot);
         return std::nullopt;
      }
      std::string refusal;
      const std::optional<size_t> producer = FindProducer(entries, consumer, entry.sr_hash[slot], entry.srv_mip[slot], entry.srv_format[slot], false, target, &refusal);
      if (!producer)
      {
         *error = (refusal.empty() ? std::format("No entry before {} wrote the resource of its SRV {} as a render target, UAV or depth stencil target, or by a copy, resolve or clear (the capture misses writes from before the captured frame, CopyTiles and DiscardResource)", consumer, slot) : refusal);
      }
      return producer;
   }

   // The target of the draw "index" of "entries" (run by "pipeline"): its pipeline, its outputs and arguments, and its place among the draws equal to it.
   // Returns the hash of the shader that writes the outputs, which names it.
   uint32_t FillDrawTarget(DeviceData& device_data, const std::vector<TraceDrawCallData>& entries, size_t index, const Shader::CachedPipeline& pipeline, D3D12Readback::Target* target)
   {
      const TraceDrawCallData& entry = entries[index];
      const bool compute = pipeline.HasComputeShader();
      target->pipelines = {entry.pipeline_handle};
      const auto signature_of = [&](const TraceDrawCallData& other)
      { return D3D12Trace::MakeDrawSignature(device_data, other.draw_dispatch_data, compute, other.rt_view[0].resource, other.ds_view.resource); };
      target->signature = signature_of(entry);
      target->instance = int32_t(std::count_if(entries.begin(), entries.begin() + index, [&](const TraceDrawCallData& other)
         { return other.pipeline_handle == entry.pipeline_handle && signature_of(other) == *target->signature; }));
      const auto output_shader = std::ranges::find_if(pipeline.shader_types, [](reshade::api::pipeline_subobject_type type)
         { return type == reshade::api::pipeline_subobject_type::pixel_shader || type == reshade::api::pipeline_subobject_type::compute_shader; });
      return pipeline.shader_hashes[(output_shader != pipeline.shader_types.end()) ? (output_shader - pipeline.shader_types.begin()) : 0];
   }

   // The debug draw's shader resource mode ("SyncD3D12DebugDraw()"): the SRV "slot" of the "instance"th (the first if negative) draw of the last capture that runs one of "pipelines" (and has
   // the "signature", if set) is a texture an earlier draw or dispatch of that capture wrote ("FindProducer()"), which is what the debug draw copies: the target that draw is armed with, its
   // kind and slot those of the view that wrote it (the SRV's mip is that view's: "FindProducer()" matches it). Nothing, with "error" saying why: no capture, no such draw or SRV, or a texture
   // written by something else than a draw. "s_mutex_generic" is held.
   std::optional<D3D12Readback::Target> FindDebugDrawSRVTarget(DeviceData& device_data, const std::unordered_set<Shader::CachedPipeline*>& pipelines, int32_t instance, const std::optional<D3D12Readback::DrawSignature>& signature, uint32_t slot, std::string* error)
   {
      CommandListData& cmd_list_data = device_data.trace_command_list_data;
      const std::shared_lock lock_trace(cmd_list_data.mutex_trace);
      const std::vector<TraceDrawCallData>& entries = cmd_list_data.trace_draw_calls_data;
      if (trace_running || entries.empty())
      {
         *error = "Capture a frame first (luma_trace_capture): the shader resource mode shows the output of the draw that wrote the SRV, which the capture says";
         return std::nullopt;
      }
      std::optional<size_t> consumer;
      int32_t draws_found = 0;
      for (size_t i = 0; i < entries.size() && !consumer; i++)
      {
         const TraceDrawCallData& entry = entries[i];
         const bool runs_pipeline = entry.type == TraceDrawCallData::TraceDrawCallType::Shader && std::ranges::any_of(pipelines, [&](const Shader::CachedPipeline* pipeline)
            { return pipeline->pipeline.handle == entry.pipeline_handle; });
         if (runs_pipeline && (!signature || D3D12Trace::MakeDrawSignature(device_data, entry.draw_dispatch_data, signature->compute, entry.rt_view[0].resource, entry.ds_view.resource) == *signature) && (instance < 0 || draws_found++ == instance))
         {
            consumer = i;
         }
      }
      if (!consumer)
      {
         *error = "The last capture has no such draw of the shader (its instance and signature are those of the draw when it was picked)";
         return std::nullopt;
      }
      D3D12Readback::Target target;
      const std::optional<size_t> producer = FindSRVProducer(entries, *consumer, slot, &target, error);
      if (!producer)
         return std::nullopt;
      const Shader::CachedPipeline* const pipeline = (entries[*producer].type == TraceDrawCallData::TraceDrawCallType::Shader) ? FindPipeline(device_data, entries[*producer].pipeline_handle) : nullptr;
      if (!pipeline)
      {
         *error = std::format("Trace entry {} wrote the SRV's resource with a copy, resolve or clear, and the debug draw copies the output of draws and dispatches", *producer);
         return std::nullopt;
      }
      FillDrawTarget(device_data, entries, *producer, *pipeline, &target);
      return target;
   }

   // Shows the draw "index" of the capture "entries" (run by "pipeline") as the D3D12 debug draw, what the overlay's buttons do on D3D11 with the hash
   // and the instance: sets its shader, instance and signature (the settings "SyncD3D12DebugDraw()" arms the copy from), and the view to show: the
   // one of "view" (the kind and slot a "FindProducer()" found), or what its pipeline most likely writes (the first render target, or the unordered
   // access views of a compute shader, or depth for a draw with no render target)
   void SetD3D12DebugDrawFromTrace(DeviceData& device_data, const std::vector<TraceDrawCallData>& entries, size_t index, const Shader::CachedPipeline& pipeline, const D3D12Readback::Target* view = nullptr)
   {
      using Kind = D3D12Readback::Kind;
      D3D12Readback::Target target;
      debug_draw_shader_hash = FillDrawTarget(device_data, entries, index, pipeline, &target);
      const std::string hash_string = Shader::Hash_NumToStr(debug_draw_shader_hash);
      strcpy_s(debug_draw_shader_hash_string, hash_string.c_str());
      debug_draw_pipeline_target_instance = target.instance;
      debug_draw_signature = target.signature;
      debug_draw_signature_hash = debug_draw_shader_hash;
      debug_draw_registered_texture.clear();
      const bool was_depth = debug_draw_mode == DebugDrawMode::Depth || debug_draw_mode == DebugDrawMode::Stencil;
      debug_draw_mip = 0;
      if (view)
      {
         switch (view->kind)
         {
         case Kind::UnorderedAccess:
            debug_draw_mode = DebugDrawMode::UnorderedAccessView;
            break;
         case Kind::Depth:
            debug_draw_mode = DebugDrawMode::Depth;
            break;
         case Kind::Stencil:
            debug_draw_mode = DebugDrawMode::Stencil;
            break;
         default:
            debug_draw_mode = DebugDrawMode::RenderTarget;
            break;
         }
         debug_draw_view_index = int(view->slot);
      }
      else
      {
         debug_draw_mode = pipeline.HasComputeShader() ? DebugDrawMode::UnorderedAccessView : DebugDrawMode::RenderTarget;
         debug_draw_view_index = 0;
         // Fall back on depth if the main render target isn't valid
         if (debug_draw_mode == DebugDrawMode::RenderTarget && !entries[index].IsRTVValid(0) && entries[index].depth_state != TraceDrawCallData::DepthStateType::Disabled)
         {
            debug_draw_mode = DebugDrawMode::Depth;
         }
      }
      if (debug_draw_mode == DebugDrawMode::Depth || debug_draw_mode == DebugDrawMode::Stencil)
      {
         debug_draw_options |= (uint32_t)DebugDrawTextureOptionsMask::RedOnly;
      }
      else if (was_depth)
      {
         debug_draw_options &= ~(uint32_t)DebugDrawTextureOptionsMask::RedOnly;
      }
   }

   // In "D3D12Readback::WriteKind" order, what a copy, resolve or clear target is named in the result ("written_by")
   constexpr const char* written_by_names[] = {"copy", "region copy", "resolve", "render target clear", "depth stencil clear"};

   // The D3D12 counterpart of the D3D11 "ResolveTarget()": the command a readback copies the output of, the entry "index" of the last capture or
   // the "instance"th draw with "hash". A capture entry gives its pipeline and signature (outputs and arguments, or, for a copy, resolve or
   // clear, what it did) and its instance among the commands equal to it, on any thread: a job system records a pass on another thread each frame.
   bool ResolveTarget(Job* job, DeviceData& device_data, CommandListData& cmd_list_data, std::optional<size_t> index, D3D12Readback::Target* target)
   {
      const std::shared_lock lock_generic(s_mutex_generic);
      const Shader::CachedPipeline* pipeline = nullptr;
      if (index)
      {
         const std::shared_lock lock_trace(cmd_list_data.mutex_trace);
         const std::vector<TraceDrawCallData>& entries = cmd_list_data.trace_draw_calls_data;
         if (trace_running || *index >= entries.size())
         {
            job->error = std::format("Trace index {} is out of range (captured {}), run luma_trace_capture first", *index, entries.size());
            return false;
         }
         const TraceDrawCallData& entry = entries[*index];
         if ((entry.type == TraceDrawCallData::TraceDrawCallType::CopyResource || entry.type == TraceDrawCallData::TraceDrawCallType::ClearResource) && target->kind != D3D12Readback::Kind::ConstantBuffer)
         {
            // What an entry did as "ReadbackCopyOrClear()" sees the command, nothing for the entries that aren't read back
            const auto write_of = [](const TraceDrawCallData& other) -> D3D12Readback::Write
            {
               using WriteKind = D3D12Readback::WriteKind;
               using DepthStateType = TraceDrawCallData::DepthStateType;
               if (UnreadableWrite(other))
                  return {};
               const D3D12State::ViewInfo& written = other.rt_view[0];
               if (other.type == TraceDrawCallData::TraceDrawCallType::ClearResource)
               {
                  const bool is_depth_clear = other.clear_kind == TraceDrawCallData::ClearKind::DepthStencil;
                  const uint8_t planes = (is_depth_clear ? ((other.depth_state == DepthStateType::WriteOnly ? 1 : 0) | (other.stencil_state == DepthStateType::WriteOnly ? 2 : 0)) : 0);
                  return {.resource = written.resource, .kind = (is_depth_clear ? WriteKind::ClearDepthStencil : WriteKind::ClearRenderTarget), .level = written.first_level, .layer = written.first_layer, .planes = planes};
               }
               if (other.type == TraceDrawCallData::TraceDrawCallType::CopyResource)
               {
                  switch (other.copy_info.kind)
                  {
                  case TraceDrawCallData::CopyKind::Resource:
                     return {.resource = written.resource, .source = other.copy_source, .kind = WriteKind::CopyResource};
                  case TraceDrawCallData::CopyKind::Region:
                     return {.resource = written.resource, .source = other.copy_source, .kind = WriteKind::CopyRegion, .subresource = other.copy_info.dest_subresource};
                  case TraceDrawCallData::CopyKind::Resolve:
                     return {.resource = written.resource, .source = other.copy_source, .kind = WriteKind::Resolve, .subresource = other.copy_info.dest_subresource, .resolve_format = other.copy_info.resolve_format};
                  default:
                     break; // Buffer copies and uploads, refused above
                  }
               }
               return {};
            };
            if (const char* refusal = UnreadableWrite(entry))
            {
               job->error = std::format("Trace entry {} {}", *index, refusal);
               return false;
            }
            if (target->kind == D3D12Readback::Kind::UnorderedAccess || target->slot != 0)
            {
               job->error = std::format("Trace entry {} is a copy, resolve or clear: it has one target (slot 0), read as rtv, depth or stencil, and no unordered access view", *index);
               return false;
            }
            const D3D12Readback::Write write = write_of(entry);
            // The planes a depth stencil clear didn't clear are left in a state that isn't known
            const bool reads_stencil = target->kind == D3D12Readback::Kind::Stencil;
            if (write.kind == D3D12Readback::WriteKind::ClearDepthStencil && (write.planes & (reads_stencil ? 2 : 1)) == 0)
            {
               job->error = std::format("Trace entry {} didn't clear {}, so its state after the clear isn't known: pass view {}", *index, (reads_stencil ? "stencil" : "depth"), (reads_stencil ? "depth" : "stencil"));
               return false;
            }
            target->write = write;
            target->instance = int32_t(std::count_if(entries.begin(), entries.begin() + *index, [&](const TraceDrawCallData& other)
               { return write_of(other) == write; }));
            job->shader_hash = 0;
            job->stage = written_by_names[size_t(write.kind)];
            job->written_resource = entry.rt_hash[0];
            job->instance = target->instance;
            return true;
         }
         pipeline = (entry.type == TraceDrawCallData::TraceDrawCallType::Shader) ? FindPipeline(device_data, entry.pipeline_handle) : nullptr;
         if (!pipeline)
         {
            job->error = (entry.type == TraceDrawCallData::TraceDrawCallType::Custom) ? std::format("Trace index {} is a Luma pass (\"{}\"): readbacks only re-issue the game's draws, read its input from the game pass that wrote it", *index, entry.custom_name)
                                                                                      : std::format("Trace index {} is not a shader pass with a live pipeline", *index);
            return false;
         }
         // Named after the shader that writes the outputs
         job->shader_hash = FillDrawTarget(device_data, entries, *index, *pipeline, target);
      }
      else if (const auto hash = job->HashArg("hash"))
      {
         const auto it = device_data.pipeline_caches_by_shader_hash.find(*hash);
         if (it == device_data.pipeline_caches_by_shader_hash.end() || it->second.empty())
         {
            job->error = std::format("Shader {} has no live pipeline", Shader::Hash_NumToStr(*hash, true));
            return false;
         }
         for (const Shader::CachedPipeline* other : it->second)
         {
            target->pipelines.push_back(other->pipeline.handle);
         }
         pipeline = *it->second.begin();
         target->instance = int32_t(job->IntArg("instance", 0));
         job->shader_hash = *hash;
      }
      else
      {
         job->error = "Pass either \"index\" (from luma_trace_list) or \"hash\" (+ optional \"instance\")";
         return false;
      }
      job->stage = StageName(*pipeline, job->shader_hash);
      job->instance = target->instance;
      return true;
   }

   // Arms "job->readback_target" for the draws of the next frames
   void ArmReadback(Job* job, DeviceData& device_data)
   {
      D3D12Readback::State& readback = device_data.readback;
      const std::unique_lock lock(readback.mutex);
      D3D12Readback::Retire(&readback);
      readback.target = job->readback_target;
      readback.instances = 0;
      D3D12Readback::armed = true;
   }

   enum class ReadbackPoll : uint8_t
   {
      Wait, // Again at the next present
      Done,
      Failed, // See "job->error"
   };

   // The GPU can run frames behind the render thread, and a copy's fence is only polled at each present
   constexpr uint32_t readback_max_gpu_frames = 30;

   // Follows the job's copy at a present until the GPU ran it: drawn ("job->phase" 1), then executed and read (2), each with its own budget.
   // Never waits for a queue: it can wait on a fence the game signals after present ("D3D12Readback::Advance()").
   ReadbackPoll PollReadback(Job* job, DeviceData& device_data, D3D12Readback::Copy* copy, std::vector<char>* bytes)
   {
      using Status = D3D12Readback::Copy::Status;
      D3D12Readback::State& readback = device_data.readback;
      const std::unique_lock lock(readback.mutex);
      readback.instances = 0; // Instances count from the last present
      if (!readback.copy)
      {
         if (++job->frames < readback_max_frames)
            return ReadbackPoll::Wait;
         job->error = std::format("The pass was not drawn in {} frames (check \"instance\": a capture entry counts it among the draws of its pipeline with the same outputs and arguments, on any thread)", readback_max_frames);
         return ReadbackPoll::Failed;
      }
      if (job->phase == 1)
      {
         job->phase = 2;
         job->frames = 0;
      }
      switch (readback.copy->status)
      {
      case Status::Failed:
         job->error = readback.copy->error;
         return ReadbackPoll::Failed;
      case Status::Ready:
         *copy = std::move(*readback.copy);
         readback.copy.reset();
         return ReadbackPoll::Done;
      case Status::Discarded:
         // Its command list was reset unexecuted: copy a later draw
         readback.copy.reset();
         readback.target = job->readback_target;
         D3D12Readback::armed = true;
         job->phase = 1;
         return ReadbackPoll::Wait;
      case Status::Recorded:
         if (++job->frames < readback_max_frames)
            return ReadbackPoll::Wait;
         job->error = std::format("The pass's command list was not executed in {} frames", readback_max_frames);
         return ReadbackPoll::Failed;
      case Status::Executed:
         break;
      }
      switch (D3D12Readback::Advance(&*readback.copy, bytes))
      {
      case D3D12Readback::Completion::Ran:
         *copy = std::move(*readback.copy);
         readback.copy.reset();
         return ReadbackPoll::Done;
      case D3D12Readback::Completion::NotRun:
         job->error = "The copy didn't run: the game's predication skipped it, or its command list wasn't submitted";
         return ReadbackPoll::Failed;
      case D3D12Readback::Completion::Pending:
         break;
      }
      if (++job->frames < readback_max_gpu_frames)
         return ReadbackPoll::Wait;
      job->error = std::format("The GPU didn't get to the copy in {} frames: its queue waits for something the game signals later", readback_max_gpu_frames);
      return ReadbackPoll::Failed;
   }

   // Describes a D3D12 texture readback in "job.result", and queues its file ("stem" names it)
   void AddD3D12TextureFile(Job* job, std::string_view stem, const D3D12Readback::Copy& copy, std::vector<char> bytes)
   {
      const D3D12_RESOURCE_DESC& desc = copy.desc;
      const bool is_3d = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
      const char* dimension = (is_3d ? "3d" : ((desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) ? "1d" : "2d"));
      const uint32_t depth = (is_3d ? (std::max)(uint32_t(desc.DepthOrArraySize) >> copy.mip, 1u) : 1u);
      const uint32_t slices = (is_3d ? 1u : uint32_t(copy.footprints.size()));
      AddTextureFile(job, ReadbackPath(*job, stem), {.dimension = dimension, .width = (std::max)(uint32_t(desc.Width >> copy.mip), 1u), .height = (std::max)(desc.Height >> copy.mip, 1u), .depth = depth, .slices = slices, .mip = copy.mip, .mips = desc.MipLevels, .samples = desc.SampleDesc.Count, .format = desc.Format, .view_format = copy.data_format, .row_pitch = copy.footprints[0].Footprint.RowPitch, .rows = copy.rows}, std::move(bytes));
      if (copy.first_layer != 0)
      {
         job->result.Field("first_slice", copy.first_layer);
      }
   }

   bool RunReadResource(Job* job, DeviceData& device_data, CommandListData& cmd_list_data, reshade::api::command_queue* queue, reshade::api::swapchain* swapchain)
   {
      // Copies mip "mip" of the first layer of "resource" (in "state") at present, on a command list of Luma's own: ReShade only flushes its queue's immediate
      // command list when its own API recorded into it, native commands there could run any time later. Waits up to 2 s for a fence signaled behind it (the
      // queue can wait on a fence the game signals later), then writes the file. A copy that timed out is retired: its buffer can still be written.
      const auto read_now = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES state, uint32_t mip, std::string_view stem)
      {
         auto* native_device = reinterpret_cast<ID3D12Device*>(queue->get_device()->get_native());
         com_ptr<ID3D12CommandAllocator> allocator;
         com_ptr<ID3D12GraphicsCommandList> native_list;
         if (!resource || FAILED(native_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) || FAILED(native_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(&native_list))))
         {
            job->error = "The texture is missing (the swapchain has no current back buffer), or the command list for the copy couldn't be created";
            return false;
         }
         D3D12Readback::Copy copy = {.status = D3D12Readback::Copy::Status::Executed, .queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()), .fence_value = 1, .signals = 1, .mip = mip, .data_format = resource->GetDesc().Format};
         bool recorded = false;
         {
            const std::unique_lock lock(device_data.readback.mutex);
            recorded = D3D12Readback::RecordTextureCopy(&device_data.readback, native_device, native_list.get(), resource, state, std::span(&mip, 1), copy.data_format, &copy, [] {});
         }
         native_list->Close();
         if (!recorded || FAILED(native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy.fence))))
         {
            job->error = "Failed to create the readback resources";
            return false;
         }
         ID3D12CommandList* const lists[] = {native_list.get()};
         copy.queue->ExecuteCommandLists(1, lists);
         const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
         const bool waited = (event && SUCCEEDED(copy.queue->Signal(copy.fence.get(), copy.fence_value)) && SUCCEEDED(copy.fence->SetEventOnCompletion(copy.fence_value, event)) && WaitForSingleObject(event, 2000) == WAIT_OBJECT_0);
         if (event)
         {
            CloseHandle(event);
         }
         if (!waited)
         {
            // "D3D12Readback::CollectRetired()" frees it at a later present, once its fence passed. Its list may still be pending on the queue,
            // so the list and its allocator are leaked rather than released under it (rare: a queue that waits on the game).
            allocator->AddRef();
            native_list->AddRef();
            const std::unique_lock lock(device_data.readback.mutex);
            device_data.readback.retired.push_back(std::move(copy));
            job->error = "The texture copy didn't finish within 2 s: the queue is waiting on something (a fence the game signals later?)";
            return false;
         }
         std::optional<std::vector<char>> bytes = D3D12Readback::TryRead(copy);
         if (!bytes)
         {
            job->error = "The texture copy didn't run";
            return false;
         }
         AddD3D12TextureFile(job, stem, copy, std::move(*bytes));
         return true;
      };

      const std::string* view = job->Arg("view");
      if (job->phase == 0)
      {
         if (view && *view == "swapchain")
         {
            // "OnPresent()" runs the jobs that read the composed back buffer after Luma's display composition
            const bool composed = job->BoolArg("composed", false);
            if (composed && device_data.display_composition.drawn_frame != cb_luma_global_settings.FrameIndex)
            {
               job->error = "Luma's display composition didn't draw at this present: it is off, the output needs none with the current settings and defines, or its shaders are missing (see luma_log)";
               return true;
            }
            // At present the back buffer is in PRESENT (COMMON), after the game's command lists on this queue
            // The back buffer the game drew into (the proxy of a proxied swapchain), which the composition writes
            const com_ptr<ID3D12Resource> game_back_buffer = GetGameBackBuffer(reinterpret_cast<IDXGISwapChain*>(swapchain->get_native()), swapchain->get_current_back_buffer_index());
            if (!read_now(game_back_buffer.get(), D3D12_RESOURCE_STATE_PRESENT, 0, "swapchain"))
               return true;
#if ENABLE_MINHOOK
            job->result.Field("creation", "swapchain");
#endif
            job->result.Field("view", "swapchain").Field("note", composed ? "Read at present, after Luma's display composition and before ReShade's effects" : "Read at present, before Luma's display composition and ReShade's effects");
            return true;
         }

         if (view && view->starts_with("registered:"))
         {
            const std::string name = view->substr(11);
            DevTexture texture;
            {
               const std::lock_guard lock(s_mutex_dev_values);
               const DevValue* dev_value = FindDevValue(name);
               if (!dev_value || dev_value->kind != DevValueKind::Texture)
               {
                  job->error = "Unknown registered texture " + name + " (see luma_dev_values, kind texture)";
                  return true;
               }
               texture = dev_value->texture(device_data);
            }
            if (!texture.resource)
            {
               job->error = name + " is not allocated (its feature is off, or hasn't run yet)";
               return true;
            }
            const uint32_t mip = uint32_t(job->IntArg("mip", 0));
            if (mip >= texture.resource->GetDesc().MipLevels)
            {
               job->error = std::format("Mip {} is out of range ({} mips)", mip, texture.resource->GetDesc().MipLevels);
               return true;
            }
            if (read_now(texture.resource.get(), texture.state, mip, name))
            {
               job->result.Field("view", *view).Field("note", "Read at present: the texture's last content, usually this frame's");
            }
            return true;
         }

         using Kind = D3D12Readback::Kind;
         static const std::unordered_map<std::string, Kind> kinds = {{"rtv", Kind::RenderTarget}, {"srv", Kind::RenderTarget}, {"uav", Kind::UnorderedAccess}, {"depth", Kind::Depth}, {"stencil", Kind::Stencil}};
         const auto kind = kinds.find(view ? *view : "rtv");
         if (kind == kinds.end())
         {
            job->error = "\"view\" must be one of rtv, srv, uav, depth, stencil, swapchain, registered:<name> (D3D12 has no UI texture yet)";
            return true;
         }
         D3D12Readback::Target target = {.kind = kind->second, .slot = uint32_t(job->IntArg("slot", 0, 0, INT32_MAX))};
         std::optional<size_t> index = (job->Arg("index") ? std::optional<size_t>(job->IntArg("index", 0)) : std::nullopt);
         if (kind->first == "srv")
         {
            // Its state at the draw isn't known (any read only combination): read it after the last pass of the capture that wrote it
            if (!index)
            {
               job->error = "D3D12 SRVs are read after the pass that wrote them, pass \"index\" (from luma_trace_list)";
               return true;
            }
            const std::shared_lock lock_trace(cmd_list_data.mutex_trace);
            const std::vector<TraceDrawCallData>& entries = cmd_list_data.trace_draw_calls_data;
            if (trace_running || *index >= entries.size())
            {
               job->error = std::format("Trace index {} is out of range (captured {}), run luma_trace_capture first", *index, entries.size());
               return true;
            }
            const uint32_t srv_slot = target.slot;
            const std::optional<size_t> producer = FindSRVProducer(entries, *index, srv_slot, &target, &job->error);
            if (!producer)
               return true;
            job->result.Field("producer_index", *producer).Field("note", std::format("SRV {} of entry {} read after entry {} wrote it (the capture misses writes from before the captured frame, CopyTiles and DiscardResource)", srv_slot, *index, *producer));
            index = producer;
         }
         if (!ResolveTarget(job, device_data, cmd_list_data, index, &target))
            return true;
         job->readback_target = std::move(target);
         ArmReadback(job, device_data);
         if (job->readback_target->write.resource != 0)
         {
            job->result.Field("written_by", job->stage).Field("resource", job->written_resource);
            if (job->readback_target->write.kind == D3D12Readback::WriteKind::Resolve)
            {
               job->result.Field("resolve_mode", "the game's: re-issued in it (Luma hooks ResolveSubresourceRegion, ReShade's event drops the mode)");
            }
         }
         else
         {
            job->result.Hash("hash", job->shader_hash);
         }
         job->result.Field("instance", job->instance).Field("view", kind->first).Field("slot", job->readback_target->slot);
         job->phase = 1;
         return false;
      }

      D3D12Readback::Copy copy;
      std::vector<char> bytes;
      const ReadbackPoll poll = PollReadback(job, device_data, &copy, &bytes);
      if (poll == ReadbackPoll::Wait)
         return false;
      if (poll == ReadbackPoll::Done)
      {
         // Named after the shader of a draw, or the command and the resource of a copy, resolve or clear
         std::string stem = (job->readback_target->write.resource != 0) ? std::format("{}_{}", job->stage, job->written_resource) : std::format("{:08X}", job->shader_hash);
         std::ranges::replace(stem, ' ', '_');
         AddD3D12TextureFile(job, std::format("{}_{}_{}{}", stem, job->instance, view ? *view : "rtv", job->readback_target->slot), copy, std::move(bytes));
#if ENABLE_MINHOOK
         // How the game created the texture: a back buffer is the swapchain's (ReShade fires no "init_resource" for those, so the map has none),
         // the hooks of "includes/d3d12_resource_creation.h" tag the rest, unknown for a resource they didn't see
         const char* creation = "swapchain";
         {
            const std::shared_lock lock_device(device_data.mutex);
            if (!device_data.back_buffers.contains(copy.texture))
            {
               const std::shared_lock lock_kinds(device_data.texture_kinds.mutex);
               const auto found = device_data.texture_kinds.by_resource.find(copy.texture);
               creation = D3D12ResourceCreation::KIND_NAMES[size_t(found != device_data.texture_kinds.by_resource.end() ? found->second : D3D12ResourceCreation::Kind::Unknown)];
            }
         }
         job->result.Field("creation", creation);
#endif
      }
      return true;
   }

   bool RunReadConstantBuffer(Job* job, DeviceData& device_data, CommandListData& cmd_list_data)
   {
      if (job->phase == 0)
      {
         // "register" (+ "space") picks the entry by its shader register, "slot" by its packed index in the capture, "indirect" reads the commands of an ExecuteIndirect()
         const bool indirect = job->BoolArg("indirect", false);
         const bool by_register = !indirect && job->Arg("register") != nullptr;
         const int64_t slot = (indirect ? 0 : job->IntArg(by_register ? "register" : "slot", 0));
         if (slot < 0 || slot >= (by_register ? INT32_MAX : int64_t(TraceDrawCallData::cbs_size)))
         {
            job->error = by_register ? "\"register\" is out of range" : "\"slot\" is out of range";
            return true;
         }
         D3D12Readback::Target target = {.kind = (indirect ? D3D12Readback::Kind::IndirectArguments : D3D12Readback::Kind::ConstantBuffer), .slot = uint32_t(slot)};
         if (by_register)
         {
            target.cb_register = uint32_t(slot);
            target.cb_space = uint32_t(job->IntArg("space", 0, 0, INT32_MAX));
         }
         const std::optional<size_t> index = (job->Arg("index") ? std::optional<size_t>(job->IntArg("index", 0)) : std::nullopt);
         if (!ResolveTarget(job, device_data, cmd_list_data, index, &target))
            return true;
         job->readback_target = std::move(target);
         ArmReadback(job, device_data);
         job->result.Hash("hash", job->shader_hash).Field("stage", job->stage).Field("instance", job->instance);
         if (!by_register && !indirect)
         {
            job->result.Field("slot", slot);
         }
         job->phase = 1;
         return false;
      }

      D3D12Readback::Copy copy;
      std::vector<char> bytes;
      const ReadbackPoll poll = PollReadback(job, device_data, &copy, &bytes);
      if (poll == ReadbackPoll::Wait)
         return false;
      if (poll == ReadbackPoll::Failed)
         return true;
#if ENABLE_MINHOOK
      // An ExecuteIndirect()'s commands: per command, the 32 bit values of each argument in the signature's order (GPU virtual addresses as two)
      if (copy.indirect_stride != 0)
      {
         auto& w = job->result;
         w.Field("stride", copy.indirect_stride).Field("offset_bytes", copy.buffer_offset).Field("commands_read", copy.indirect_commands).Key("commands").BeginArray();
         for (uint32_t i = 0; i < copy.indirect_commands; i++)
         {
            size_t offset = size_t(i) * copy.indirect_stride;
            w.BeginArray();
            for (const D3D12_INDIRECT_ARGUMENT_DESC& argument : copy.indirect_arguments)
            {
               const size_t size = D3D12CommandSignatures::ArgumentSize(argument);
               w.BeginObject().Field("type", D3D12CommandSignatures::ArgumentName(argument.Type));
               if (size == 0 || offset + size > bytes.size())
               {
                  w.Field("error", "unknown size, the rest of the command isn't read").EndObject();
                  break;
               }
               w.Key("values").BeginArray();
               for (size_t value_offset = offset; value_offset + sizeof(uint32_t) <= offset + size; value_offset += sizeof(uint32_t))
               {
                  uint32_t value = 0;
                  std::memcpy(&value, bytes.data() + value_offset, sizeof(value));
                  w.Value(value);
               }
               w.EndArray().EndObject();
               offset += size;
            }
            w.EndArray();
         }
         w.EndArray();
         return true;
      }
#endif
      const bool is_root_constants = !copy.root_constants.empty();
      if (is_root_constants)
      {
         bytes.assign(reinterpret_cast<const char*>(copy.root_constants.data()), reinterpret_cast<const char*>(copy.root_constants.data() + copy.root_constants.size()));
      }
      const std::string hex = HexDwords(bytes.data(), bytes.size());
      auto& w = job->result;
      // Multi frame series: one read per frame, armed again after each
      const uint32_t frames = uint32_t(job->IntArg("frames", 1, 1, 600));
      if (frames > 1)
      {
         job->series.push_back(std::format("{}:{}", cb_luma_global_settings.FrameIndex, hex));
         if (job->series.size() < frames)
         {
            job->phase = 1;
            job->frames = 0;
            ArmReadback(job, device_data);
            return false;
         }
         w.Key("series").BeginArray();
         for (const std::string& entry : job->series)
         {
            w.Value(entry);
         }
         w.EndArray().Field("series_format", "luma_frame:hex_dwords");
      }
      else
      {
         w.Field("bytes", bytes.size()).Field("hex_dwords", hex);
      }
      w.Field("register", std::format("b{}", copy.register_index)).Field("space", copy.register_space);
      if (is_root_constants)
      {
         w.Field("root_constants", true);
      }
      else
      {
         w.Field("resource", copy.resource).Field("offset_bytes", copy.buffer_offset);
      }
      return true;
   }
#endif // DX11

   bool RunDevValues(Job* job, DeviceData& device_data)
   {
      const std::string* filter = job->Arg("filter");
      const std::lock_guard lock(s_mutex_dev_values);
      auto& w = job->result;
      w.Key("values").BeginArray();
      for (const DevValue& dev_value : dev_values)
      {
         if (filter && dev_value.name.find(*filter) == std::string::npos)
            continue;
         w.BeginObject().Field("name", dev_value.name);
         switch (dev_value.kind)
         {
         case DevValueKind::Toggle:
            w.Field("kind", "toggle").Field("value", *static_cast<const bool*>(dev_value.value));
            break;
         case DevValueKind::Float:
            w.Field("kind", "float").Field("value", *static_cast<const float*>(dev_value.value));
            if (dev_value.min != -FLT_MAX || dev_value.max != FLT_MAX)
               w.Field("min", dev_value.min).Field("max", dev_value.max);
            break;
         case DevValueKind::Int:
            w.Field("kind", "int").Field("value", *static_cast<const int*>(dev_value.value)).Field("min", dev_value.min).Field("max", dev_value.max);
            break;
         case DevValueKind::Counter:
            w.Field("kind", "counter").Field("value", *static_cast<const uint32_t*>(dev_value.value));
            break;
         case DevValueKind::Texture:
         {
            w.Field("kind", "texture");
#if DX11
            const com_ptr<ID3D11Resource> resource = GetTexture(dev_value, device_data);
            const bool allocated = bool(resource);
#else
            const DevTexture texture = dev_value.texture(device_data);
            const bool allocated = bool(texture.resource);
#endif
            w.Field("allocated", allocated);
            if (allocated)
            {
               uint4 size = {};
               DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
#if DX11
               GetResourceInfo(resource.get(), size, format);
#else
               GetResourceInfo(device_data.reshade_device, {reinterpret_cast<uint64_t>(texture.resource.get())}, size, format);
#endif
               w.Format("format", format).Key("size").BeginArray().Value(size.x).Value(size.y).Value(size.z).Value(size.w).EndArray();
            }
            break;
         }
         }
         w.EndObject();
      }
      w.EndArray();
      return true;
   }

   bool RunSetDevValue(Job* job, DeviceData& device_data)
   {
      const std::string* name = job->Arg("name");
      const std::string* value = job->Arg("value");
      if (!name || !value)
      {
         job->error = "Pass \"name\" and \"value\"";
         return true;
      }
      const std::lock_guard lock(s_mutex_dev_values);
      DevValue* dev_value = FindDevValue(*name);
      if (!dev_value)
      {
         job->error = "Unknown dev value " + *name + " (see luma_dev_values)";
         return true;
      }
      // Strict, a typo must not silently become 0 or false
      double parsed = 0.0;
      char* end = nullptr;
      if (dev_value->kind == DevValueKind::Toggle)
      {
         if (*value != "1" && *value != "true" && *value != "0" && *value != "false")
         {
            job->error = *name + " takes 1, 0, true or false";
            return true;
         }
         parsed = (*value == "1" || *value == "true") ? 1.0 : 0.0;
      }
      else if (dev_value->kind == DevValueKind::Float)
         parsed = std::strtof(value->c_str(), &end);
      else if (dev_value->kind == DevValueKind::Int)
         parsed = double(std::strtoll(value->c_str(), &end, 0));
      else
      {
         job->error = "Counters and textures are read only";
         return true;
      }
      if (end && *end != '\0')
      {
         job->error = "\"" + *value + "\" is not a number";
         return true;
      }
      if (parsed < dev_value->min || parsed > dev_value->max)
      {
         job->error = std::format("{} takes {} to {}", *name, dev_value->min, dev_value->max);
         return true;
      }
      job->result.Field("name", *name).Field("value", parsed);
      device_data.cb_luma_global_settings_dirty = true; // For values mirrored into the settings buffer
      if (dev_value->set)
         job->error = dev_value->set(device_data, parsed);
      else if (dev_value->kind == DevValueKind::Toggle)
         *static_cast<bool*>(dev_value->value) = parsed != 0.0;
      else if (dev_value->kind == DevValueKind::Float)
         *static_cast<float*>(dev_value->value) = float(parsed);
      else
         *static_cast<int*>(dev_value->value) = int(parsed);
      return true;
   }

   bool RunSrState(Job* job, DeviceData& device_data)
   {
      auto& w = job->result;
      w.Field("has_drawn_sr", device_data.has_drawn_sr.load()).Field("texture_mip_lod_bias_offset", device_data.texture_mip_lod_bias_offset);
      w.Key("implementations").BeginArray();
#if ENABLE_SR
      const size_t history = size_t(job->IntArg("history", 32, 0, int64_t(sr_history_size))); // Read even without implementations
      for (const auto& [type, implementation] : sr_implementations)
      {
         w.BeginObject().Field("type", SR::GetTypeName(type));
         if (const auto* tap = dynamic_cast<const SrTap*>(implementation.get()))
            tap->Write(&w, history);
         w.EndObject();
      }
      w.EndArray();
      w.Field("sr_type", SR::GetTypeName(device_data.sr_type));
      w.Field("sr_suppressed", device_data.sr_suppressed.load()).Field("force_reset_sr", device_data.force_reset_sr.load());
      w.Field("sr_render_resolution_scale", device_data.sr_render_resolution_scale.load());
#else
      w.EndArray();
      job->read_args.emplace("history");
#endif
      return true;
   }

#if DX11
   bool RunSrCapture(Job* job, ID3D11DeviceContext* native_device_context)
#else
   bool RunSrCapture(Job* job, DeviceData& device_data)
#endif
   {
      if (job->phase == 0)
      {
         {
            const std::lock_guard lock(s_mutex_sr_capture);
            sr_capture.reset();
         }
#if DX12
         {
            const std::unique_lock lock(device_data.readback.mutex);
            D3D12Readback::Retire(&device_data.readback); // The copies of an earlier job
         }
#endif
         sr_capture_armed = true;
         job->phase = 1;
         return false;
      }
      std::optional<SrCapture> captured;
      {
         const std::lock_guard lock(s_mutex_sr_capture);
#if DX11
         captured.swap(sr_capture);
#else
         captured = sr_capture; // Kept until the copies are read, which takes presents
#endif
      }
      const uint32_t max_frames = uint32_t(job->IntArg("max_frames", 30, 1, 3600)); // Read even when the first frame has the capture
      if (!captured)
      {
         if (++job->frames < max_frames)
            return false;
         sr_capture_armed = false;
         job->error = std::format("No super resolution draw in {} frames (off, suppressed, or its pass isn't reached)", job->frames);
         return true;
      }
      auto& w = job->result;
#if DX11
      w.Field("draw_succeeded", captured->draw_succeeded).Key("textures").BeginArray();
      for (const auto& [name, texture] : captured->textures)
      {
         w.BeginObject().Field("name", name);
         std::string error = texture ? "" : "Failed to copy it";
         if (texture)
            ReadTexture(native_device_context, texture.get(), DXGI_FORMAT_UNKNOWN, 0, ReadbackPath(*job, std::string("sr_") + name), job, &error);
         if (!error.empty())
            w.Field("error", error);
         w.EndObject();
      }
      w.EndArray();
#else
      if (!captured->device_data)
      {
         job->error = "The super resolution draw's device isn't one of Luma's";
         const std::lock_guard lock(s_mutex_sr_capture);
         sr_capture.reset();
         return true;
      }
      // The copies run when the game executes the draw's list, which can be some presents later ("D3D12Readback::Advance()" never waits for the queue)
      using Status = D3D12Readback::Copy::Status;
      D3D12Readback::State& readback = captured->device_data->readback;
      const std::unique_lock lock_readback(readback.mutex);
      if (job->phase == 1)
      {
         job->phase = 2;
         job->frames = 0;
      }
      bool pending = false;
      for (D3D12Readback::Capture& capture : readback.captures)
      {
         D3D12Readback::Copy& copy = capture.copy;
         if (copy.status == Status::Executed && !capture.bytes && copy.error.empty())
         {
            std::vector<char> bytes;
            const D3D12Readback::Completion completion = D3D12Readback::Advance(&copy, &bytes);
            if (completion == D3D12Readback::Completion::Ran)
            {
               capture.bytes = std::move(bytes);
            }
            else if (completion == D3D12Readback::Completion::NotRun)
            {
               // Stays executed: its buffer is retired below, the GPU may still write it
               copy.error = "The copy didn't run: the game's predication skipped it, or its command list wasn't submitted";
            }
         }
         else if (copy.status == Status::Discarded)
         {
            copy.status = Status::Failed;
            copy.error = "Its command list was reset without being executed";
         }
         pending |= !capture.bytes && copy.error.empty() && (copy.status == Status::Recorded || copy.status == Status::Executed);
      }
      if (pending && ++job->frames < readback_max_gpu_frames)
         return false;
      w.Field("draw_succeeded", captured->draw_succeeded).Key("textures").BeginArray();
      for (D3D12Readback::Capture& capture : readback.captures)
      {
         w.BeginObject().Field("name", capture.name);
         if (capture.bytes)
         {
            AddD3D12TextureFile(job, std::string("sr_") + capture.name, capture.copy, std::move(*capture.bytes));
         }
         else
         {
            w.Field("error", capture.copy.error.empty() ? std::format("The GPU didn't get to the copy in {} frames: its queue waits for something the game signals later", readback_max_gpu_frames) : capture.copy.error);
         }
         w.EndObject();
      }
      w.EndArray();
      std::erase_if(readback.captures, [](const D3D12Readback::Capture& capture)
         { return capture.bytes.has_value(); });
      D3D12Readback::Retire(&readback); // What the GPU may still write
      const std::lock_guard lock(s_mutex_sr_capture);
      sr_capture.reset();
#endif
      return true;
   }

   bool RunShaderList(Job* job, DeviceData& device_data)
   {
      const std::string* stage = job->Arg("stage");
      const bool replaced_only = job->BoolArg("replaced_only", false);
      // Paged like "trace_list", ~150 B per entry: the default stays under an agent's tool output cap
      const size_t offset = size_t((std::max)(job->IntArg("offset", 0), int64_t(0)));
      const size_t limit = size_t(job->IntArg("limit", 200, 1, 100000));
      const std::shared_lock lock_generic(s_mutex_generic);
      const std::lock_guard lock_dumping(s_mutex_dumping);
      auto& w = job->result;
      size_t matched = 0;
      const auto write_shader = [&](uint32_t hash, const Shader::CachedPipeline* pipeline, size_t pipelines)
      {
         const size_t index = matched++;
         if (index < offset || index >= offset + limit)
            return;
         const auto cached_it = shader_cache.find(hash);
         const auto* cached = cached_it != shader_cache.end() ? cached_it->second : nullptr;
         w.BeginObject().Hash("hash", hash);
         if (pipeline)
            w.Field("stage", StageName(*pipeline, hash)).Field("pipelines", pipelines).Field("replaced", ReplacementName(*pipeline));
         else
            w.Field("live", false);
         if (cached)
         {
            w.Field("version", cached->type_and_version).Field("bytes", cached->size);
#if DX12 // The per slot reflections have no register spaces, and can't read DXIL
            if (cached->found_bindings)
            {
               // As the "HLSL Bind" column of the disassembly, plus the count of arrays
               w.Key("bindings").BeginArray();
               for (const CachedShader::ResourceBinding& binding : cached->bindings)
               {
                  std::string text = std::format("{}{}", binding.type, binding.index);
                  if (binding.space != 0)
                     std::format_to(std::back_inserter(text), ",space{}", binding.space);
                  if (binding.count == UINT32_MAX)
                     text += "[unbounded]";
                  else if (binding.count != 1)
                     std::format_to(std::back_inserter(text), "[{}]", binding.count);
                  w.Value(text);
               }
               w.EndArray();
            }
            if (cached->uses_descriptor_heap)
               w.Field("uses_descriptor_heap", true);
#else
            if (cached->found_reflections)
            {
               const auto write_slots = [&](const char* key, const bool* slots, size_t count)
               {
                  w.Key(key).BeginArray();
                  for (size_t i = 0; i < count; i++)
                     if (slots[i])
                        w.Value(i);
                  w.EndArray();
               };
               write_slots("cbs", cached->cbs, std::size(cached->cbs));
               write_slots("srvs", cached->srvs, std::size(cached->srvs));
               write_slots("uavs", cached->uavs, std::size(cached->uavs));
               write_slots("rtvs", cached->rtvs, std::size(cached->rtvs));
               write_slots("samplers", cached->samplers, std::size(cached->samplers));
            }
#endif
         }
         w.EndObject();
      };
      w.Key("shaders").BeginArray();
      size_t live = 0;
      for (const auto& [hash, pipelines] : device_data.pipeline_caches_by_shader_hash)
      {
         if (pipelines.empty())
            continue;
         live++;
         const auto* pipeline = *pipelines.begin();
         if ((stage && _stricmp(stage->c_str(), StageName(*pipeline, hash)) != 0) || (replaced_only && !IsReplaced(*pipeline)))
            continue;
         write_shader(hash, pipeline, pipelines.size());
      }
      // Loaded at some point but without a live pipeline now (the dump folder can still have them)
      if (job->BoolArg("include_unloaded", false) && !replaced_only)
      {
         for (const auto& [hash, cached] : shader_cache)
         {
            const auto live_it = device_data.pipeline_caches_by_shader_hash.find(hash);
            if (live_it != device_data.pipeline_caches_by_shader_hash.end() && !live_it->second.empty())
               continue;
            if (stage && (cached->type_and_version.size() < 2 || _strnicmp(stage->c_str(), cached->type_and_version.c_str(), 2) != 0))
               continue;
            write_shader(hash, nullptr, 0);
         }
      }
      w.EndArray().Field("live", live).Field("loaded", shader_cache.size());
#if DX12
      w.Field("bindings_note", "A shader's bindings are read at its first frame capture (luma_trace_capture)");
#endif
      w.Field("matched", matched).Field("offset", offset).Field("returned", matched > offset ? (std::min)(matched - offset, limit) : 0);
      return true;
   }

   bool RunDumpShader(Job* job)
   {
      const auto hash = job->HashArg("hash");
      if (!hash)
      {
         job->error = "Pass \"hash\"";
         return true;
      }
      const std::lock_guard lock_dumping(s_mutex_dumping);
      if (!shader_cache.contains(*hash)) // "DumpShader()" expects a known hash
      {
         job->error = std::format("Shader {} was never loaded", Shader::Hash_NumToStr(*hash, true));
         return true;
      }
      DumpShader(*hash);
      // The name "DumpShader()" writes (plus a ".meta" in DEVELOPMENT)
      const std::string& version = shader_cache[*hash]->type_and_version;
      job->result.Path("file", shaders_dump_path / (Shader::Hash_NumToStr(*hash, true) + (version.empty() ? "" : "." + version) + ".cso"));
      return true;
   }

   bool RunShaderState(Job* job, DeviceData& device_data)
   {
      auto& w = job->result;
      const std::shared_lock lock_generic(s_mutex_generic);
      const std::shared_lock lock_loading(s_mutex_loading);
      if (const auto hash = job->HashArg("hash"))
      {
         w.Hash("hash", *hash);
         w.Key("pipelines").BeginArray();
         if (const auto it = device_data.pipeline_caches_by_shader_hash.find(*hash); it != device_data.pipeline_caches_by_shader_hash.end())
         {
            for (const auto* pipeline : it->second)
            {
               w.BeginObject().Field("stage", StageName(*pipeline, *hash)).Field("replaced", ReplacementName(*pipeline)).Field("cloned", pipeline->cloned);
#if DX12
               // How the replacement is built: from the stream the game created the pipeline with ("native") or by ReShade's lossy rebuild of it ("fallback"),
               // the clone bound in place of the pipeline now (null if none), and how many replaced ones are kept for a reload to reuse
               const uint64_t clone = pipeline->active_clone.load(std::memory_order_acquire);
#if ENABLE_MINHOOK
               w.Field("capture", pipeline->capture ? "native" : "fallback");
#else
               w.Field("capture", "fallback");
#endif
               w.Key("clone");
               if (clone != 0)
                  w.Value(std::format("0x{:X}", clone));
               else
                  w.Null();
               w.Field("retired", pipeline->retired_clones.size());
#endif
               w.Field("replace_draw_type", Shader::CachedPipeline::shader_replace_draw_type_names[size_t(pipeline->replace_draw_type)]).EndObject();
            }
         }
         w.EndArray();
         if (const auto* custom_shader = FindCustomShader(*hash))
            WriteCustomShader(&w, *custom_shader, job->BoolArg("disasm", false));
         return true;
      }
      w.Field("compilation_errors", shaders_compilation_errors);
      w.Key("replaced").BeginArray();
      for (const auto& [handle, pipeline] : device_data.pipeline_cache_by_pipeline_handle)
      {
         if (!pipeline || !IsReplaced(*pipeline))
            continue;
#if DX12 // One entry per replaced stage
         for (const uint32_t shader_hash : pipeline->shader_hashes)
         {
            const auto* custom_shader = FindCustomShader(shader_hash);
            if (custom_shader && !custom_shader->code.empty())
               w.BeginObject().Hash("hash", shader_hash).Field("stage", StageName(*pipeline, shader_hash)).Field("replaced", ReplacementName(*pipeline)).Path("file", custom_shader->file_path.filename()).EndObject();
         }
#else
         w.BeginObject().Hash("hash", pipeline->shader_hashes[0]).Field("stage", pipeline->StageName()).Field("replaced", ReplacementName(*pipeline));
         if (const auto* custom_shader = FindCustomShader(pipeline->shader_hashes[0]))
            w.Path("file", custom_shader->file_path.filename());
         w.EndObject();
#endif
      }
      w.EndArray();
      return true;
   }

   bool RunReloadShaders(Job* job, DeviceData& device_data)
   {
      if (job->phase == 0)
      {
         // Like the dev UI's "Reload Shaders" / "Unload Shaders" buttons, processed in "OnReShadePresent"
         ForceToggleShaders(device_data, !job->BoolArg("unload", false));
         job->phase = 1;
         return false;
      }
      if (needs_load_shaders || needs_unload_shaders)
         return false;
      job->result.Field("file_clones", CountFileClones(device_data));
      const std::shared_lock lock_loading(s_mutex_loading);
      job->result.Field("compilation_errors", shaders_compilation_errors);
      return true;
   }

   bool RunGetSettings(Job* job)
   {
      auto& w = job->result;
      {
         const std::shared_lock lock_reshade(s_mutex_reshade);
         const auto& settings = cb_luma_global_settings;
         w.Field("display_mode", display_mode_preset_strings[std::min<size_t>(size_t(settings.DisplayMode), std::size(display_mode_preset_strings) - 1)]);
         w.Field("scene_peak_white", settings.ScenePeakWhite).Field("scene_paper_white", settings.ScenePaperWhite).Field("ui_paper_white", settings.UIPaperWhite);
         w.Field("sr_type", settings.SRType).Field("frame_index", settings.FrameIndex);
         w.Key("dev_settings").BeginArray();
         for (size_t i = 0; i < CB::LumaDevSettings::SettingsNum; i++)
         {
            w.BeginObject().Field("index", i).Field("name", cb_luma_dev_settings_names[i]).Field("value", settings.DevSettings.Settings[i]);
            w.Field("default", cb_luma_dev_settings_default_value.Settings[i]).Field("min", cb_luma_dev_settings_min_value.Settings[i]).Field("max", cb_luma_dev_settings_max_value.Settings[i]).EndObject();
         }
         w.EndArray();
         // The per game struct has no reflection, its layout is in the game's "main.cpp" and shaders
         const float* game_settings = reinterpret_cast<const float*>(&settings.GameSettings);
         w.Key("game_settings").BeginArray();
         for (size_t i = 0; i < sizeof(settings.GameSettings) / sizeof(float); i++)
            w.BeginObject().Field("index", i).Field("float", game_settings[i]).Field("uint", std::bit_cast<uint32_t>(game_settings[i])).EndObject();
         w.EndArray();
      }
      const std::shared_lock lock_defines(s_mutex_shader_defines);
      // Define values are a single character
      const auto value_of = [](const auto& define)
      { return std::string_view(define.GetValue(), strnlen(define.GetValue(), 1)); };
      {
         // Everything the custom shaders were compiled with, Core's own defines included ("GAME_*", "LUMA_*_CB_INDEX"...)
         const std::shared_lock lock_loading(s_mutex_loading);
         w.Key("compile_defines").BeginObject();
         for (size_t i = 0; i + 1 < custom_shaders_compile_defines.size(); i += 2)
            w.Field(custom_shaders_compile_defines[i].c_str(), custom_shaders_compile_defines[i + 1]);
         w.EndObject();
      }
      w.Key("defines").BeginArray();
      for (const auto& define : shader_defines_data)
      {
         if (define.IsEmpty())
            continue;
         w.BeginObject().Field("name", define.editable_data.GetName()).Field("value", value_of(define.editable_data));
         w.Field("compiled", value_of(define.compiled_data)).Field("default", value_of(define.default_data));
         w.Field("editable", define.IsValueEditable());
         if (define.HasTooltip())
            w.Field("tooltip", define.GetTooltip());
         w.EndObject();
      }
      w.EndArray();
      return true;
   }

   bool RunSetSetting(Job* job, DeviceData& device_data)
   {
      const std::string* name = job->Arg("name");
      const std::string* value = job->Arg("value");
      if (!name || !value)
      {
         job->error = "Pass \"name\" and \"value\"";
         return true;
      }
      if (name->starts_with("define:"))
      {
         const std::string define_name = name->substr(7);
         const std::unique_lock lock_defines(s_mutex_shader_defines);
         for (auto& define : shader_defines_data)
         {
            if (define_name != define.editable_data.GetName())
               continue;
            if (!define.IsValueEditable() || value->size() != 1)
            {
               job->error = "The define is not editable, or the value is not a single character";
               return true;
            }
            define.SetValue((*value)[0]);
            job->result.Field("needs_compilation", define.NeedsCompilation());
            return true;
         }
         job->error = "Unknown define " + define_name;
         return true;
      }

      const std::unique_lock lock_reshade(s_mutex_reshade);
      auto& settings = cb_luma_global_settings;
      float* target = nullptr;
      if (*name == "scene_peak_white")
         target = &settings.ScenePeakWhite;
      else if (*name == "scene_paper_white")
         target = &settings.ScenePaperWhite;
      else if (*name == "ui_paper_white")
         target = &settings.UIPaperWhite;
      else if (name->starts_with("dev:") || name->starts_with("game:"))
      {
         const bool dev = name->starts_with("dev:");
         const size_t index = std::strtoul(name->c_str() + (dev ? 4 : 5), nullptr, 0);
         float* base = dev ? settings.DevSettings.Settings : reinterpret_cast<float*>(&settings.GameSettings);
         const size_t count = dev ? CB::LumaDevSettings::SettingsNum : sizeof(settings.GameSettings) / sizeof(float);
         if (index < count)
            target = base + index;
      }
      if (!target)
      {
         job->error = "Unknown setting, use scene_peak_white, scene_paper_white, ui_paper_white, dev:<index>, game:<index> or define:<NAME>";
         return true;
      }
      // "game:" slots can be uints, pass "as_uint=1" to write the raw bits
      *target = job->BoolArg("as_uint", false) ? std::bit_cast<float>(uint32_t(std::strtoul(value->c_str(), nullptr, 0))) : std::strtof(value->c_str(), nullptr);
      device_data.cb_luma_global_settings_dirty = true;
      job->result.Field("value", *target).Field("note", "Not persisted to the config, games that rewrite these every frame will override it");
      return true;
   }

   // Lets a knob change settle (history resets, targets rebuilt) before the bridge reads back again
   bool RunWaitFrames(Job* job)
   {
      if (++job->frames < uint32_t(job->IntArg("frames", 1, 1, 600)))
         return false;
      job->result.Field("frames", job->frames).Field("frame_index", cb_luma_global_settings.FrameIndex);
      return true;
   }

   // Frame times over a window: CPU present to present, and the GPU timestamp span between the same two points (it includes GPU
   // idle time when the game is CPU bound). A frame cap or vsync lowers the GPU clocks and bounds both.
   bool RunPerf(Job* job, DeviceData& device_data, reshade::api::command_queue* queue)
   {
      const uint32_t frames = uint32_t(job->IntArg("frames", 120, 10, 3600));
      const uint32_t settle = uint32_t(job->IntArg("settle", 30, 0, 600));
      if (!job->perf)
         job->perf = std::make_unique<Job::PerfState>();
      auto& perf = *job->perf;
      const auto present = std::chrono::steady_clock::now();
      const bool measuring = job->frames > settle; // The first frame has no previous present
      if (measuring)
         perf.cpu_frame_ms.push_back(std::chrono::duration<double, std::milli>(present - perf.last_present).count());
      perf.last_present = present;
      job->frames++;
#if DX11
      auto* native_device_context = reinterpret_cast<ID3D11DeviceContext*>(queue->get_immediate_command_list()->get_native());
      perf.disjoint += perf.timestamps.Collect(native_device_context, measuring, [&perf](const Perf::TimestampReadback<2>& readback)
         { perf.gpu_frame_ms.push_back(readback.Ms(Perf::FRAME_START, Perf::FRAME_END)); });
      perf.timestamps.Close(native_device_context);
      if (perf.cpu_frame_ms.size() < frames)
      {
         com_ptr<ID3D11Device> native_device;
         native_device_context->GetDevice(&native_device);
         perf.timestamps.Open(native_device.get(), native_device_context);
         return false;
      }
#else
      auto* native_queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
      if (!perf.query_heap)
      {
         auto* native_device = reinterpret_cast<ID3D12Device*>(queue->get_device()->get_native());
         for (com_ptr<ID3D12CommandAllocator>& allocator : perf.allocators)
         {
            native_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
         }
         if (perf.allocators[0] && SUCCEEDED(native_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, perf.allocators[0].get(), nullptr, IID_PPV_ARGS(&perf.list))))
         {
            perf.list->Close();
         }
         const D3D12_QUERY_HEAP_DESC heap_desc = {.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = UINT(Job::PerfState::slots)};
         native_device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&perf.query_heap));
         perf.readback = D3D12Readback::CreateBuffer(native_device, D3D12_HEAP_TYPE_READBACK, Job::PerfState::slots * sizeof(uint64_t));
         native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&perf.fence));
         native_queue->GetTimestampFrequency(&perf.frequency);
      }
      const bool created_allocators = std::ranges::none_of(perf.allocators, [](const com_ptr<ID3D12CommandAllocator>& allocator)
         { return !allocator; });
      if (!perf.query_heap || !perf.readback || !perf.fence || !perf.list || !created_allocators || perf.frequency == 0)
      {
         job->error = "Failed to create the D3D12 timestamp queries";
         return true;
      }
      // The timestamps the GPU passed, in present order
      const uint64_t completed = perf.fence->GetCompletedValue();
      std::vector<size_t> passed;
      for (size_t slot = 0; slot < Job::PerfState::slots; slot++)
      {
         if (perf.slot_fence_values[slot] != 0 && perf.slot_fence_values[slot] <= completed)
         {
            passed.push_back(slot);
         }
      }
      std::sort(passed.begin(), passed.end(), [&perf](size_t a, size_t b)
         { return perf.slot_presents[a] < perf.slot_presents[b]; });
      for (const size_t slot : passed)
      {
         uint64_t ticks = 0;
         const D3D12_RANGE read_range = {.Begin = slot * sizeof(uint64_t), .End = (slot + 1) * sizeof(uint64_t)};
         void* data = nullptr;
         if (SUCCEEDED(perf.readback->Map(0, &read_range, &data)))
         {
            std::memcpy(&ticks, static_cast<const char*>(data) + read_range.Begin, sizeof(ticks));
            const D3D12_RANGE nothing_written = {};
            perf.readback->Unmap(0, &nothing_written);
         }
         if (measuring && perf.last_read && perf.last_read->first + 1 == perf.slot_presents[slot])
         {
            perf.gpu_frame_ms.push_back(1000.0 * double(ticks - perf.last_read->second) / double(perf.frequency));
         }
         perf.last_read = {perf.slot_presents[slot], ticks};
         perf.slot_fence_values[slot] = 0;
      }
      if (perf.cpu_frame_ms.size() < frames)
      {
         // Skipped while the GPU is a whole ring behind, then the pair around it isn't measured
         const size_t slot = perf.presents % Job::PerfState::slots;
         if (perf.slot_fence_values[slot] == 0)
         {
            perf.allocators[slot]->Reset();
            perf.list->Reset(perf.allocators[slot].get(), nullptr);
            perf.list->EndQuery(perf.query_heap.get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(slot));
            perf.list->ResolveQueryData(perf.query_heap.get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(slot), 1, perf.readback.get(), slot * sizeof(uint64_t));
            perf.list->Close();
            ID3D12CommandList* const lists[] = {perf.list.get()};
            native_queue->ExecuteCommandLists(1, lists);
            native_queue->Signal(perf.fence.get(), ++perf.fence_value);
            perf.slot_fence_values[slot] = perf.fence_value;
            perf.slot_presents[slot] = perf.presents;
         }
         perf.presents++;
         return false;
      }
      // The readback buffer is released with the job, after the resolves recorded into it (a null event blocks until then)
      perf.fence->SetEventOnCompletion(perf.fence_value, nullptr);
#endif

      auto& w = job->result;
      // The GPU sets read back a few frames late, so a few less of them
      const auto write_stats = [&w](std::string_view key, std::vector<double>* values)
      {
         w.Key(key).BeginObject().Field("frames", values->size());
         if (!values->empty())
         {
            std::sort(values->begin(), values->end());
            w.Field("median", (*values)[values->size() / 2]).Field("p95", (*values)[values->size() * 95 / 100]).Field("min", values->front()).Field("max", values->back());
         }
         w.EndObject();
      };
      write_stats("cpu_frame_ms", &perf.cpu_frame_ms);
      write_stats("gpu_frame_ms", &perf.gpu_frame_ms);
      w.Field("disjoint", perf.disjoint).Field("settle", settle);
      w.Key("render_resolution").BeginArray().Value(device_data.render_resolution.x).Value(device_data.render_resolution.y).EndArray();
      w.Key("output_resolution").BeginArray().Value(device_data.output_resolution.x).Value(device_data.output_resolution.y).EndArray();
#if ENABLE_SR
      w.Field("sr_type", int(device_data.sr_type_selected.load()));
#endif
      return true;
   }

   bool RunJob(Job* job, DeviceData& device_data, CommandListData& cmd_list_data, reshade::api::command_queue* queue, reshade::api::swapchain* swapchain)
   {
      const std::string& tool = job->tool;
      if (tool == "status")
         return RunStatus(job, device_data);
      if (tool == "wait_frames")
         return RunWaitFrames(job);
      if (tool == "perf")
         return RunPerf(job, device_data, queue);
      DeviceData* traced_device_data = &device_data;
      CommandListData* traced_cmd_list_data = &cmd_list_data;
#if DX12
      // The capture is the one of the device it records (see "trace_device_data"), whose pipelines its entries name: "device" (luma_trace_capture) picks it, for the next captures
      // too. The readbacks at its entries run on the presenting device only.
      if (tool == "trace_capture" || tool == "trace_list" || tool == "trace_get" || ((tool == "read_resource" || tool == "read_cbuffer") && job->Arg("index")))
      {
         const int64_t device_index = ((tool == "trace_capture" && job->phase == 0) ? job->IntArg("device", -1) : -1);
         {
            const std::shared_lock lock_device(s_mutex_device);
            const std::unique_lock lock_trace(s_mutex_trace);
            if (device_index >= int64_t(global_devices_data.size()))
            {
               job->error = std::format("No device {}: the game has {} (luma_status lists them)", device_index, global_devices_data.size());
               return true;
            }
            if (device_index >= 0 && !trace_running && !trace_scheduled)
            {
               trace_device_data = global_devices_data[size_t(device_index)];
            }
            if (trace_device_data)
            {
               traced_device_data = trace_device_data;
               traced_cmd_list_data = &trace_device_data->trace_command_list_data;
            }
         }
         if (traced_device_data != &device_data && (tool == "read_resource" || tool == "read_cbuffer"))
         {
            job->error = "The capture is of another device than the presenting one (luma_trace_capture device), and readbacks only run on the presenting device";
            return true;
         }
      }
#endif
#if DX11
      auto* native_device_context = reinterpret_cast<ID3D11DeviceContext*>(queue->get_immediate_command_list()->get_native());
      if (tool == "read_resource")
         return RunReadResource(job, device_data, cmd_list_data, native_device_context, swapchain);
      if (tool == "read_cbuffer")
         return RunReadConstantBuffer(job, device_data, cmd_list_data, native_device_context);
      if (tool == "sr_capture")
         return RunSrCapture(job, native_device_context);
#else
      if (tool == "read_resource")
         return RunReadResource(job, device_data, cmd_list_data, queue, swapchain);
      if (tool == "read_cbuffer")
         return RunReadConstantBuffer(job, device_data, cmd_list_data);
      if (tool == "sr_capture")
         return RunSrCapture(job, device_data);
#endif
      if (tool == "trace_capture")
         return RunTraceCapture(job, *traced_device_data, *traced_cmd_list_data);
      if (tool == "trace_list")
         return RunTraceList(job, *traced_device_data, *traced_cmd_list_data);
      if (tool == "trace_get")
         return RunTraceGet(job, *traced_device_data, *traced_cmd_list_data);
      if (tool == "shader_state")
         return RunShaderState(job, device_data);
      if (tool == "reload_shaders")
         return RunReloadShaders(job, device_data);
      if (tool == "get_settings")
         return RunGetSettings(job);
      if (tool == "set_setting")
         return RunSetSetting(job, device_data);
      if (tool == "dev_values")
         return RunDevValues(job, device_data);
      if (tool == "set_dev_value")
         return RunSetDevValue(job, device_data);
      if (tool == "sr_state")
         return RunSrState(job, device_data);
      if (tool == "shader_list")
         return RunShaderList(job, device_data);
      if (tool == "dump_shader")
         return RunDumpShader(job);
      job->error = "Unknown tool " + tool;
      return true;
   }

   bool ReadExact(HANDLE pipe, void* data, DWORD size)
   {
      auto* bytes = static_cast<char*>(data);
      while (size > 0)
      {
         DWORD read = 0;
         if (!ReadFile(pipe, bytes, size, &read, nullptr) || read == 0)
            return false;
         bytes += read;
         size -= read;
      }
      return true;
   }

   std::shared_ptr<Job> ParseRequest(const std::string& request)
   {
      auto job = std::make_shared<Job>();
      size_t line_start = 0;
      bool first_line = true;
      while (line_start <= request.size())
      {
         size_t line_end = request.find('\n', line_start);
         if (line_end == std::string::npos)
            line_end = request.size();
         std::string_view line(request.data() + line_start, line_end - line_start);
         if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
         if (first_line)
            job->tool = line;
         else if (const size_t equals = line.find('='); equals != std::string_view::npos)
            job->args.emplace(line.substr(0, equals), line.substr(equals + 1));
         first_line = false;
         line_start = line_end + 1;
      }
      return job;
   }

   // "status" answered by the pipe thread while the game doesn't present (minimized, paused, hung, or a loading screen that doesn't
   // present), when every queued tool would only time out
   std::optional<std::string> StatusWithoutPresents()
   {
      const uint64_t since_present_ms = GetTickCount64() - last_present_ms;
      if (since_present_ms < 1000)
         return std::nullopt;
      JsonWriter w;
      w.BeginObject().Field("ok", true).Field("game", Globals::GAME_NAME).Field("pid", GetCurrentProcessId());
      w.Field("presenting", false).Field("ms_since_present", since_present_ms).Field("frame_index", last_frame_index.load());
      {
         const std::lock_guard lock(s_mutex_jobs);
         w.Field("pending_jobs", pending_jobs.size()).Field("active_tool", active_tool);
      }
      w.Field("note", "Answered without the render thread: the game isn't presenting, the other tools would time out").EndObject();
      return std::move(w.out);
   }

   // One connected client's requests, on its own thread
   void ServeClient(HANDLE pipe)
   {
      connected_clients++;
      const auto write_response = [pipe](const std::string& response)
      {
         const uint32_t response_size = uint32_t(response.size());
         DWORD written = 0;
         return WriteFile(pipe, &response_size, sizeof(response_size), &written, nullptr) && WriteFile(pipe, response.data(), response_size, &written, nullptr) && written == response_size;
      };
      uint32_t size = 0;
      while (!server_stop && ReadExact(pipe, &size, sizeof(size)) && size <= (1u << 20))
      {
         std::string request(size, '\0');
         if (!ReadExact(pipe, request.data(), size))
            break;
         auto job = ParseRequest(request);
         if (job->tool == "status")
         {
            if (const std::optional<std::string> status = StatusWithoutPresents())
            {
               if (!write_response(*status))
                  break;
               continue;
            }
         }
         const DWORD timeout_ms = DWORD(job->IntArg("timeout_ms", 15000, 100, 600000)); // Mirrored by "TIMEOUT_MS" in "Scripts/luma_mcp.py"
         // Readbacks may only land inside "ReadbackRoot()": refused before the job runs, and checked again per file below (the stems come from arguments too)
         // Not "Arg()", so a tool that doesn't take it still reports it in "ignored_args"
         const auto out_dir = job->args.find("out_dir");
         const bool refused = out_dir != job->args.end() && !out_dir->second.empty() && !IsInReadbackRoot(Utf8Path(out_dir->second));
         if (!refused)
         {
            const std::lock_guard lock(s_mutex_jobs);
            pending_jobs.push_back(job);
            has_pending_jobs = true;
         }
         const ULONGLONG start = GetTickCount64();
         bool finished = false;
         while (!refused && !server_stop && !(finished = WaitForSingleObject(job->done, 50) == WAIT_OBJECT_0) && GetTickCount64() - start < timeout_ms)
         {
         }
         // A timed out job that never started is dropped (nobody reads its result), a started one finishes (and restores the dev UI state) on its own
         bool dropped = false;
         std::string running_tool;
         if (!refused && !finished)
         {
            const std::lock_guard lock(s_mutex_jobs);
            dropped = std::erase(pending_jobs, job) != 0;
            has_pending_jobs = !pending_jobs.empty();
            running_tool = active_tool;
         }
         const auto path_error = [](std::string_view error, std::string_view key, const std::filesystem::path& path)
         {
            JsonWriter w;
            w.BeginObject().Field("ok", false).Field("error", error).Path(key, path).EndObject();
            return std::move(w.out);
         };
         const auto timeout_error = [&]
         {
            const std::string_view reason = (dropped ? "it was queued behind an earlier call that is still running, and was dropped" : "the game might not be presenting (minimized or paused?) or the job is waiting for a pass that doesn't draw");
            JsonWriter w;
            w.BeginObject().Field("ok", false).Field("error", std::format("Timed out after {} ms, {}", timeout_ms, reason));
            w.Field("ms_since_present", GetTickCount64() - last_present_ms).Field("active_tool", running_tool).EndObject();
            return std::move(w.out);
         };
         constexpr std::string_view outside_root = "out_dir must be the \"root\" folder or a subfolder of it";
         std::string response = refused ? path_error(outside_root, "root", ReadbackRoot()) : finished ? std::move(job->result.out)
                                                                                                      : timeout_error();
         // Readbacks are written here rather than on the render thread, once the job finished: a timed out one may still add files
         if (finished)
         {
            for (const auto& [path, bytes] : job->files)
            {
               if (!IsInReadbackRoot(path.parent_path()))
               {
                  response = path_error(outside_root, "root", ReadbackRoot());
                  break;
               }
               std::error_code ec;
               std::filesystem::create_directories(path.parent_path(), ec);
               std::ofstream file(path, std::ios::binary | std::ios::trunc);
               if (!file.write(bytes.data(), std::streamsize(bytes.size())))
               {
                  response = path_error("Failed to write", "path", path);
                  break;
               }
            }
         }
         if (!write_response(response))
            break;
      }
      connected_clients--;
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
   }

   void ServerThread()
   {
      // The default pipe security of an elevated game (e.g. a "Run as administrator" compatibility flag) has a high integrity label and no
      // write access for its user, so a non elevated bridge couldn't talk to it: grant the game's user and lower the label to medium
      std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
      HANDLE token = nullptr;
      if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
      {
         alignas(TOKEN_USER) BYTE token_user[256];
         DWORD size = 0;
         LPWSTR user_sid = nullptr;
         if (GetTokenInformation(token, TokenUser, token_user, sizeof(token_user), &size) && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(token_user)->User.Sid, &user_sid))
         {
            sddl += std::format(L"(A;;GA;;;{})", user_sid);
            LocalFree(user_sid);
         }
         CloseHandle(token);
      }
      sddl += L"S:(ML;;NW;;;ME)";
      SECURITY_ATTRIBUTES security_attributes = {sizeof(SECURITY_ATTRIBUTES)};
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security_attributes.lpSecurityDescriptor, nullptr))
         security_attributes.lpSecurityDescriptor = nullptr;

      struct Client
      {
         std::thread thread;
         std::atomic<bool> finished = false;
      };
      std::vector<std::shared_ptr<Client>> clients;
      const auto unfinished = [&clients]
      {
         return std::ranges::any_of(clients, [](const std::shared_ptr<Client>& client)
            { return !client->finished; });
      };
      while (!server_stop)
      {
         // One listening instance at a time, each connected client keeps its own
         HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, &security_attributes);
         if (pipe == INVALID_HANDLE_VALUE)
         {
            Sleep(1000);
            continue;
         }
         if (!(ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) || server_stop)
         {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
         }
         std::erase_if(clients, [](const std::shared_ptr<Client>& client)
            {
               if (!client->finished)
                  return false;
               client->thread.join();
               return true; });
         auto client = std::make_shared<Client>();
         client->thread = std::thread([client, pipe]
            {
               ServeClient(pipe);
               client->finished = true; });
         clients.push_back(std::move(client));
      }
      // Cancelled repeatedly, a client could enter a blocking read right after a cancel
      for (int i = 0; i < 150 && unfinished(); i++)
      {
         for (const auto& client : clients)
         {
            if (!client->finished)
            {
               CancelSynchronousIo(client->thread.native_handle());
            }
         }
         Sleep(10);
      }
      // Joined, as each holds a reference to the add-on module until it has exited (see "Shutdown()"). One still blocked is left
      // behind, and its reference keeps the add-on loaded.
      for (const auto& client : clients)
      {
         if (client->finished)
         {
            client->thread.join();
         }
         else
         {
            client->thread.detach();
         }
      }
      LocalFree(security_attributes.lpSecurityDescriptor);
      server_running = false;
   }
#if !DX11
   // "core.log_vram" (one shot): logs the video memory Luma allocates next to the game, as the device sizes each allocation (the system's
   // per process counters miss what the driver keeps pooled), and the process' usage as DXGI reports it. Needs "s_mutex_dev_values"
   void LogVram(DeviceData& device_data)
   {
      auto* const device = reinterpret_cast<ID3D12Device*>(device_data.native_device);
      const auto allocation_size = [device](ID3D12Resource* resource) -> uint64_t
      {
         if (!resource)
            return 0;
         const D3D12_RESOURCE_DESC desc = resource->GetDesc();
         return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
      };
      constexpr double mb = 1024.0 * 1024.0;
      std::string text = "[VRAM]";
      com_ptr<IDXGIFactory4> factory;
      com_ptr<IDXGIAdapter3> adapter;
      DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
      if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
          SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
      {
         text += std::format(" Process {:.0f} MB of a {:.0f} MB budget.", local.CurrentUsage / mb, local.Budget / mb);
      }
      uint64_t textures_total = 0;
      text += " Luma textures (MB):";
      for (const DevValue& dev_value : dev_values)
      {
         const uint64_t size = (dev_value.kind == DevValueKind::Texture ? allocation_size(dev_value.texture(device_data).resource.get()) : 0);
         if (size != 0)
         {
            textures_total += size;
            text += std::format(" {} {:.1f},", dev_value.name, size / mb);
         }
      }
      {
         const std::unique_lock lock(device_data.display_composition.mutex);
         const uint64_t size = allocation_size(device_data.display_composition.source.get());
         textures_total += size;
         text += std::format(" composition copy {:.1f}; total {:.0f}.", size / mb, textures_total / mb);
      }
#if ENABLE_MINHOOK
      // The clones of the upgraded textures, next to their originals (the game's memory, which stays allocated)
      std::map<DXGI_FORMAT, std::array<uint64_t, 3>> clones_by_format; // Count, clones' size, originals' size
      {
         const std::shared_lock lock(D3D12Clones::registry.mutex);
         for (const auto& [original, clone] : D3D12Clones::registry.by_original)
         {
            std::array<uint64_t, 3>& totals = clones_by_format[clone.format];
            totals[0]++;
            totals[1] += allocation_size(clone.resource.get());
            totals[2] += allocation_size(original);
         }
      }
      uint64_t clones_total = 0;
      text += " Texture upgrade clones (format: count, clones MB / originals MB):";
      for (const auto& [format, totals] : clones_by_format)
      {
         clones_total += totals[1];
         text += std::format(" {}: {}, {:.1f} / {:.1f};", uint32_t(format), totals[0], totals[1] / mb, totals[2] / mb);
      }
      text += std::format(" clones total {:.0f} MB.", clones_total / mb);
#endif
      reshade::log::message(reshade::log::level::info, text.c_str());
   }
#endif // !DX11

   // Called from "Init()": starts the pipe server and registers Core's knobs
   void Start()
   {
      // The live safe Core knobs. Not here: "debug_draw_*" (the readbacks drive them), "trace_ignore_*" ("luma_trace_list" filters
      // instead), and what needs shaders, resources or samplers recreated ("custom_sdr_gamma", "enable_ui_separation", the mip bias).
      RegisterToggles({{"core.hide_ui", &hide_ui}, {"core.force_disable_display_composition", &force_disable_display_composition}, {"core.ignore_upgraded_samplers", &ignore_upgraded_samplers},
         {"core.enable_upgraded_texture_resource_copy_redirection", &enable_upgraded_texture_resource_copy_redirection}});
      RegisterInts({{"core.frame_sleep_ms", &frame_sleep_ms, 0, 100}, {"core.frame_sleep_interval", &frame_sleep_interval, 1, 30}});
#if DX11
      RegisterCounters({{"core.shader_creates_game", &Perf::g_shader_creates.game_count}, {"core.shader_creates_game_ms", &Perf::g_shader_creates.game_ms},
         {"core.shader_creates_game_max_us", &Perf::g_shader_creates.game_max_us}, {"core.shader_creates_luma", &Perf::g_shader_creates.luma_count},
         {"core.shader_creates_luma_ms", &Perf::g_shader_creates.luma_ms}});
#endif
#if ENABLE_FIDELITY_SK && ENABLE_FSR_DEBUG_VIEW
      RegisterToggles({{"core.fsr_debug_view", &FidelityFX::draw_debug_view}});
#endif
#if ENABLE_FIDELITY_SK && DX12 // An index in the versions "FSR: upscaler versions" logs at init, -1 for the runtime's default
      RegisterInts({{"core.fsr_version_index", &FidelityFX::version_index_override, -1, 15}});
#endif
#if ENABLE_SR
      // As the "Super Resolution" combo, not saved to the config: 0 None, 1 Auto, 2 DLSS, 3 FSR, 4 Luma TAA, 5 XeSS
      static_assert(sizeof(SR::UserType) == sizeof(int));
      RegisterInts({{"core.sr_user_type", reinterpret_cast<int*>(&sr_user_type), 0, 5, [](DeviceData& device_data, double value)
         {
            const SR::UserType user_type = SR::UserType(int(value));
            SR::Type type = SR::Type::None;
            switch (user_type)
            {
            case SR::UserType::None: break;
            case SR::UserType::Auto: type = GetSRAutoType(device_data); break;
            case SR::UserType::DLSS: type = SR::Type::DLSS; break;
            case SR::UserType::FSR: type = SR::Type::FSR; break;
            case SR::UserType::LumaTAA: type = SR::Type::LumaTAA; break;
            case SR::UserType::XeSS: type = SR::Type::XeSS; break;
            }
            if (user_type != SR::UserType::None && !device_data.sr_implementations_instances.contains(type))
               return std::string("Not supported on this device");
            const std::unique_lock lock_reshade(s_mutex_reshade);
            sr_user_type = user_type;
            device_data.sr_type_selected = type;
            return std::string(); }}});
#endif
#if DX11
      // "DrawSMAA" and "DrawBloom"'s intermediates, the bloom's mips through "mip"
      RegisterTextures({{"core.smaa_edges", [](DeviceData& device_data) -> ID3D11DeviceChild*
                           { return device_data.managed_resources.shader_resource_views["smaa_edge_detection"_h].get(); }},
         {"core.smaa_weights", [](DeviceData& device_data) -> ID3D11DeviceChild*
            { return device_data.managed_resources.shader_resource_views["smaa_blending_weight_calculation"_h].get(); }},
         {"core.bloom", [](DeviceData&) -> ID3D11DeviceChild*
            { return draw_bloom_mips.srv_mips_y.empty() ? nullptr : draw_bloom_mips.srv_mips_y[0]; }}});
#else
      // The same ones, as the pass layer leaves them between its passes (shader resource states, see "DrawSMAA()" and "DrawBloom()"), with "core.bloom"'s mips through "mip"
      RegisterTextures({
#if ENABLE_D3D12_PASSES && ENABLE_SMAA
         {"core.smaa_edges", [](DeviceData& device_data) -> DevTexture
            {
               const std::shared_lock lock(device_data.smaa.mutex);
               return {reinterpret_cast<ID3D12Resource*>(device_data.smaa.edges.handle), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE};
            }},
         {"core.smaa_weights", [](DeviceData& device_data) -> DevTexture
            {
               const std::shared_lock lock(device_data.smaa.mutex);
               return {reinterpret_cast<ID3D12Resource*>(device_data.smaa.weights.handle), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE};
            }},
#endif
#if ENABLE_D3D12_PASSES && ENABLE_BLOOM
         {"core.bloom", [](DeviceData& device_data) -> DevTexture
            {
               const std::shared_lock lock(device_data.bloom.mutex);
               return {reinterpret_cast<ID3D12Resource*>(device_data.bloom.mips_y.handle), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE};
            }},
#endif
#if ENABLE_SR
         // Where "SR::SuperResolutionImpl::DrawData" has the output while NGX writes it. Nothing transitions it afterwards yet: a D3D12 caller of "Draw()" that does must say so here
         {"core.sr_output", [](DeviceData& device_data) -> DevTexture
            { return {device_data.sr_output_color.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS}; }},
#endif
         // The copy of the view the overlay's debug draw shows ("D3D12Readback::State::debug_copy"), read like the others at present
         {"core.debug_draw", [](DeviceData& device_data) -> DevTexture
            {
               const std::shared_lock lock(device_data.readback.mutex);
               const std::optional<D3D12Readback::Copy>& copy = device_data.readback.debug_copy;
               if (!copy || copy->status != D3D12Readback::Copy::Status::Executed)
                  return {};
               return {copy->debug_texture.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
            }},
      });

      static bool log_vram = false;
      RegisterDevValue({"core.log_vram", DevValueKind::Toggle, &log_vram, nullptr, -DBL_MAX, DBL_MAX, nullptr, [](DeviceData& device_data, double value)
         {
            if (value != 0.0)
            {
               LogVram(device_data);
            }
            return std::string(); }});

      // The overlay's debug draw ("SyncD3D12DebugDraw()"), for the agents that have no overlay: the settings its widgets edit, the entry of the last
      // capture to show (-1: off, it sets the shader, instance and signature like the overlay's buttons) and the registered texture to show in place of
      // a draw's view (the 1st, 2nd... of "luma_dev_values"' textures, 0: none)
      static int debug_draw_capture_index = -1;
      static int debug_draw_registered_index = 0;
      RegisterInts({
         {"core.debug_draw_mode", reinterpret_cast<int*>(&debug_draw_mode), 0, int(DebugDrawMode::Stencil)},
         {"core.debug_draw_slot", &debug_draw_view_index, 0, 7},
         {"core.debug_draw_instance", &debug_draw_pipeline_target_instance, -1, 1000},
         {"core.debug_draw_options", reinterpret_cast<int*>(&debug_draw_options), 0, INT_MAX},
         {"core.debug_draw_index", &debug_draw_capture_index, -1, INT_MAX, [](DeviceData& device_data, double value) -> std::string
            {
               debug_draw_capture_index = int(value);
               if (value < 0)
               {
                  debug_draw_shader_hash = 0;
                  debug_draw_shader_hash_string[0] = 0;
                  return {};
               }
               CommandListData& cmd_list_data = device_data.trace_command_list_data;
               const std::shared_lock lock_generic(s_mutex_generic);
               const std::shared_lock lock_trace(cmd_list_data.mutex_trace);
               const std::vector<TraceDrawCallData>& entries = cmd_list_data.trace_draw_calls_data;
               const size_t index = size_t(value);
               if (trace_running || index >= entries.size())
                  return std::format("Trace index {} is out of range (captured {}), run luma_trace_capture first", index, entries.size());
               const Shader::CachedPipeline* pipeline = (entries[index].type == TraceDrawCallData::TraceDrawCallType::Shader) ? FindPipeline(device_data, entries[index].pipeline_handle) : nullptr;
               if (!pipeline)
                  return std::format("Trace index {} is not a shader pass with a live pipeline", index);
               SetD3D12DebugDrawFromTrace(device_data, entries, index, *pipeline);
               return {};
            }},
         {"core.debug_draw_registered", &debug_draw_registered_index, 0, INT_MAX, [](DeviceData&, double value) -> std::string
            {
               debug_draw_registered_index = int(value);
               debug_draw_registered_texture.clear();
               int remaining = int(value);
               for (const DevValue& dev_value : dev_values) // The caller holds "s_mutex_dev_values"
               {
                  if (dev_value.kind == DevValueKind::Texture && --remaining == 0)
                  {
                     debug_draw_registered_texture = dev_value.name;
                  }
               }
               return (value > 0 && debug_draw_registered_texture.empty()) ? std::string("There are not that many registered textures") : std::string();
            }},
      });
      RegisterCounter("core.debug_draw_copied_frame", &debug_draw_copied_frame);
#endif

      // "Init()" runs again when ReShade re-inits the addon without unloading it (The Witcher creates a probe D3D9 device first):
      // assigning over the first run's joinable thread would call std::terminate. Keep that server, or restart one that has exited.
      if (server_thread.joinable())
      {
         if (server_running)
            return;
         server_thread.join();
      }
      pipe_name = L"\\\\.\\pipe\\luma-mcp-" + std::to_wstring(GetCurrentProcessId());
      server_running = true;
      server_thread = std::thread(ServerThread);
   }

   // Called from "OnPresent" on the render thread, before display composition. "cmd_list_data" holds the frame capture.
   // D3D12: called again after the composition ("after_composition"), where only the job that reads the composed back buffer runs.
   // Ends "active_job", run on "device_data": restores what it changed, completes its result and wakes its client. "s_mutex_active_job" is held.
   void FinishActiveJob(DeviceData& device_data)
   {
      Job& job = *active_job;
#if DX11
      if (job.saved_debug_draw)
      {
         job.saved_debug_draw->Apply();
         device_data.debug_draw_texture = nullptr;
      }
      if (job.saved_track_buffer)
         job.saved_track_buffer->Apply();
#else
      if (job.readback_target)
      {
         // Disarmed if its draw never came, and its copy kept until the GPU is done with it
         const std::unique_lock lock(device_data.readback.mutex);
         device_data.readback.target.reset();
         D3D12Readback::UpdateArmed(device_data.readback);
         D3D12Readback::Retire(&device_data.readback);
      }
#endif
      if (!job.error.empty())
      {
         job.result = {};
         job.result.BeginObject().Field("ok", false).Field("error", job.error);
      }
      // Arguments no tool read (a typo, or a key the backend doesn't know) would otherwise silently fall back to defaults
      std::vector<std::string_view> ignored_args;
      for (const auto& [key, value] : job.args)
         if (!job.read_args.contains(key))
            ignored_args.push_back(key);
      if (!ignored_args.empty())
      {
         job.result.Key("ignored_args").BeginArray();
         for (const std::string_view key : ignored_args)
            job.result.Value(key);
         job.result.EndArray();
      }
      job.result.EndObject();
      SetEvent(job.done);
      active_job.reset();
      has_active_job = false;
      const std::lock_guard lock(s_mutex_jobs);
      active_tool.clear();
   }

   void OnPresent(DeviceData& device_data, CommandListData& cmd_list_data, reshade::api::command_queue* queue, reshade::api::swapchain* swapchain, bool after_composition = false)
   {
      // D3D12: the call after the composition only runs the job that reads the composed back buffer
      if (!after_composition)
      {
         last_present_ms = GetTickCount64();
         last_frame_index = cb_luma_global_settings.FrameIndex;
#if DX12
         D3D12Readback::CollectRetired(&device_data.readback);
#endif
      }
      if (!has_active_job && !has_pending_jobs) // Unlocked early out, every present comes through here
         return;
      // Another presenting thread running the job skips this present (multi frame jobs stay on the device they started on anyway). Not
      // waited for: the job calls the game's device and queue.
      const std::unique_lock lock_active_job(s_mutex_active_job, std::try_to_lock);
      if (!lock_active_job.owns_lock())
         return;
#if DX12
      if (after_composition && (!active_job || !active_job->ReadsComposedBackBuffer()))
         return;
#endif
      if (!active_job)
      {
         const std::lock_guard lock(s_mutex_jobs);
         if (pending_jobs.empty())
            return;
         active_job = pending_jobs.front();
         pending_jobs.erase(pending_jobs.begin());
         has_pending_jobs = !pending_jobs.empty();
         has_active_job = true;
         active_tool = active_job->tool;
      }
      if (active_job->device_data && active_job->device_data != &device_data)
         return;
      active_job->device_data = &device_data;
#if DX12
      // The jobs that read the composed back buffer wait for the call after the composition, the others run in the first one
      if (active_job->ReadsComposedBackBuffer() != after_composition)
         return;
#endif
      if (RunJob(active_job.get(), device_data, cmd_list_data, queue, swapchain))
      {
         FinishActiveJob(device_data);
      }
   }

   // Called from Core's "OnDestroyDevice()" once "trace_device_data" no longer names the device. Fails the multi frame job that runs on it,
   // which would otherwise wait for its presents forever, and waits for a job running on another presenting thread, which can read this
   // device's capture.
   void OnDestroyDevice(DeviceData& device_data)
   {
      const std::lock_guard lock(s_mutex_active_job);
      if (!active_job || active_job->device_data != &device_data)
         return;
      active_job->perf.reset(); // Its queries and command lists are the device's
      active_job->error = "The device the job ran on was destroyed";
      FinishActiveJob(device_data);
   }

   // Called from "Uninit()", before ReShade frees the add-on: a thread holds a reference to the add-on module until it has exited, so a
   // thread still running keeps "FreeLibrary()" from unloading it, and one still exiting unloads it later from its own thread
   void Shutdown()
   {
      if (!server_thread.joinable())
         return;
      server_stop = true;
      // Unblock the pending connection wait by connecting to ourselves; the server thread then cancels its clients' reads
      CancelSynchronousIo(server_thread.native_handle());
      HANDLE self = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (self != INVALID_HANDLE_VALUE)
         CloseHandle(self);
      server_thread.join();
   }
} // namespace Mcp
