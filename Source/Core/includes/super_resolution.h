#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGIAdapter;

namespace SR
{
	// The objects of an API. DLSS, FSR and XeSS have a D3D12 path, which a D3D11 build can host too (its D3D12 implementations live in
	// "unity_build_d3d12.cpp", see "sr_bridge/D3D12SR.h").
	struct D3D11Api
	{
		using Device = ID3D11Device;
		using CommandList = ID3D11DeviceContext;
		using Resource = ID3D11Resource;
	};
	struct D3D12Api
	{
		using Device = ID3D12Device;
		using CommandList = ID3D12GraphicsCommandList;
		using Resource = ID3D12Resource;
	};
	// The API the mod is built for
#if DX12
	using CurrentApi = D3D12Api;
#else
	using CurrentApi = D3D11Api;
#endif
	using Device = CurrentApi::Device;
	using CommandList = CurrentApi::CommandList;
	using Resource = CurrentApi::Resource;

	// Note that these don't necessarily match 1:1 with classes, some might be different configurations of the same implementation.
	enum class UserType
	{
		None,
		Auto,
		DLSS,
		FSR, // FSR 3 on D3D11, FSR 4 or 3.1 on D3D12
		LumaTAA,
		XeSS, // D3D12 only
	};

	// Put these in order of preference (most preferred first). For automatic selection.
	enum class Type
	{
		DLSS,
		FSR,
		LumaTAA,
		XeSS, // After the others, as the shaders' "SRType" (type + 1) values are fixed
		None = -1
	};

	__forceinline bool AreTypesEqual(UserType user_type, Type type)
	{
		switch (user_type)
		{	
		case SR::UserType::None:
			return type == Type::None;
		case SR::UserType::Auto:
			return true; // Always accept it for now, given that it's the first one we find compatible
		case SR::UserType::DLSS:
			return type == Type::DLSS;
		case SR::UserType::FSR:
			return type == Type::FSR;
		case SR::UserType::LumaTAA:
			return type == Type::LumaTAA;
		case SR::UserType::XeSS:
			return type == Type::XeSS;
		}
		return false;
	}

	constexpr const char* GetTypeName(Type type)
	{
		switch (type)
		{
		case Type::DLSS:
			return "DLSS";
		case Type::FSR:
			return "FSR";
		case Type::LumaTAA:
			return "Luma TAA";
		case Type::XeSS:
			return "XeSS";
		case Type::None:
			break;
		}
		return "None";
	}
	
	// E.g. use base 2 for x and base 3 for y.
	// Index should go from 0 to phases-1.
	// Returns [-0.5 0.5] (pixel space).
	constexpr float HaltonSequence(unsigned int index, unsigned int base)
	{
      index += 1; // Add 1 to avoid skewing towards 0, given that 0 always results in 0.
		float result = 0.0f;
		float inv_base = 1.0f / float(base);
		float fraction = inv_base;
		while (index > 0)
		{
			result += float(index % base) * fraction;
			index /= base;
			fraction *= inv_base;
		}
		return result - 0.5;
	}

	// 8 is usually great for native resolution TAA
	constexpr int GetDefaultJitterPhases() { return 8; }
	// NVIDIA's suggested formula, from the default at native resolution (Y resolution, as it's generally more reliable)
	static int GetDlssJitterPhases(unsigned int render_height, unsigned int output_height)
	{
		return std::lrintf(float(GetDefaultJitterPhases()) * std::pow((std::max)(float(output_height) / float(render_height), 1.f), 2.f));
	}
	// FidelityFX's "ffxFsr3UpscalerGetJitterPhaseCount()", for builds without FSR (x86)
	static int GetFsrJitterPhases(unsigned int render_width, unsigned int output_width)
	{
		return int(8.f * std::pow(float(output_width) / float(render_width), 2.f));
	}
	
	static float GetMipLODBias(float render_height, float output_height)
	{
		return std::log2(float(render_height) / float((std::max)(render_height, output_height))) - 1.f;
	}
	static float GetMipLODBias(unsigned int render_height, unsigned int output_height)
	{
		return GetMipLODBias(float(render_height), float(output_height));
	}
	// The XeSS guide's log2(render / output), without DLSS and FSR's extra -1: 0 at native resolution
	static float GetXeSSMipLODBias(unsigned int render_height, unsigned int output_height)
	{
		return std::log2(float(render_height) / float((std::max)(render_height, output_height)));
	}

	// The camera an upscaler needs (FSR), from a view projection with an affine view: element (row, column) at m[row * 4 + column] for
	// column vectors (clip = M * p), at m[column * 4 + row] with "row_vectors" (clip = p * M). Clip z = A * clip w + B, so device
	// depth = A + B / view depth (A = 1 for an infinite far plane, then B = -near). Each value is 0 when the projection lacks it.
	struct ViewProjectionCamera
	{
		double depth_a = 0.0;
		double depth_b = 0.0;
		double vert_fov = 0.0; // Radians: the up axis has length 1 / tan(fov / 2)
		double near_plane = 0.0;
		double far_plane = 0.0; // Finite: near x 1e6 for an infinite projection
	};
	template <typename T>
	ViewProjectionCamera GetViewProjectionCamera(const T* m, bool row_vectors)
	{
		const auto at = [&](int row, int column) { return double(row_vectors ? m[column * 4 + row] : m[row * 4 + column]); };
		double dot_23 = 0.0, dot_33 = 0.0;
		for (int column = 0; column < 3; column++)
		{
			dot_23 += at(2, column) * at(3, column);
			dot_33 += at(3, column) * at(3, column);
		}
		ViewProjectionCamera camera;
		camera.depth_a = (dot_33 > 0.0 ? dot_23 / dot_33 : 1.0);
		camera.depth_b = at(2, 3) - camera.depth_a * at(3, 3);
		const double up_length = std::sqrt(at(1, 0) * at(1, 0) + at(1, 1) * at(1, 1) + at(1, 2) * at(1, 2));
		camera.vert_fov = (up_length > 0.0 ? 2.0 * std::atan(1.0 / up_length) : 0.0);
		camera.near_plane = (camera.depth_a != 0.0 ? -camera.depth_b / camera.depth_a : 0.0);
		if (camera.near_plane > 0.0)
		{
			camera.far_plane = (camera.depth_a > 1.0 + 1e-6 ? camera.depth_b / (1.0 - camera.depth_a) : camera.near_plane * 1e6);
		}
		return camera;
	}

	// D3D12 command lists that used a replaced upscaler object (DLSS feature, FSR context) can still be running, so it's released a few
	// presents later ("SuperResolutionImpl::ReleaseRetired()"), also when its upscaler isn't picked anymore.
	template <typename T>
	struct DeferredReleases
	{
		struct Entry
		{
			T object;
			uint64_t update = 0;
		};
		std::vector<Entry> entries;
		uint64_t updates = 0;

		void Push(const T& object) { entries.push_back({.object = object, .update = updates}); }
		// Pushes "*object" (if any) and clears it
		void Retire(T* object)
		{
			if (*object)
			{
				Push(*object);
				*object = {};
			}
		}
		// Once per present: releases what the swapchain's up to 3 frames in flight can't use anymore
		template <typename F>
		void Release(F&& release)
		{
			updates++;
			std::erase_if(entries, [&](Entry& entry)
			{
				if (updates - entry.update <= 3)
				{
					return false;
				}
				release(&entry.object);
				return true;
			});
		}
		// At the shutdown
		template <typename F>
		void ReleaseAll(F&& release)
		{
			for (Entry& entry : entries)
			{
				release(&entry.object);
			}
			entries.clear();
		}
	};

	struct SettingsData
	{
		unsigned int output_width = 1;
		unsigned int output_height = 1;

		// In case of dynamic resolution, it's the target res (the mid point one).
		unsigned int render_width = 1;
		unsigned int render_height = 1;
		// TODO: add min/max render res for dynamic res
		// Whether dynamic rendering resolution is requested/desired
		bool dynamic_resolution = false;

		// Whether buffers are in linear space HDR or gamma space sRGB (depending on the implementation they might be required to be UNORM, ro also be fine as FLOAT)
		bool hdr = true;
		bool inverted_depth = false;
		bool mvs_jittered = false;
		// MVs need to have positive values when moving towards the top left of the screen
		float mvs_x_scale = 1.f; // Flip or scale by render res
		float mvs_y_scale = 1.f; // Flip or scale by render res
		// Alternatively, either force the exposure to 1 (or 0.18 mid grey) if we run after tonemapping, or feed the correct one if we run before
		bool auto_exposure = false;
		// Render preset hint if supported.
		unsigned int render_preset = 0;

		bool operator==(const SettingsData& other) const {
			return 
				(output_width == other.output_width) &&
				(output_height == other.output_height) &&
				(render_width == other.render_width) &&
				(render_height == other.render_height) &&
				(dynamic_resolution == other.dynamic_resolution) &&
				(hdr == other.hdr) &&
				(inverted_depth == other.inverted_depth) &&
				(mvs_jittered == other.mvs_jittered) &&
				(fabs(mvs_x_scale - other.mvs_x_scale) < FLT_EPSILON) &&
				(fabs(mvs_y_scale - other.mvs_y_scale) < FLT_EPSILON) &&
				(auto_exposure == other.auto_exposure) &&
				(render_preset == other.render_preset);
    	}
	};

	// Interface to be subclassed. Represents a handle.
	struct InstanceData
	{
		bool							is_supported = false;

		bool							supports_dynamic_resolution = true;
		// Whether it supports an input resolution lower than an output one. As opposite to matching resolutions (AA only). None of them support downscaling as of now.
		bool							supports_upscaling = true;
		// Whether the aspect ratio is expected to be the same between the render resolution and the output resolution
		bool							supports_non_uniform_aspect_ratio_upscaling = true;
		// Whether the application can just pick any rendering resolution (without any restrictive boundaries), or whether it needs to query the supported ones from the SR implementation
		bool							supports_arbitrary_resolutions = true;
		// Whether the jittering phase/period is determined by the implementation or can be picked by the application
		bool							supports_arbitrary_jitter_phases = true;
		bool							supports_sdr = true;
		// Whether the implementation will preserve negative values in scRGB linear buffers, instead of clipping them (all sr implementations support hdr float buffers).
		// Most implementations will assume sRGB/Rec.709 colors.
		bool							supports_scrgb_hdr = false;
		bool							requires_unordered_access_output_texture = true;
		bool							automatically_restores_pipeline_state = false;
		unsigned int				min_resolution = 1; // Min width or height
		// The version the runtime picked (e.g. FSR 4 or 3.1 on D3D12), for display. Owned by the implementation's dll, nullptr if unknown.
		// Atomic as FSR changes it on recording threads (its context recreation) while the overlay reads it: load it once.
		std::atomic<const char*>	version_name = nullptr;

		SettingsData				settings_data = {};

		virtual ~InstanceData() {}
	};

	// A super resolution implementation,
	// this is a generic term to include Temporal Anti Aliasing (with jittered rendering and motion vectors) and upscaling.
	template <typename Api>
	class SuperResolutionImplT
	{
	public:
		using Device = typename Api::Device;
		using CommandList = typename Api::CommandList;
		using Resource = typename Api::Resource;

		// The implementations are owned and deleted through this base.
		// A new virtual must also be forwarded by the DEVELOPMENT MCP wrapper ("Mcp::SrTap"), or it silently gets the defaults below there.
		virtual ~SuperResolutionImplT() = default;
		virtual bool HasInit(const InstanceData* data) const { return data != nullptr; }
		// Needs init to be called first
		virtual bool IsSupported(const InstanceData* data) const { return data && data->is_supported; }

		// Must be called once before usage. Still expects Deinit() to be called even if it failed.
		// Returns whether this SR implementation is supported by hardware and driver.
		// Fill in a data "handle", there can be more than one at a time.
		virtual bool Init(InstanceData*& data, Device* device, IDXGIAdapter* adapter = nullptr) { return false; }
		// Should be called before shutdown or on device destruction.
		virtual void Deinit(InstanceData*& data, Device* optional_device = nullptr) {}
		// Frees what "UpdateSettings()" and "Draw()" created (the upscaler's own resources, the SR bridge's helper) but keeps the implementation loaded,
		// for when it's not selected anymore. The next "UpdateSettings()" (for the SR bridge, "Draw()") creates them again.
		virtual void ReleaseResources(InstanceData* data) {}

		// Note that this might expect the same command list all the times.
		// Returns true if the settings changed or were up to date.
		virtual bool UpdateSettings(InstanceData* data, CommandList* command_list, const SettingsData& settings_data) { return false; }

		struct DrawData
		{
			bool reset = false;
			// The first draws into a new "output_color", until one succeeds. DLSS writes nothing into a new output until its feature is
			// created again after a draw (DLSS-Best-Practices OUT-4): a successful draw with this set recreates it at the next "UpdateSettings()".
			bool output_recreated = false;

			// On D3D12 the caller puts the inputs in "D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE" and the output
			// in "D3D12_RESOURCE_STATE_UNORDERED_ACCESS" (implementations leave them in these states), and binds its own state back after "Draw()" (see "NeedsStateRestoration()").
			Resource* output_color = nullptr;
			Resource* source_color = nullptr;
			Resource* motion_vectors = nullptr;
			Resource* depth_buffer = nullptr; // SR expects the depth to be the device/HW one (1 being near, not 1 being the camera (linear depth)), though it might actually not matter

			// Optional data:
			Resource* exposure = nullptr; // Can be left nullptr to default to 1 or to auto exposure
			Resource* bias_mask = nullptr; // Reactivity/Bias mask (might not be used by all SR implementations)
			Resource* transparency_alpha = nullptr; // Amount of intensity in this pixel (only used by some FSR implementations)

			unsigned int render_width = 0;
			unsigned int render_height = 0;

			float pre_exposure = 0.f; // Ignored if 0
			float jitter_x = 0.f; // In pixel space (from -0.5 to 0.5, the value here is influenced by resolution)
			float jitter_y = 0.f; // In pixel space (from -0.5 to 0.5, the value here is influenced by resolution)
			float vert_fov = 0.f; // Radians. Ignored if 0 (not always needed)
			float near_plane = 0.01f; // In meters
			float far_plane = 1000.f; // In meters
			float time_delta = -1.f; // Seconds. Ignored if < 0 (not always needed)
			unsigned long long frame_index = 0;
			float user_sharpness = -1.f; // Ignored/default if < 0. Neutral at 0 (at least in FSR).
		};

		// Returns true if drawing didn't fail.
		// Note that this might expect the same command list all the times.
		virtual bool Draw(const InstanceData* data, CommandList* command_list, const DrawData& draw_data) { return false; }

		// Returns the suggested or requested period, depending on the implementation.
		// All implementations work well with Halton for now, so always use that.
		virtual int GetJitterPhases(const SR::InstanceData* data) const { return 1 /*jitters disabled*/; }

		// Whether "Draw()" upscales, rather than still preparing (e.g. the SR bridge's helper starting, which copies the color as it is)
		virtual bool IsReady(const InstanceData* data) const { return true; }

		// Returns the suggested mip lod bias for the current resolution scale.
		// This assumes the jitter phases are increased with lower resolution scales.
		// -1 at native resolution as we'd still be running TAA (DLSS and FSR's guides).
		virtual float GetMipLODBias(const SR::InstanceData* data) const
		{
			return SR::GetMipLODBias(data->settings_data.render_height, data->settings_data.output_height);
		}
		
		// At every present, for every initialized implementation, with "DeviceData::sr_mutex" held exclusively (no "UpdateSettings()" or "Draw()" runs):
		// releases what its replaced objects' frames can't use anymore (see "DeferredReleases")
		virtual void ReleaseRetired(InstanceData* data) {}

		// Whether the implementation leaves the state dirty compared to when it begun
		virtual bool NeedsStateRestoration() const { return false; }
	};

	// The implementations' interface for the API the mod is built for. Its different instantiations have distinct types (and vtables) even
	// within one dll, while the D3D12 implementations are in an inline namespace (e.g. "NGX::D3D12::DLSS") when built as a D3D12 mod.
	using SuperResolutionImpl = SuperResolutionImplT<CurrentApi>;
	namespace D3D12
	{
		using SuperResolutionImpl = SuperResolutionImplT<D3D12Api>;
	}
}
