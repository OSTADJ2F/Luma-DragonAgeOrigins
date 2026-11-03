// x64 helper for 32-bit Luma mods: DLSS (NGX), FSR (FidelityFX API) and XeSS are x64 only, so the game's SR bridge
// ("Source/Core/sr_bridge/SRBridge.cpp") starts this next to the game's exe and hands it NT-handle shared textures and
// two shared fences. It runs Core's D3D12 upscalers (Source/Core/dlss, fsr, xess) on its own device of the same adapter, through
// Core's "D3D12SRRunner.h". Protocol: "SRBridgeProtocol.h".
#include "../../Core/sr_bridge/D3D12SRRunner.h"

#include "../../Core/dlss/DLSS.h"
#include "../../Core/fsr/FSR.h"
#include "../../Core/xess/XeSS.h"

#include <iostream>
#include <memory>
#include <string>

using namespace SRBridgeProtocol;

// Core logs through ReShade, which isn't loaded here (the helper is built with RESHADE_API_LIBRARY_EXPORT and BUILTIN_ADDON): the log is stdout
extern "C" __declspec(dllexport) void ReShadeLogMessage(void* module, int level, const char* message)
{
   printf("[Luma Upscaler] %s\n", message);
   fflush(stdout);
}

namespace
{
   struct Settings
   {
      uint32_t render_width = 0, render_height = 0, output_width = 0, output_height = 0;
      int hdr = 1, inverted_depth = 0, mvs_jittered = 0, auto_exposure = 0, dynamic_resolution = 0;
      float mvs_x_scale = 1.f, mvs_y_scale = 1.f;
      int render_preset = 0;

      SR::SettingsData ToSr() const
      {
         return {
            .output_width = output_width,
            .output_height = output_height,
            .render_width = render_width,
            .render_height = render_height,
            .dynamic_resolution = dynamic_resolution != 0,
            .hdr = hdr != 0,
            .inverted_depth = inverted_depth != 0,
            .mvs_jittered = mvs_jittered != 0,
            .mvs_x_scale = mvs_x_scale,
            .mvs_y_scale = mvs_y_scale,
            .auto_exposure = auto_exposure != 0,
            .render_preset = unsigned(render_preset),
         };
      }
   };

   // The protocol's "<settings>"
   std::istream& operator>>(std::istream& stream, Settings& settings)
   {
      return stream >> settings.render_width >> settings.render_height >> settings.output_width >> settings.output_height >> settings.hdr >> settings.inverted_depth >>
             settings.mvs_jittered >> settings.auto_exposure >> settings.dynamic_resolution >> settings.mvs_x_scale >> settings.mvs_y_scale >> settings.render_preset;
   }
} // namespace

int main()
{
   // Only this reads stdin
   std::ios::sync_with_stdio(false);
   std::cin.tie(nullptr);
   std::string mode;
   int version = 0, upscaler = kDlss;
   Settings settings;
   LUID luid = {};
   uint64_t handles[kCount] = {}, fence_handles[2] = {};
   std::cin >> mode >> version;
   if (mode != "bridge" || version != kVersion)
   {
      return printf("[Luma Upscaler] FAIL protocol \"%s\" %d, expected \"bridge\" %d\n", mode.c_str(), version, kVersion), 1;
   }
   std::cin >> upscaler >> luid.LowPart >> luid.HighPart >> settings;
   for (uint64_t& handle : handles)
   {
      std::cin >> handle;
   }
   std::cin >> fence_handles[0] >> fence_handles[1];
   if (!std::cin)
   {
      return printf("[Luma Upscaler] FAIL bad start line\n"), 1;
   }

   const SRRunner::ComPtr<IDXGIAdapter1> adapter = SRRunner::FindAdapter(luid);
   if (!adapter)
   {
      return printf("[Luma Upscaler] FAIL no adapter with the game's LUID\n"), 1;
   }
   SRRunner::Runner runner;
   if (!runner.gpu.Init(adapter.Get()))
   {
      return printf("[Luma Upscaler] FAIL device\n"), 1;
   }

   // Shared textures and fences of the game's D3D11 device, opened on this one
   if (const std::string error = runner.OpenInputs(handles, fence_handles); !error.empty())
   {
      return printf("[Luma Upscaler] FAIL %s\n", error.c_str()), 1;
   }

   std::unique_ptr<SR::D3D12::SuperResolutionImpl> upscaler_impl;
   SR::Type type = SR::Type::DLSS;
   switch (upscaler)
   {
   case kDlss:
      upscaler_impl = std::make_unique<NGX::DLSS>();
      break;
   case kFsr:
      type = SR::Type::FSR;
      upscaler_impl = std::make_unique<FidelityFX::FSR>();
      break;
   case kXess:
      type = SR::Type::XeSS;
      upscaler_impl = std::make_unique<Intel::XeSS>();
      break;
   default:
      return printf("[Luma Upscaler] FAIL unknown upscaler %d\n", upscaler), 1;
   }

   // Ends the run: waits for the GPU, then releases the upscaler (it expects that also when its init failed)
   const auto quit = [&](int exit_code)
   {
      runner.Quit();
      return exit_code;
   };
   if (!runner.Create(std::move(upscaler_impl), adapter.Get(), settings.ToSr()))
   {
      return printf("[Luma Upscaler] FAIL %s init or create\n", SR::GetTypeName(type)), quit(1);
   }
   const char* version_name = runner.data->version_name.load();
   printf("[Luma Upscaler] ready: %s%s%s %ux%u -> %ux%u\n", SR::GetTypeName(type), version_name ? " " : "", version_name ? version_name : "", settings.render_width, settings.render_height,
      settings.output_width, settings.output_height);
   fflush(stdout);

   char profile_path[MAX_PATH] = {};
   if (GetEnvironmentVariableA("LUMA_UPSCALER_PROFILE", profile_path, MAX_PATH))
   {
      runner.StartProfile(profile_path);
   }

   SRRunner::Frame frame;
   uint32_t frames = 0;
   std::string message;
   while (std::cin >> message)
   {
      if (message == "settings")
      {
         if (!(std::cin >> settings))
         {
            runner.ReleaseGame();
            printf("[Luma Upscaler] FAIL bad settings line\n");
            return quit(1);
         }
         if (!runner.UpdateSettings(settings.ToSr()))
         {
            runner.ReleaseGame();
            return quit(1);
         }
         printf("[Luma Upscaler] recreated: %ux%u -> %ux%u, preset %d\n", settings.render_width, settings.render_height, settings.output_width, settings.output_height,
            settings.render_preset);
         fflush(stdout);
         continue;
      }
      if (message != "frame" || !(std::cin >> frame.n >> frame.jitter_x >> frame.jitter_y >> frame.reset >> frame.render_width >> frame.render_height >> frame.pre_exposure >>
                                   frame.sharpness >> frame.near_plane >> frame.far_plane >> frame.vert_fov))
      {
         runner.ReleaseGame();
         printf("[Luma Upscaler] FAIL bad line \"%s\"\n", message.c_str());
         return quit(1);
      }
      const bool drawn = runner.RunFrame(frame);
      frames++;
      // Exiting tells the game: its bridge then fails like an in-process upscaler and the game falls back, instead of showing the
      // last good output as its frames
      if (!drawn)
      {
         runner.ReleaseGame();
         runner.FinishProfile();
         printf("[Luma Upscaler] FAIL frame %llu: the upscaler failed\n", (unsigned long long)frame.n);
         return quit(1);
      }
   }
   printf("[Luma Upscaler] the game closed the pipe after %u frames\n", frames);
   runner.FinishProfile();
   return quit(0);
}
