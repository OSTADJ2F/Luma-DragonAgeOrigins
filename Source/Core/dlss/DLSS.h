#pragma once

#include "../includes/super_resolution.h"

#if defined(_WIN64) && __has_include("nvsdk_ngx.h")
#ifndef ENABLE_NGX
#define ENABLE_NGX 1
#endif // ENABLE_NGX
#else
#undef ENABLE_NGX
#define ENABLE_NGX 0
#endif

#if ENABLE_NGX

#if DX12
namespace NGX::inline D3D12
#else
namespace NGX
#endif
{
	// DLSS SR
	class DLSS : public SR::SuperResolutionImpl
	{
	public:
		virtual bool Init(SR::InstanceData*& data, SR::Device* device, IDXGIAdapter* adapter = nullptr) override;
		virtual void Deinit(SR::InstanceData*& data, SR::Device* optional_device = nullptr) override;
		virtual void ReleaseResources(SR::InstanceData* data) override;

		virtual bool UpdateSettings(SR::InstanceData* data, SR::CommandList* command_list, const SR::SettingsData& settings_data) override;

		virtual bool Draw(const SR::InstanceData* data, SR::CommandList* command_list, const DrawData& draw_data) override;
		
		virtual int GetJitterPhases(const SR::InstanceData* data) const override;

#if DX12
		virtual void ReleaseRetired(SR::InstanceData* data) override;

		// The D3D12 evaluation (and creation) binds NGX's own descriptor heaps, root signature and pipeline on the command list
		virtual bool NeedsStateRestoration() const override { return true; }
#endif
	};
}

#endif