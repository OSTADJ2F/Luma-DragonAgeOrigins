#pragma once

#include "../includes/super_resolution.h"

// D3D11: FSR 3.1 from the statically linked FidelityFX SDK 1.x. D3D12: the FidelityFX API of AMD's signed dlls (FSR 4, or FSR 3.1 where FSR 4 isn't supported).
#if defined(_WIN64) && ((DX12 && __has_include("api/include/ffx_api.h")) || (!DX12 && __has_include("FidelityFX/host/ffx_fsr3.h")))
#ifndef ENABLE_FIDELITY_SK
#define ENABLE_FIDELITY_SK 1
#endif // ENABLE_FIDELITY_SK
#elif defined(ENABLE_FIDELITY_SK)
#undef ENABLE_FIDELITY_SK
#define ENABLE_FIDELITY_SK 0
#endif

#if ENABLE_FIDELITY_SK

// On D3D11 the view's shaders only exist in the debug FFX libs
#if DEVELOPMENT && (DX12 || !defined(NDEBUG))
#define ENABLE_FSR_DEBUG_VIEW 1
#else
#define ENABLE_FSR_DEBUG_VIEW 0
#endif

#if DX12
namespace FidelityFX::inline D3D12
#else
namespace FidelityFX
#endif
{
#if DX12
   // Index in the upscaler versions the runtime lists for the device (logged at init), -1 for its default. Applies at the next context creation.
   inline int version_index_override = -1; // "core.fsr_version_index"
#endif
#if ENABLE_FSR_DEBUG_VIEW
   inline bool draw_debug_view = !DX12; // "core.fsr_debug_view" on D3D12
#endif

   // FSR 3 SR on D3D11, FSR 4 (or the newest version the GPU supports) on D3D12
   class FSR : public SR::SuperResolutionImpl
   {
   public:
#if !DX12
      virtual bool HasInit(const SR::InstanceData* data) const override;
      virtual bool IsSupported(const SR::InstanceData* data) const override;
#endif

      virtual bool Init(SR::InstanceData*& data, SR::Device* device, IDXGIAdapter* adapter = nullptr) override;
      virtual void Deinit(SR::InstanceData*& data, SR::Device* optional_device = nullptr) override;
      virtual void ReleaseResources(SR::InstanceData* data) override;

      virtual bool UpdateSettings(SR::InstanceData* data, SR::CommandList* command_list, const SR::SettingsData& settings_data) override;

      virtual bool Draw(const SR::InstanceData* data, SR::CommandList* command_list, const DrawData& draw_data) override;

      virtual int GetJitterPhases(const SR::InstanceData* data) const override;

#if DX12
      virtual void ReleaseRetired(SR::InstanceData* data) override;
#endif

      virtual bool NeedsStateRestoration() const override
      {
         return true;
      }
   };
} // namespace FidelityFX

#endif
