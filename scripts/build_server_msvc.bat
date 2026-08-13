@echo off
REM AudioRouter - Windows MSVC Build Script
setlocal enabledelayedexpansion

REM Always run from the project root (script dir/..), no matter where the
REM user invoked this script from.
cd /d "%~dp0.."

echo =================================================
echo  Building AudioRouter Windows Server (MSVC)
echo =================================================

if not exist build_msvc (
    mkdir build_msvc
)

cd build_msvc
cmake -G "Visual Studio 17 2022" -A x64 ..
if %ERRORLEVEL% NEQ 0 (
    echo CMake generation failed with Visual Studio 2022, trying Visual Studio 2019...
    cmake -G "Visual Studio 16 2019" -A x64 ..
)

cmake --build . --config Release --target audiorouter_server
if %ERRORLEVEL% NEQ 0 (
    echo Build failed.
    exit /b %ERRORLEVEL%
)

cd ..
if not exist bin (
    mkdir bin
)

copy build_msvc\src\server\Release\audiorouter_server.exe bin\ >nul 2>&1
if not exist bin\audiorouter_server.exe (
    copy build_msvc\bin\Release\audiorouter_server.exe bin\ >nul 2>&1
)

if not exist bin\audiorouter_server.exe (
    echo Error: audiorouter_server.exe not found after build.
    echo Looked in build_msvc\src\server\Release\ and build_msvc\bin\Release\.
    exit /b 1
)

echo.
echo =================================================
echo  Build Succeeded!
echo  Executable: bin\audiorouter_server.exe
echo =================================================
