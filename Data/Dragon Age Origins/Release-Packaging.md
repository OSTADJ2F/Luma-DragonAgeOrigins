# Dragon Age: Origins - Nexus release package requirements

Rules for every release zip (Nexus). Follow them when packaging.

## Build and package
- Build `Publishing-Release`, `Win32`. When building the .vcxproj directly pass the solution dir, or the dev shader fallback path is wrong:
  `-p:SolutionDir="C:\Stuff\Luma\\" -p:SolutionName=Luma`
- Package with `Scripts/package.ps1 -Project "Dragon Age Origins" -Config Publishing-Release -Platform Win32 -OutDir <dir> -NoExe`
  (run it from PowerShell: in bash the backslashes in `-OutDir` get eaten).

## Zip contents
- NO executables (`-NoExe`): no `dgVoodooCpl.exe`, no `.exe` anywhere. Dlls are fine.
- ReShade included as `dxgi.dll` (32-bit ReShade 6.8.0 with add-on support). The packaging script copies it from
  `Source/External/reshade/bin/Win32/Release/ReShade32.dll` (git-ignored; put the ReShade 32-bit dll there).
- DLSS dll is the user's own `nvngx_dlss.dll`, kept in `Source/External/NGX/bin/rel/` (Publishing) and `bin/dev/`.
  Originals are backed up in `C:\Stuff\Luma-DAO-dev-backup\ngx-original`.
- Also ships: the addon, `Luma-Upscaler.dll`, `d3d9.dll` + `dgVoodoo.conf` (dgVoodoo2), the two AMD FidelityFX dlls
  and the `Luma/` shader folder.
- No dev leftovers: no `Dump` folders, compiled `.cso`, `.bak`/`.prev` addons or logs. Zip paths use forward slashes.
- No `ReShade.ini` / presets: users get ReShade defaults.

## Deploying to the local game (`C:\Games\Dragon Age Origins\bin_ship`)
- Copy only the addon, `Luma-Upscaler.dll` and the `Luma/` folder. Never overwrite the user's `dgVoodoo.conf`, `ReShade.ini`,
  `ReShadePreset.ini` or their dlls.
- Dev files moved out of the game folder live in `C:\Stuff\Luma-DAO-dev-backup`.

## Nexus description
- `Nexus-Description.txt` starts with the Ko-fi line (https://ko-fi.com/ostadgaming), then the WIP banner.
- XeSS is deprecated: `ENABLE_SR_BRIDGE_XESS 0` in `main.cpp`, so no XeSS option and no `libxess.dll` in the zip.
