@echo off
REM AudioRouter - Windows MSVC Build Script (Professional)
REM Builds audiorouter_server.exe (and client if possible) using CMake + MSVC
REM Supports x64, Release/Debug, clean, and fallback VS generators.
setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%SCRIPT_DIR%.."
pushd "%PROJECT_ROOT%"

set "BUILD_DIR=build_msvc"
set "BIN_DIR=bin"
set "CONFIG=Release"
set "CLEAN=0"
set "ARCH=x64"

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="/?" goto :help
if /I "%~1"=="-h" goto :help
if /I "%~1"=="--help" goto :help
if /I "%~1"=="--clean" set "CLEAN=1" & shift & goto :parse_args
if /I "%~1"=="clean" set "CLEAN=1" & shift & goto :parse_args
if /I "%~1"=="--debug" set "CONFIG=Debug" & shift & goto :parse_args
if /I "%~1"=="--config" set "CONFIG=%~2" & shift & shift & goto :parse_args
shift
goto :parse_args
:args_done

if "%CLEAN%"=="1" (
    echo [INFO] Cleaning %BUILD_DIR% and %BIN_DIR%\audiorouter_server.exe
    if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
    if exist "%BIN_DIR%\audiorouter_server.exe" del /Q "%BIN_DIR%\audiorouter_server.exe"
)

echo ============================================================
echo  AudioRouter - Windows Server Build (MSVC / CMake)
echo  Config: %CONFIG%  Arch: %ARCH%  BuildDir: %BUILD_DIR%
echo ============================================================

where cmake >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] cmake not found in PATH. Install CMake 3.20+ and add to PATH.
    echo         https://cmake.org/download/
    popd
    exit /b 1
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

cd "%BUILD_DIR%"
set "CMAKE_GEN="

echo [INFO] Generating CMake project...

cmake -G "Visual Studio 17 2022" -A %ARCH% -DCMAKE_BUILD_TYPE=%CONFIG% ..
if %ERRORLEVEL% EQU 0 (
    set "CMAKE_GEN=Visual Studio 17 2022"
    goto :build
)

echo [WARN] VS2022 generator failed, trying VS2019...
cmake -G "Visual Studio 16 2019" -A %ARCH% -DCMAKE_BUILD_TYPE=%CONFIG% ..
if %ERRORLEVEL% EQU 0 (
    set "CMAKE_GEN=Visual Studio 16 2019"
    goto :build
)

echo [WARN] VS2019 generator failed, trying default Visual Studio generator...
cmake -DCMAKE_BUILD_TYPE=%CONFIG% ..
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake generation failed with all VS generators.
    echo         Ensure Visual Studio with C++ desktop workload is installed.
    popd
    exit /b 1
)

:build
echo [INFO] Building with %CMAKE_GEN% (%CONFIG%)...
cmake --build . --config %CONFIG% --target audiorouter_server --parallel
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed.
    cd ..
    popd
    exit /b %ERRORLEVEL%
)

cd ..

set "EXE_SRC_1=%BUILD_DIR%\src\server\%CONFIG%\audiorouter_server.exe"
set "EXE_SRC_2=%BUILD_DIR%\bin\%CONFIG%\audiorouter_server.exe"
set "EXE_SRC_3=%BUILD_DIR%\src\server\audiorouter_server.exe"
set "EXE_DST=%BIN_DIR%\audiorouter_server.exe"

if exist "%EXE_SRC_1%" (
    copy /Y "%EXE_SRC_1%" "%EXE_DST%" >nul
) else if exist "%EXE_SRC_2%" (
    copy /Y "%EXE_SRC_2%" "%EXE_DST%" >nul
) else if exist "%EXE_SRC_3%" (
    copy /Y "%EXE_SRC_3%" "%EXE_DST%" >nul
)

if not exist "%EXE_DST%" (
    echo [ERROR] Expected binary not found after build.
    echo        Checked: %EXE_SRC_1%
    echo                 %EXE_SRC_2%
    echo                 %EXE_SRC_3%
    popd
    exit /b 1
)

echo.
echo ============================================================
echo  Build succeeded
echo  Executable: %EXE_DST%
echo  Generator : %CMAKE_GEN%
echo  Config    : %CONFIG%
echo ============================================================
echo  Next steps:
echo    %EXE_DST% --list-if
echo    %EXE_DST% --help
echo ============================================================

popd
exit /b 0

:help
echo Usage: %~nx0 [options]
echo.
echo Options:
echo   --clean          Remove build directory before building
echo   --debug          Build Debug configuration (default Release)
echo   --config NAME    Specify configuration: Release, Debug, RelWithDebInfo
echo   -h, /?, --help   Show this help
echo.
echo Examples:
echo   %~nx0
echo   %~nx0 --clean
echo   %~nx0 --debug
exit /b 0
