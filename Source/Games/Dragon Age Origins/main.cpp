// Dragon Age: Origins - Luma HDR mod (Eclipse engine 2009, 32-bit, DX9 -> D3D11 via dgVoodoo2).
// Started from the template and the Mass Effect / Medal of Honor Airborne / Borderlands 2 DX9 ports.
// Shader hashes are dgVoodoo-TRANSLATED and change per wrapper build, so pin one dgVoodoo version (this mod pins
// 2.87.6) and dump the shaders again if you change it.

// No DEVELOPMENT auto-debugger MessageBox on DLL attach: invisible under borderless/fullscreen and it blocks the
// loader (ReShade times out the addon load -> error 1114). Same failure as the other dgVoodoo ports.
#define DISABLE_AUTO_DEBUGGER 1

#define GAME_DRAGON_AGE_ORIGINS 1

// XeSS is not built: no libxess.dll is ever shipped and the dev folder's copy is moved out, so the SR bridge reports it unsupported and
// Core greys its entry out. DLSS and FSR stay.
#define ENABLE_NGX 0 // NGX is x64-only and the game is 32-bit
#define ENABLE_FIDELITY_SK 0
#define GEOMETRY_SHADER_SUPPORT 0
// Luma's pyramid bloom in place of the game's own ("Luma_Bloom_impl.hlsl")
#define ENABLE_BLOOM 1
// "SceneMotionVectors" reads the shaders as created from the patch context (no game patch of its own here)
#define LUMA_PATCH_BYTECODE_SYNC 1
// Outside DEVELOPMENT only this define makes original_draw_dispatch_func non-null (the grade is drawn after the upscaler)
#define ENABLE_POST_DRAW_DISPATCH_CALLBACK 1
// The motion vector draw key reads "last_draw_dispatch_data", which Publishing only fills with this
#define ENABLE_DRAW_DISPATCH_DATA_CACHE 1
// No NVIDIA Reflex: latency is left to the game
#define ENABLE_REFLEX 0
// RCAS sharpening on the upscaler's output ("DrawRCAS()")
#define ENABLE_RCAS 1

#include "..\..\Core\core.hpp"
#include "MotionVectorPatches.h"
#include "includes\safetyhook.hpp" // The GUI scale mid hook (see "OnGUIScaleRead()"): vendored per game, as in the other Luma mods that patch code
#include <shellapi.h> // ShellExecuteA for the About links (system() hangs the render thread in exclusive fullscreen)

// HDR output: always on (the scRGB swapchain, the HDR settings and the recovered highlights). On a standard display Core keeps the SDR display mode.
static constexpr bool enable_hdr = true;
// RCAS sharpening after the upscaler (0 = off), see "DrawUpscaler()"
static float g_rcas_sharpness = 0.25f;

// Hides the HUD (for clean screenshots): the game's draws into the canvas after the colour grade are skipped. Session-only, never
// persisted: a stuck "on" would look like a broken HUD.
static bool g_hide_ui = false;

// Ambient occlusion: the game has none, so XeGTAO is added on the scene depth (see "RunXeGTAO()").
static bool g_ao_enable = true;
// The AO main pass: XeGTAO (horizon based), or GT-VBAO (visibility bitmask, with a thickness behind every depth sample). The depth
// prefilter, the denoise and the way the result is applied are the same.
enum class AOTechnique : int
{
   XeGTAO = 0,
   GTVBAO = 1,
};
static AOTechnique g_ao_technique = AOTechnique::GTVBAO;
static float g_ao_strength = 0.75f;          // 0 = no AO, 1 = the full XeGTAO visibility multiplied into the scene (the shipped default is the author's own tuning)
static float g_ao_darkness = 0.40f;          // 0-1, how dark the occluded areas get: XeGTAO's final value power is 4x this (0.5 = 2, Intel's default; 0.4 = 1.6, the shipped default)
static float g_gtao_radius_override = 0.f;   // Meters, 0 = the shader's default (EFFECT_RADIUS)
#if DEVELOPMENT
static int g_gtao_debug_view = 0; // 0 = off, 1 = depth gradient, 2 = normals, 3 = AO x8, 4 = edges
#endif
// Knobs buffer slot, matches "register(b9)" in Luma_DAO_XeGTAO.hlsl and Luma_DAO_AOApply.hlsl (core's bloom owns b11).
static constexpr UINT kGTAOKnobsCBSlot = 9;
// The pyramid's Gaussian sigmas in mip texels (so nothing depends on the resolution): the first level is half the scene's size
static constexpr float g_bloom_sigmas[] = {1.5f, 2.f, 2.f, 2.f, 1.5f, 1.f};
// The game's bloom: the bright pass (scene -> half resolution), its two blurs, and the composite at full resolution
static constexpr const char* kBloomBrightPassHash = "3549D37F";
static constexpr const char* kBloomBlurHash = "6184D504";
static constexpr const char* kBloomCompositeHash = "769096CC";

// Whether the game's display was in HDR at start: decides between the scRGB swapchain and a real 8 bit SDR one (see "DllMain").
static bool g_hdr_enabled_at_start = false;

// Anisotropic filtering is fixed at 16x through Core's sampler upgrade (see "DllMain"): exposing a level would need a Core change.

// The HUD scale. The game lays its GUI out for 1024x768: the engine computes a scalar (1.0 from 1024x768 up, below it 768/height for a screen
// wider than 4:3 or 1024/width for a narrower one), stores it on the stack and reads it back to build the GUI layout: because the engine's own
// layout, drawing AND mouse hit testing all consume that one scalar, replacing it is safe at any resolution and needs no render side geometry
// hacks. A mid hook on the load of it ("D9 44 24 08", "fld dword ptr [esp+8]": the single match of DAFix's signature in the GOG exe, RVA
// 0x111318) rewrites the slot before the read. 1 is the game's own scale, higher is a bigger HUD, 0 picks the automatic one (as if the game ran
// at 768 lines, which is the fraction of the screen the HUD was designed for). Automatic is the default: the game's own size leaves the HUD tiny on
// a modern display, which is the whole reason to have the option. Hook technique from Lyall's DAFix (MIT).
static float g_ui_scale = 0.f;
static safetyhook::MidHook g_gui_scale_hook;
static bool g_gui_scale_hook_installed = false;

// The game's width and height at the hook: written into the layout struct it is building, in two places (see the notes in "OnGUIScaleRead()")
static bool PlausibleGameResolution(int64_t width, int64_t height)
{
   if (width < 320 || width > 32768 || height < 200 || height > 32768)
      return false;
   const float aspect = float(width) / float(height);
   return aspect >= 0.4f && aspect <= 5.f;
}

static void OnGUIScaleRead(safetyhook::Context& context)
{
   // The engine's own scale, computed a few instructions before: 1.0 from 1024x768 up, the 768/height or 1024/width rule below it
   float& engine_scale = *reinterpret_cast<float*>(context.esp + 8);
   const float engine_original = engine_scale;
   if (!(engine_original > 0.001f && engine_original < 10.f)) // Not the scale we expect: leave the frame alone rather than divide it by the setting
      return;

   const float wanted = g_ui_scale;

   // The layout struct holds the game's width and height as integers at [esp+0x28]/[esp+0x2c] when the game has a viewport of its own, and at
   // [esp+0x10]/[esp+0x14] otherwise: they are floats in place there when the engine had to compute its own scale, which is always below
   // 1024x768, where the automatic scale changes nothing anyway.
   int64_t width = *reinterpret_cast<int*>(context.esp + 0x28);
   int64_t height = *reinterpret_cast<int*>(context.esp + 0x2c);
   if (!PlausibleGameResolution(width, height))
   {
      width = *reinterpret_cast<int*>(context.esp + 0x10);
      height = *reinterpret_cast<int*>(context.esp + 0x14);
      if (!PlausibleGameResolution(width, height))
      {
         width = 0;
         height = 0;
      }
   }

   if (wanted > 0.f)
   {
      // The user's multiple of the HUD's size: the number the engine uses is a divisor (lower is a larger HUD), so a bigger HUD is a smaller value
      engine_scale = engine_original / std::clamp(wanted, 0.1f, 8.f);
   }
   else if (width > 0)
   {
      // Automatic: the engine's own below 1024x768 formula applied at any resolution, which keeps the HUD at the fraction of the screen it was
      // designed for (the constants in that path are the game's own 768.0, 1024.0 and 1.3333). Without a usable resolution the engine's value stays.
      const float aspect = float(width) / float(height);
      if (aspect > 1.333333f && height > 768)
         engine_scale = 768.f / float(height);
      else if (aspect <= 1.333333f && width > 1024)
         engine_scale = 1024.f / float(width);
   }

#if DEVELOPMENT
   // The first layouts of a session, to check the slots read above against the real resolution: they are the only guesses left in this hook
   static std::atomic<int> layouts_seen = 0;
   if (layouts_seen.fetch_add(1) < 6)
      reshade::log::message(reshade::log::level::info, std::format("[Luma] Dragon Age Origins: GUI layout {}x{}: the engine scale at [esp+8] was {:.4f} (at [esp+C] {:.4f}), wrote {:.4f}",
         width, height, engine_original, *reinterpret_cast<float*>(context.esp + 0xc), engine_scale).c_str());
#endif
}

static void InstallGUIScaleHook()
{
   // DAFix's signature: the function that builds the GUI layout struct, whose first "fld" loads the engine's GUI scale
   const std::vector<System::BytePattern> pattern = {
      0xD9, System::ANY, System::ANY, System::ANY, 0x8B, System::ANY, 0xD9, System::ANY, System::ANY, System::ANY,
      0xD9, System::ANY, System::ANY, System::ANY, 0x8B, System::ANY, System::ANY, 0x53};
   const std::vector<std::byte*> matches = System::ScanModuleForPattern(pattern);

   // One hit, and it has to be "fld dword ptr [esp+8]": the mod hooks that instruction and nothing else, so any other exe build (another
   // storefront, a patch) leaves the setting off instead of hanging the engine somewhere else
   if (matches.size() != 1 || matches[0][0] != std::byte{0xD9} || matches[0][1] != std::byte{0x44} || matches[0][2] != std::byte{0x24} || matches[0][3] != std::byte{0x08})
   {
      reshade::log::message(reshade::log::level::warning, std::format("[Luma] Dragon Age Origins: the GUI scale pattern matched {} times, the HUD scale setting is off (this exe version is not supported)", matches.size()).c_str());
      return;
   }

   g_gui_scale_hook = safetyhook::create_mid(matches[0], &OnGUIScaleRead);
   g_gui_scale_hook_installed = static_cast<bool>(g_gui_scale_hook);
   if (g_gui_scale_hook_installed)
      reshade::log::message(reshade::log::level::info, std::format("[Luma] Dragon Age Origins: the GUI scale is hooked at {} + {:#x}", System::GetProcessExecutableName(), reinterpret_cast<uintptr_t>(matches[0]) - reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr))).c_str());
   else
      reshade::log::message(reshade::log::level::warning, "[Luma] Dragon Age Origins: the GUI scale hook could not be made, the HUD scale setting is off");
}


// The game's projection, see "Shaders/Dragon Age Origins/Includes/DAOCamera.hlsl"
static constexpr float DAO_NEAR_CPP = 0.15f;
static constexpr float DAO_FAR_CPP = 4000.f;
static constexpr float DAO_TAN_HALF_FOV_Y_CPP = 0.577350269f;

// FSR's masks, written by the scene's alpha blended draws (particles, smoke, glows) past dgVoodoo's targets: their pixel
// shaders patched with "PatchPixelShaderReactive", the slot max blended in every blend state ("PatchedDraws::OnCreateBlendState")
constexpr UINT REACTIVE_SLOT = MotionVectorPatches::reactive_slot;
// The additive draws' reactivity, scaled and capped at 0.9; the alpha blended draws' threshold (Metal Gear Rising's and Bayonetta's values)
static float g_sr_reactive_scale = 2.f;
static float g_sr_reactive_threshold = 0.5f;
static bool g_sr_reactive_enable = true;
// A game pixel shader's patch: the mask target of an alpha blended or additive draw
enum class PixelShaderPatch : uint8_t
{
   MASK_ALPHA,
   MASK_ADDITIVE,
};
constexpr const char* PIXEL_SHADER_PATCH_NAMES[] = {"Mask", "Mask (additive)"};

#if DEVELOPMENT && ENABLE_MINHOOK
// Development only: slows or pauses the game's own clock, to study a frame of a cutscene (the game has no pause there). The game keeps
// presenting frames, its time stops advancing: only the calls made from the game's exe are scaled, so Luma, ReShade and the other modules keep
// the real time. A paused clock still ticks a little on every call, so the game's wait loops finish.
namespace GameTime
{
static std::atomic<float> speed = 1.f; // 0 = paused, 1 = normal
// What the menu's checkbox and slider show, and the pause hotkey toggles (the speed it goes back to when unpaused)
static std::atomic<bool> paused = false;
static std::atomic<float> resume_speed = 1.f;
static void SetPaused(bool value)
{
   paused = value;
   speed.store(value ? 0.f : resume_speed.load());
}
static HMODULE exe_module = nullptr;
static uintptr_t exe_size = 0;

static bool FromGame(void* return_address)
{
   const uintptr_t address = reinterpret_cast<uintptr_t>(return_address);
   const uintptr_t base = reinterpret_cast<uintptr_t>(exe_module);
   return address >= base && address < base + exe_size;
}

// One virtual clock (in the units of its function): advances by the real time passed times the speed
struct Clock
{
   SRWLOCK lock = SRWLOCK_INIT;
   uint64_t last_real = 0;
   double virtual_time = 0.0;
   bool started = false;
   uint32_t calls_since_tick = 0;

   uint64_t Get(uint64_t real, uint32_t calls_per_unit)
   {
      AcquireSRWLockExclusive(&lock);
      if (!started)
      {
         started = true;
         last_real = real;
         virtual_time = double(real);
      }
      const float current_speed = speed.load();
      virtual_time += double(real - last_real) * double(current_speed);
      last_real = real;
      if (current_speed <= 0.f && ++calls_since_tick >= calls_per_unit)
      {
         calls_since_tick = 0;
         virtual_time += 1.0;
      }
      const uint64_t result = uint64_t(virtual_time);
      ReleaseSRWLockExclusive(&lock);
      return result;
   }
};
static Clock clock_qpc, clock_tick, clock_tick64, clock_time_get;

using QueryPerformanceCounterFunction = BOOL(WINAPI*)(LARGE_INTEGER*);
using GetTickCountFunction = DWORD(WINAPI*)();
using GetTickCount64Function = ULONGLONG(WINAPI*)();
using TimeGetTimeFunction = DWORD(WINAPI*)();
static MinHook::HookedFunction<QueryPerformanceCounterFunction> hook_qpc;
static MinHook::HookedFunction<GetTickCountFunction> hook_tick;
static MinHook::HookedFunction<GetTickCount64Function> hook_tick64;
static MinHook::HookedFunction<TimeGetTimeFunction> hook_time_get;

static BOOL WINAPI DetourQueryPerformanceCounter(LARGE_INTEGER* counter)
{
   const BOOL result = hook_qpc.original(counter);
   if (result && counter && speed.load() != 1.f && FromGame(_ReturnAddress()))
      counter->QuadPart = LONGLONG(clock_qpc.Get(uint64_t(counter->QuadPart), 1));
   return result;
}
static DWORD WINAPI DetourGetTickCount()
{
   const DWORD real = hook_tick.original();
   return (speed.load() != 1.f && FromGame(_ReturnAddress())) ? DWORD(clock_tick.Get(real, 5000)) : real;
}
static ULONGLONG WINAPI DetourGetTickCount64()
{
   const ULONGLONG real = hook_tick64.original();
   return (speed.load() != 1.f && FromGame(_ReturnAddress())) ? ULONGLONG(clock_tick64.Get(real, 5000)) : real;
}
static DWORD WINAPI DetourTimeGetTime()
{
   const DWORD real = hook_time_get.original();
   return (speed.load() != 1.f && FromGame(_ReturnAddress())) ? DWORD(clock_time_get.Get(real, 5000)) : real;
}

static void Install()
{
   static bool installed = false;
   if (std::exchange(installed, true))
      return;
   exe_module = GetModuleHandleW(nullptr);
   const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_module);
   const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(exe_module) + dos->e_lfanew);
   exe_size = nt->OptionalHeader.SizeOfImage;
   const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
   if (kernel32)
   {
      MinHook::Attach(&hook_qpc, reinterpret_cast<void*>(GetProcAddress(kernel32, "QueryPerformanceCounter")), &DetourQueryPerformanceCounter);
      MinHook::Attach(&hook_tick, reinterpret_cast<void*>(GetProcAddress(kernel32, "GetTickCount")), &DetourGetTickCount);
      MinHook::Attach(&hook_tick64, reinterpret_cast<void*>(GetProcAddress(kernel32, "GetTickCount64")), &DetourGetTickCount64);
   }
   if (const HMODULE winmm = GetModuleHandleW(L"winmm.dll"))
      MinHook::Attach(&hook_time_get, reinterpret_cast<void*>(GetProcAddress(winmm, "timeGetTime")), &DetourTimeGetTime);
}

static void Uninstall()
{
   speed.store(1.f);
   paused = false;
   MinHook::Detach(&hook_qpc);
   MinHook::Detach(&hook_tick);
   MinHook::Detach(&hook_tick64);
   MinHook::Detach(&hook_time_get);
}
} // namespace GameTime
#endif // DEVELOPMENT && ENABLE_MINHOOK

struct DragonAgeOriginsGameDeviceData final : public GameDeviceData
{
   bool has_drawn_tonemap = false;
   // The canvas the colour grade wrote this frame (the HUD blends onto it afterwards): Hide UI skips the game's later draws into it.
   // Released every Present.
   ComPtr<ID3D11Resource> canvas_res;

   // XeGTAO scratch, all full resolution and sized from the scene depth buffer. Created on first use, freed when AO is turned off
   // (the process is 32 bit: at 4K these hold well over 100 MB).
   ComPtr<ID3D11Texture2D> ao_linear_depth; // R32F linear view depth in meters
   ComPtr<ID3D11UnorderedAccessView> ao_linear_depth_uav;
   ComPtr<ID3D11ShaderResourceView> ao_linear_depth_srv;
   ComPtr<ID3D11Texture2D> ao_depth_mips; // R32F, 5 mips, the prefiltered depth pyramid
   ComPtr<ID3D11UnorderedAccessView> ao_depth_mip_uavs[5];
   ComPtr<ID3D11ShaderResourceView> ao_depth_mips_srv;
   ComPtr<ID3D11Texture2D> ao_working[2]; // R8G8_UNORM AO + edges, ping pong
   ComPtr<ID3D11UnorderedAccessView> ao_working_uavs[2];
   ComPtr<ID3D11ShaderResourceView> ao_working_srvs[2];
   ComPtr<ID3D11Texture2D> ao_final; // R8_UNORM visibility
   ComPtr<ID3D11UnorderedAccessView> ao_final_uav;
   ComPtr<ID3D11ShaderResourceView> ao_final_srv;
   uint32_t ao_width = 0; // The size the scratch was created (or failed to be created) for: a matching size with no resources means "failed", no retry
   uint32_t ao_height = 0;
   ComPtr<ID3D11Buffer> ao_cb; // XeGTAO knobs
   float ao_cb_data[8] = {};
   ComPtr<ID3D11Buffer> ao_apply_cb;
   float ao_apply_strength = -1.f;
   ComPtr<ID3D11BlendState> ao_multiply_blend;
   // The shader resource view of the game's depth, kept while the depth texture stays the same
   ID3D11Resource* ao_depth_resource = nullptr; // Only compared, never dereferenced: the SRV below holds the reference
   ComPtr<ID3D11ShaderResourceView> ao_depth_srv;
   bool ao_done_this_frame = false;
   bool ao_warned = false;

   // The sharp scene at the cutscene depth of field's composite, for the bokeh gather ("Shaders/Dragon Age Origins/DoFComposite_0xB507EFCE")
   ComPtr<ID3D11Texture2D> dof_scene;
   ComPtr<ID3D11ShaderResourceView> dof_scene_srv;
   uint32_t dof_width = 0;
   uint32_t dof_height = 0;
   DXGI_FORMAT dof_format = DXGI_FORMAT_UNKNOWN;
   ComPtr<ID3D11SamplerState> dof_sampler; // Bilinear clamp at s4 for the gather

   // Luma's bloom of this frame (half the scene's size), made at the game's bright pass and read in place of its blurred bloom at the composite
   ComPtr<ID3D11ShaderResourceView> srv_luma_bloom;
   bool bloom_drawn_this_frame = false;

   // ---- DLAA / FSR Native AA / DLSS (Core's SR bridge: the game is 32-bit) on the motion vectors of Core's
   // "SceneMotionVectors" (vc4 layout in "MotionVectorPatches"), before the grade ----
   // An upscaler is picked, hasn't failed and the game's MSAA is off, latched at present for the whole frame
   bool sr_active = false;
   SceneMotionVectors::State mv;
   // The scene's depth (the D24 of the first scene draw) and its view for the fill
   com_ptr<ID3D11Resource> scene_depth;
   com_ptr<ID3D11ShaderResourceView> scene_depth_srv;
   ID3D11DepthStencilView* open_refused_dsv = nullptr; // The last depth view "OpenScene" refused (reset at present)
   com_ptr<ID3D11ShaderResourceView> sr_output_srv;
   // The RCAS sharpened copy of the upscaler's output, made when sharpening is on
   com_ptr<ID3D11Texture2D> sr_sharpened;
   com_ptr<ID3D11ShaderResourceView> sr_sharpened_srv;
   com_ptr<ID3D11RenderTargetView> sr_sharpened_rtv;
   // The projection jitter (pixels, +y down) of this frame, zero unless the upscaler drew the last one, applied as a viewport offset
   std::array<float, 2> jitter = {};
   bool viewport_jittered = false;
   D3D11_VIEWPORT game_viewport = {};
   D3D11_VIEWPORT jittered_viewport = {};
   // "IsSceneDepthTested"'s last depth stencil state (forgotten at present: a released state's address comes back for another)
   ID3D11DepthStencilState* depth_test_state = nullptr;
   bool depth_test = false;
   bool depth_write = false;

   // FSR's masks: what the alpha blended draws wrote (x reactive, y transparency & composition, max blended; the module's
   // "reactive_target", made here), the masks the fill makes of it, and the additive draws' mask of the last frame
   com_ptr<ID3D11Texture2D> reactive;
   com_ptr<ID3D11UnorderedAccessView> reactive_uav;
   com_ptr<ID3D11Texture2D> transparency;
   com_ptr<ID3D11UnorderedAccessView> transparency_uav;
   com_ptr<ID3D11Texture2D> reactive_previous;
   com_ptr<ID3D11UnorderedAccessView> reactive_previous_uav;
   // This frame's mask target was cleared when the scene opened: the alpha blended draws after it write it
   bool reactive_ready = false;
   // "DrawWithReactiveMask"'s last blend state and its mask patch, none if not blended by source alpha (forgotten at present)
   ID3D11BlendState* blend_state = nullptr;
   std::optional<PixelShaderPatch> blend_patch;
   // The game's pixel shaders' mask patches by original hash and patch, null for one that can't be patched. Never erased.
   std::shared_mutex patched_mutex;
   std::unordered_map<uint64_t, com_ptr<ID3D11PixelShader>> patched_pixel_shaders;

   void ReleaseReactiveMasks()
   {
      mv.reactive_target_srv.reset();
      mv.reactive_target_rtv.reset();
      mv.reactive_target.reset();
      reactive_uav.reset();
      reactive.reset();
      transparency_uav.reset();
      transparency.reset();
      reactive_previous_uav.reset();
      reactive_previous.reset();
   }
};

class DragonAgeOrigins final : public Game // Go view "Source/Core/includes/game.h" for all the overridable virtual functions
{
   static DragonAgeOriginsGameDeviceData& GetGameDeviceData(DeviceData& device_data)
   {
      return *static_cast<DragonAgeOriginsGameDeviceData*>(device_data.game);
   }

public:
   void OnInit(bool async) override
   {
#if DEVELOPMENT && ENABLE_MINHOOK
      GameTime::Install();
#endif
      std::vector<ShaderDefineData> game_shader_defines_data = {
         {"XE_GTAO_QUALITY", '3', true, false, "Ambient occlusion quality (slice count).\n0 - Low\n1 - Medium\n2 - High\n3 - Very High\n4 - Ultra", 4},
      };
      shader_defines_data.append_range(game_shader_defines_data);
      assert(shader_defines_data.size() < MAX_SHADER_DEFINES);

      // The ambient occlusion: depth linearization (single sample and MSAA depth), the 4 XeGTAO passes (the two denoisers differ only by
      // XE_GTAO_FINAL_APPLY), and the pass multiplying the result into the scene.
      native_shaders_definitions.emplace(CompileTimeStringHash("DAO Depth Linearize CS"),
         ShaderDefinition{"Luma_DAO_DepthLinearize", reshade::api::pipeline_subobject_type::compute_shader});
      native_shaders_definitions.emplace(CompileTimeStringHash("DAO Depth Linearize MS CS"),
         ShaderDefinition{"Luma_DAO_DepthLinearize", reshade::api::pipeline_subobject_type::compute_shader, nullptr, nullptr, {{"DAO_DEPTH_MS", "1"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DAO XeGTAO Prefilter Depths CS"),
         ShaderDefinition{"Luma_DAO_XeGTAO", reshade::api::pipeline_subobject_type::compute_shader, nullptr, "prefilter_depths16x16_cs"});
      native_shaders_definitions.emplace(CompileTimeStringHash("DAO XeGTAO Main Pass CS"),
         ShaderDefinition{"Luma_DAO_XeGTAO", reshade::api::pipeline_subobject_type::compute_shader, nullptr, "main_pass_cs"});
      native_shaders_definitions.emplace(CompileTimeStringHash("DAO GT-VBAO Main Pass CS"),
         ShaderDefinition{"Luma_DAO_XeGTAO", reshade::api::pipeline_subobject_type::compute_shader, nullptr, "gtvbao_main_pass_cs"});
      native_shaders_definitions.emplace(CompileTimeStringHash("DAO XeGTAO Denoise Pass 1 CS"),
         ShaderDefinition{"Luma_DAO_XeGTAO", reshade::api::pipeline_subobject_type::compute_shader, nullptr, "denoise_pass_cs", {{"XE_GTAO_FINAL_APPLY", "0"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DAO XeGTAO Denoise Pass 2 CS"),
         ShaderDefinition{"Luma_DAO_XeGTAO", reshade::api::pipeline_subobject_type::compute_shader, nullptr, "denoise_pass_cs", {{"XE_GTAO_FINAL_APPLY", "1"}}});
      native_shaders_definitions.emplace(CompileTimeStringHash("DAO AO Apply PS"),
         ShaderDefinition{"Luma_DAO_AOApply", reshade::api::pipeline_subobject_type::pixel_shader});

      native_shaders_definitions.emplace(CompileTimeStringHash("DAO Motion Vector Fill CS"),
         ShaderDefinition{"Luma_DAO_MotionVectorFill", reshade::api::pipeline_subobject_type::compute_shader});
      sr_game_tooltip = "Needs Anti-Aliasing (MSAA) off in the game's options. DLSS and FSR require Luma-Upscaler.dll next to the game's exe.\n";
      SetEvents(true);

      // The game's encoding: the vanilla grade is sRGB (VANILLA_ENCODING_TYPE 0), decoded as gamma 2.2 (GAMMA_CORRECTION_TYPE 1); no early display encoding.
      GetShaderDefineData(POST_PROCESS_SPACE_TYPE_HASH).SetDefaultValue('0');
      GetShaderDefineData(EARLY_DISPLAY_ENCODING_HASH).SetDefaultValue('0');
      GetShaderDefineData(VANILLA_ENCODING_TYPE_HASH).SetDefaultValue('0');
      GetShaderDefineData(GAMMA_CORRECTION_TYPE_HASH).SetDefaultValue('1');
      // The HUD blends in gamma space onto the same canvas as the scene: the display composition scales the whole canvas by UI / Game paper
      // white, so the grade and the videos (the only scene writes) are pre-scaled by the inverse ("GetUIPaperWhitePreScale()"). Gives the
      // "UI Paper White" slider.
      GetShaderDefineData(UI_DRAW_TYPE_HASH).SetDefaultValue('2');
      // Manual paper white sliders: Core only shows the UI one when the OS reference white level isn't followed
      use_os_reference_white_level = false;
      // The scene's paper white (102 nits) is applied game-side in "ApplyScenePaperWhiteDefault()" and the UI keeps Core's own 203 (ITU)
      // default, so Core's defaults and the peak white (whatever the display reports) stay untouched.
      // Clip the final image to BT.2020: the gamut expansion and the UI blending over it can leave a few pixels a hair outside.
      GetShaderDefineData(GAMUT_MAPPING_TYPE_HASH).SetDefaultValue('3');

      luma_settings_cbuffer_index = 13;
      luma_data_cbuffer_index = 12;
      luma_ui_cbuffer_index = -1;

      // Game settings, see "Shaders/Dragon Age Origins/Includes/GameCBuffers.hlsl".
      default_luma_global_game_settings.HighlightExpansion = cb_luma_global_settings.GameSettings.HighlightExpansion = 1.f; // 1 = clipped highlights reach the display peak
      default_luma_global_game_settings.HighlightDesaturation = cb_luma_global_settings.GameSettings.HighlightDesaturation = 0.f;
      default_luma_global_game_settings.HighlightColor = cb_luma_global_settings.GameSettings.HighlightColor = 0.15f; // A little of their colour back in the highlights the game clipped to white
      // The tuning constants: the values the shaders were written with
      {
         auto& defaults = default_luma_global_game_settings;
         auto& settings = cb_luma_global_settings.GameSettings;
         defaults.ShadowLiftRange = settings.ShadowLiftRange = 0.3f;
         defaults.HighlightDesatStart = settings.HighlightDesatStart = 1.f;
         defaults.HighlightDesatEnd = settings.HighlightDesatEnd = 1.f;
         defaults.BokehTaps = settings.BokehTaps = 24.f;
         defaults.BokehHighlightBoost = settings.BokehHighlightBoost = 4.f;
      }
      default_luma_global_game_settings.HighlightWhite = cb_luma_global_settings.GameSettings.HighlightWhite = 11.f; // The p99 of the recovered highlights (fire and sunbeam scene): 1-2% of them reach the peak on any display
      default_luma_global_game_settings.HighlightPunch = cb_luma_global_settings.GameSettings.HighlightPunch = 0.f; // The plain Reinhard shoulder
      default_luma_global_game_settings.LightSourceBoost = cb_luma_global_settings.GameSettings.LightSourceBoost = 0.5f; // The cores of fire and torch sprites reach 1.5x
      default_luma_global_game_settings.ShadowLift = cb_luma_global_settings.GameSettings.ShadowLift = 0.1f; // Against the black crush of the gamma 2.2 decode
      default_luma_global_game_settings.GamutExpansion = cb_luma_global_settings.GameSettings.GamutExpansion = 0.5f; // Subtle by nature: the game's colours are mostly well inside BT.709
      // Light AutoHDR on the Bink movie pass (Video_0x39642745): movies bypass the grade. At 0.5 the peak is ~165 nits (2x paper white).
      default_luma_global_game_settings.VideoAutoHDREnable = cb_luma_global_settings.GameSettings.VideoAutoHDREnable = 1.f;
      default_luma_global_game_settings.VideoAutoHDRBoost = cb_luma_global_settings.GameSettings.VideoAutoHDRBoost = 0.5f;
      // Luma's bloom: the threshold is a gamma space brightness (1 = SDR white)
      default_luma_global_game_settings.BloomType = cb_luma_global_settings.GameSettings.BloomType = 2.f; // Luma's
      default_luma_global_game_settings.BloomIntensity = cb_luma_global_settings.GameSettings.BloomIntensity = 1.f;
      default_luma_global_game_settings.BloomThreshold = cb_luma_global_settings.GameSettings.BloomThreshold = 0.25f;
      default_luma_global_game_settings.BloomSpread = cb_luma_global_settings.GameSettings.BloomSpread = 1.5f;
      default_luma_global_game_settings.BloomSaturation = cb_luma_global_settings.GameSettings.BloomSaturation = 1.f;
      default_luma_global_game_settings.BloomWarmth = cb_luma_global_settings.GameSettings.BloomWarmth = 0.f;
      default_luma_global_game_settings.FilmGrainStrength = cb_luma_global_settings.GameSettings.FilmGrainStrength = 0.35f; // The game has no grain of its own
      default_luma_global_game_settings.VignetteStrength = cb_luma_global_settings.GameSettings.VignetteStrength = 0.f; // Off
      default_luma_global_game_settings.VignetteRadius = cb_luma_global_settings.GameSettings.VignetteRadius = 0.8f;
      default_luma_global_game_settings.ChromaticAberration = cb_luma_global_settings.GameSettings.ChromaticAberration = 0.f; // Off
      default_luma_global_game_settings.Exposure = cb_luma_global_settings.GameSettings.Exposure = 0.f;
      default_luma_global_game_settings.Contrast = cb_luma_global_settings.GameSettings.Contrast = 1.f;
      default_luma_global_game_settings.Saturation = cb_luma_global_settings.GameSettings.Saturation = 1.f;
      default_luma_global_game_settings.Temperature = cb_luma_global_settings.GameSettings.Temperature = 0.f;
      default_luma_global_game_settings.BokehDoFEnable = cb_luma_global_settings.GameSettings.BokehDoFEnable = 1.f;
   }

   void OnCreateDevice(ID3D11Device* native_device, DeviceData& device_data) override
   {
      device_data.game = new DragonAgeOriginsGameDeviceData;
#if DEVELOPMENT
      // The last complete frame's motion vector counts ("mv.*")
      SceneMotionVectors::RegisterDevValues(static_cast<DragonAgeOriginsGameDeviceData*>(device_data.game)->mv, &device_data);
#endif
   }

   // Core's default deletes through GameDeviceData*, which has no virtual destructor: delete the concrete type.
   void OnDestroyDeviceData(DeviceData& device_data) override
   {
      delete static_cast<DragonAgeOriginsGameDeviceData*>(device_data.game);
      device_data.game = nullptr;
   }

   // The water's pixel shader (an environment mapped, alpha blended material) wrote its colour, alpha and fog mix unclamped: the 8 bit canvas
   // clamped them before the blend, the fp16 one doesn't, and the fog factor and the alpha's power term went to values of -100 to 100 and alphas of
   // 65504 (wild colours in the reflections). The clamp is put back on the shader as it is created, so the motion vector patch (built on top of it)
   // and the plain draw both have it, which a replaced shader wouldn't give: it skips the motion vector patch and the water ghosts.
   // BLEND_INPUT (not SATURATE): the alpha is a blend weight (SRC_ALPHA/INV_SRC_ALPHA), so it must stay in [0, 1] (a 65504 alpha makes INV_SRC_ALPHA
   // negative); the reflection colour only needs its negatives floored (the fog mix can subtract), and above 1 it rides into HDR like the DoF
   // composite's blurred colour instead of being capped at SDR white.
   std::unique_ptr<std::byte[]> PatchShaderBytecodeSync(const std::byte* code, size_t& size, reshade::api::pipeline_subobject_type type, uint64_t shader_hash, const std::byte* shader_object, size_t shader_object_size) override
   {
      static const uint32_t pixel_shader_hash_water = Shader::Hash_StrToNum("4A96DC95");
      if (type == reshade::api::pipeline_subobject_type::pixel_shader && uint32_t(shader_hash) == pixel_shader_hash_water)
         return DXBC::ClampOutput0(code, &size, DXBC::Output0Clamp::BLEND_INPUT);
      return nullptr;
   }

   template <typename ShaderMap>
   static auto* FindShader(const ShaderMap& shaders, uint32_t name_hash)
   {
      const auto it = shaders.find(name_hash);
      return it != shaders.end() ? it->second.get() : nullptr;
   }

   // ---- Super resolution: the scene opens at its first mesh draw, the meshes take motion vectors and the viewport the jitter, the fill and
   // the upscaler run at the grade, which then reads the upscaler's output ----

   // Whether the bound draw tests depth against the scene's depth: the scene's draws, not full screen passes or shadow maps
   static bool IsSceneDepthTested(ID3D11DeviceContext* native_device_context, DragonAgeOriginsGameDeviceData* gd)
   {
      com_ptr<ID3D11DepthStencilView> dsv;
      native_device_context->OMGetRenderTargets(0, nullptr, &dsv);
      if (!dsv)
         return false;
      com_ptr<ID3D11Resource> depth;
      dsv->GetResource(&depth);
      if (depth != gd->scene_depth)
         return false;
      com_ptr<ID3D11DepthStencilState> state;
      native_device_context->OMGetDepthStencilState(&state, nullptr);
      if (state.get() != gd->depth_test_state)
      {
         D3D11_DEPTH_STENCIL_DESC desc = CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
         if (state)
         {
            state->GetDesc(&desc);
         }
         gd->depth_test = desc.DepthEnable;
         gd->depth_write = desc.DepthEnable && desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
         gd->depth_test_state = state.get();
      }
      return gd->depth_test;
   }

   // Every scene draw takes the jitter as a subpixel offset of its viewport: the same NDC shift as a projection jitter, with the game's
   // shaders as they are. The patched vertex shaders add the jitter buffer (zero here) to the same positions, and their motion vectors
   // never contain a jitter, so the two don't stack.
   static void JitterSceneViewport(ID3D11DeviceContext* native_device_context, DragonAgeOriginsGameDeviceData* gd, bool jitter)
   {
      UINT count = 1;
      D3D11_VIEWPORT viewport;
      native_device_context->RSGetViewports(&count, &viewport);
      if (count == 0)
         return;
      // The game sets its viewports itself: ours is still bound only if it hasn't since
      const bool jittered = gd->viewport_jittered && std::memcmp(&viewport, &gd->jittered_viewport, sizeof(viewport)) == 0;
      if (jitter == jittered)
         return;
      if (jitter)
      {
         gd->game_viewport = viewport;
         viewport.TopLeftX += gd->jitter[0];
         viewport.TopLeftY += gd->jitter[1];
         gd->jittered_viewport = viewport;
      }
      else
      {
         viewport = gd->game_viewport;
      }
      gd->viewport_jittered = jitter;
      native_device_context->RSSetViewports(1, &viewport);
   }

   // Opens the scene at the first scene mesh draw into the scene's depth: a single sample D24 of the colour target's size. Takes the
   // scene depth and its view for the fill and rolls the last frame's camera and objects over. Once per present.
   static void OpenScene(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, DeviceData& device_data, ID3D11DepthStencilView* dsv, const uint2& color_size, bool reactive)
   {
      auto& gd = GetGameDeviceData(device_data);
      com_ptr<ID3D11Resource> depth;
      dsv->GetResource(&depth);
      D3D11_TEXTURE2D_DESC depth_desc = {};
      if (com_ptr<ID3D11Texture2D> depth_texture; SUCCEEDED(depth->QueryInterface(&depth_texture)))
      {
         depth_texture->GetDesc(&depth_desc);
      }
      const uint2 depth_size = {depth_desc.Width, depth_desc.Height};
      if (depth_desc.SampleDesc.Count != 1 || depth_desc.Format != DXGI_FORMAT_R24G8_TYPELESS || (depth_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 || depth_size != color_size)
      {
         static bool logged = false;
         if (!std::exchange(logged, true))
         {
            reshade::log::message(reshade::log::level::warning, std::format("[Luma] Dragon Age Origins: the scene depth was refused (samples {}, format {}, bind flags {:#x}, size {}x{} against the colour's {}x{})", depth_desc.SampleDesc.Count, uint32_t(depth_desc.Format), uint32_t(depth_desc.BindFlags), depth_size.x, depth_size.y, color_size.x, color_size.y).c_str());
         }
         gd.open_refused_dsv = dsv;
         return;
      }
      if (depth != gd.scene_depth)
      {
         gd.scene_depth = depth;
         gd.scene_depth_srv.reset();
         const D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = CD3D11_SHADER_RESOURCE_VIEW_DESC(D3D11_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
         native_device->CreateShaderResourceView(depth.get(), &srv_desc, &gd.scene_depth_srv);
      }
      if (!SceneMotionVectors::BeginFrame(&gd.mv, native_device, native_device_context, dsv, depth_size, {}))
         return;

      // The masks at the scene's size; what the alpha blended draws write starts from zero every frame
      if (reactive && GetViewTextureSize(gd.mv.reactive_target_srv.get()) != depth_size)
      {
         gd.ReleaseReactiveMasks();
         const CD3D11_TEXTURE2D_DESC target_desc(DXGI_FORMAT_R8G8_UNORM, depth_size.x, depth_size.y, 1, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
         const CD3D11_TEXTURE2D_DESC mask_desc(DXGI_FORMAT_R8_UNORM, depth_size.x, depth_size.y, 1, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
         const CD3D11_TEXTURE2D_DESC previous_desc(DXGI_FORMAT_R32_FLOAT, depth_size.x, depth_size.y, 1, 1, D3D11_BIND_UNORDERED_ACCESS);
         if (FAILED(native_device->CreateTexture2D(&target_desc, nullptr, &gd.mv.reactive_target)) || FAILED(native_device->CreateRenderTargetView(gd.mv.reactive_target.get(), nullptr, &gd.mv.reactive_target_rtv)) ||
             FAILED(native_device->CreateShaderResourceView(gd.mv.reactive_target.get(), nullptr, &gd.mv.reactive_target_srv)) || FAILED(SRBridge::CreateSharableTexture(native_device, mask_desc, &gd.reactive)) ||
             FAILED(native_device->CreateUnorderedAccessView(gd.reactive.get(), nullptr, &gd.reactive_uav)) || FAILED(SRBridge::CreateSharableTexture(native_device, mask_desc, &gd.transparency)) ||
             FAILED(native_device->CreateUnorderedAccessView(gd.transparency.get(), nullptr, &gd.transparency_uav)) || FAILED(native_device->CreateTexture2D(&previous_desc, nullptr, &gd.reactive_previous)) ||
             FAILED(native_device->CreateUnorderedAccessView(gd.reactive_previous.get(), nullptr, &gd.reactive_previous_uav)))
         {
            gd.ReleaseReactiveMasks();
         }
         else
         {
            constexpr float zero[4] = {};
            native_device_context->ClearUnorderedAccessViewFloat(gd.reactive_previous_uav.get(), zero);
         }
      }
      gd.reactive_ready = reactive && gd.reactive_previous_uav;
      if (gd.reactive_ready)
      {
         constexpr FLOAT zero[4] = {};
         native_device_context->ClearRenderTargetView(gd.mv.reactive_target_rtv.get(), zero);
      }
   }

   // The upscaler that reads the masks: FSR (DLSS's current presets ignore them)
   static bool IsReactiveMaskUsed(const DeviceData& device_data)
   {
      return g_sr_reactive_enable && device_data.sr_type == SR::Type::FSR;
   }

   // The bound pixel shader's mask patch, from the pipeline cache's bytecode on first use (null if it can't be patched)
   static ID3D11PixelShader* GetPatchedPixelShader(ID3D11Device* native_device, DeviceData& device_data, uint64_t hash, reshade::api::pipeline pipeline, PixelShaderPatch patch)
   {
      auto& game_device_data = GetGameDeviceData(device_data);
      const uint64_t key = hash | (uint64_t(patch) << 32);
      {
         const std::shared_lock lock(game_device_data.patched_mutex);
         if (const auto it = game_device_data.patched_pixel_shaders.find(key); it != game_device_data.patched_pixel_shaders.end())
            return it->second.get();
      }
      std::vector<uint8_t> patched;
      std::string error;
      {
         const std::shared_lock lock(s_mutex_generic);
         if (const auto it = device_data.pipeline_cache_by_pipeline_handle.find(pipeline.handle); it != device_data.pipeline_cache_by_pipeline_handle.end() && it->second->subobjects_cache)
         {
            const auto* desc = static_cast<const reshade::api::shader_desc*>(it->second->subobjects_cache[0].data);
            patched = MotionVectorPatch::PatchPixelShaderReactive(static_cast<const uint8_t*>(desc->code), desc->code_size, MotionVectorPatch::DgVoodoo::MakeLayout(), REACTIVE_SLOT,
               (patch == PixelShaderPatch::MASK_ADDITIVE ? MotionVectorPatch::ReactiveMode::ADDITIVE : MotionVectorPatch::ReactiveMode::ALPHA), &error);
         }
      }
      com_ptr<ID3D11PixelShader> shader;
      if (!patched.empty() && FAILED(native_device->CreatePixelShader(patched.data(), patched.size(), nullptr, &shader)))
      {
         error = "create failed";
      }
      reshade::log::message(shader ? reshade::log::level::info : reshade::log::level::warning, std::format("[DAO] {} 0x{:08X} {}", PIXEL_SHADER_PATCH_NAMES[size_t(patch)], uint32_t(hash), shader ? (error.empty() ? std::string("patched") : "patched, " + error) : error).c_str());
      const std::unique_lock lock(game_device_data.patched_mutex);
      return game_device_data.patched_pixel_shaders.try_emplace(key, shader).first->second.get();
   }

   // An alpha blended scene draw after the opaque scene (the particles: smoke, sparks, glows, glass), which has no motion vectors of its
   // own, also writes its mask: its pixel shader patched, the mask target added at "REACTIVE_SLOT", both put back after. False if it
   // isn't one or can't be patched: the caller lets the game draw it.
   static bool DrawWithReactiveMask(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, DeviceData& device_data, uint64_t pixel_shader_hash, reshade::api::pipeline pipeline, const std::function<void()>& draw, com_ptr<ID3D11RenderTargetView> (&rtvs)[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT], ID3D11DepthStencilView* dsv)
   {
      auto& gd = GetGameDeviceData(device_data);
      com_ptr<ID3D11BlendState> blend_state;
      native_device_context->OMGetBlendState(&blend_state, nullptr, nullptr);
      if (blend_state.get() != gd.blend_state)
      {
         D3D11_BLEND_DESC desc = CD3D11_BLEND_DESC(D3D11_DEFAULT);
         if (blend_state)
         {
            blend_state->GetDesc(&desc);
         }
         // SRC_ALPHA / ONE (sparks, glows): reactive; SRC_ALPHA / INV_SRC_ALPHA (smoke, glass): reactive and transparency & composition
         const D3D11_RENDER_TARGET_BLEND_DESC& rt0 = desc.RenderTarget[0];
         gd.blend_patch = std::nullopt;
         if (rt0.BlendEnable && rt0.SrcBlend == D3D11_BLEND_SRC_ALPHA)
         {
            gd.blend_patch = (rt0.DestBlend == D3D11_BLEND_ONE ? PixelShaderPatch::MASK_ADDITIVE : PixelShaderPatch::MASK_ALPHA);
         }
         gd.blend_state = blend_state.get();
      }
      // A blended draw that writes depth is solid geometry (the masks are for content without motion vectors). The caller already
      // checked that the draw tests the scene's depth and gave the bound targets: the masks are the scene's size
      if (!gd.blend_patch || gd.depth_write)
         return false;
      // Into the scene: a full size target (not a smaller particle buffer)
      if (!rtvs[0] || GetViewTextureSize(rtvs[0].get()) != gd.mv.scene_size)
         return false;
      ID3D11PixelShader* const mask_shader = GetPatchedPixelShader(native_device, device_data, pixel_shader_hash, pipeline, *gd.blend_patch);
      if (!mask_shader)
         return false;

      com_ptr<ID3D11PixelShader> game_shader;
      native_device_context->PSGetShader(&game_shader, nullptr, nullptr);
      ID3D11RenderTargetView* targets[REACTIVE_SLOT + 1] = {};
      for (UINT slot = 0; slot < REACTIVE_SLOT; slot++)
      {
         targets[slot] = rtvs[slot].get();
      }
      targets[REACTIVE_SLOT] = gd.mv.reactive_target_rtv.get();
      native_device_context->OMSetRenderTargets(REACTIVE_SLOT + 1, targets, dsv);
      native_device_context->PSSetShader(mask_shader, nullptr, 0);
      draw();
      native_device_context->PSSetShader(game_shader.get(), nullptr, 0);
      native_device_context->OMSetRenderTargets(REACTIVE_SLOT, targets, dsv);
      return true;
   }

   // Logs why the upscaler didn't run, once per reason
   template <typename F>
   static void WarnSR(uint32_t reason, [[maybe_unused]] F&& make_text)
   {
#if DEVELOPMENT
      static std::atomic<uint32_t> logged = 0;
      if (!(logged.fetch_or(1u << reason) & (1u << reason)))
         reshade::log::message(reshade::log::level::warning, (std::string("[Luma] Dragon Age Origins: SR skipped: ") + make_text()).c_str());
#endif
   }

   // The fill ("Luma_DAO_MotionVectorFill.hlsl"): the camera motion of the texels no motion vector draw wrote, and the upscalers' depth.
   // Unbinds the output targets; the caller restores the game's state.
   static bool FillMotionVectors(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, DeviceData& device_data, bool write_reactive)
   {
      auto& gd = GetGameDeviceData(device_data);
      auto* const fill_shader = FindShader(device_data.native_compute_shaders, "DAO Motion Vector Fill CS"_h);
      // The targets exist once a draw was accepted ("DrawWithMotionVectors"), the camera once a draw of a camera kind was
      if (!fill_shader || !gd.mv.uav || !gd.mv.device_depth_uav || !gd.mv.camera || !gd.scene_depth_srv)
      {
         WarnSR(0, [&] { return std::format("fill inputs missing (shader {}, motion vector uav {}, depth uav {}, camera {}, depth srv {})", fill_shader != nullptr, gd.mv.uav != nullptr, gd.mv.device_depth_uav != nullptr, gd.mv.camera != nullptr, gd.scene_depth_srv != nullptr); });
         return false;
      }
      const uint2 size = gd.mv.scene_size;
      // The frame camera's reprojection and the viewport jitter (in the depth, not in the motion vectors)
      const float reactive_scale = g_sr_reactive_scale;
      CB::MotionVectorFillConstants fill_constants = {
         .jitter_ndc = {gd.jitter[0] * 2.f / float(size.x), gd.jitter[1] * -2.f / float(size.y)},
         .reactive_scale = reactive_scale,
         .reactive_threshold = g_sr_reactive_threshold,
         .reactive_enabled = (write_reactive ? 1.f : 0.f),
      };
      for (int i = 0; i < 16; i++)
      {
         fill_constants.reprojection.GetData()[i] = float(gd.mv.reprojection.GetData()[i]);
      }
      if (!PatchedDraws::WriteDynamicConstants(native_device, native_device_context, std::addressof(gd.mv.fill_buffer), &fill_constants, sizeof(fill_constants)))
         return false;
      // The depth and the motion vectors may be bound as targets
      native_device_context->OMSetRenderTargets(0, nullptr, nullptr);
      ID3D11Buffer* const buffer = gd.mv.fill_buffer.get();
      ID3D11ShaderResourceView* const srvs[2] = {gd.scene_depth_srv.get(), (write_reactive ? gd.mv.reactive_target_srv.get() : nullptr)};
      ID3D11UnorderedAccessView* const uavs[5] = {gd.mv.uav.get(), gd.mv.device_depth_uav.get(), (write_reactive ? gd.reactive_uav.get() : nullptr), (write_reactive ? gd.transparency_uav.get() : nullptr),
         (write_reactive ? gd.reactive_previous_uav.get() : nullptr)};
      native_device_context->CSSetConstantBuffers(0, 1, &buffer);
      native_device_context->CSSetShaderResources(0, UINT(std::size(srvs)), srvs);
      native_device_context->CSSetUnorderedAccessViews(0, UINT(std::size(uavs)), uavs, nullptr);
      native_device_context->CSSetShader(fill_shader, nullptr, 0);
      native_device_context->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
      ID3D11ShaderResourceView* const null_srvs[std::size(srvs)] = {};
      ID3D11UnorderedAccessView* const null_uavs[std::size(uavs)] = {};
      native_device_context->CSSetShaderResources(0, UINT(std::size(null_srvs)), null_srvs);
      native_device_context->CSSetUnorderedAccessViews(0, UINT(std::size(null_uavs)), null_uavs, nullptr);
      return true;
   }

   // The fill, then DLAA, FSR Native AA or DLSS on the jittered scene the grade reads ("scene"). The output's view (the
   // grade reads it in the scene's place), null if it didn't draw (missing input, or the upscaler failed).
   static ID3D11ShaderResourceView* DrawUpscaler(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, DeviceData& device_data, ID3D11ShaderResourceView* scene)
   {
      auto& gd = GetGameDeviceData(device_data);
      if (!scene || !gd.mv.texture || !std::exchange(gd.mv.fill_pending, false))
      {
         WarnSR(1, [&] { return std::format("no input (scene bound {}, motion vector target {})", scene != nullptr, gd.mv.texture != nullptr); });
         return nullptr;
      }
      com_ptr<ID3D11Resource> scene_resource;
      scene->GetResource(&scene_resource);
      com_ptr<ID3D11Texture2D> scene_texture;
      if (FAILED(scene_resource->QueryInterface(&scene_texture)))
         return nullptr;
      D3D11_TEXTURE2D_DESC scene_desc;
      scene_texture->GetDesc(&scene_desc);
      D3D11_TEXTURE2D_DESC mv_desc;
      gd.mv.texture->GetDesc(&mv_desc);
      if (scene_desc.Width != mv_desc.Width || scene_desc.Height != mv_desc.Height || scene_desc.SampleDesc.Count != 1)
      {
         WarnSR(2, [&] { return std::format("the grade's scene is {}x{} ({} samples), the motion vectors {}x{}", scene_desc.Width, scene_desc.Height, scene_desc.SampleDesc.Count, mv_desc.Width, mv_desc.Height); });
         return nullptr;
      }

      D3D11_TEXTURE2D_DESC output_desc = {};
      if (device_data.sr_output_color)
      {
         device_data.sr_output_color->GetDesc(&output_desc);
      }
      bool output_recreated = false;
      if (output_desc.Width != scene_desc.Width || output_desc.Height != scene_desc.Height)
      {
         device_data.sr_output_color.reset();
         gd.sr_output_srv.reset();
         output_desc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R16G16B16A16_FLOAT, scene_desc.Width, scene_desc.Height, 1, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
         if (SUCCEEDED(SRBridge::CreateSharableTexture(native_device, output_desc, &device_data.sr_output_color)))
         {
            native_device->CreateShaderResourceView(device_data.sr_output_color.get(), nullptr, &gd.sr_output_srv);
         }
         output_recreated = true;
      }
      if (!device_data.sr_output_color || !gd.sr_output_srv)
      {
         // Back to the native scene until the upscaler is picked again
         device_data.sr_suppressed = true;
         return nullptr;
      }
      // The masks from what this frame's alpha blended draws wrote
      const bool reactive_mask = gd.reactive_ready && IsReactiveMaskUsed(device_data);
      DrawStateStack<DrawStateStackType::FullGraphics> graphics_state;
      DrawStateStack<DrawStateStackType::Compute> compute_state;
      graphics_state.Cache(native_device_context, device_data.uav_max_count);
      compute_state.Cache(native_device_context, device_data.uav_max_count);
      if (!FillMotionVectors(native_device, native_device_context, device_data, reactive_mask))
      {
         compute_state.Restore(native_device_context);
         graphics_state.Restore(native_device_context);
         return nullptr;
      }

      bool drawn = false;
      {
         const SRDrawLock sr = LockSRForDraw(device_data);
         if (!sr.implementation)
         {
            WarnSR(3, [] { return "no SR implementation selected"; });
         }
         if (sr.implementation)
         {
            const SR::SettingsData settings_data = {
               .output_width = scene_desc.Width,
               .output_height = scene_desc.Height,
               .render_width = scene_desc.Width,
               .render_height = scene_desc.Height,
               // The scene before the grade is the unclamped fp16 one (the HDR aware resolve keeps it)
               .hdr = true,
               // The motion vectors are UV deltas, previous minus current
               .mvs_x_scale = float(scene_desc.Width),
               .mvs_y_scale = float(scene_desc.Height),
               // Not for DLSS (a fixed exposure): its transformer presets (J to M) flickered the dark, smooth sky, and an outline of it ghosted ahead
               // of the camera's rotation, as the auto exposure followed what was on screen. FSR doesn't have the problem.
               .auto_exposure = device_data.sr_type != SR::Type::DLSS,
               .render_preset = dlss_render_preset,
            };
            sr.implementation->UpdateSettings(sr.instance_data, native_device_context, settings_data);
            const SR::SuperResolutionImpl::DrawData draw_data = {
               .reset = device_data.force_reset_sr || output_recreated,
               .output_color = device_data.sr_output_color.get(),
               .source_color = scene_texture.get(),
               .motion_vectors = gd.mv.texture.get(),
               .depth_buffer = gd.mv.device_depth.get(),
               .bias_mask = (reactive_mask ? gd.reactive.get() : nullptr),
               .transparency_alpha = (reactive_mask ? gd.transparency.get() : nullptr),
               // As applied (pixels, +y down)
               .jitter_x = gd.jitter[0],
               .jitter_y = gd.jitter[1],
               .vert_fov = 2.f * std::atan(DAO_TAN_HALF_FOV_Y_CPP),
               .near_plane = DAO_NEAR_CPP,
               .far_plane = DAO_FAR_CPP,
            };
            drawn = sr.implementation->Draw(sr.instance_data, native_device_context, draw_data);
            // Back to the native scene until the upscaler is picked again
            device_data.sr_suppressed = !drawn;
            // Not while the bridge's helper starts (the color copied as it is): the frame stays native and the next one resets
            device_data.has_drawn_sr = drawn && sr.implementation->IsReady(sr.instance_data);
         }
      }
      ID3D11ShaderResourceView* result = (drawn ? gd.sr_output_srv.get() : nullptr);
      if (drawn && g_rcas_sharpness > 0.f && PrepareRCAS(native_device, device_data))
      {
         // The scene before the grade is linear-ish and unclamped: RCAS runs on it as it is (it limits its lobe against the neighbours)
         D3D11_TEXTURE2D_DESC sharpened_desc = {};
         if (gd.sr_sharpened)
            gd.sr_sharpened->GetDesc(&sharpened_desc);
         if (!gd.sr_sharpened || sharpened_desc.Width != scene_desc.Width || sharpened_desc.Height != scene_desc.Height)
         {
            gd.sr_sharpened.reset();
            gd.sr_sharpened_srv.reset();
            gd.sr_sharpened_rtv.reset();
            const CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R16G16B16A16_FLOAT, scene_desc.Width, scene_desc.Height, 1, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
            if (SUCCEEDED(native_device->CreateTexture2D(&desc, nullptr, &gd.sr_sharpened)))
            {
               native_device->CreateShaderResourceView(gd.sr_sharpened.get(), nullptr, &gd.sr_sharpened_srv);
               native_device->CreateRenderTargetView(gd.sr_sharpened.get(), nullptr, &gd.sr_sharpened_rtv);
            }
         }
         if (gd.sr_sharpened_srv && gd.sr_sharpened_rtv)
         {
            DrawRCAS(native_device_context, device_data, gd.sr_output_srv.get(), gd.sr_sharpened_rtv.get(), g_rcas_sharpness);
            result = gd.sr_sharpened_srv.get();
         }
      }
      compute_state.Restore(native_device_context);
      graphics_state.Restore(native_device_context);
      return result;
   }

   // The game's motion vector state for Core's buffer and bind events, null before the device data exists
   static SceneMotionVectors::State* GetSceneMotionVectors(reshade::api::device* device)
   {
      DeviceData* const device_data = device->get_private_data<DeviceData>();
      return ((device_data && device_data->game) ? &GetGameDeviceData(*device_data).mv : nullptr);
   }

   // What "SceneMotionVectors" asks of the game
   struct MotionVectorGame
   {
      static constexpr const char* log_tag = "DAO";
      // The depth prepass (PS 38A5D9CD) writes no colour (its blend state masks it), and is the only scene draw with one target: it carries the motion vectors
      static constexpr bool allow_colorless_draws = true;
      // The frame view projection (cb4 rows 238-241) is in the vc4 of every scene draw, also of the shaders that position with their own matrix
      static constexpr bool frame_camera_in_every_draw = true;
      // The static meshes carry a world view projection and a view space world matrix: both move with the camera, so a previous frame draw of
      // the same mesh cannot be told from its twins. Their motion is the camera's (the skinned meshes, drawn once each, match by key)
      static constexpr bool static_transform_from_frame_camera = true;
      // The lit materials (the characters among them, PS D24D2C23) are never in the prepass: they draw colour and a second target together
      static constexpr bool allow_extra_targets = true;
      // The frame camera is sometimes read from another pass for a frame or two (it flips by half a turn and back): the last good reprojection
      // is kept for the sky and the other texels the fill gives the camera motion to
      static constexpr bool reuse_last_reprojection = true;
      // The static meshes' world view projection and the frame view projection are D3D9 row vector matrices (position * M, the translation in the 4th row; checked: WVP = world * VP)
      static constexpr bool row_vector_matrices = true;
      static constexpr MotionVectorPatch::Layout layout = MotionVectorPatches::layout;
      static constexpr auto cameras = MotionVectorPatches::cameras;
      static constexpr auto tie_break_rows = MotionVectorPatches::tie_break_rows;
      static constexpr auto frame_camera = MotionVectorPatches::frame_camera;
      static constexpr const char* patch_names[size_t(SceneMotionVectors::PixelShaderPatch::COUNT)] = {"MV PS", "", "", "MV PS (blended)", "MV PS (blended, mask)", "", "", ""};
      // The unmatched alpha blended draws (particles: their vertices are rewritten every frame, so no draw key repeats) also write the masks
      static constexpr SceneMotionVectors::PixelShaderPatch blended_mask_patch = SceneMotionVectors::PixelShaderPatch::GAME_0;

      // The scene's colour: single sample, the upgraded fp16 target (or its 8-bit original)
      static bool AcceptSceneColor(const D3D11_RENDER_TARGET_VIEW_DESC& rtv_desc, const D3D11_TEXTURE2D_DESC& color_desc)
      {
         return color_desc.SampleDesc.Count == 1 && (rtv_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || rtv_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || rtv_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
      }

      static bool MaskUnmatchedBlended(DeviceData& device_data)
      {
         return GetGameDeviceData(device_data).reactive_ready && IsReactiveMaskUsed(device_data);
      }
      // The alpha blended draws blend their motion vectors over the opaque ones: the texels none wrote get their camera motion first
      static void FillMarkersBeforeBlend(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, DeviceData& device_data)
      {
         FillMotionVectors(native_device, native_device_context, device_data, /* write_reactive */ false);
      }
      static bool IsExcludedPixelShader(uint32_t) { return false; }
      static bool ReactiveMaskUsed(const DeviceData&) { return false; }
      static bool CanFill(DeviceData& device_data)
      {
         return GetGameDeviceData(device_data).scene_depth_srv && HasShaders(device_data.native_compute_shaders, "DAO Motion Vector Fill CS"_h);
      }

      // A game pixel shader's "patch", from the bytecode its shader object was created with; empty with "error" if it can't be patched.
      static std::vector<uint8_t> PatchPixelShader(SceneMotionVectors::PixelShaderPatch patch, const uint8_t* code, size_t code_size, std::string* error)
      {
         switch (patch)
         {
         case SceneMotionVectors::PixelShaderPatch::MOTION_VECTORS:
            return MotionVectorPatch::PatchPixelShader(code, code_size, MotionVectorPatches::layout, error, /* targets_only */ true);
         case SceneMotionVectors::PixelShaderPatch::BLENDED:
            return MotionVectorPatch::PatchPixelShaderReactive(code, code_size, MotionVectorPatches::layout, UINT32_MAX, MotionVectorPatch::ReactiveMode::ALPHA_MOTION_VECTORS, error);
         case blended_mask_patch:
            return MotionVectorPatch::PatchPixelShaderReactive(code, code_size, MotionVectorPatches::layout, MotionVectorPatches::reactive_slot, MotionVectorPatch::ReactiveMode::ALPHA_MOTION_VECTORS, error);
         }
         return {};
      }
   };

public:
   // The fill and the ambient occlusion read the scene's depth, which dgVoodoo creates typeless (R24G8) without a shader resource bind
   // when it is single sample (the MSAA one has it). The bind flag only: the format stays as created.
   static bool OnCreateResource(reshade::api::device* device, reshade::api::resource_desc& desc, reshade::api::subresource_data* initial_data, reshade::api::resource_usage initial_state)
   {
      if (desc.type == reshade::api::resource_type::texture_2d && desc.texture.samples == 1 && desc.texture.format == reshade::api::format::r24_g8_typeless && (desc.usage & reshade::api::resource_usage::depth_stencil) != 0 &&
          (desc.usage & reshade::api::resource_usage::shader_resource) == 0)
      {
         desc.usage |= reshade::api::resource_usage::shader_resource;
         return true;
      }
      return false;
   }

   // Core's motion vector events (the motion vector target in every blend state, the tracked bindings, the vc4 copies) and the extra binds
   static void SetEvents(bool enable)
   {
      if (enable)
      {
         SceneMotionVectors::RegisterEvents<GetSceneMotionVectors>();
         reshade::register_event<reshade::addon_event::create_resource>(OnCreateResource);
      }
      else
      {
         SceneMotionVectors::UnregisterEvents<GetSceneMotionVectors>();
         reshade::unregister_event<reshade::addon_event::create_resource>(OnCreateResource);
      }
   }

private:
   // A texture plus the views it needs, for the AO scratch (one UAV per mip)
   static bool CreateAOTexture(ID3D11Device* device, uint32_t width, uint32_t height, UINT mips, DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& srv, ComPtr<ID3D11UnorderedAccessView>* uavs)
   {
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = width;
      desc.Height = height;
      desc.MipLevels = mips;
      desc.ArraySize = 1;
      desc.Format = format;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
      if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.put())) || FAILED(device->CreateShaderResourceView(texture.get(), nullptr, srv.put())))
         return false;
      for (UINT i = 0; i < mips; i++)
      {
         D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
         uav_desc.Format = format;
         uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
         uav_desc.Texture2D.MipSlice = i;
         if (FAILED(device->CreateUnorderedAccessView(texture.get(), &uav_desc, uavs[i].put())))
            return false;
      }
      return true;
   }

   static void ReleaseAOScratch(DragonAgeOriginsGameDeviceData& gd)
   {
      gd.ao_linear_depth.reset();
      gd.ao_linear_depth_uav.reset();
      gd.ao_linear_depth_srv.reset();
      gd.ao_depth_mips.reset();
      for (auto& uav : gd.ao_depth_mip_uavs)
         uav.reset();
      gd.ao_depth_mips_srv.reset();
      for (int i = 0; i < 2; i++)
      {
         gd.ao_working[i].reset();
         gd.ao_working_uavs[i].reset();
         gd.ao_working_srvs[i].reset();
      }
      gd.ao_final.reset();
      gd.ao_final_uav.reset();
      gd.ao_final_srv.reset();
      gd.ao_depth_srv.reset();
      gd.ao_depth_resource = nullptr;
      gd.ao_cb.reset();
      gd.ao_width = gd.ao_height = 0;
   }

   static bool CreateImmutableCB(ID3D11Device* device, const void* data, UINT size, ComPtr<ID3D11Buffer>& out)
   {
      out.reset();
      D3D11_BUFFER_DESC desc = {};
      desc.ByteWidth = size;
      desc.Usage = D3D11_USAGE_IMMUTABLE;
      desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      D3D11_SUBRESOURCE_DATA init = {};
      init.pSysMem = data;
      return SUCCEEDED(device->CreateBuffer(&desc, &init, out.put()));
   }

   static void WarnAO(DragonAgeOriginsGameDeviceData& gd, const char* reason)
   {
      if (gd.ao_warned)
         return;
      gd.ao_warned = true;
      char message[256];
      snprintf(message, sizeof(message), "[Luma] Dragon Age Origins: ambient occlusion skipped (%s)", reason);
      reshade::log::message(reshade::log::level::warning, message);
   }

   // Ambient occlusion, added on the scene depth (the game has none): linearize the depth, run the 4 XeGTAO passes into our own
   // textures, then multiply the visibility into the scene colour. It runs at the first particle draw, which comes after the
   // opaque geometry and the post-opaque copy pass, with the scene colour (MSAA or not) and the depth still bound.
   static void RunXeGTAO(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, DeviceData& device_data, DragonAgeOriginsGameDeviceData& gd)
   {
      ComPtr<ID3D11RenderTargetView> scene_rtv;
      ComPtr<ID3D11DepthStencilView> depth_dsv;
      native_device_context->OMGetRenderTargets(1, scene_rtv.put(), depth_dsv.put());
      if (!scene_rtv || !depth_dsv)
         return;

      ComPtr<ID3D11Resource> depth_resource;
      depth_dsv->GetResource(depth_resource.put());
      ComPtr<ID3D11Resource> scene_resource;
      scene_rtv->GetResource(scene_resource.put());
      ComPtr<ID3D11Texture2D> depth_texture;
      ComPtr<ID3D11Texture2D> scene_texture;
      if (FAILED(depth_resource->QueryInterface(depth_texture.put())) || FAILED(scene_resource->QueryInterface(scene_texture.put())))
         return;
      D3D11_TEXTURE2D_DESC depth_desc;
      D3D11_TEXTURE2D_DESC scene_desc;
      depth_texture->GetDesc(&depth_desc);
      scene_texture->GetDesc(&scene_desc);
      // Only the main scene pass: a depth buffer of the same size and sample count as the colour, that we can read as D24 (a shadow map etc. is not)
      if (depth_desc.Format != DXGI_FORMAT_R24G8_TYPELESS || (depth_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 || depth_desc.Width != scene_desc.Width || depth_desc.Height != scene_desc.Height || depth_desc.SampleDesc.Count != scene_desc.SampleDesc.Count || depth_desc.ArraySize != 1)
         return;
      const uint32_t w = depth_desc.Width;
      const uint32_t h = depth_desc.Height;
      const bool ms = depth_desc.SampleDesc.Count > 1;

      ID3D11ComputeShader* cs_linearize = FindShader(device_data.native_compute_shaders, (ms ? "DAO Depth Linearize MS CS"_h : "DAO Depth Linearize CS"_h));
      ID3D11ComputeShader* cs_prefilter = FindShader(device_data.native_compute_shaders, CompileTimeStringHash("DAO XeGTAO Prefilter Depths CS"));
      const bool gtvbao = g_ao_technique == AOTechnique::GTVBAO;
      ID3D11ComputeShader* cs_main = FindShader(device_data.native_compute_shaders, (gtvbao ? "DAO GT-VBAO Main Pass CS"_h : "DAO XeGTAO Main Pass CS"_h));
      ID3D11ComputeShader* cs_denoise_1 = FindShader(device_data.native_compute_shaders, CompileTimeStringHash("DAO XeGTAO Denoise Pass 1 CS"));
      ID3D11ComputeShader* cs_denoise_2 = FindShader(device_data.native_compute_shaders, CompileTimeStringHash("DAO XeGTAO Denoise Pass 2 CS"));
      ID3D11PixelShader* ps_apply = FindShader(device_data.native_pixel_shaders, CompileTimeStringHash("DAO AO Apply PS"));
      ID3D11VertexShader* vs_copy = FindShader(device_data.native_vertex_shaders, CompileTimeStringHash("Copy VS"));
      if (!cs_linearize || !cs_prefilter || !cs_main || !cs_denoise_1 || !cs_denoise_2 || !ps_apply || !vs_copy) // Still compiling (or reloading in development builds)
         return;

      // Scratch, all or nothing. A failure latches for this size so a failing allocation isn't retried (and fragmenting) every frame.
      if (gd.ao_width != w || gd.ao_height != h)
      {
         ReleaseAOScratch(gd);
         ComPtr<ID3D11UnorderedAccessView> linear_uavs[1];
         ComPtr<ID3D11UnorderedAccessView> working_uavs[2][1];
         ComPtr<ID3D11UnorderedAccessView> final_uavs[1];
         const bool ok = CreateAOTexture(native_device, w, h, 1, DXGI_FORMAT_R32_FLOAT, gd.ao_linear_depth, gd.ao_linear_depth_srv, linear_uavs) && CreateAOTexture(native_device, w, h, 5, DXGI_FORMAT_R32_FLOAT, gd.ao_depth_mips, gd.ao_depth_mips_srv, gd.ao_depth_mip_uavs) && CreateAOTexture(native_device, w, h, 1, DXGI_FORMAT_R8G8_UNORM, gd.ao_working[0], gd.ao_working_srvs[0], working_uavs[0]) && CreateAOTexture(native_device, w, h, 1, DXGI_FORMAT_R8G8_UNORM, gd.ao_working[1], gd.ao_working_srvs[1], working_uavs[1]) && CreateAOTexture(native_device, w, h, 1, DXGI_FORMAT_R8_UNORM, gd.ao_final, gd.ao_final_srv, final_uavs);
         if (ok)
         {
            gd.ao_linear_depth_uav = linear_uavs[0];
            gd.ao_working_uavs[0] = working_uavs[0][0];
            gd.ao_working_uavs[1] = working_uavs[1][0];
            gd.ao_final_uav = final_uavs[0];
         }
         else
         {
            ReleaseAOScratch(gd);
         }
         gd.ao_width = w; // Latched, also on failure
         gd.ao_height = h;
      }
      if (!gd.ao_final_uav || !gd.ao_linear_depth_uav)
      {
         WarnAO(gd, "the scratch textures could not be created (out of memory?)");
         return;
      }

      // The game's depth as a shader resource (R24 + the unused X8), single sample or MSAA (the linearizer reads sample 0)
      if (gd.ao_depth_resource != depth_resource.get() || !gd.ao_depth_srv)
      {
         gd.ao_depth_srv.reset();
         D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
         srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
         srv_desc.ViewDimension = ms ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
         if (!ms)
            srv_desc.Texture2D.MipLevels = 1;
         if (FAILED(native_device->CreateShaderResourceView(depth_resource.get(), &srv_desc, gd.ao_depth_srv.put())))
         {
            gd.ao_depth_resource = nullptr;
            WarnAO(gd, "the depth buffer can't be read as a shader resource");
            return;
         }
         gd.ao_depth_resource = depth_resource.get();
      }

      // Knobs: power, depth scale (the game's units are meters), radius, debug view, 1 / size, tan(fov / 2) for x and y (60 degrees vertical)
      constexpr float tan_half_fov_y = 0.577350269f;
#if DEVELOPMENT
      const float debug_view = (float)g_gtao_debug_view;
#else
      const float debug_view = 0.f;
#endif
      // GT-VBAO's visibility is brighter at the same power (the bitmask counts occluded sectors, thickness included), so it takes a lower one
      // to look as dark as XeGTAO at the same slider position (calibrated against it in other mods at 1.6 vs 2.2).
      const float power_scale = gtvbao ? (1.6f / 2.2f) : 1.f;
      const float knobs[8] = {g_ao_darkness * 4.f * power_scale, 1.f, g_gtao_radius_override, debug_view, 1.f / (float)w, 1.f / (float)h, tan_half_fov_y * (float)w / (float)h, tan_half_fov_y};
      if (!gd.ao_cb || memcmp(knobs, gd.ao_cb_data, sizeof(knobs)) != 0)
      {
         if (!CreateImmutableCB(native_device, knobs, sizeof(knobs), gd.ao_cb))
            return;
         memcpy(gd.ao_cb_data, knobs, sizeof(knobs));
      }
      if (!gd.ao_apply_cb || gd.ao_apply_strength != g_ao_strength)
      {
         const float apply_params[4] = {g_ao_strength, 0.f, 0.f, 0.f};
         if (!CreateImmutableCB(native_device, apply_params, sizeof(apply_params), gd.ao_apply_cb))
            return;
         gd.ao_apply_strength = g_ao_strength;
      }
      if (!gd.ao_multiply_blend)
      {
         // dest * source (the apply pass outputs the visibility), alpha untouched
         D3D11_BLEND_DESC blend_desc = {};
         blend_desc.RenderTarget[0].BlendEnable = TRUE;
         blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
         blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
         blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
         blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
         blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
         blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
         blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
         if (FAILED(native_device->CreateBlendState(&blend_desc, gd.ao_multiply_blend.put())))
            return;
      }

      // The depth is still bound as the depth stencil view: D3D11 would silently null our shader resource view of it, so unbind
      // the outputs first (the graphics stack restores them), and keep the compute state apart.
      DrawStateStack<DrawStateStackType::FullGraphics> graphics_state;
      graphics_state.Cache(native_device_context, device_data.uav_max_count);
      native_device_context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);

      DrawStateStack<DrawStateStackType::Compute> compute_state;
      compute_state.Cache(native_device_context, device_data.uav_max_count);
      native_device_context->CSSetConstantBuffers(kGTAOKnobsCBSlot, 1, gd.ao_cb.get_addressof());
      ID3D11SamplerState* point_sampler = device_data.sampler_state_point.get();
      native_device_context->CSSetSamplers(0, 1, &point_sampler);

      static constexpr std::array<ID3D11UnorderedAccessView*, 5> uav_nulls = {};
      static constexpr std::array<ID3D11ShaderResourceView*, 2> srv_nulls = {};

      // Linearize the depth. Each destination UAV is bound before its source SRVs: D3D11 otherwise silently nulls the conflicting SRV.
      native_device_context->CSSetShaderResources(0, 2, srv_nulls.data());
      native_device_context->CSSetUnorderedAccessViews(0, 1, gd.ao_linear_depth_uav.get_addressof(), nullptr);
      native_device_context->CSSetShaderResources(0, 1, gd.ao_depth_srv.get_addressof());
      native_device_context->CSSetShader(cs_linearize, nullptr, 0);
      native_device_context->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
      native_device_context->CSSetUnorderedAccessViews(0, 5, uav_nulls.data(), nullptr);

      // Prefilter into the mip pyramid, each thread covers 2x2 pixels
      native_device_context->CSSetShaderResources(0, 2, srv_nulls.data());
      {
         ID3D11UnorderedAccessView* uavs[5] = {gd.ao_depth_mip_uavs[0].get(), gd.ao_depth_mip_uavs[1].get(), gd.ao_depth_mip_uavs[2].get(), gd.ao_depth_mip_uavs[3].get(), gd.ao_depth_mip_uavs[4].get()};
         native_device_context->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
      }
      native_device_context->CSSetShaderResources(0, 1, gd.ao_linear_depth_srv.get_addressof());
      native_device_context->CSSetShader(cs_prefilter, nullptr, 0);
      native_device_context->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
      native_device_context->CSSetUnorderedAccessViews(0, 5, uav_nulls.data(), nullptr);

      // Main pass (the normals are generated from depth inside the shader)
      native_device_context->CSSetShaderResources(0, 2, srv_nulls.data());
      native_device_context->CSSetUnorderedAccessViews(0, 1, gd.ao_working_uavs[0].get_addressof(), nullptr);
      native_device_context->CSSetShaderResources(0, 1, gd.ao_depth_mips_srv.get_addressof());
      native_device_context->CSSetShader(cs_main, nullptr, 0);
      native_device_context->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

      // First denoiser, two horizontal pixels per thread
      native_device_context->CSSetShaderResources(0, 2, srv_nulls.data());
      native_device_context->CSSetUnorderedAccessViews(0, 1, gd.ao_working_uavs[1].get_addressof(), nullptr);
      native_device_context->CSSetShaderResources(0, 1, gd.ao_working_srvs[0].get_addressof());
      native_device_context->CSSetShader(cs_denoise_1, nullptr, 0);
      native_device_context->Dispatch((w + 15) / 16, (h + 7) / 8, 1);

      // Second denoiser, into the final visibility
      native_device_context->CSSetShaderResources(0, 2, srv_nulls.data());
      native_device_context->CSSetUnorderedAccessViews(0, 1, gd.ao_final_uav.get_addressof(), nullptr);
      native_device_context->CSSetShaderResources(0, 1, gd.ao_working_srvs[1].get_addressof());
      native_device_context->CSSetShader(cs_denoise_2, nullptr, 0);
      native_device_context->Dispatch((w + 15) / 16, (h + 7) / 8, 1);
      native_device_context->CSSetShaderResources(0, 2, srv_nulls.data());
      native_device_context->CSSetUnorderedAccessViews(0, 5, uav_nulls.data(), nullptr);

      compute_state.Restore(native_device_context);

      // Multiply it into the scene colour (all the samples, if MSAA)
      native_device_context->PSSetConstantBuffers(kGTAOKnobsCBSlot, 1, gd.ao_apply_cb.get_addressof());
      DrawCustomPixelShader(native_device_context, device_data.default_depth_stencil_state.get(), gd.ao_multiply_blend.get(), nullptr, vs_copy, ps_apply, gd.ao_final_srv.get(), scene_rtv.get(), w, h, true);

      graphics_state.Restore(native_device_context);
   }

   DrawOrDispatchOverrideType OnDrawOrDispatch(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, CommandListData& cmd_list_data, DeviceData& device_data, reshade::api::shader_stage stages, const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes, bool is_custom_pass, bool& updated_cbuffers, std::function<void()>* original_draw_dispatch_func) override
   {
      auto& game_device_data = GetGameDeviceData(device_data);
      const bool is_immediate = native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
      const bool can_draw = is_immediate && original_draw_dispatch_func && *original_draw_dispatch_func;

      // Hide UI: the game's draws after the grade into the same canvas are the HUD, menus and cursor. The target test is load-bearing:
      // dgVoodoo's present pass (0x880A17D3) also runs after the grade, reading the canvas into the swapchain. Menus without a 3D scene
      // (no grade that frame) are never hidden.
      if (g_hide_ui && is_immediate && !is_custom_pass && game_device_data.has_drawn_tonemap && game_device_data.canvas_res)
      {
         ComPtr<ID3D11RenderTargetView> rtv;
         native_device_context->OMGetRenderTargets(1, rtv.put(), nullptr);
         ComPtr<ID3D11Resource> rt;
         if (rtv)
            rtv->GetResource(rt.put());
         if (rt && rt.get() == game_device_data.canvas_res.get())
            return DrawOrDispatchOverrideType::Replaced;
      }

      // Before the super resolution block, which can take this draw over (a particle with a mask or motion vectors is drawn there)
      // The first particle draw of the frame comes after the opaque geometry and the post-opaque copy pass that redraws the scene
      // (9617B1F5), and the depth and the scene colour are still bound: the point to add the ambient occlusion.
      static uint32_t pixel_shader_hash_particles = Shader::Hash_StrToNum("5DE6B6F6");
      if (g_ao_enable && !game_device_data.ao_done_this_frame && original_shader_hashes.Contains(pixel_shader_hash_particles, reshade::api::shader_stage::pixel))
      {
         game_device_data.ao_done_this_frame = true;
         RunXeGTAO(native_device, native_device_context, device_data, game_device_data);
      }

      // Bloom off: the game's bright pass, blurs and composite (the only draw that adds the bloom to the scene) are all skipped
      if (cb_luma_global_settings.GameSettings.BloomType < 0.5f)
      {
         static const uint32_t pixel_shader_hashes_bloom[] = {Shader::Hash_StrToNum(kBloomBrightPassHash), Shader::Hash_StrToNum(kBloomBlurHash), Shader::Hash_StrToNum(kBloomCompositeHash)};
         for (const uint32_t hash : pixel_shader_hashes_bloom)
         {
            if (original_shader_hashes.Contains(hash, reshade::api::shader_stage::pixel))
               return DrawOrDispatchOverrideType::Replaced;
         }
      }

      // Luma's bloom in place of the game's: at the bright pass it is made from the scene the pass reads, the game's bright pass and blurs
      // are skipped, and its composite reads ours in place of the blurred one
      if (can_draw && cb_luma_global_settings.GameSettings.BloomType > 1.5f)
      {
         static const uint32_t pixel_shader_hash_bloom_bright = Shader::Hash_StrToNum(kBloomBrightPassHash);
         static const uint32_t pixel_shader_hash_bloom_blur = Shader::Hash_StrToNum(kBloomBlurHash);
         static const uint32_t pixel_shader_hash_bloom_composite = Shader::Hash_StrToNum(kBloomCompositeHash);
         if (original_shader_hashes.Contains(pixel_shader_hash_bloom_bright, reshade::api::shader_stage::pixel))
         {
            if (!game_device_data.bloom_drawn_this_frame)
            {
               const std::shared_lock lock_shader_objects(s_mutex_shader_objects);
               if (FindShader(device_data.native_vertex_shaders, "Bloom VS"_h) && FindShader(device_data.native_pixel_shaders, "Bloom Prefilter PS"_h) && FindShader(device_data.native_pixel_shaders, "Bloom Downsample PS"_h) &&
                   FindShader(device_data.native_pixel_shaders, "Bloom Upsample PS"_h) && FindShader(device_data.native_compute_shaders, "Karis Average CS"_h))
               {
                  ComPtr<ID3D11ShaderResourceView> srv_scene;
                  native_device_context->PSGetShaderResources(0, 1, srv_scene.put());
                  if (srv_scene)
                  {
                     // The prefilter reads the threshold and the intensity from LumaSettings: push them at this seam, then cache them with the rest
                     SetLumaConstantBuffers(native_device_context, cmd_list_data, device_data, reshade::api::shader_stage::pixel, LumaConstantBufferType::LumaSettings);
                     DrawStateStack<DrawStateStackType::FullGraphics> bloom_state;
                     bloom_state.Cache(native_device_context, device_data.uav_max_count);
                     ComPtr<ID3D11ShaderResourceView> srv_karis;
                     DrawKarisAverage(native_device, native_device_context, device_data, srv_scene.get(), srv_karis.put());
                     if (srv_karis)
                     {
                        // The blurs' widths, scaled by the Bloom Spread setting
                        float sigmas[std::size(g_bloom_sigmas)];
                        for (size_t i = 0; i < std::size(g_bloom_sigmas); i++)
                        {
                           sigmas[i] = g_bloom_sigmas[i] * std::clamp(cb_luma_global_settings.GameSettings.BloomSpread, 0.25f, 3.f);
                        }
                        DrawBloom(native_device, native_device_context, device_data, srv_karis.get(), (int)std::size(g_bloom_sigmas), sigmas, game_device_data.srv_luma_bloom.put());
                     }
                     bloom_state.Restore(native_device_context);
                     game_device_data.bloom_drawn_this_frame = (game_device_data.srv_luma_bloom != nullptr);
                  }
               }
            }
            // Only skipped once ours exists: until then (shaders still compiling) the game's bloom draws
            if (game_device_data.bloom_drawn_this_frame)
               return DrawOrDispatchOverrideType::Replaced;
         }
         else if (game_device_data.bloom_drawn_this_frame && original_shader_hashes.Contains(pixel_shader_hash_bloom_blur, reshade::api::shader_stage::pixel))
         {
            return DrawOrDispatchOverrideType::Replaced;
         }
         else if (game_device_data.bloom_drawn_this_frame && game_device_data.srv_luma_bloom && original_shader_hashes.Contains(pixel_shader_hash_bloom_composite, reshade::api::shader_stage::pixel))
         {
            ComPtr<ID3D11ShaderResourceView> game_bloom;
            native_device_context->PSGetShaderResources(0, 1, game_bloom.put());
            ID3D11ShaderResourceView* const luma_bloom = game_device_data.srv_luma_bloom.get();
            native_device_context->PSSetShaderResources(0, 1, &luma_bloom);
            (*original_draw_dispatch_func)();
            ID3D11ShaderResourceView* const restored = game_bloom.get();
            native_device_context->PSSetShaderResources(0, 1, &restored);
            return DrawOrDispatchOverrideType::Replaced;
         }
      }

      // The cutscene depth of field's composite blends the game's blurred scene over the scene: the bokeh gather reads the sharp scene, so a copy
      // of the target is bound at t4 for the draw (the shader falls back on the game's blur when none is bound)
      static const uint32_t pixel_shader_hash_dof = Shader::Hash_StrToNum("B507EFCE");
      if (can_draw && cb_luma_global_settings.GameSettings.BokehDoFEnable > 0.5f && original_shader_hashes.Contains(pixel_shader_hash_dof, reshade::api::shader_stage::pixel))
      {
         if (BindDoFScene(native_device, native_device_context, game_device_data))
         {
            (*original_draw_dispatch_func)();
            ID3D11ShaderResourceView* const null_srv = nullptr;
            native_device_context->PSSetShaderResources(4, 1, &null_srv);
            ID3D11SamplerState* const null_sampler = nullptr;
            native_device_context->PSSetSamplers(4, 1, &null_sampler);
            return DrawOrDispatchOverrideType::Replaced;
         }
      }

      // The colour grade (matrix + 256x1 curve lookup), see "ColorGrade_0x6B504D9F.ps_5_0.hlsl".
      static uint32_t pixel_shader_hash_tonemap = Shader::Hash_StrToNum("6B504D9F");
      // The scene's opaque mesh materials (the first one of the frame opens the scene)
      static const uint32_t pixel_shader_hashes_scene[] = {Shader::Hash_StrToNum("38A5D9CD"), Shader::Hash_StrToNum("D24D2C23")};

      // DLAA/FSR/DLSS: the scene opens at its first mesh draw and the draws into it take motion vectors (see
      // "DrawWithMotionVectors") and the viewport jitter; the fill and the upscaler run at the grade
      if (game_device_data.mv.active && is_immediate && !game_device_data.has_drawn_tonemap)
      {
         if (!game_device_data.mv.scene_open && !game_device_data.mv.scene_done)
         {
            for (const uint32_t hash : pixel_shader_hashes_scene)
            {
               if (!original_shader_hashes.Contains(hash, reshade::api::shader_stage::pixel))
                  continue;
               com_ptr<ID3D11RenderTargetView> rtv;
               com_ptr<ID3D11DepthStencilView> dsv;
               native_device_context->OMGetRenderTargets(1, &rtv, &dsv);
               if (rtv && dsv && dsv.get() != game_device_data.open_refused_dsv)
               {
                  OpenScene(native_device, native_device_context, device_data, dsv.get(), GetViewTextureSize(rtv.get()), IsReactiveMaskUsed(device_data));
               }
               break;
            }
         }
         if (game_device_data.mv.scene_open)
         {
            const bool scene_draw = IsSceneDepthTested(native_device_context, &game_device_data);
            if (game_device_data.jitter != std::array<float, 2>{} || game_device_data.viewport_jittered)
            {
               JitterSceneViewport(native_device_context, &game_device_data, scene_draw);
            }
            const bool has_vertex_shader = (stages & reshade::api::shader_stage::vertex) == reshade::api::shader_stage::vertex;
            if (scene_draw && can_draw && !is_custom_pass && has_vertex_shader && original_shader_hashes.pixel_shaders[0] != SHADER_HASH_NONE)
            {
               com_ptr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
               com_ptr<ID3D11DepthStencilView> dsv;
               native_device_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, &rtvs[0], &dsv);
               game_device_data.mv.object_motion = true;
               // The draws that write depth (the opaque meshes) and the alpha blended ones (their own motion blended by their alpha),
               // into a target of the scene's size (not a shadow map or a smaller particle buffer)
               bool offered = game_device_data.depth_write;
               if (!offered)
               {
                  SceneMotionVectors::ClassifyBoundBlend(native_device_context, &game_device_data.mv);
                  offered = game_device_data.mv.alpha_blended;
               }
               if (offered && (!rtvs[0] || GetViewTextureSize(rtvs[0].get()) != game_device_data.mv.scene_size))
               {
                  offered = false;
               }
               const bool motion_vectors = offered && SceneMotionVectors::DrawWithMotionVectors<MotionVectorGame>(&game_device_data.mv, native_device, native_device_context, cmd_list_data, device_data, original_shader_hashes, *original_draw_dispatch_func, rtvs, dsv.get());
               // An alpha blending state blends the motion vector target by the alpha written there: a draw that doesn't write it
               // must not have it bound
               if (!motion_vectors && rtvs[MotionVectorPatches::target_slot] && rtvs[MotionVectorPatches::target_slot] == game_device_data.mv.rtv)
               {
                  SceneMotionVectors::ClassifyBoundBlend(native_device_context, &game_device_data.mv);
                  if (game_device_data.mv.alpha_blended)
                  {
                     rtvs[MotionVectorPatches::target_slot].reset();
                     ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
                     for (UINT slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; slot++)
                     {
                        targets[slot] = rtvs[slot].get();
                     }
                     native_device_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, dsv.get());
                  }
               }
               if (motion_vectors)
                  return DrawOrDispatchOverrideType::Replaced;
               if (game_device_data.reactive_ready && DrawWithReactiveMask(native_device, native_device_context, device_data, original_shader_hashes.pixel_shaders[0], cmd_list_data.pipeline_state_original_pixel_shader, *original_draw_dispatch_func, rtvs, dsv.get()))
                  return DrawOrDispatchOverrideType::Replaced;
            }
         }
      }
      // Every draw without a patched shader: the game's own, if the last patched draw's are still bound
      if (is_immediate)
      {
         PatchedDraws::RestoreGameShader(native_device_context, &game_device_data.mv.bound_vertex_shader);
         PatchedDraws::RestoreGameShader(native_device_context, &game_device_data.mv.bound_pixel_shader);
      }

      if (!game_device_data.has_drawn_tonemap && original_shader_hashes.Contains(pixel_shader_hash_tonemap, reshade::api::shader_stage::pixel))
      {
         game_device_data.has_drawn_tonemap = true;

         device_data.has_drawn_main_post_processing = true;

         // What the grade draws into (see "canvas_res")
         {
            ComPtr<ID3D11RenderTargetView> canvas_rtv;
            native_device_context->OMGetRenderTargets(1, canvas_rtv.put(), nullptr);
            game_device_data.canvas_res.reset();
            if (canvas_rtv)
               canvas_rtv->GetResource(game_device_data.canvas_res.put());
         }

         // The scene ends here: the fill, then the upscaler, before the grade reads the scene
         if (can_draw && std::exchange(game_device_data.mv.scene_open, false))
         {
            game_device_data.mv.scene_done = true;
            ID3D11ShaderResourceView* sr_output = nullptr;
            com_ptr<ID3D11ShaderResourceView> scene;
            native_device_context->PSGetShaderResources(0, 1, &scene);
            if (game_device_data.sr_active)
            {
               const std::shared_lock lock_shader_objects(s_mutex_shader_objects);
               sr_output = DrawUpscaler(native_device, native_device_context, device_data, scene.get());
            }
            if (game_device_data.sr_active && !sr_output)
               WarnSR(4, [] { return "DrawUpscaler returned no output"; });
            // The grade's jitter is gone: it is no scene draw
            JitterSceneViewport(native_device_context, &game_device_data, false);
            if (sr_output)
            {
               native_device_context->PSSetShaderResources(0, 1, &sr_output);
               (*original_draw_dispatch_func)();
               ID3D11ShaderResourceView* const game_scene = scene.get();
               native_device_context->PSSetShaderResources(0, 1, &game_scene);
               return DrawOrDispatchOverrideType::Replaced;
            }
         }
      }

      return DrawOrDispatchOverrideType::None;
   }
   // Copies the bound target (the scene the composite blends onto) and binds it at t4. False if it can't be (not a single sample texture).
   static bool BindDoFScene(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, DragonAgeOriginsGameDeviceData& gd)
   {
      com_ptr<ID3D11RenderTargetView> rtv;
      native_device_context->OMGetRenderTargets(1, &rtv, nullptr);
      if (!rtv)
         return false;
      com_ptr<ID3D11Resource> resource;
      rtv->GetResource(&resource);
      com_ptr<ID3D11Texture2D> texture;
      if (!resource || FAILED(resource->QueryInterface(&texture)))
         return false;
      D3D11_TEXTURE2D_DESC desc;
      texture->GetDesc(&desc);
      if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1)
         return false;
      if (!gd.dof_scene || !gd.dof_scene_srv || gd.dof_width != desc.Width || gd.dof_height != desc.Height || gd.dof_format != desc.Format)
      {
         gd.dof_scene_srv.reset();
         gd.dof_scene.reset();
         D3D11_TEXTURE2D_DESC copy_desc = desc;
         copy_desc.Usage = D3D11_USAGE_DEFAULT;
         // A full mip chain, made after every copy: the bokeh gather reads the level that suits each sample's distance (fewer samples for the same smoothness)
         copy_desc.MipLevels = 0;
         copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
         copy_desc.CPUAccessFlags = 0;
         copy_desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
         // The target's typeless format has no shader resource view of its own: read it as what the target's view draws
         D3D11_RENDER_TARGET_VIEW_DESC rtv_desc;
         rtv->GetDesc(&rtv_desc);
         D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
         srv_desc.Format = rtv_desc.Format;
         srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
         srv_desc.Texture2D.MipLevels = UINT(-1);
         if (FAILED(native_device->CreateTexture2D(&copy_desc, nullptr, gd.dof_scene.put())) || FAILED(native_device->CreateShaderResourceView(gd.dof_scene.get(), &srv_desc, gd.dof_scene_srv.put())))
         {
            gd.dof_scene_srv.reset();
            gd.dof_scene.reset();
            return false;
         }
         gd.dof_width = desc.Width;
         gd.dof_height = desc.Height;
         gd.dof_format = desc.Format;
      }
      if (!gd.dof_sampler)
      {
         CD3D11_SAMPLER_DESC sampler_desc(D3D11_DEFAULT);
         sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
         sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
         if (FAILED(native_device->CreateSamplerState(&sampler_desc, gd.dof_sampler.put())))
            return false;
      }
      // Only the first mip (the copy has more), then the others from it
      native_device_context->CopySubresourceRegion(gd.dof_scene.get(), 0, 0, 0, 0, texture.get(), 0, nullptr);
      native_device_context->GenerateMips(gd.dof_scene_srv.get());
      ID3D11ShaderResourceView* const srv = gd.dof_scene_srv.get();
      native_device_context->PSSetShaderResources(4, 1, &srv);
      ID3D11SamplerState* const sampler = gd.dof_sampler.get();
      native_device_context->PSSetSamplers(4, 1, &sampler);
      return true;
   }

   void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
   {
      auto& game_device_data = GetGameDeviceData(device_data);

#if DEVELOPMENT && ENABLE_MINHOOK
      // Pause/Break toggles "Pause Game Time" (the same state as the menu's checkbox), only while the game's window has the focus
      {
         static bool was_down = false;
         DWORD foreground_process = 0;
         GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process);
         const bool is_down = foreground_process == GetCurrentProcessId() && (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
         if (is_down && !was_down)
         {
            GameTime::SetPaused(!GameTime::paused.load());
         }
         was_down = is_down;
      }
#endif

      game_device_data.ao_done_this_frame = false;
      game_device_data.bloom_drawn_this_frame = false;
      if (cb_luma_global_settings.GameSettings.BloomType < 1.5f && game_device_data.srv_luma_bloom)
      {
         // Luma's bloom off: give the pyramid and the Karis average back
         game_device_data.srv_luma_bloom.reset();
         ReleaseBloom();
         ReleaseKarisAverage(device_data);
      }
      if (!g_ao_enable && game_device_data.ao_width != 0)
         ReleaseAOScratch(game_device_data); // Give the address space back

      game_device_data.has_drawn_tonemap = false;
      game_device_data.canvas_res.reset();
      device_data.has_drawn_main_post_processing = true;

      // The upscaler: the history restarts after any frame it didn't draw (menus, loading, a scene that never opened); the selection and
      // the motion vector state are fixed here for the next frame. The scene jitters only after a frame it drew. Not with the game's MSAA
      const bool jitter_allowed = device_data.has_drawn_sr;
      game_device_data.sr_active = LatchSRFrame(device_data);
#if DEVELOPMENT
      {
         static int last_state = -1;
         const int state = (device_data.sr_type != SR::Type::None ? 1 : 0) | (game_device_data.mv.scene_open ? 4 : 0) | (game_device_data.mv.scene_done ? 8 : 0);
         if (state != last_state)
         {
            last_state = state;
            reshade::log::message(reshade::log::level::info, std::format("[Luma] Dragon Age Origins: SR state at present: sr picked {}, scene open {}, scene done {}, active {}", state & 1, (state >> 1) & 1, (state >> 2) & 1, game_device_data.sr_active).c_str());
         }
      }
#endif
      if (!game_device_data.mv.active.exchange(game_device_data.sr_active) && game_device_data.sr_active)
      {
         game_device_data.mv.constants.Restart();
      }
      // A scene no grade ended ends here: its jitter must not reach the next frame's draws before its first mesh
      game_device_data.mv.scene_open = false;
      game_device_data.mv.scene_done = false;
      game_device_data.mv.frame_ended = true;
      game_device_data.mv.fill_pending = false;
      game_device_data.open_refused_dsv = nullptr;
      game_device_data.viewport_jittered = false;
      game_device_data.depth_test_state = nullptr;
      game_device_data.reactive_ready = false;
      game_device_data.blend_state = nullptr;
      game_device_data.jitter = {};
      if (game_device_data.sr_active && jitter_allowed)
      {
         const SRDrawLock sr = LockSRForDraw(device_data);
         if (sr.implementation && sr.implementation->IsReady(sr.instance_data))
         {
            game_device_data.jitter = GetSRJitter(device_data, sr.instance_data);
         }
      }
      // (the upscalers' live mip bias, "SetTextureMipLodBias", was axed with the framework: textures pick mips for the internal resolution)
      // None picked: Core stopped the SR bridge's helper, our inputs and output go too (recreated when an upscaler is picked again)
      if (device_data.sr_type == SR::Type::None && device_data.release_sr_resources.exchange(false))
      {
         game_device_data.sr_output_srv.reset();
         game_device_data.sr_sharpened_srv.reset();
         game_device_data.sr_sharpened_rtv.reset();
         game_device_data.sr_sharpened.reset();
         const std::unique_lock lock(game_device_data.mv.mutex);
         game_device_data.mv.ReleaseMotionVectorTargets();
         game_device_data.scene_depth_srv.reset();
         game_device_data.scene_depth.reset();
         game_device_data.mv.depth.reset();
         game_device_data.mv.scene_color.reset();
         game_device_data.mv.scene_rtv.reset();
         game_device_data.mv.scene_size = {};
         game_device_data.mv.objects.Reset();
         game_device_data.mv.camera.reset();
         game_device_data.mv.previous_camera.reset();
         game_device_data.mv.fill_buffer.reset();
         game_device_data.mv.jitter_buffer.reset();
         game_device_data.mv.previous_constants = {}; // Its 4 MiB dynamic ring is address space too
         game_device_data.mv.constants.ReleasePool();
      }
      game_device_data.mv.constants.TrimPool(game_device_data.mv.active);
      // DLSS or none picked: no reader of the masks (recreated when FSR is picked again)
      if (!IsReactiveMaskUsed(device_data) && game_device_data.mv.reactive_target)
      {
         game_device_data.ReleaseReactiveMasks();
      }
#if DEVELOPMENT
      game_device_data.mv.stats.constants_pool = uint32_t(game_device_data.mv.constants.pool.size());
      game_device_data.mv.last_stats = std::exchange(game_device_data.mv.stats, {});
#endif
   }

   // Core's paper white default (203 nits, ITU) is meant for the UI: the scene's highlight tuning was measured at 102 nits. A scene value
   // that still sits at Core's default is moved to 102, in HDR only (Core pins both to 80 nits in SDR), so a value coming from the config or
   // the slider is kept. Done game-side: Core's own defaults stay untouched.
   void ApplyScenePaperWhiteDefault()
   {
      if (cb_luma_global_settings.DisplayMode != DisplayModeType::SDR && cb_luma_global_settings.ScenePaperWhite == default_paper_white)
         cb_luma_global_settings.ScenePaperWhite = 102.f;
   }
   void OnDisplayModeChanged() override
   {
      ApplyScenePaperWhiteDefault();
   }

   void LoadConfigs() override
   {
      reshade::api::effect_runtime* runtime = nullptr;
      ApplyScenePaperWhiteDefault();

#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "HighlightExpansion", cb_luma_global_settings.GameSettings.HighlightExpansion);
#endif
      reshade::get_config_value(runtime, NAME, "HighlightDesaturation", cb_luma_global_settings.GameSettings.HighlightDesaturation);
#if DEVELOPMENT // Fixed in the release: not in its menu, and an older config's values are ignored
      reshade::get_config_value(runtime, NAME, "LightSourceBoost", cb_luma_global_settings.GameSettings.LightSourceBoost);
#endif
#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "ShadowLift", cb_luma_global_settings.GameSettings.ShadowLift);
#endif
      reshade::get_config_value(runtime, NAME, "ShadowLiftRange", cb_luma_global_settings.GameSettings.ShadowLiftRange);
#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "HighlightWhite", cb_luma_global_settings.GameSettings.HighlightWhite);
      reshade::get_config_value(runtime, NAME, "HighlightPunch", cb_luma_global_settings.GameSettings.HighlightPunch);
      reshade::get_config_value(runtime, NAME, "HighlightColor", cb_luma_global_settings.GameSettings.HighlightColor);
#endif
      reshade::get_config_value(runtime, NAME, "HighlightDesatStart", cb_luma_global_settings.GameSettings.HighlightDesatStart);
      reshade::get_config_value(runtime, NAME, "HighlightDesatEnd", cb_luma_global_settings.GameSettings.HighlightDesatEnd);
      reshade::get_config_value(runtime, NAME, "BokehTaps", cb_luma_global_settings.GameSettings.BokehTaps);
      reshade::get_config_value(runtime, NAME, "BokehHighlightBoost", cb_luma_global_settings.GameSettings.BokehHighlightBoost);
      reshade::get_config_value(runtime, NAME, "GamutExpansion", cb_luma_global_settings.GameSettings.GamutExpansion);
      reshade::get_config_value(runtime, NAME, "VideoAutoHDREnable", cb_luma_global_settings.GameSettings.VideoAutoHDREnable);
      reshade::get_config_value(runtime, NAME, "VideoAutoHDRBoost", cb_luma_global_settings.GameSettings.VideoAutoHDRBoost);
      // "BloomType" (Off / Game / Luma) replaced the "LumaBloomEnable" on/off (Luma / Game): an older config's choice is carried over
      if (!reshade::get_config_value(runtime, NAME, "BloomType", cb_luma_global_settings.GameSettings.BloomType))
      {
         float luma_bloom_enable = 1.f;
         if (reshade::get_config_value(runtime, NAME, "LumaBloomEnable", luma_bloom_enable))
            cb_luma_global_settings.GameSettings.BloomType = luma_bloom_enable > 0.5f ? 2.f : 1.f;
      }
      cb_luma_global_settings.GameSettings.BloomType = std::clamp(std::round(cb_luma_global_settings.GameSettings.BloomType), 0.f, 2.f);
      reshade::get_config_value(runtime, NAME, "BloomIntensity", cb_luma_global_settings.GameSettings.BloomIntensity);
#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "BloomThreshold", cb_luma_global_settings.GameSettings.BloomThreshold);
#endif
#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "BloomSpread", cb_luma_global_settings.GameSettings.BloomSpread);
#endif
#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "BloomSaturation", cb_luma_global_settings.GameSettings.BloomSaturation);
#endif
#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "BloomWarmth", cb_luma_global_settings.GameSettings.BloomWarmth);
#endif
      reshade::get_config_value(runtime, NAME, "FilmGrainStrength", cb_luma_global_settings.GameSettings.FilmGrainStrength);
      reshade::get_config_value(runtime, NAME, "VignetteStrength", cb_luma_global_settings.GameSettings.VignetteStrength);
      reshade::get_config_value(runtime, NAME, "VignetteRadius", cb_luma_global_settings.GameSettings.VignetteRadius);
      reshade::get_config_value(runtime, NAME, "ChromaticAberration", cb_luma_global_settings.GameSettings.ChromaticAberration);
      reshade::get_config_value(runtime, NAME, "Exposure", cb_luma_global_settings.GameSettings.Exposure);
      reshade::get_config_value(runtime, NAME, "Contrast", cb_luma_global_settings.GameSettings.Contrast);
      reshade::get_config_value(runtime, NAME, "Saturation", cb_luma_global_settings.GameSettings.Saturation);
      reshade::get_config_value(runtime, NAME, "Temperature", cb_luma_global_settings.GameSettings.Temperature);
      reshade::get_config_value(runtime, NAME, "BokehDoFEnable", cb_luma_global_settings.GameSettings.BokehDoFEnable);
      reshade::get_config_value(runtime, NAME, "RCASSharpness", g_rcas_sharpness);
      reshade::get_config_value(runtime, NAME, "XeGTAOEnable", g_ao_enable);
      {
         int technique = (int)g_ao_technique;
         reshade::get_config_value(runtime, NAME, "AOTechnique", technique);
         g_ao_technique = technique == (int)AOTechnique::GTVBAO ? AOTechnique::GTVBAO : AOTechnique::XeGTAO;
      }
#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "XeGTAOStrength", g_ao_strength);
      reshade::get_config_value(runtime, NAME, "XeGTAODarkness", g_ao_darkness);
      reshade::get_config_value(runtime, NAME, "XeGTAORadius", g_gtao_radius_override);
#endif
   }

   void DrawImGuiSettings(DeviceData& device_data) override
   {
      reshade::api::effect_runtime* runtime = nullptr;

      // The HDR Color and Videos settings do nothing in SDR (the shaders only apply them with "DisplayMode > 0"): greyed out there
      const bool hdr_display_mode = cb_luma_global_settings.DisplayMode != DisplayModeType::SDR;
      if (enable_hdr)
      {
      ImGui::SeparatorText("HDR Color");
      ImGui::BeginDisabled(!hdr_display_mode);
#if DEVELOPMENT
      if (ImGui::SliderFloat("Highlight Expansion", &cb_luma_global_settings.GameSettings.HighlightExpansion, 0.f, 1.f, "%.2f"))
         reshade::set_config_value(runtime, NAME, "HighlightExpansion", cb_luma_global_settings.GameSettings.HighlightExpansion);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Recovers the highlight detail the game's tone curve clips (sky, sun, fire) and extends it up to the display peak brightness.\n0 is the vanilla clipped look, 1 is the full recovery.\nHDR only.");
      DrawResetButton(cb_luma_global_settings.GameSettings.HighlightExpansion, default_luma_global_game_settings.HighlightExpansion, "HighlightExpansion", runtime);
#endif

#if DEVELOPMENT // Fixed at their defaults in the release (not loaded from the config either, see "LoadConfigs()")
      if (ImGui::SliderFloat("Shadow Lift", &cb_luma_global_settings.GameSettings.ShadowLift, 0.f, 0.25f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "ShadowLift", cb_luma_global_settings.GameSettings.ShadowLift);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Brings back detail in the darkest areas. The game's colours are encoded sRGB, and the gamma 2.2 display decode is darker near black, which crushes them.\nThis raises only the darks (the mids and highlights stay as they are). 0 is off. HDR only.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.ShadowLift, default_luma_global_game_settings.ShadowLift, "ShadowLift", runtime))
         device_data.cb_luma_global_settings_dirty = true;
      if (ImGui::SliderFloat("Light Source Boost", &cb_luma_global_settings.GameSettings.LightSourceBoost, 0.f, 6.f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "LightSourceBoost", cb_luma_global_settings.GameSettings.LightSourceBoost);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Makes the glow of fire, torches, sparks and magic brighter at its core, so they reach your display's brightness while walls and other surfaces stay as they are.\n0 is as the game drew them. HDR only.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.LightSourceBoost, default_luma_global_game_settings.LightSourceBoost, "LightSourceBoost", runtime))
         device_data.cb_luma_global_settings_dirty = true;
      if (ImGui::SliderFloat("Highlight Range", &cb_luma_global_settings.GameSettings.HighlightWhite, 2.f, 25.f, "%.1f"))
      {
         reshade::set_config_value(runtime, NAME, "HighlightWhite", cb_luma_global_settings.GameSettings.HighlightWhite);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("How bright (in multiples of normal white) the game's bright parts (sun, fire, magic, torches) have to be to reach your Peak White.\nLower: highlights get brighter, but more of the brightest ones flatten at the peak.\nHigher: more detail kept in the brightest spots, which stay a little below the peak.\nIt adapts to your Peak White and Paper White by itself. HDR only.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.HighlightWhite, default_luma_global_game_settings.HighlightWhite, "HighlightWhite", runtime))
         device_data.cb_luma_global_settings_dirty = true;
      if (ImGui::SliderFloat("Highlight Punch", &cb_luma_global_settings.GameSettings.HighlightPunch, 0.f, 1.f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "HighlightPunch", cb_luma_global_settings.GameSettings.HighlightPunch);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Lifts the middle of the highlights towards your Peak White, for brighter, punchier highlights.\nThe very brightest spots still keep their detail. 0 is the gentlest. HDR only.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.HighlightPunch, default_luma_global_game_settings.HighlightPunch, "HighlightPunch", runtime))
         device_data.cb_luma_global_settings_dirty = true;
      if (ImGui::SliderFloat("Highlight Color", &cb_luma_global_settings.GameSettings.HighlightColor, 0.f, 1.f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "HighlightColor", cb_luma_global_settings.GameSettings.HighlightColor);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Where the game's colour curve clipped a highlight to plain white (fire, sunlit windows, light shafts), brings back the colour the scene really had there.\n0 leaves them white. Higher is more colourful. HDR only.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.HighlightColor, default_luma_global_game_settings.HighlightColor, "HighlightColor", runtime))
         device_data.cb_luma_global_settings_dirty = true;
#endif
      if (ImGui::SliderFloat("Highlight Desaturation", &cb_luma_global_settings.GameSettings.HighlightDesaturation, 0.f, 0.5f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "HighlightDesaturation", cb_luma_global_settings.GameSettings.HighlightDesaturation);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("The brightest highlights (sun, fire, magic) roll off towards white as they near your Peak White, instead of keeping a vivid colour all the way up.\n0 keeps the hue at any brightness. HDR only.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.HighlightDesaturation, default_luma_global_game_settings.HighlightDesaturation, "HighlightDesaturation", runtime))
         device_data.cb_luma_global_settings_dirty = true;

      if (ImGui::SliderFloat("Gamut Expansion", &cb_luma_global_settings.GameSettings.GamutExpansion, 0.f, 1.f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "GamutExpansion", cb_luma_global_settings.GameSettings.GamutExpansion);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Stretches the colors from BT.709 towards BT.2020.\nThe game's colors are mostly well inside BT.709, so the effect is subtle.\n0 is vanilla. HDR only.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.GamutExpansion, default_luma_global_game_settings.GamutExpansion, "GamutExpansion", runtime))
         device_data.cb_luma_global_settings_dirty = true;
      ImGui::EndDisabled();
      } // enable_hdr

      ImGui::SeparatorText("Color Grading");
      {
         auto& settings = cb_luma_global_settings.GameSettings;
         auto& defaults = default_luma_global_game_settings;
         if (ImGui::SliderFloat("Exposure", &settings.Exposure, -2.f, 2.f, "%+.2f EV"))
         {
            reshade::set_config_value(runtime, NAME, "Exposure", settings.Exposure);
            device_data.cb_luma_global_settings_dirty = true;
         }
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Brightens or darkens the scene, in stops, before the HUD.");
         if (DrawResetButton(settings.Exposure, defaults.Exposure, "Exposure", runtime))
            device_data.cb_luma_global_settings_dirty = true;
         if (ImGui::SliderFloat("Contrast", &settings.Contrast, 0.5f, 1.5f, "%.2f"))
         {
            reshade::set_config_value(runtime, NAME, "Contrast", settings.Contrast);
            device_data.cb_luma_global_settings_dirty = true;
         }
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Stretches (above 1) or squeezes (below 1) the brightness around mid grey.");
         if (DrawResetButton(settings.Contrast, defaults.Contrast, "Contrast", runtime))
            device_data.cb_luma_global_settings_dirty = true;
         if (ImGui::SliderFloat("Saturation", &settings.Saturation, 0.f, 2.f, "%.2f"))
         {
            reshade::set_config_value(runtime, NAME, "Saturation", settings.Saturation);
            device_data.cb_luma_global_settings_dirty = true;
         }
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("1 is the game's colors, 0 is black and white.");
         if (DrawResetButton(settings.Saturation, defaults.Saturation, "Saturation", runtime))
            device_data.cb_luma_global_settings_dirty = true;
         if (ImGui::SliderFloat("Temperature", &settings.Temperature, -1.f, 1.f, "%+.2f"))
         {
            reshade::set_config_value(runtime, NAME, "Temperature", settings.Temperature);
            device_data.cb_luma_global_settings_dirty = true;
         }
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Cooler (blue, below 0) or warmer (orange, above 0) colors.");
         if (DrawResetButton(settings.Temperature, defaults.Temperature, "Temperature", runtime))
            device_data.cb_luma_global_settings_dirty = true;
      }

      ImGui::SeparatorText("Bloom");
      int bloom_type = int(cb_luma_global_settings.GameSettings.BloomType + 0.5f);
      if (ImGui::Combo("Bloom", &bloom_type, "Off\0Game\0Luma\0"))
      {
         cb_luma_global_settings.GameSettings.BloomType = float(bloom_type);
         reshade::set_config_value(runtime, NAME, "BloomType", cb_luma_global_settings.GameSettings.BloomType);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Off: no bloom.\nGame: the game's own bloom.\nLuma: a wide, soft pyramid bloom in place of the game's, made at the game's own point in the frame.\nThe game's Bloom option in its settings has to be on for Game and Luma: Luma takes over the game's bloom pass.");
      const bool luma_bloom = bloom_type == 2;
      ImGui::BeginDisabled(!luma_bloom);
      if (ImGui::SliderFloat("Bloom Intensity", &cb_luma_global_settings.GameSettings.BloomIntensity, 0.f, 3.f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "BloomIntensity", cb_luma_global_settings.GameSettings.BloomIntensity);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (DrawResetButton(cb_luma_global_settings.GameSettings.BloomIntensity, default_luma_global_game_settings.BloomIntensity, "BloomIntensity", runtime))
         device_data.cb_luma_global_settings_dirty = true;
#if DEVELOPMENT
      if (ImGui::SliderFloat("Bloom Threshold", &cb_luma_global_settings.GameSettings.BloomThreshold, 0.1f, 1.f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "BloomThreshold", cb_luma_global_settings.GameSettings.BloomThreshold);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("The scene brightness (gamma encoded, 1 = SDR white) where the bloom starts, with a soft knee.\nLower: more of the image glows. Higher: only the brightest highlights.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.BloomThreshold, default_luma_global_game_settings.BloomThreshold, "BloomThreshold", runtime))
         device_data.cb_luma_global_settings_dirty = true;
      if (ImGui::SliderFloat("Bloom Spread", &cb_luma_global_settings.GameSettings.BloomSpread, 0.5f, 2.f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "BloomSpread", cb_luma_global_settings.GameSettings.BloomSpread);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("How wide the glow spreads. Lower is a tight glow around bright things, higher is a wide soft haze.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.BloomSpread, default_luma_global_game_settings.BloomSpread, "BloomSpread", runtime))
         device_data.cb_luma_global_settings_dirty = true;
      if (ImGui::SliderFloat("Bloom Saturation", &cb_luma_global_settings.GameSettings.BloomSaturation, 0.f, 2.f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "BloomSaturation", cb_luma_global_settings.GameSettings.BloomSaturation);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("How colourful the glow is. 1 takes the scene's colours, 0 is a white glow, above 1 is more vivid.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.BloomSaturation, default_luma_global_game_settings.BloomSaturation, "BloomSaturation", runtime))
         device_data.cb_luma_global_settings_dirty = true;
      if (ImGui::SliderFloat("Bloom Warmth", &cb_luma_global_settings.GameSettings.BloomWarmth, -1.f, 1.f, "%+.2f"))
      {
         reshade::set_config_value(runtime, NAME, "BloomWarmth", cb_luma_global_settings.GameSettings.BloomWarmth);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Tints the glow cooler (blue, below 0) or warmer (orange, above 0).");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.BloomWarmth, default_luma_global_game_settings.BloomWarmth, "BloomWarmth", runtime))
         device_data.cb_luma_global_settings_dirty = true;
#endif
      ImGui::EndDisabled();

      ImGui::SeparatorText("Film Grain");
      if (ImGui::SliderFloat("Film Grain", &cb_luma_global_settings.GameSettings.FilmGrainStrength, 0.f, 1.f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "FilmGrainStrength", cb_luma_global_settings.GameSettings.FilmGrainStrength);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Luma's film grain, added to the scene before the HUD. The game has no film grain of its own.\nIt is stronger on brighter areas, like real film. 0 is off.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.FilmGrainStrength, default_luma_global_game_settings.FilmGrainStrength, "FilmGrainStrength", runtime))
         device_data.cb_luma_global_settings_dirty = true;

      ImGui::SeparatorText("Lens Effects");
      if (ImGui::SliderFloat("Vignette", &cb_luma_global_settings.GameSettings.VignetteStrength, 0.f, 1.f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "VignetteStrength", cb_luma_global_settings.GameSettings.VignetteStrength);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Darkens the corners of the scene, before the HUD. 0 is off.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.VignetteStrength, default_luma_global_game_settings.VignetteStrength, "VignetteStrength", runtime))
         device_data.cb_luma_global_settings_dirty = true;
      ImGui::BeginDisabled(cb_luma_global_settings.GameSettings.VignetteStrength <= 0.f);
      if (ImGui::SliderFloat("Vignette Radius", &cb_luma_global_settings.GameSettings.VignetteRadius, 0.3f, 1.5f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "VignetteRadius", cb_luma_global_settings.GameSettings.VignetteRadius);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("How far from the centre the darkening starts. Lower is a tighter, heavier vignette.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.VignetteRadius, default_luma_global_game_settings.VignetteRadius, "VignetteRadius", runtime))
         device_data.cb_luma_global_settings_dirty = true;
      ImGui::EndDisabled();
      if (ImGui::SliderFloat("Chromatic Aberration", &cb_luma_global_settings.GameSettings.ChromaticAberration, 0.f, 0.5f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "ChromaticAberration", cb_luma_global_settings.GameSettings.ChromaticAberration);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Red and blue colour fringes that grow towards the screen edges, like a cheap lens. 0 is off.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.ChromaticAberration, default_luma_global_game_settings.ChromaticAberration, "ChromaticAberration", runtime))
         device_data.cb_luma_global_settings_dirty = true;

      ImGui::SeparatorText("Depth of Field");
      {
         bool bokeh = cb_luma_global_settings.GameSettings.BokehDoFEnable > 0.5f;
         if (ImGui::Checkbox("Bokeh Depth of Field", &bokeh))
         {
            cb_luma_global_settings.GameSettings.BokehDoFEnable = bokeh ? 1.f : 0.f;
            reshade::set_config_value(runtime, NAME, "BokehDoFEnable", cb_luma_global_settings.GameSettings.BokehDoFEnable);
            device_data.cb_luma_global_settings_dirty = true;
         }
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Replaces the game's blur in the cutscenes that use depth of field with a bokeh: out of focus highlights become soft discs.\nThe game only uses depth of field in cutscenes.");
      }

      if (enable_hdr)
      {
      ImGui::SeparatorText("Videos");
      // Read in Video_0x39642745.ps_5_0.hlsl, which only applies it in HDR
      ImGui::BeginDisabled(!hdr_display_mode);
      bool video_auto_hdr = cb_luma_global_settings.GameSettings.VideoAutoHDREnable > 0.5f;
      if (ImGui::Checkbox("Video AutoHDR", &video_auto_hdr))
      {
         cb_luma_global_settings.GameSettings.VideoAutoHDREnable = video_auto_hdr ? 1.f : 0.f;
         reshade::set_config_value(runtime, NAME, "VideoAutoHDREnable", cb_luma_global_settings.GameSettings.VideoAutoHDREnable);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Adds HDR highlights to the pre-rendered videos (HDR only).");
      ImGui::BeginDisabled(!video_auto_hdr);
      if (ImGui::SliderFloat("Video HDR Boost", &cb_luma_global_settings.GameSettings.VideoAutoHDRBoost, 0.f, 1.f, "%.2f"))
      {
         reshade::set_config_value(runtime, NAME, "VideoAutoHDRBoost", cb_luma_global_settings.GameSettings.VideoAutoHDRBoost);
         device_data.cb_luma_global_settings_dirty = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("How bright the videos' highlights get (0 = off, 1 = peak of 250 nits, 3x paper white).\nThe videos are low bitrate: pushing it far amplifies their compression blocks.");
      if (DrawResetButton(cb_luma_global_settings.GameSettings.VideoAutoHDRBoost, default_luma_global_game_settings.VideoAutoHDRBoost, "VideoAutoHDRBoost", runtime))
         device_data.cb_luma_global_settings_dirty = true;
      ImGui::EndDisabled();
      ImGui::EndDisabled();
      } // enable_hdr

      ImGui::SeparatorText("UI");
      ImGui::Checkbox("Hide Gameplay UI", &g_hide_ui); // Session-only: a stuck "on" would look like a broken HUD
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Hides the in-game HUD, menus and cursor, for clean screenshots. The main menu and loading screens are not affected.\nNot saved: it is off again on the next launch.");

      {
         // The game draws its HUD at a fixed pixel size from 1024x768 up, so it gets relatively smaller on a bigger display
         ImGui::BeginDisabled(!g_gui_scale_hook_installed);
         float ui_scale = g_ui_scale;
         if (ImGui::SliderFloat("HUD Scale", &ui_scale, 0.f, 3.f, ui_scale > 0.f ? "%.2f" : "Auto"))
         {
            g_ui_scale = std::round(ui_scale * 20.f) / 20.f; // Steps of 0.05, and 0 stays Auto
            reshade::set_config_value(runtime, NAME, "UIScale", g_ui_scale);
         }
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Scales the HUD, menus and their click targets together (the engine's own layout scale is replaced, so nothing can come\n"
                              "adrift from the mouse). 0 (Auto, the default) uses the size the game gives its HUD on a 768 line screen, which is the\n"
                              "fraction of the screen it was designed for: that is the comfortable one on a modern display. 1 is the game's own\n"
                              "size (tiny on a big display), higher is bigger.\n"
                              "It is applied the next time the game lays its interface out: change screen, or restart the game to be sure.");
         if (DrawResetButton(g_ui_scale, 0.f, "UIScale", runtime))
         {
            // The hook reads the value on every layout: there is nothing else to update
         }
         if (!g_gui_scale_hook_installed)
            ImGui::TextDisabled("Not available: this exe version is not supported.");
         ImGui::EndDisabled();
      }

      ImGui::SeparatorText("Sharpening");
      ImGui::BeginDisabled(device_data.sr_type == SR::Type::None);
      if (ImGui::SliderFloat("RCAS Sharpness", &g_rcas_sharpness, 0.f, 1.f, "%.2f"))
         reshade::set_config_value(runtime, NAME, "RCASSharpness", g_rcas_sharpness);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("AMD's RCAS sharpening on the upscaler's output (DLSS, FSR), which can look a little soft. It limits itself on bright pixels.\n0 is off.");
      DrawResetButton(g_rcas_sharpness, 0.25f, "RCASSharpness", runtime);
      ImGui::EndDisabled();

      ImGui::SeparatorText("Ambient Occlusion");
      if (ImGui::Checkbox("Ambient Occlusion", &g_ao_enable))
         reshade::set_config_value(runtime, NAME, "XeGTAOEnable", g_ao_enable);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("The game has no ambient occlusion: this adds it on the scene depth, darkening creases and contact points.\nThe quality is the XE_GTAO_QUALITY shader define.");
      ImGui::BeginDisabled(!g_ao_enable);
      {
         int technique = (int)g_ao_technique;
         if (ImGui::Combo("AO Technique", &technique, "XeGTAO\0GT-VBAO\0"))
         {
            g_ao_technique = technique == (int)AOTechnique::GTVBAO ? AOTechnique::GTVBAO : AOTechnique::XeGTAO;
            reshade::set_config_value(runtime, NAME, "AOTechnique", (int)g_ao_technique);
         }
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("XeGTAO: horizon based, Intel's ground truth AO. Crisp and cheap.\nGT-VBAO: visibility bitmask. It assumes a thickness behind every surface, so thin objects (railings, foliage, props) are not over darkened, and it is a bit softer.");
      }
#if DEVELOPMENT // Fixed at their defaults in the release (not loaded from the config either, see "LoadConfigs()")
      if (ImGui::CollapsingHeader("Advanced (for Enthusiasts)"))
      {
         if (ImGui::SliderFloat("AO Strength", &g_ao_strength, 0.f, 1.f, "%.2f"))
            reshade::set_config_value(runtime, NAME, "XeGTAOStrength", g_ao_strength);
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("How much of the ambient occlusion is applied (0 = none, 1 = all of it).");
         DrawResetButton(g_ao_strength, 0.75f, "XeGTAOStrength", runtime);
         if (ImGui::SliderFloat("AO Darkness", &g_ao_darkness, 0.f, 1.f, "%.2f"))
            reshade::set_config_value(runtime, NAME, "XeGTAODarkness", g_ao_darkness);
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("How dark the occluded areas get (0 = not at all, 1 = the darkest).");
         DrawResetButton(g_ao_darkness, 0.40f, "XeGTAODarkness", runtime);
         if (ImGui::SliderFloat("AO Radius", &g_gtao_radius_override, 0.f, 3.f, g_gtao_radius_override > 0.f ? "%.2f m" : "Default"))
            reshade::set_config_value(runtime, NAME, "XeGTAORadius", g_gtao_radius_override);
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("How far, in meters, an occluder can be from a point and still darken it (0 = the default).");
         DrawResetButton(g_gtao_radius_override, 0.f, "XeGTAORadius", runtime);
         ImGui::Combo("AO Debug View", &g_gtao_debug_view, "Off\0Depth gradient\0Normals\0AO x8\0Edges\0");
      }
#endif
      ImGui::EndDisabled();

#if DEVELOPMENT
      // Every constant the shaders were written with, as a slider, to tune them by eye: which of them stay in the release build is decided later
      ImGui::SeparatorText("Tuning (dev)");
      {
         const auto tune = [&](const char* label, float CB::LumaGameSettings::*member, const char* key, float minimum, float maximum, const char* format, const char* tooltip)
         {
            float& value = cb_luma_global_settings.GameSettings.*member;
            if (ImGui::SliderFloat(label, &value, minimum, maximum, format))
            {
               reshade::set_config_value(runtime, NAME, key, value);
               device_data.cb_luma_global_settings_dirty = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
               ImGui::SetTooltip("%s", tooltip);
            if (DrawResetButton(value, default_luma_global_game_settings.*member, key, runtime))
               device_data.cb_luma_global_settings_dirty = true;
         };
         ImGui::TextDisabled("Shadows and highlights");
         tune("Shadow Lift Range", &CB::LumaGameSettings::ShadowLiftRange, "ShadowLiftRange", 0.05f, 1.f, "%.2f", "How high up the darks the Shadow Lift reaches, as an encoded brightness: it has faded out by here.");
         tune("Highlight Desat Start", &CB::LumaGameSettings::HighlightDesatStart, "HighlightDesatStart", 0.5f, 3.f, "%.2f", "Where the highlight desaturation begins, in multiples of paper white.");
         tune("Highlight Desat End", &CB::LumaGameSettings::HighlightDesatEnd, "HighlightDesatEnd", 0.25f, 1.5f, "%.2f", "Where it is complete, as a fraction of your Peak White (in paper whites).");
         ImGui::TextDisabled("Bokeh depth of field");
         tune("Bokeh Taps", &CB::LumaGameSettings::BokehTaps, "BokehTaps", 16.f, 192.f, "%.0f", "Samples of the bokeh blur: more is smoother and slower.");
         tune("Bokeh Highlight Boost", &CB::LumaGameSettings::BokehHighlightBoost, "BokehHighlightBoost", 0.f, 12.f, "%.1f", "How much more the bright samples weigh, which turns highlights into discs. 0 is a plain blur.");
      }
#endif

#if DEVELOPMENT && ENABLE_MINHOOK
      ImGui::SeparatorText("Debug");
      {
         bool paused = GameTime::paused.load();
         float game_speed = GameTime::resume_speed.load();
         if (ImGui::Checkbox("Pause Game Time", &paused))
            GameTime::SetPaused(paused);
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Stops the game's own clock (cutscenes too) while it keeps drawing frames, to study or capture a frame.\nOnly the game's time stops: sound and videos may keep going. Untick to continue.\nHotkey: Pause/Break (while the game window is focused).");
         if (ImGui::SliderFloat("Slow Motion Speed", &game_speed, 0.f, 1.f, "%.2f"))
         {
            GameTime::resume_speed.store(game_speed);
            GameTime::SetPaused(paused);
         }
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The game's speed while Pause Game Time is off (1 = normal, lower = slow motion).");
      }
#endif
   }

   void PrintImGuiAbout() override
   {
      ImGui::PushTextWrapPos(0.f);
      ImGui::Text(
         "Luma for \"Dragon Age: Origins\" is developed by Ostad Gaming and is open source and free.\n"
         "It runs through dgVoodoo2 (DirectX 9 -> 11) and adds DLSS or FSR upscaling and anti-aliasing,\n"
         "a wider bloom, XeGTAO or GT-VBAO ambient occlusion and 16x anisotropic filtering.\n"
         "The game's clipped highlights (sky, sun, fire) are recovered and extended up to your display's peak brightness, and the colors can be stretched towards BT.2020.\n"
         "The pre-rendered videos get a light AutoHDR.\n"
         "Do NOT run another graphics or tone-mapping mod (e.g. RenoDX) alongside it.\n"
         "Thanks to the Luma team and contributors.\n"
         "If you enjoy it, consider donating.");
      ImGui::PopTextWrapPos();

      ImGui::NewLine();
      ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 134, 0, 255));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70 + 9, 134 + 9, 0, 255));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(70 + 18, 134 + 18, 0, 255));
      static const std::string donation_link = std::string("Buy Ostad Gaming a Coffee on ko-fi ") + std::string(ICON_FK_OK);
      if (ImGui::Button(donation_link.c_str()))
      {
         ShellExecuteA(nullptr, "open", "https://ko-fi.com/ostadgaming", nullptr, nullptr, SW_SHOWNORMAL);
      }
      ImGui::PopStyleColor(3);

      ImGui::NewLine();
      static const std::string social_link = std::string("Join our \"HDR Den\" Discord ") + std::string(ICON_FK_SEARCH);
      if (ImGui::Button(social_link.c_str()))
      {
         // Unique link for Luma's HDR Den (tracks the origin of people joining); do not share for other purposes.
         static const std::string discord_link = std::string("https://discord.gg/J9fM") + std::string("3EVuEZ");
         ShellExecuteA(nullptr, "open", discord_link.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }

      ImGui::NewLine();
      ImGui::Text("Build Date: %s %s", __DATE__, __TIME__);

      ImGui::NewLine();
      ImGui::Text("Credits:"
                  "\n\nMain:"
                  "\nOstad Gaming"
                  "\n\nThird Party:"
                  "\nReShade"
                  "\nImGui"
                  "\ndgVoodoo2 (Dege)"

                  "\nXeGTAO (Intel)"
                  "\nGT-VBAO (Mirko Salm, TinyTexel)"
                  "\nAMD FidelityFX (RCAS + FSR)"
                  "\nNVIDIA NGX (DLSS)");
   }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   if (ul_reason_for_call == DLL_PROCESS_ATTACH)
   {
      const char* project_name = PROJECT_NAME;
      const char* cleared_project_name = (project_name[0] == '_') ? (project_name + 1) : project_name;

      uint32_t mod_version = 1;
      Globals::SetGlobals(cleared_project_name, "Dragon Age: Origins Luma HDR mod", "", mod_version);
      Globals::DEVELOPMENT_STATE = Globals::ModDevelopmentState::WorkInProgress;

      // scRGB fp16 swapchain (the game's backbuffer is 8-bit), or a real 8 bit SDR one when the game's display isn't in HDR at start: the
      // scRGB one would be encoded to SDR by DWM, and Core's own rule (the SDR display mode when HDR isn't engaged) then puts the frame in
      // SDR. Decided once, before the swapchain exists; the game's window doesn't yet, so the primary display is used. Turning Windows HDR
      // on or off later needs a game restart.
      swapchain_format_upgrade_type = TextureFormatUpgradesType::AllowedEnabled;
      {
         bool hdr_supported = false;
         Display::IsHDRSupportedAndEnabled(nullptr, hdr_supported, g_hdr_enabled_at_start, nullptr, MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
      }
      swapchain_upgrade_type = g_hdr_enabled_at_start ? SwapchainUpgradeType::scRGB : SwapchainUpgradeType::None;
      // The in-game brightness slider is a D3D9 SetGammaRamp that dgVoodoo forwards to the OS ramp, distorting scRGB.
      allow_disabling_gamma_ramp = true;

      prevent_fullscreen_state = true;
      force_borderless = true;

      // Same dgVoodoo scheme as Mass Effect: changing a dgVoodoo creation format can black-screen it, so upgrade INDIRECTLY.
      // Starting point only: re-check the formats the game really uses once shaders are dumped.
      texture_format_upgrades_type = TextureFormatUpgradesType::AllowedEnabled;
      enable_indirect_texture_format_upgrades = true;
      enable_chain_indirect_texture_format_upgrades = ChainTextureFormatUpgradesType::DirectDependencies;
      texture_upgrade_formats = {
         reshade::api::format::r8g8b8a8_typeless,     // dgVoodoo's D3D9 backbuffer surface (the LDR canvas)
         reshade::api::format::r16g16b16a16_typeless,
      };
      // "No1Px" is mandatory under dgVoodoo: 1x1 placeholders fill unused slots and pass the aspect filter.
      texture_format_upgrades_2d_size_filters = (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainResolution | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainAspectRatio | (uint32_t)TextureFormatUpgrades2DSizeFilters::No1Px;

      // The upscalers' mip bias (Core biases the samplers at bias 0, "SetTextureMipLodBias"); boot-time only
      enable_samplers_upgrade = true;
      samplers_upgrade_mode = 3;
      force_upgrade_linear_samplers = true;

      game = new DragonAgeOrigins();
   }

   else if (ul_reason_for_call == DLL_PROCESS_DETACH)
   {
#if DEVELOPMENT && ENABLE_MINHOOK
      GameTime::Uninstall();
#endif
      DragonAgeOrigins::SetEvents(false);
   }

   CoreMain(hModule, ul_reason_for_call, lpReserved);

   if (ul_reason_for_call == DLL_PROCESS_ATTACH)
   {
      // Before the swapchain is created, as decided above ("swapchain_upgrade_type" is read at its creation)
      swapchain_upgrade_type = g_hdr_enabled_at_start ? SwapchainUpgradeType::scRGB : SwapchainUpgradeType::None;
      // Anisotropic filtering: Core's sampler upgrade at its own fixed 16x ("samplers_upgrade_mode" and "force_upgrade_linear_samplers" are
      // set above). No per-level setting, as exposing one would need a Core change.

      // The HUD scale: its setting is read here (the game can lay its GUI out before "LoadConfigs()" runs) and the hook goes in now, so it is
      // there for the first layout. A failure only turns the setting off, see "InstallGUIScaleHook()".
      float ui_scale = g_ui_scale;
      if (reshade::get_config_value(nullptr, NAME, "UIScale", ui_scale))
         g_ui_scale = std::clamp(ui_scale, 0.f, 3.f);
      InstallGUIScaleHook();
   }

   return TRUE;
}
