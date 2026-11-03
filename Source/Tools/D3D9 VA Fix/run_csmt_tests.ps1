# Offline regression for the CSMT layer (csmt.h): the command-ring unit test, then d3d9_csmt_test.exe through the proxy
# with CSMT off and on, against the Windows d3d9 runtime and dgVoodoo. Every frame's CRC must match between off and on,
# and each run must pass its own checks (queries, thread content). All runs start at once, each in its own folder;
# a run that takes longer than -TimeoutSeconds is killed and fails. -Repeat N runs the set N times: races between the
# game threads and the worker show up only now and then (use 10 after a threading change). -Bench adds a CPU benchmark of both (sequential).
# Build first with build.cmd and Tests\build.cmd.
# Usage: pwsh -File run_csmt_tests.ps1 -DgVoodoo <folder with dgVoodoo's D3D9.dll (or d3d9_chain.dll) and dgVoodoo.conf>
# Use dgVoodoo 2.87.3 (_tools\dgv_re873\MS): offline, 2.87.5 behind the proxy overflows its stack (it runs in
# the game; same shader hashes as 2.87.3). The test exe imports d3d9.dll like a game, so it runs from its folder.
param([Parameter(Mandatory)][string]$DgVoodoo, [switch]$Bench, [int]$TimeoutSeconds = 20, [int]$Repeat = 1)
$ErrorActionPreference = "Stop"
$bin = Join-Path $PSScriptRoot "..\..\..\Binaries\Win32-Tools\D3D9 VA Fix"
$env:D3D9_MEMLOG_MS = "1000"
$failed = 0

"=== command ring"
& (Join-Path $bin "csmt_queue_test.exe") | ForEach-Object { "  $_" }
if ($LASTEXITCODE -ne 0) { $failed++ }

$dgvDll = @("D3D9.dll", "d3d9_chain.dll") | ForEach-Object { Join-Path $DgVoodoo $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
function New-Folder([string]$name, [string]$chain, [bool]$csmt, [bool]$vaFix) {
    $dir = Join-Path $bin "csmt_tests\$name"
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item (Join-Path $bin "d3d9.dll"), (Join-Path $bin "d3d9_csmt_test.exe") $dir -Force
    Copy-Item $chain (Join-Path $dir "d3d9_chain.dll") -Force
    if ($chain -eq $dgvDll) { Copy-Item (Join-Path $DgVoodoo "dgVoodoo.conf") $dir -Force }
    New-Item -ItemType File -Force (Join-Path $dir "d3d9_memlog.on") | Out-Null
    $vaFlag = Join-Path $dir "d3d9_vafix.on"
    if ($vaFix) { New-Item -ItemType File -Force $vaFlag | Out-Null } else { Remove-Item $vaFlag -ErrorAction SilentlyContinue }
    $csmtFlag = Join-Path $dir "d3d9_csmt.on"
    if ($csmt) { New-Item -ItemType File -Force $csmtFlag | Out-Null } else { Remove-Item $csmtFlag -ErrorAction SilentlyContinue }
    return $dir
}

function Start-Test([string]$dir, [string[]]$arguments) {
    Set-Content (Join-Path $dir "d3d9_memlog.log") ""
    $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $dir "d3d9_csmt_test.exe"), (@("import") + $arguments) -join " ")
    $info.WorkingDirectory = $dir
    $info.RedirectStandardOutput = $true
    $info.UseShellExecute = $false
    $process = [Diagnostics.Process]::Start($info)
    return @{ Process = $process; Output = $process.StandardOutput.ReadToEndAsync(); Dir = $dir }
}

function Wait-Test($test) {
    $exit = -1
    if ($test.Process.WaitForExit($TimeoutSeconds * 1000)) {
        $exit = $test.Process.ExitCode
    }
    else {
        $test.Process.Kill()
        $test.Process.WaitForExit()
    }
    $output = @($test.Output.Result -split "`r?`n" | Where-Object { $_ })
    if ($exit -eq -1) { $output += "FAIL timeout after $TimeoutSeconds s" }
    return @{ Output = $output; Exit = $exit; Log = (Get-Content (Join-Path $test.Dir "d3d9_memlog.log")) }
}

function Invoke-Test([string]$dir, [string[]]$arguments) {
    return Wait-Test (Start-Test $dir $arguments)
}

# CSMT refuses the Windows runtime; its runs check that the request is harmless there, with and without the VA fix
# (windows_vafix: the test's state block drops the VA fix's device hooks unless they are restored).
$runtimes = @(@{ Name = "windows"; Chain = "$env:SystemRoot\SysWOW64\d3d9.dll"; VaFix = $false },
    @{ Name = "windows_vafix"; Chain = "$env:SystemRoot\SysWOW64\d3d9.dll"; VaFix = $true },
    @{ Name = "dgvoodoo"; Chain = $dgvDll; VaFix = $true })
$runs = @(
    @{ Name = "frames"; Args = @("frames=8") },
    @{ Name = "reset"; Args = @("frames=8", "reset") },
    @{ Name = "redevice"; Args = @("frames=4", "redevice") },
    @{ Name = "thread"; Args = @("frames=12", "thread") },
    # A D3D9Ex game (Bayonetta): its own Direct3DCreate9Ex and CreateDeviceEx reach the layer
    @{ Name = "ex"; Args = @("frames=8", "ex") },
    @{ Name = "ex_reset"; Args = @("frames=8", "reset", "ex") },
    # dgVoodoo's uploaded vertex constants against the proxy's mirror at every D3D11 draw (Luma reads the mirror)
    @{ Name = "constants"; Args = @("frames=8", "constants") }
)
# Every run gets its own off and on folder; the runs of one round start at once, rounds go one after another (all
# rounds at once overload the GPU until the in-test watchdog fires).
foreach ($round in 1..$Repeat) {
$started = foreach ($runtime in $runtimes) {
    foreach ($run in $runs) {
        $off = New-Folder "$($runtime.Name)_$($run.Name)_$($round)_off" $runtime.Chain $false $runtime.VaFix
        $on = New-Folder "$($runtime.Name)_$($run.Name)_$($round)_on" $runtime.Chain $true $runtime.VaFix
        @{ Name = "$($runtime.Name) / $($run.Name)"; A = (Start-Test $off $run.Args); B = (Start-Test $on $run.Args) }
    }
}
foreach ($test in $started) {
    $a = Wait-Test $test.A
    $b = Wait-Test $test.B
    $crcA = $a.Output | Where-Object { $_ -like "crc *" }
    $crcB = $b.Output | Where-Object { $_ -like "crc *" }
    $diff = Compare-Object @($crcA) @($crcB)
    # dgVoodoo runs threaded; on the Windows runtime the flag must be refused, and everything else still pass.
    $expected = if ($test.Name -like "dgvoodoo*") { "CSMT: on" } else { "CSMT: requested, but" }
    $ok = ($a.Exit -eq 0) -and ($b.Exit -eq 0) -and -not $diff -and $crcA.Count -gt 0 -and
        ($b.Log | Select-String $expected)
    # With CSMT on dgVoodoo's draws must have been compared (the mirror is live), not merely found equal by having none
    if ($test.Name -eq "dgvoodoo / constants") { $ok = $ok -and ($b.Output | Select-String "constants: compared [1-9]") }
    "=== $($test.Name): $(if ($ok) { 'ok' } else { 'FAILED' }) ($($crcA.Count) frames)"
    if ($ok) {
        Remove-Item $test.A.Dir, $test.B.Dir -Recurse -Force # failed runs keep their folders for diagnosis
    }
    else {
        $failed++
        "  off exit $($a.Exit), on exit $($b.Exit)"
        $diff | Select-Object -First 4 | ForEach-Object { "  crc differs: $($_.InputObject) ($($_.SideIndicator))" }
        ($a.Output + $b.Output) | Where-Object { $_ -match "FAIL" } | Select-Object -First 6 | ForEach-Object { "  $_" }
    }
}
}
if ($Bench) {
    foreach ($runtime in $runtimes) {
        $off = New-Folder "$($runtime.Name)_bench_off" $runtime.Chain $false $runtime.VaFix
        $on = New-Folder "$($runtime.Name)_bench_on" $runtime.Chain $true $runtime.VaFix
        foreach ($benchArgs in @(@("frames=100", "bench=3000"), @("frames=100", "bench=3000", "work=3"))) {
            $a = Invoke-Test $off $benchArgs
            $b = Invoke-Test $on $benchArgs
            $ms = { param($r) ($r.Output | Select-String "ms per frame").Line }
            "=== $($runtime.Name) / $($benchArgs -join ' '): off: $(& $ms $a) | on: $(& $ms $b)"
        }
        Remove-Item $off, $on -Recurse -Force
    }
}
if ($failed) { "FAILED ($failed)"; exit 1 } else { "PASSED" }
