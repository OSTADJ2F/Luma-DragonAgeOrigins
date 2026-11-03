@echo off
rem Builds the x86 test programs into Binaries\Win32-Tools\D3D9 VA Fix\ (needs the VS x86 build tools).
setlocal
set "OUT=%~dp0..\..\..\..\Binaries\Win32-Tools\D3D9 VA Fix"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
call "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
set RESULT=0
for %%T in (d3d9_pool_test vram_probe csmt_queue_test d3d9_csmt_test) do (
  cl /nologo /std:c++20 /EHsc /O2 /MD /W3 "%~dp0%%T.cpp" /Fo"%OUT%\obj\\" /Fe"%OUT%\%%T.exe" /link /LARGEADDRESSAWARE user32.lib psapi.lib > "%OUT%\build_%%T.log" 2>&1
  if errorlevel 1 set RESULT=1
)
echo build exit %RESULT% (logs: %OUT%\build_*.log)
exit /b %RESULT%
