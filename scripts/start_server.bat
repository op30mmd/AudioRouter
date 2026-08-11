@echo off
REM AudioRouter - Windows Server Interactive Launcher (Professional)
REM User-friendly launcher with interface listing, port/mute options, and stats.
setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%SCRIPT_DIR%.."
pushd "%PROJECT_ROOT%"

set "BIN_DIR=bin"
set "EXE=%BIN_DIR%\audiorouter_server.exe"
set "PORT=44100"
set "BIND=0.0.0.0"
set "FRAMES=240"
set "MUTE_MODE=mute"
set "NO_MUTE=0"
set "TEST_TONE=0"
set "FREQ=440"

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="-h" goto :help
if /I "%~1"=="--help" goto :help
if /I "%~1"=="/?" goto :help
if /I "%~1"=="-p" set "PORT=%~2" & shift & shift & goto :parse_args
if /I "%~1"=="--port" set "PORT=%~2" & shift & shift & goto :parse_args
if /I "%~1"=="-b" set "BIND=%~2" & shift & shift & goto :parse_args
if /I "%~1"=="--bind" set "BIND=%~2" & shift & shift & goto :parse_args
shift
goto :parse_args
:args_done

if not exist "%EXE%" (
    echo [ERROR] Server binary not found at %EXE%
    echo        Build first with:
    echo          scripts\build_server_msvc.bat
    echo          scripts\build_server_mingw.bat
    echo          scripts\build_all.bat
    popd & exit /b 1
)

echo ============================================================
echo  AudioRouter - Windows Server Launcher
echo  Binary: %EXE%
echo ============================================================
echo.

echo [1/3] Available network interfaces:
"%EXE%" --list-if 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [WARN] Could not list interfaces via audiorouter_server, falling back to ipconfig:
    ipconfig | findstr /R "IPv4.*Address Wireless.*Adapter Ethernet.*adapter"
)
echo.

echo Current settings:
echo   Port       : %PORT%
echo   Bind IP    : %BIND%
echo   Frames/pkt : %FRAMES% (5ms at 48kHz)
echo   Mute mode  : %MUTE_MODE%
echo   No-mute    : %NO_MUTE%
echo   Test tone  : %TEST_TONE%
echo.

set /P USER_PORT="Enter UDP port [%PORT%]: "
if not "%USER_PORT%"=="" set "PORT=%USER_PORT%"

set /P USER_MUTE="Mute mode: mute/zero/both/no [%MUTE_MODE%]: "
if not "%USER_MUTE%"=="" (
    if /I "%USER_MUTE%"=="no" (
        set "NO_MUTE=1"
    ) else (
        set "MUTE_MODE=%USER_MUTE%"
        set "NO_MUTE=0"
    )
)

set /P USER_TONE="Use test tone instead of WASAPI loopback? y/N: "
if /I "%USER_TONE%"=="y" set "TEST_TONE=1"
if /I "%USER_TONE%"=="yes" set "TEST_TONE=1"

echo.
echo ============================================================
echo  Starting server on %BIND%:%PORT%
echo  Mute: %MUTE_MODE%  No-mute: %NO_MUTE%  Test tone: %TEST_TONE%
echo  Press Ctrl+C to stop
echo ============================================================
echo.

set "ARGS=-p %PORT% -b %BIND% -f %FRAMES% --mute-mode %MUTE_MODE%"

if "%NO_MUTE%"=="1" set "ARGS=%ARGS% --no-mute"
if "%TEST_TONE%"=="1" set "ARGS=%ARGS% --test-tone --freq %FREQ%"

echo [INFO] Command: %EXE% %ARGS%
echo.

"%EXE%" %ARGS%

echo.
echo Server exited with code %ERRORLEVEL%
echo If PC speakers did not restore, run: %EXE% --help or check audio endpoint control logs.

popd
exit /b %ERRORLEVEL%

:help
echo Usage: %~nx0 [options]
echo.
echo Options:
echo   -p, --port PORT   UDP port (default 44100)
echo   -b, --bind IP     Bind IP (default 0.0.0.0)
echo   -h, --help        Show this help
echo.
echo This is an interactive launcher that:
echo   - Shows network interfaces
echo   - Prompts for port and mute mode
echo   - Starts audiorouter_server with chosen options
echo.
echo Non-interactive use:
echo   %~nx0 --port 44100 --bind 0.0.0.0
echo   Then you will still be prompted, but defaults are set.
echo.
echo For fully non-interactive, run directly:
echo   bin\audiorouter_server.exe -p 44100 --mute-mode both
exit /b 0
