@echo off
rem Builds winocr_bench.exe with MSVC (C++/WinRT does not build with MinGW). Usage: build.cmd <outDir>
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0..\..\..\build\ocr_bench\winocr
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if "%VS%"=="" (echo MSVC not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W3 "%~dp0winocr_bench.cpp" /Fo"%OUT%\\" /Fe"%OUT%\winocr_bench.exe" windowsapp.lib advapi32.lib
