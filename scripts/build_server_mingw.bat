@echo off
REM AudioRouter - Windows MinGW Build Script
setlocal enabledelayedexpansion

echo =================================================
echo  Building AudioRouter Windows Server (MinGW)
echo =================================================

g++ -std=c++17 -O3 -Wall -Wextra ^
    -Isrc\common -Isrc\server ^
    src\server\main.cpp ^
    src\server\server.cpp ^
    src\server\wasapi_capture.cpp ^
    src\server\dummy_capture.cpp ^
    src\server\audio_endpoint_control.cpp ^
    src\common\socket_util.cpp ^
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
