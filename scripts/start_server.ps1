# AudioRouter - Windows Server Launcher (PowerShell, Professional)
# Interactive launcher with interface detection, port/mute options, and firewall hint.
param(
    [int]$Port = 44100,
    [string]$Bind = "0.0.0.0",
    [int]$Frames = 240,
    [ValidateSet("mute","zero","both")] [string]$MuteMode = "mute",
    [switch]$NoMute,
    [switch]$TestTone,
    [int]$Freq = 440,
    [switch]$Help
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Resolve-Path "$ScriptDir\.."
Push-Location $ProjectRoot

$BinDir = Join-Path $ProjectRoot "bin"
$Exe = Join-Path $BinDir "audiorouter_server.exe"

function Show-Help {
    Write-Host @"
AudioRouter Windows Server Launcher (PowerShell)

Usage: .\scripts\start_server.ps1 [options]

Options:
  -Port INT          UDP port (default 44100)
  -Bind STRING       Bind IP (default 0.0.0.0)
  -Frames INT        Frames per packet (default 240 = 5ms)
  -MuteMode mute|zero|both  Mute method (default mute)
  -NoMute            Do not mute PC speakers on client connect (debug)
  -TestTone          Generate test sine tone instead of WASAPI capture
  -Freq INT          Test tone frequency Hz (default 440)
  -Help              Show this help

Examples:
  .\scripts\start_server.ps1
  .\scripts\start_server.ps1 -Port 44100 -MuteMode both
  .\scripts\start_server.ps1 -NoMute -TestTone -Freq 1000

This script lists network interfaces, prompts for settings if not given via
parameters, and launches audiorouter_server.exe.

For fully non-interactive:
  .\bin\audiorouter_server.exe -p 44100 -b 0.0.0.0 --mute-mode both
"@
}

if ($Help) { Show-Help; Pop-Location; exit 0 }

if (!(Test-Path $Exe)) {
    Write-Host "[ERROR] Server binary not found at $Exe" -ForegroundColor Red
    Write-Host "        Build with one of:"
    Write-Host "          .\scripts\build_server_msvc.bat"
    Write-Host "          .\scripts\build_server_mingw.bat"
    Write-Host "          .\scripts\build_all.bat"
    Pop-Location
    exit 1
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " AudioRouter - Windows Server Launcher (PowerShell)"
Write-Host " Binary: $Exe"
Write-Host "============================================================"

Write-Host "`n[1/3] Available network interfaces (via audiorouter_server --list-if):"
try {
    & $Exe --list-if
    if ($LASTEXITCODE -ne 0) { throw "list-if failed" }
} catch {
    Write-Host "[WARN] Could not run $Exe --list-if, falling back to Get-NetIPAddress:" -ForegroundColor Yellow
    try {
        Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -ne "127.0.0.1" } | Format-Table IPAddress, InterfaceAlias, PrefixOrigin -AutoSize | Out-String | Write-Host
    } catch {
        Write-Host "  Fallback ipconfig:"
        ipconfig | Select-String "IPv4|Wireless|Ethernet" | ForEach-Object { Write-Host "  $_" }
    }
}

Write-Host "`nCurrent settings:"
Write-Host "  Port       : $Port"
Write-Host "  Bind IP    : $Bind"
Write-Host "  Frames/pkt : $Frames"
Write-Host "  Mute mode  : $MuteMode"
Write-Host "  No-mute    : $NoMute"
Write-Host "  Test tone  : $TestTone"

# Interactive prompts if running in interactive console and no explicit args passed via pipeline
if ([Environment]::UserInteractive -and $Host.Name -match "ConsoleHost") {
    $userPort = Read-Host "Enter UDP port [$Port]"
    if ($userPort) { $Port = [int]$userPort }

    $userMute = Read-Host "Mute mode: mute/zero/both/no [$MuteMode]"
    if ($userMute) {
        if ($userMute -eq "no") { $NoMute = $true }
        elseif ($userMute -in @("mute","zero","both")) { $MuteMode = $userMute }
    }

    $userTone = Read-Host "Use test tone? y/N"
    if ($userTone -match "^y") { $TestTone = $true }
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Starting server on ${Bind}:$Port  Mute: $MuteMode  NoMute: $NoMute  TestTone: $TestTone"
Write-Host " Press Ctrl+C to stop"
Write-Host "============================================================"

$argsList = @("-p", $Port, "-b", $Bind, "-f", $Frames, "--mute-mode", $MuteMode)
if ($NoMute) { $argsList += "--no-mute" }
if ($TestTone) { $argsList += @("--test-tone", "--freq", $Freq) }

Write-Host "[INFO] Command: $Exe $($argsList -join ' ')" -ForegroundColor Green
Write-Host ""

# Firewall hint
Write-Host "[INFO] Ensure Windows Firewall allows UDP $Port inbound" -ForegroundColor Yellow
Write-Host "       If client cannot connect, run as admin: New-NetFirewallRule -DisplayName AudioRouter -Direction Inbound -Protocol UDP -LocalPort $Port -Action Allow" -ForegroundColor DarkGray

& $Exe @argsList
$exitCode = $LASTEXITCODE

Write-Host ""
Write-Host "Server exited with code $exitCode"
Write-Host "If PC speakers did not restore, check logs above."

Pop-Location
exit $exitCode
