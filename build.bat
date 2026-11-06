@echo off
rem Builds build\VobWay.dll (32-bit, static CRT) with Visual Studio 2019 or newer.
rem Needs the "Desktop development with C++" workload.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found. Install Visual Studio with the C++ workload.
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo No Visual Studio with C++ tools found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
if not exist build mkdir build
cl /nologo /O2 /MT /W4 /EHa /std:c++17 /DWIN32 /D_CRT_SECURE_NO_WARNINGS /LD src\vobway.cpp /Fo:build\ /Fe:build\VobWay.dll /link /SAFESEH:NO
exit /b %ERRORLEVEL%
