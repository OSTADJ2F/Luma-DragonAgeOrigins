# Dragon Age: Origins - Nexus release package requirements

Rules for every release zip (Nexus). Follow them when packaging.

## Build
Build three things, with the solution dir passed (a development build bakes its shader fallback path into itself):

```
MSBuild "Source\Games\Dragon Age Origins\Dragon Age Origins.vcxproj" -p:Configuration=Publishing-Release ^
  -p:Platform=Win32 "-p:SolutionDir=<repo>\\" -p:SolutionName=Luma -m

MSBuild "Source\Tools\SR Bridge Helper\SR Bridge Helper DLL.vcxproj" -p:Configuration=Publishing-Release ^
  -p:Platform=x64 "-p:SolutionDir=<repo>\\" -m

MSBuild "Source\Games\Dragon Age Origins\SR Helper Shim\Luma Upscaler Shim.vcxproj" -p:Configuration=Publishing-Release ^
  -p:Platform=x64 "-p:SolutionDir=<repo>\\" -m
```

The addon lands in `Binaries\Win32-Publishing-Release`, the framework's x64 helper as `Luma-Upscaler.dll` and the mod's shim as
`Luma-Upscaler-Shim.dll`; the package renames the pair (see the zip contents below).

## Package
```
powershell -NoProfile -ExecutionPolicy Bypass -File "Data\Dragon Age Origins\package-release.ps1" -OutDir "C:\Stuff\Luma-DAO-release"
```
It runs the framework's `Scripts\package.ps1` (the addon, the `Luma/` shader mounts, ReShade as `dxgi.dll`,
`d3dcompiler_47_x32.dll`), then adds the mod's runtime files, refuses development leftovers and **refuses any `.exe`**,
and writes `Luma-Dragon_Age_Origins-x32.<yyyy-MM-dd-HHmm>.zip`.

## Zip contents (root)
- `Luma-Dragon Age Origins.addon`
- `dxgi.dll` - ReShade 6.8.0, 32 bit, add-on support, from `Source/External/reshade/bin/Win32/Release/ReShade32.dll`
  (git-ignored; the file must be put there before packaging).
- `d3d9.dll` + `dgVoodoo.conf` - **dgVoodoo2 2.87.6**: the x86 `D3D9.dll` from
  `Source/External/dgVoodoo/dgVoodoo2_87_6.zip` and the config from `Data/Dragon Age Origins/dgVoodoo.conf`.
  The shader hashes are tied to this wrapper version: no other version ships.
- `nvngx_dlss.dll` - DLSS, from `Source/External/NGX/bin/rel/`. NVIDIA's file, taken from a driver install; it must
  sit next to the helper.
- `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` - FSR, from
  `Source/External/FidelityFX-API/signedbin/`, plus `Licenses/FidelityFX_LICENSE.md`.
- `Luma-Upscaler.dll` - the **x64** helper the framework's SR bridge runs through Windows' own `rundll32`
  (`rundll32 "Luma-Upscaler.dll",LumaUpscaler`), which is why the package needs no executable. It is required:
  without it DLSS and FSR stay greyed out. **This name is the mod's shim** (`Source\Games\Dragon Age Origins\SR Helper Shim`):
  it loads `amd_fidelityfx_loader_dx12.dll` from the game's folder and then forwards to the framework's helper.
- `Luma-Upscaler-Files.txt` - the pair's FNV-1a 64 hashes, written by `package-release.ps1`. The shim compares them and refuses
  loudly (naming the file) when only one of the two was replaced - the common support case. No manifest: the check is skipped.
- `Luma-Upscaler-Core.dll` - the framework's x64 helper, unchanged, renamed so the shim can take the name Core checks for. Keep the
  pair together: Core opens the FidelityFX loader by name against `rundll32`'s folder (System32), so without the shim's preload FSR
  fails to start (`FAIL FSR init or create`) and the frame is left with no upscaler.
- `Luma/` - the shader mounts (`Global`, `Includes`, `Dragon Age Origins`) and `d3dcompiler_47_x32.dll`.

## The shim's knobs, and A/B testing an upscaler version

`Source/Games/Dragon Age Origins/SR Helper Shim` runs as `Luma-Upscaler.dll` (Core's name for it) and reads `Luma-Upscaler.ini` from the
game's folder when one is there. Both keys are optional and nothing ships with them:

```
[LumaUpscaler]
; a folder next to the game whose copies of the dlls below take precedence (absolute paths work too)
VariantFolder=Upscalers\FSR-3.1.5
; 1: keep going when Luma-Upscaler-Files.txt doesn't match the pair
AllowMismatch=0
```

A file in the variant folder wins over the game's own copy of the same name: `nvngx_dlss.dll`, `amd_fidelityfx_loader_dx12.dll` or
`amd_fidelityfx_upscaler_dx12.dll`. Most recent helper start in the log, from `Luma-Upscaler.log`:

```
[Luma Upscaler] SR environment: shim built 2026-11-03 15:47 a344b263858a09d8 | core 1.0.0.0 a8f0c575ccdefd3c
[Luma Upscaler] SR files: DLSS nvngx_dlss.dll 310.9.16.0 | FSR amd_fidelityfx_loader_dx12.dll 2.3.0.2740
[Luma Upscaler] SR files: FSR upscaler 4.1.1.2740 | no variant folder
[Luma Upscaler] SR pair: matches Luma-Upscaler-Files.txt
```

To test a different FSR version, put a container that carries that version - same file name - in a variant folder and point
`VariantFolder` at it: the FidelityFX loader then lists only what that container has, and Core's own default (its newest) is what runs,
so 3.1.5 needs no version override. DLSS is the same with `nvngx_dlss.dll` (versions are reported by the log line above). Delete the
folder (or the key) to go back; nothing in the repo or the package changes.

## Never in the zip
- **NO executables at all**: no `dgVoodooCpl.exe`, no `Luma-Upscaler.exe`, no `.exe` anywhere. Dlls are fine, and the
  helper is a dll precisely for this rule.
- Development leftovers: `Dump` folders, compiled `.cso`, `.bak`/`.prev` addons, `.pdb`/`.exp`/`.lib`, logs.
- `ReShade.ini`, `ReShadePreset.ini` or any other user config: users get ReShade defaults.
- XeSS is not in this build (`ENABLE_XESS 0`, no `libxess.dll`): no XeSS option, no `libxess.dll` in the zip.

## Deploying to the local game (`C:\Games\Dragon Age Origins\bin_ship`)
- Dev builds: copy only the addon, `Luma-Upscaler.dll` (the shim) and `Luma-Upscaler-Core.dll`. A development build loads shaders from the repository
  path baked in at build time, so the `Luma/` folder is not needed.
- Release builds: copy the addon, `Luma-Upscaler.dll`, `Luma-Upscaler-Core.dll` and the `Luma/` folder.
- Never overwrite the user's `dgVoodoo.conf`, `ReShade.ini`, `ReShadePreset.ini` or their dlls.
- Dev files moved out of the game folder live in `C:\Stuff\Luma-DAO-dev-backup`.

## Nexus description
- `Nexus-Description.txt` starts with the Ko-fi line (https://ko-fi.com/ostadgaming), then the WIP banner.
- It tells users to extract everything next to the game's exe and not to swap the dgVoodoo version: keep that true.
