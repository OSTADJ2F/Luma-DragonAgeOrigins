# Builds the Nexus release zip for Dragon Age: Origins.
#
# The framework's Scripts\package.ps1 stages what every Luma game ships (the addon, the Luma\ shader mounts,
# ReShade as dxgi.dll, d3dcompiler_47_x32.dll). This script runs it, then adds what only this mod needs, and
# zips the result with a dated name:
#
#   d3d9.dll + dgVoodoo.conf   dgVoodoo2 2.87.6 (the shader hashes are tied to this version)
#   Luma-Upscaler.exe          the x64 DLSS/FSR helper (Core launches it by that exact name)
#   nvngx_dlss.dll             DLSS, loaded by the helper
#   amd_fidelityfx_*.dll       FSR, loaded by the helper
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File "Data\Dragon Age Origins\package-release.ps1"

param(
    [string]$OutDir = "C:\Stuff\Luma-DAO-release",
    [ValidateSet("Publishing-Release", "Test-Release")]
    [string]$Config = "Publishing-Release"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$project = "Dragon Age Origins"
$d3d9Zip = Join-Path $repoRoot "Source\External\dgVoodoo\dgVoodoo2_87_6.zip"

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$OutDir = (Resolve-Path $OutDir).Path

# 1. Framework staging, into a scratch dir of our own so we control the final zip
$stage = Join-Path $env:TEMP ("DAO-Package-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
    & (Join-Path $repoRoot "Scripts\package.ps1") -Project $project -Config $Config -Platform Win32 -OutDir $stage
    $inner = Get-ChildItem $stage -Filter "*.zip" | Select-Object -First 1
    if (-not $inner) { throw "package.ps1 produced no zip" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $pkg = Join-Path $stage "pkg"
    [IO.Compression.ZipFile]::ExtractToDirectory($inner.FullName, $pkg)

    # 2. The mod's own runtime files
    $addon = Get-ChildItem (Join-Path $repoRoot "Binaries\Win32-$Config") -Filter "*.addon" |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $addon) { throw "no $Config addon built" }
    Copy-Item $addon.FullName -Destination (Join-Path $pkg "Luma-$project.addon") -Force

    # dgVoodoo2 2.87.6: the 32 bit d3d9.dll only (the control panel exe is not shipped)
    $dg = Join-Path $stage "dgv"
    [IO.Compression.ZipFile]::ExtractToDirectory($d3d9Zip, $dg)
    $d3d9 = Join-Path $dg "MS\x86\D3D9.dll"
    if (-not (Test-Path $d3d9)) { throw "MS\x86\D3D9.dll not found in $d3d9Zip" }
    Copy-Item $d3d9 -Destination (Join-Path $pkg "d3d9.dll") -Force
    Copy-Item (Join-Path $repoRoot "Data\$project\dgVoodoo.conf") -Destination (Join-Path $pkg "dgVoodoo.conf") -Force

    # The x64 helpers: the SR bridge and the two upscaler libraries it loads
    $helper = Join-Path $repoRoot "Binaries\Win32-$Config\Luma-Upscaler.exe"
    if (-not (Test-Path $helper)) { throw "Luma-Upscaler.exe not built (build Source\Tools\SR Bridge Helper, x64)" }
    Copy-Item $helper -Destination (Join-Path $pkg "Luma-Upscaler.exe") -Force
    Copy-Item (Join-Path $repoRoot "Source\External\NGX\bin\rel\nvngx_dlss.dll") -Destination $pkg -Force
    Copy-Item (Join-Path $repoRoot "Source\External\FidelityFX-API\signedbin\amd_fidelityfx_loader_dx12.dll") -Destination $pkg -Force
    Copy-Item (Join-Path $repoRoot "Source\External\FidelityFX-API\signedbin\amd_fidelityfx_upscaler_dx12.dll") -Destination $pkg -Force
    New-Item -ItemType Directory -Path (Join-Path $pkg "Licenses") -Force | Out-Null
    Copy-Item (Join-Path $repoRoot "Source\External\FidelityFX-API\FidelityFX_LICENSE.md") -Destination (Join-Path $pkg "Licenses") -Force -ErrorAction SilentlyContinue

    # 3. Guards: nothing from a development run, no user config
    $bad = Get-ChildItem $pkg -Recurse -File | Where-Object {
        $_.FullName -match "\\Dump" -or $_.Extension -in @(".cso", ".bak", ".prev", ".log", ".pdb", ".exp", ".lib") -or
        $_.Name -in @("ReShade.ini", "ReShadePreset.ini")
    }
    if ($bad) { throw ("refusing to ship development leftovers: " + (($bad | ForEach-Object { $_.Name }) -join ", ")) }

    # 4. Zip
    $name = "Luma-Dragon_Age_Origins-x32.{0}.zip" -f (Get-Date -Format "yyyy-MM-dd-HHmm")
    $zip = Join-Path $OutDir $name
    [IO.Compression.ZipFile]::CreateFromDirectory($pkg, $zip)
    $size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
    Write-Host "Packaged: $zip ($size MB)"
    Get-ChildItem $pkg | Sort-Object Name | ForEach-Object { Write-Host ("  {0}" -f $_.Name) }
}
finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
