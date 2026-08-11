@echo off
REM AudioRouter - Windows MinGW Build Script (Professional)
REM Builds audiorouter_server.exe using g++ with C++23 and hardening flags.
setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%SCRIPT_DIR%.."
pushd "%PROJECT_ROOT%"

set "BIN_DIR=bin"
set "CLEAN=0"
set "CONFIG=Release"

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="/?" goto :help
if /I "%~1"=="-h" goto :help
if /I "%~1"=="--help" goto :help
if /I "%~1"=="--clean" set "CLEAN=1" & shift & goto :parse_args
if /I "%~1"=="clean" set "CLEAN=1" & shift & goto :parse_args
if /I "%~1"=="--debug" set "CONFIG=Debug" & shift & goto :parse_args
shift
goto :parse_args
:args_done

if "%CLEAN%"=="1" (
    if exist "%BIN_DIR%\audiorouter_server.exe" del /Q "%BIN_DIR%\audiorouter_server.exe"
)

echo ============================================================
echo  AudioRouter - Windows Server Build (MinGW / g++)
echo  Config: %CONFIG%
echo ============================================================

where g++ >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] g++ not found in PATH. Install MinGW-w64 (GCC 13+ with C++23 support).
    popd
    exit /b 1
)

echo [INFO] Compiler: 
g++ --version | findstr /R "g++"

if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

set "CXXFLAGS=-std=c++23 -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Isrc\common -Isrc\server"
if /I "%CONFIG%"=="Debug" set "CXXFLAGS=-std=c++23 -O0 -g -Wall -Wextra -Wpedantic -fstack-protector-strong -Isrc\common -Isrc\server -fno-omit-frame-pointer"

set "LDFLAGS=-lws2_32 -liphlpapi -lavrt -lole32"

echo [INFO] Compiling...

g++ %CXXFLAGS% ^
    src\server\main.cpp ^
    src\server\server.cpp ^
    src\server\wasapi_capture.cpp ^
    src\server\dummy_capture.cpp ^
    src\server\audio_endpoint_control.cpp ^
    src\common\socket_util.cpp ^
    -o %BIN_DIR%\audiorouter_server.exe %LDFLAGS%

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] MinGW build failed. Check errors above.
    echo        Ensure you have Windows SDK headers for WASAPI (audioclient.h) and mmdeviceapi.h
    popd
    exit /b %ERRORLEVEL%
)

if not exist "%BIN_DIR%\audiorouter_server.exe" (
    echo [ERROR] Binary not found after build.
    popd
    exit /b 1
)

echo.
echo ============================================================
echo  Build succeeded
echo  Executable: %BIN_DIR%\audiorouter_server.exe
echo  Compiler  : g++ %CONFIG%
echo ============================================================
echo  Next steps:
echo    %BIN_DIR%\audiorouter_server.exe --list-if
echo    %BIN_DIR%\audiorouter_server.exe --help
echo ============================================================

popd
exit /b 0

:help
echo Usage: %~nx0 [options]
echo.
echo Options:
echo   --clean          Remove output binary before build
echo   --debug          Build with -O0 -g (default -O3)
echo   -h, /?, --help   Show this help
echo.
echo Examples:
echo   %~nx0
echo   %~nx0 --clean
echo   %~nx0 --debug
exit /b 0
