@echo off
rem ---------------------------------------------------------------------------
rem  ResourceIsland - one-shot MSVC build (no CMake needed).
rem  Locates vcvars64.bat automatically; override with the VCVARS64 env var.
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion
set "ROOT=%~dp0"

set "VCVARS=%VCVARS64%"

rem 1) vswhere (ships with every VS 2017+ installer)
if not defined VCVARS (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "!VSWHERE!" (
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -prerelease -products * ^
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
        -property installationPath 2^>nul`) do (
      if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
  )
)

rem 2) brute-force scan of the usual install roots
if not defined VCVARS (
  for %%r in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
    for /d %%v in ("%%~r\Microsoft Visual Studio\*") do (
      for /d %%e in ("%%v\*") do (
        if exist "%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%e\VC\Auxiliary\Build\vcvars64.bat"
      )
    )
  )
)

if not defined VCVARS (
  echo [X] vcvars64.bat not found.
  echo     Install "Visual Studio Build Tools" with the "Desktop development with C++"
  echo     workload, or point the VCVARS64 environment variable at vcvars64.bat.
  exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
  echo [X] vcvars64 failed:
  echo     "%VCVARS%"
  exit /b 1
)

if not exist "%ROOT%build" mkdir "%ROOT%build"
pushd "%ROOT%"

cl /nologo /std:c++20 /utf-8 /EHsc /O2 /MT /DNDEBUG ^
   /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /DUNICODE /D_UNICODE /DNOMINMAX ^
   /Fobuild\ /Febuild\ResourceIsland.exe ^
   src\main.cpp src\island.cpp src\render.cpp src\sampler.cpp src\nvml_dyn.cpp ^
   src\config.cpp src\fps.cpp src\avatar_data.cpp ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
   user32.lib gdi32.lib shell32.lib advapi32.lib pdh.lib

set "RC=%ERRORLEVEL%"
popd

if not "%RC%"=="0" (
  echo [X] build failed with %RC%
  exit /b %RC%
)
echo [OK] %ROOT%build\ResourceIsland.exe
exit /b 0
