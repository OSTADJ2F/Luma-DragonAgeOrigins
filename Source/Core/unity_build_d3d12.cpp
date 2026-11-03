// The D3D12 flavor of the super resolution implementations, compiled into an x64 D3D11 mod ("UseLumaD3D12SR" in Luma.Features.props) as a
// second translation unit: with DX12 defined, the implementations live in inline namespaces ("NGX::D3D12", "FidelityFX::D3D12", "Intel::D3D12")
// next to the D3D11 ones of "unity_build.cpp", and "SR::SuperResolutionImpl" is the D3D12 interface ("SR::D3D12::SuperResolutionImpl" everywhere).
// The D3D11 side reaches them through "sr_bridge/D3D12SR.h" only.
#ifndef DX12
#define DX12 1
#endif

#include "sr_bridge/D3D12SR.h"
#include "sr_bridge/D3D12SRRunner.h"

#include "dlss/DLSS.cpp"
#include "fsr/FSR.cpp"
#include "xess/XeSS.cpp"

std::unique_ptr<SR::D3D12::SuperResolutionImpl> SR::D3D12::CreateSuperResolution(SR::Type type)
{
   switch (type)
   {
#if ENABLE_NGX
   case SR::Type::DLSS:
      return std::make_unique<NGX::DLSS>();
#endif
#if ENABLE_FIDELITY_SK
   case SR::Type::FSR:
      return std::make_unique<FidelityFX::FSR>();
#endif
#if ENABLE_XESS
   case SR::Type::XeSS:
      return std::make_unique<Intel::XeSS>();
#endif
   default:
      return nullptr;
   }
}

namespace
{
   // One device and queue for the upscalers of the process: a picked-again upscaler reuses them. Freed by "ReleaseProcessState()".
   std::shared_ptr<SRRunner::Gpu> shared_gpu;
   LUID shared_gpu_luid = {};

   std::shared_ptr<SRRunner::Gpu> AcquireGpu(const LUID& luid, IDXGIAdapter1* adapter)
   {
      if (shared_gpu && shared_gpu_luid.LowPart == luid.LowPart && shared_gpu_luid.HighPart == luid.HighPart)
      {
         return shared_gpu;
      }
      auto gpu = std::make_shared<SRRunner::Gpu>();
      if (!gpu->Init(adapter))
      {
         return nullptr;
      }
      shared_gpu_luid = luid;
      return shared_gpu = gpu;
   }

   class LocalHostImpl : public SR::D3D12::LocalHost
   {
   public:
      SRRunner::Runner runner;

      explicit LocalHostImpl(std::shared_ptr<SRRunner::Gpu> gpu) : runner(std::move(gpu))
      {
      }

      ~LocalHostImpl() override
      {
         runner.FinishProfile();
         runner.Quit();
      }

      bool UpdateSettings(const SR::SettingsData& settings) override
      {
         return runner.UpdateSettings(settings);
      }
      bool RunFrame(const SRBridgeProtocol::Frame& frame) override
      {
         return runner.RunFrame(frame);
      }
      int GetJitterPhases() const override
      {
         return runner.impl->GetJitterPhases(runner.data);
      }
      float GetMipLODBias() const override
      {
         return runner.impl->GetMipLODBias(runner.data);
      }
      const char* GetVersionName() const override
      {
         return runner.data->version_name.load();
      }
   };
} // namespace

std::unique_ptr<SR::D3D12::LocalHost> SR::D3D12::CreateLocalHost(SR::Type type, const LUID& luid, const SR::SettingsData& settings,
   const uint64_t (&texture_handles)[SRBridgeProtocol::kCount], const uint64_t (&fence_handles)[2], std::string* error)
{
   const SRRunner::ComPtr<IDXGIAdapter1> adapter = SRRunner::FindAdapter(luid);
   std::unique_ptr<SR::D3D12::SuperResolutionImpl> impl = CreateSuperResolution(type);
   if (!adapter || !impl)
   {
      *error = std::string(SR::GetTypeName(type)) + " isn't available in this build or the game's adapter wasn't found";
      return nullptr;
   }
   std::shared_ptr<SRRunner::Gpu> gpu = AcquireGpu(luid, adapter.Get());
   if (!gpu)
   {
      *error = "no D3D12 device";
      return nullptr;
   }
   auto host = std::make_unique<LocalHostImpl>(std::move(gpu));
   SRRunner::Runner& runner = host->runner;
   if (const std::string open_error = runner.OpenInputs(texture_handles, fence_handles); !open_error.empty())
   {
      *error = "can't open the shared " + open_error;
      return nullptr;
   }
   if (!runner.Create(std::move(impl), adapter.Get(), settings))
   {
      *error = std::string(SR::GetTypeName(type)) + " init or create failed";
      return nullptr;
   }
   char profile_path[MAX_PATH] = {};
   if (GetEnvironmentVariableA("LUMA_UPSCALER_PROFILE", profile_path, MAX_PATH))
   {
      runner.StartProfile(profile_path);
   }
   return host;
}

void SR::D3D12::ReleaseProcessState()
{
   shared_gpu.reset();
}
