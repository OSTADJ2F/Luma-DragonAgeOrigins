#pragma once

#include "../includes/super_resolution.h"
#include "SRBridgeProtocol.h"

#include <Windows.h>

#include <memory>
#include <string>

// The D3D12 super resolution implementations of an x64 D3D11 mod (DLSS, FSR 4 / 3.1, XeSS), built by "unity_build_d3d12.cpp" (project opt-in
// "UseLumaD3D12SR", which defines ENABLE_D3D12_SR). A D3D11 translation unit includes only this: "SR::D3D12::SuperResolutionImpl" is the
// same interface as "SR::SuperResolutionImpl" with ID3D12* arguments (only forward declared here, no d3d12.h needed), so it can own and call
// the implementations, while their classes stay private to the D3D12 translation unit (distinct inline namespaces, no ODR clashes).
// The caller provides the D3D12 objects, or uses the in-process host below ("D3D12SRLocal.h" does).
#if ENABLE_D3D12_SR
namespace SR::D3D12
{
   // nullptr if "type" isn't built into this mod (DLSS, FSR and XeSS are)
   std::unique_ptr<SuperResolutionImpl> CreateSuperResolution(Type type);

   // The D3D12 half of the in-process SR bridge ("D3D12SRLocal.h"): an implementation on its own D3D12 device and queue (the runner of
   // "D3D12SRRunner.h"), working on the textures and fences the game's D3D11 device shares by NT handle. Everything runs on the caller's thread.
   class LocalHost
   {
   public:
      virtual ~LocalHost() = default;
      // Creates the upscaler's objects again for the settings, on the same textures. False if it can't.
      virtual bool UpdateSettings(const SettingsData& settings) = 0;
      // The GPU waits for the D3D11 side's "in" fence to reach "frame.n", upscales and signals "out" n. False if the upscaler failed (it
      // still signals, so no D3D11 wait is left hanging).
      virtual bool RunFrame(const SRBridgeProtocol::Frame& frame) = 0;
      virtual int GetJitterPhases() const = 0;
      virtual float GetMipLODBias() const = 0;
      // The upscaler's version for display, owned by the host, nullptr if unknown
      virtual const char* GetVersionName() const = 0;
   };

   // At the device's teardown, after every host is gone: frees the D3D12 device the hosts share
   void ReleaseProcessState();

   // Opens the shared textures ("SRBridgeProtocol::Resource" order, 0: none) and the fences "in", "out" on a D3D12 device of the adapter
   // "luid", creates the upscaler and signals "out" at "SRBridgeProtocol::kReady" (the handles stay open: the caller closes them).
   // nullptr and the reason in "error" if any of it fails.
   std::unique_ptr<LocalHost> CreateLocalHost(Type type, const LUID& luid, const SettingsData& settings, const uint64_t (&texture_handles)[SRBridgeProtocol::kCount],
      const uint64_t (&fence_handles)[2], std::string* error);
} // namespace SR::D3D12
#endif
