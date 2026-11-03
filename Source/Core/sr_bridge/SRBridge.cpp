#include "SRBridge.h"

#if ENABLE_SR_BRIDGE

#include "SRBridgeProtocol.h"
#include "SRBridgeD3D11.h"
#include "../utils/system.h"

#include <include/reshade.hpp>

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>

namespace SRBridge
{
   using Microsoft::WRL::ComPtr;
   using namespace SRBridgeProtocol;

   namespace
   {
      void Log(reshade::log::level level, const std::string& message)
      {
         reshade::log::message(level, ("[SR Bridge] " + message).c_str());
      }

      // The protocol's "<settings>"
      std::string FormatSettings(const SR::SettingsData& settings)
      {
         return std::format("{} {} {} {} {} {} {} {} {} {} {} {}", settings.render_width, settings.render_height, settings.output_width, settings.output_height,
            int(settings.hdr), int(settings.inverted_depth), int(settings.mvs_jittered), int(settings.auto_exposure), int(settings.dynamic_resolution), settings.mvs_x_scale,
            settings.mvs_y_scale, settings.render_preset);
      }
   } // namespace

   struct BridgeInstanceData : public SR::InstanceData, public Link
   {
      SR::Type type = SR::Type::None;
      LUID luid = {};

      // The running helper
      HANDLE job = nullptr; // Ends the helper with the game ("JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE")
      HANDLE process = nullptr;
      HANDLE pipe = nullptr; // Its stdin
      bool settings_pending = false; // Changed since the helper started: sent before the next frame
      bool failed = false;

      ~BridgeInstanceData()
      {
         Stop();
      }

      void Stop();
      bool Fail(const std::string& reason);
      bool Send(const std::string& line);
      bool Start(ID3D11DeviceContext* command_list, ID3D11Resource* const* resources);
   };

   // One helper at a time: another instance that's still running stops here
   BridgeInstanceData* running = nullptr;

   void BridgeInstanceData::Stop()
   {
      if (pipe)
         CloseHandle(pipe); // EOF: the helper ends by itself
      if (process)
      {
         WaitForSingleObject(process, 500);
         CloseHandle(process);
      }
      if (job)
         CloseHandle(job); // Ends it if it's still running
      job = process = pipe = nullptr;
      Reset();
      settings_pending = false;
      if (running == this)
         running = nullptr;
   }

   bool BridgeInstanceData::Fail(const std::string& reason)
   {
      Stop();
      failed = true;
      Log(reshade::log::level::warning, reason);
      return false;
   }

   // False (failed) if the pipe broke
   bool BridgeInstanceData::Send(const std::string& line)
   {
      DWORD written = 0;
      return WriteFile(pipe, line.data(), DWORD(line.size()), &written, nullptr) || Fail("the helper's pipe broke");
   }

   bool BridgeInstanceData::Start(ID3D11DeviceContext* command_list, ID3D11Resource* const* resources)
   {
      if (running && running != this)
         running->Stop();
      Stop();
      HANDLE handles[kCount + 2] = {}; // The textures', then the fences'
      const auto fail = [&](const std::string& reason)
      {
         for (HANDLE handle : handles)
            if (handle)
               CloseHandle(handle);
         return Fail(reason);
      };
      if (const std::string error = Open(command_list, resources, handles); !error.empty())
         return fail(error);

      job = CreateJobObjectW(nullptr, nullptr);
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
         return fail("no job object");

      // The helper next to the exe: its stdin a pipe, its output the log file there
      const std::filesystem::path directory = System::GetModulePath().parent_path();
      const std::filesystem::path helper_path = directory / kHelperName;
      SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
      HANDLE pipe_read = nullptr;
      const HANDLE log_file = CreateFileW((directory / kLogName).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, 0, nullptr);
      PROCESS_INFORMATION process_info = {};
      bool started = false;
      if (log_file != INVALID_HANDLE_VALUE && CreatePipe(&pipe_read, &pipe, &inherit, 0))
      {
         SetHandleInformation(pipe, HANDLE_FLAG_INHERIT, 0);
         STARTUPINFOW startup = {sizeof(startup)};
         startup.dwFlags = STARTF_USESTDHANDLES;
         startup.hStdInput = pipe_read;
         startup.hStdOutput = startup.hStdError = log_file;
         std::wstring command = L"\"" + helper_path.wstring() + L"\"";
         // Suspended until it's in the job, so it can't outlive the game even if it's killed right away
         started = CreateProcessW(helper_path.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory.c_str(), &startup, &process_info);
         if (started)
         {
            process = process_info.hProcess;
            started = AssignProcessToJobObject(job, process) && ResumeThread(process_info.hThread) != DWORD(-1);
            CloseHandle(process_info.hThread);
         }
      }
      const DWORD error = GetLastError();
      if (!started && process)
         TerminateProcess(process, 0); // Created, but not in the job
      for (HANDLE handle : {pipe_read, log_file})
         if (handle && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
      if (!started)
         return fail(std::format("the helper couldn't start (error {}): {}", error, helper_path.string()));

      std::string message =
         std::format("bridge {} {} {} {} {}", kVersion, type == SR::Type::DLSS ? int(kDlss) : (type == SR::Type::XeSS ? int(kXess) : int(kFsr)), luid.LowPart, luid.HighPart, FormatSettings(settings_data));
      for (HANDLE handle : handles)
      {
         HANDLE remote = nullptr;
         if (handle)
            DuplicateHandle(GetCurrentProcess(), handle, process, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS);
         message += std::format(" {}", uint64_t(uintptr_t(remote)));
         if (handle)
            CloseHandle(handle);
      }
      message += "\n";
      if (!Send(message))
         return false;
      running = this;
      Log(reshade::log::level::info, std::format("started the helper for {} ({}x{} -> {}x{})", SR::GetTypeName(type), settings_data.render_width,
                                        settings_data.render_height, settings_data.output_width, settings_data.output_height));
      return true;
   }

   bool Bridge::HasInit(const SR::InstanceData* data) const
   {
      return data != nullptr;
   }

   bool Bridge::IsSupported(const SR::InstanceData* data) const
   {
      return data && data->is_supported;
   }

   // No process yet (Core initializes every type at device creation): supported if the helper (and for DLSS, NVIDIA's dll and GPU)
   // is there
   bool Bridge::Init(SR::InstanceData*& data, ID3D11Device* device, IDXGIAdapter* adapter)
   {
      if (data)
         Deinit(data);
      auto* const custom_data = new BridgeInstanceData();
      data = custom_data;
      custom_data->type = type;
      custom_data->min_resolution = 32;
      custom_data->requires_unordered_access_output_texture = false; // The helper writes its own copy
      if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&custom_data->device))))
         return false;
      DXGI_ADAPTER_DESC adapter_desc = {};
      if (!adapter || FAILED(adapter->GetDesc(&adapter_desc)))
         return false;
      custom_data->luid = adapter_desc.AdapterLuid;
      const std::filesystem::path directory = System::GetModulePath().parent_path();
      std::error_code error;
      bool files_present = false;
      switch (type)
      {
      case SR::Type::DLSS:
         files_present = adapter_desc.VendorId == 0x10DE && std::filesystem::exists(directory / L"nvngx_dlss.dll", error);
         break;
      case SR::Type::FSR:
         files_present = std::filesystem::exists(directory / L"amd_fidelityfx_loader_dx12.dll", error) && std::filesystem::exists(directory / L"amd_fidelityfx_upscaler_dx12.dll", error);
         break;
      case SR::Type::XeSS:
         files_present = std::filesystem::exists(directory / L"libxess.dll", error);
         break;
      }
      custom_data->is_supported = files_present && std::filesystem::exists(directory / kHelperName, error);
      return custom_data->is_supported;
   }

   void Bridge::Deinit(SR::InstanceData*& data, ID3D11Device* optional_device)
   {
      delete static_cast<BridgeInstanceData*>(data);
      data = nullptr;
   }

   void Bridge::ReleaseResources(SR::InstanceData* data)
   {
      if (auto* const bridge = static_cast<BridgeInstanceData*>(data))
      {
         bridge->Stop();
         // A new pick starts over after a failure (games stop drawing a failed upscaler until it's picked again)
         bridge->failed = false;
      }
   }

   // The helper starts on the next draw with these, or a running one recreates the upscaler with them
   bool Bridge::UpdateSettings(SR::InstanceData* data, ID3D11DeviceContext* command_list, const SR::SettingsData& settings_data)
   {
      auto* const custom_data = static_cast<BridgeInstanceData*>(data);
      if (!custom_data || !custom_data->is_supported || !command_list)
         return false;
      if (settings_data == custom_data->settings_data)
         return !custom_data->failed;
      custom_data->settings_data = settings_data;
      custom_data->settings_pending = custom_data->process != nullptr;
      custom_data->failed = false;
      return true;
   }

   bool Bridge::Draw(const SR::InstanceData* data, ID3D11DeviceContext* command_list, const DrawData& draw_data)
   {
      auto& custom_data = *const_cast<BridgeInstanceData*>(static_cast<const BridgeInstanceData*>(data));
      // Until picked again ("ReleaseResources") or new settings
      if (custom_data.failed)
         return false;
      ID3D11Resource* const resources[kCount] = {draw_data.source_color, draw_data.motion_vectors, draw_data.depth_buffer, draw_data.bias_mask, draw_data.transparency_alpha,
         draw_data.exposure, draw_data.output_color};
      if (custom_data.process)
      {
         // A dead process's fences read UINT64_MAX, so no GPU wait is left hanging
         if (WaitForSingleObject(custom_data.process, 0) == WAIT_OBJECT_0 || custom_data.fences[1]->GetCompletedValue() == UINT64_MAX)
            return custom_data.Fail(std::format("the helper exited (see {} next to the game's exe)", std::filesystem::path(kLogName).string()));
         if (const int changed = custom_data.FindNewInput(resources); changed >= 0)
         {
            // Recreated: the helper restarts on them (named, so a game that alternates an input is seen as a restart loop's cause)
            Log(reshade::log::level::info, std::format("restarting the helper: a new {} texture", Link::names[changed]));
            custom_data.Stop();
         }
      }
      if (!custom_data.process && !custom_data.Start(command_list, resources))
         return false;

      if (custom_data.frame == 0)
      {
         if (!custom_data.IsReadySignaled())
         {
            // Still creating the upscaler
            custom_data.CopyColorToOutput(command_list, draw_data);
            return true;
         }
         custom_data.frame = kReady;
         Log(reshade::log::level::info, std::format("running {}", SR::GetTypeName(custom_data.type)));
      }
      if (custom_data.settings_pending)
      {
         // Once ready, so changes while it's starting become one
         if (!custom_data.Send("settings " + FormatSettings(custom_data.settings_data) + "\n"))
            return false;
         custom_data.settings_pending = false;
         Log(reshade::log::level::info, "new settings for the helper: " + FormatSettings(custom_data.settings_data));
      }

      const UINT render_width = (draw_data.render_width ? draw_data.render_width : custom_data.settings_data.render_width);
      const UINT render_height = (draw_data.render_height ? draw_data.render_height : custom_data.settings_data.render_height);
      const uint64_t frame = custom_data.Submit(command_list, resources, render_width, render_height);
      if (!custom_data.Send(std::format("frame {} {} {} {} {} {} {} {} {} {} {}\n", frame, draw_data.jitter_x, draw_data.jitter_y, draw_data.reset ? 1 : 0, render_width,
             render_height, draw_data.pre_exposure, draw_data.user_sharpness, draw_data.near_plane, draw_data.far_plane, draw_data.vert_fov)))
         return false;
      SR_BRIDGE_PROFILE(command_list, 3);
      custom_data.WaitOutput(command_list, draw_data.output_color, frame);
      return true;
   }

   bool Bridge::IsReady(const SR::InstanceData* data) const
   {
      return data && static_cast<const BridgeInstanceData*>(data)->frame != 0;
   }

   float Bridge::GetMipLODBias(const SR::InstanceData* data) const
   {
      const SR::SettingsData& settings = data->settings_data;
      return type == SR::Type::XeSS ? SR::GetXeSSMipLODBias(settings.render_height, settings.output_height) : SR::GetMipLODBias(settings.render_height, settings.output_height);
   }

   // Each upscaler's own formula (as Core's DLSS and FSR)
   int Bridge::GetJitterPhases(const SR::InstanceData* data) const
   {
      const SR::SettingsData& settings = data->settings_data;
      return type == SR::Type::DLSS ? SR::GetDlssJitterPhases(settings.render_height, settings.output_height) : SR::GetFsrJitterPhases(settings.render_width, settings.output_width);
   }
} // namespace SRBridge

#endif
