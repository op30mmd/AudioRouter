@echo off
REM AudioRouter - USB tethering one-time PC-side setup.
REM Keeps the phone's USB link from ever taking over the PC's internet routing:
REM gives the RNDIS adapter a static IP with NO default gateway (persists per
REM adapter). Run once while USB tethering is active on the phone.
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting elevation ^(admin rights are needed to change the network config^)...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0usb_tether_setup.ps1"
echo.
echo Press any key to close...
pause >nul
