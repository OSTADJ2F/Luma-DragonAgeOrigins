# Dragon Age: Origins - Nexus release package requirements

Rules for every release zip (Nexus). Follow them when packaging.

## Build
Build two things, with the solution dir passed (a development build bakes its shader fallback path into itself):

```
MSBuild "Source\Games\Dragon Age Origins\Dragon Age Origins.vcxproj" -p:Configuration=Publishing-Release ^
  -p:Platform=Win32 "-p:SolutionDir=<repo>\\" -p:SolutionName=Luma -m

MSBuild "Source\Tools\SR Bridge Helper\SR Bridge Helper DLL.vcxproj" -p:Configuration=Publishing-Release ^
  -p:Platform=x64 "-p:SolutionDir=<repo>\\" -m
```

The addon lands in `Binaries\Win32-Publishing-Release`, the x64 helper next to it as `Luma-Upscaler.dll`.

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
- `Luma-Upscaler.dll` - the **x64** SR helper. Core runs it through Windows' own `rundll32`
  (`rundll32 "Luma-Upscaler.dll",LumaUpscaler`), which is why the package needs no executable. It is required:
  without it DLSS and FSR stay greyed out.
- `Luma/` - the shader mounts (`Global`, `Includes`, `Dragon Age Origins`) and `d3dcompiler_47_x32.dll`.

## Never in the zip
- **NO executables at all**: no `dgVoodooCpl.exe`, no `Luma-Upscaler.exe`, no `.exe` anywhere. Dlls are fine, and the
  helper is a dll precisely for this rule.
- Development leftovers: `Dump` folders, compiled `.cso`, `.bak`/`.prev` addons, `.pdb`/`.exp`/`.lib`, logs.
- `ReShade.ini`, `ReShadePreset.ini` or any other user config: users get ReShade defaults.
- XeSS is not in this build (`ENABLE_XESS 0`, no `libxess.dll`): no XeSS option, no `libxess.dll` in the zip.

## Deploying to the local game (`C:\Games\Dragon Age Origins\bin_ship`)
- Dev builds: copy only the addon and `Luma-Upscaler.dll`. A development build loads shaders from the repository
  path baked in at build time, so the `Luma/` folder is not needed.
- Release builds: copy the addon, `Luma-Upscaler.dll` and the `Luma/` folder.
- Never overwrite the user's `dgVoodoo.conf`, `ReShade.ini`, `ReShadePreset.ini` or their dlls.
- Dev files moved out of the game folder live in `C:\Stuff\Luma-DAO-dev-backup`.

## Nexus description
- `Nexus-Description.txt` starts with the Ko-fi line (https://ko-fi.com/ostadgaming), then the WIP banner.
- It tells users to extract everything next to the game's exe and not to swap the dgVoodoo version: keep that true.
