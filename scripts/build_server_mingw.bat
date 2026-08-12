@echo off
REM AudioRouter - Windows MinGW Build Script
setlocal enabledelayedexpansion

echo =================================================
echo  Building AudioRouter Windows Server (MinGW)
echo =================================================

if not exist bin (
    mkdir bin
)
if not exist build_mingw (
    mkdir build_mingw
)

where windres >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Error: windres was not found. Install the complete MinGW binutils package.
    exit /b 1
)

REM Embed requireAdministrator as RT_MANIFEST so loopback capture runs at the
REM same integrity level as elevated audio-producing programs.
windres --include-dir src\server src\server\audiorouter_server.rc -O coff ^
    -o build_mingw\audiorouter_server_manifest.o
if %ERRORLEVEL% NEQ 0 (
    echo Failed to compile the Windows elevation manifest.
    exit /b %ERRORLEVEL%
)

g++ -std=c++23 -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread ^
    -Isrc\common -Isrc\server ^
    src\server\main.cpp ^
    src\server\server.cpp ^
    src\server\wasapi_capture.cpp ^
    src\server\dummy_capture.cpp ^
    src\server\audio_endpoint_control.cpp ^
    src\common\socket_util.cpp ^
    build_mingw\audiorouter_server_manifest.o ^
    -o bin\audiorouter_server.exe ^
    -lws2_32 -liphlpapi -lavrt -lole32

if %ERRORLEVEL% NEQ 0 (
    echo MinGW build failed.
    exit /b %ERRORLEVEL%
)

echo.
echo =================================================
echo  Build Succeeded!
echo  Executable: bin\audiorouter_server.exe
echo =================================================
