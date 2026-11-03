#pragma once

#include "../includes/super_resolution.h"

// D3D12 only: XeSS on D3D11 runs on Intel Arc GPUs only, on D3D12 on any GPU with SM 6.4 and DP4a
#if defined(_WIN64) && DX12 && __has_include("xess/xess_d3d12.h")
#ifndef ENABLE_XESS
#define ENABLE_XESS 1
#endif // ENABLE_XESS
#elif defined(ENABLE_XESS)
#undef ENABLE_XESS
#define ENABLE_XESS 0
#endif

#if ENABLE_XESS

#if DX12
namespace Intel::inline D3D12
#else
namespace Intel
#endif
{
   // XeSS SR, from Intel's signed "libxess.dll" (loaded at runtime)
   class XeSS : public SR::SuperResolutionImpl
   {
   public:
      virtual bool Init(SR::InstanceData*& data, SR::Device* device, IDXGIAdapter* adapter = nullptr) override;
      virtual void Deinit(SR::InstanceData*& data, SR::Device* optional_device = nullptr) override;
      virtual void ReleaseResources(SR::InstanceData* data) override;
      virtual void ReleaseRetired(SR::InstanceData* data) override;

      virtual bool UpdateSettings(SR::InstanceData* data, SR::CommandList* command_list, const SR::SettingsData& settings_data) override;

      virtual bool Draw(const SR::InstanceData* data, SR::CommandList* command_list, const DrawData& draw_data) override;

      virtual int GetJitterPhases(const SR::InstanceData* data) const override;
      virtual float GetMipLODBias(const SR::InstanceData* data) const override;

      virtual bool NeedsStateRestoration() const override
      {
         return true;
      }
   };
} // namespace Intel

#endif
