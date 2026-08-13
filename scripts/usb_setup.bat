@echo off
REM AudioRouter - Voice over USB tunnel setup (run on the Windows PC)
REM Usage: usb_setup.bat [port]   (default port: 44100)
setlocal enabledelayedexpansion

set PORT=%~1
if "%PORT%"=="" set PORT=44100

echo =================================================
echo  AudioRouter - Voice over USB tunnel setup
echo =================================================
echo.
echo  Requirements:
echo    * Phone connected to this PC with a USB cable
echo    * USB debugging enabled on the phone (Developer options)
echo    * If the phone shows an "Allow USB debugging?" prompt, authorize it
echo.
echo  Note: adb cannot forward UDP sockets, so the tunnel is TCP; the
echo        AudioRouter server and client relay the UDP stream over it.
echo.
echo  Checking adb...

where adb >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] 'adb' not found on PATH.
    echo         Install Android platform-tools and add it to PATH,
    echo         or run this script from the platform-tools folder.
    exit /b 1
)

echo.
echo  Connected devices:
adb devices

echo.
echo  Setting up tunnel: adb reverse tcp:%PORT% tcp:%PORT%
adb reverse tcp:%PORT% tcp:%PORT%
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] adb reverse failed. Check that a device is listed above
    echo         and authorized (device state should be 'device').
    exit /b %ERRORLEVEL%
)

echo.
echo  Active reverse tunnels:
adb reverse --list

echo.
echo [OK] USB tunnel is up (tcp:%PORT% over the USB cable).
echo      Note: adb cannot forward UDP, so the AudioRouter server/client
echo      relay the stream over this TCP tunnel automatically.
echo.
echo  Next steps:
echo    * On this PC :  bin\audiorouter_server.exe --usb
echo    * On the phone: ./audiorouter_client -u
echo.
echo  Teardown when done:  adb reverse --remove-all
endlocal
