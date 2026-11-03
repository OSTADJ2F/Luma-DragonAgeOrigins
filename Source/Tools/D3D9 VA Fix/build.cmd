@echo off
rem Builds the x86 d3d9.dll proxy (the "D3D9 VA Fix" Luma.sln project, the same binary package.ps1 ships) and copies it
rem into Binaries\Win32-Tools\D3D9 VA Fix\ next to the tests (needs the VS x86 build tools).
setlocal
set "ROOT=%~dp0..\..\.."
set "OUT=%ROOT%\Binaries\Win32-Tools\D3D9 VA Fix"
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
msbuild "%ROOT%\Luma.sln" /nologo /t:"D3D9 VA Fix" /p:Configuration=Development-Release /p:Platform=Win32 > "%OUT%\build.log" 2>&1
set RESULT=%errorlevel%
if %RESULT%==0 copy /Y "%ROOT%\Binaries\Win32-Development-Release\Luma-D3D9-VA-Fix.dll" "%OUT%\d3d9.dll" >nul
echo build exit %RESULT% (log: %OUT%\build.log)
exit /b %RESULT%
