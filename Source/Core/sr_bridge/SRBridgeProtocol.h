#pragma once

#include <cstdint>

// The protocol between a 32-bit game's SR bridge ("SRBridge.h") and the x64 "Luma-Upscaler.exe" next to the game's exe
// (Source/Tools/SR Bridge Helper), which runs DLSS, FSR (the FidelityFX API: FSR 4, else 3.1) or XeSS on its own D3D12 device (all x64 only)
// on NT-handle shared textures between two shared fences.
// Text lines over the helper's stdin, its stdout and stderr go to "Luma-Upscaler.log" next to the exe.
//
// Settings: "<render width> <render height> <output width> <output height> <hdr> <inverted depth> <jittered motion vectors>
//   <auto exposure> <dynamic resolution> <motion vector scale x> <motion vector scale y> <DLSS render preset>"
// Start line: "bridge <version> <upscaler> <luid low> <luid high> <settings> <a handle per "Resource" (0: none)> <fence in> <fence out>"
// The helper signals "out" at "kReady" once the upscaler exists (it exits if it can't create it).
// Then a line per message:
// "settings <settings>": the helper recreates the upscaler on the same textures (it exits if it can't).
// "frame <n> <jitter x> <jitter y> <reset> <render width> <render height> <pre exposure> <sharpness> <near> <far> <vertical fov>":
//   the helper waits for "in" n, upscales and signals "out" n (n past "kReady"); it exits if the upscaler failed.
// Any exit after "kReady" first signals "out" UINT64_MAX (no game wait is left hanging; the bridge reads it as an exit).
// EOF ends it.
namespace SRBridgeProtocol
{
   constexpr int kVersion = 6;

   constexpr const wchar_t* kHelperName = L"Luma-Upscaler.exe";
   constexpr const wchar_t* kLogName = L"Luma-Upscaler.log";

   enum Upscaler
   {
      kDlss,
      kFsr,
      kXess,
   };

   // The shared textures, in the order their handles travel. A missing optional one (0) isn't passed to the upscaler.
   enum Resource
   {
      kColor,        // The render resolution color
      kMotion,       // Motion vectors
      kDepth,        // Device depth
      kReactive,     // Optional: reactive / bias mask
      kTransparency, // Optional: FSR's transparency & composition mask
      kExposure,     // Optional: 1x1 exposure
      kOutput,       // The output resolution color (the helper writes it through a UAV)
      kCount
   };

   constexpr uint64_t kReady = 1;

   // The "frame" line's values: what the upscaler needs of a frame besides the shared textures (also the in-process host's "RunFrame()")
   struct Frame
   {
      uint64_t n = 0;
      float jitter_x = 0.f, jitter_y = 0.f;
      int reset = 0;
      uint32_t render_width = 0, render_height = 0;
      float pre_exposure = 0.f, sharpness = -1.f, near_plane = 0.f, far_plane = 0.f, vert_fov = 0.f;
   };
} // namespace SRBridgeProtocol
