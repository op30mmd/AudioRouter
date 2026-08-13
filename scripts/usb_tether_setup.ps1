# usb_tether_setup.ps1 - AudioRouter USB tethering: one-time PC-side setup.
#
# When the phone enables USB tethering, Windows gets a DHCP lease from the
# phone - INCLUDING a default gateway - and can start routing the PC's
# internet traffic through the phone, which is exactly what you do NOT want.
#
# This script runs once while tethering is active and:
#   1. keeps the IP the phone's DHCP just handed out (same subnet, so the
#      link keeps working),
#   2. makes that IP STATIC with NO default gateway (the config persists per
#      adapter, so every future tethering session comes up on-link only),
#   3. pins the interface route metric high so Windows never prefers the USB
#      link for internet.
#
# Result: the phone is reachable over the cable, discovery/audio work, and
# your PC's internet routing is untouched.
#
# Run from the .bat wrapper (elevated): scripts\usb_tether_setup.bat

$ErrorActionPreference = 'Stop'

$adapter = Get-NetAdapter | Where-Object {
    $_.InterfaceDescription -match 'Remote NDIS|RNDIS'
} | Select-Object -First 1

if (-not $adapter) {
    Write-Host "No USB tethering (RNDIS) adapter found."
    Write-Host "Enable USB tethering on the phone (or run './scripts/termux_run.sh --tether'),"
    Write-Host "then run this script again while the link is up."
    exit 1
}
Write-Host "Found USB tethering adapter: $($adapter.Name) ($($adapter.InterfaceDescription))"

# Wait for the phone's DHCP lease (the PC configures itself within a few
# seconds of the link coming up). If this adapter was already set up before,
# there is no lease - fall back to the existing static address.
$lease = $null
for ($i = 0; $i -lt 15; $i++) {
    $lease = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.PrefixOrigin -ne 'Manual' -and $_.IPAddress -ne '0.0.0.0' } |
        Select-Object -First 1
    if ($lease) { break }
    Start-Sleep -Seconds 1
}

if ($lease) {
    $ip     = $lease.IPAddress
    $prefix = $lease.PrefixLength
} else {
    # Already configured: reuse the existing (static) address.
    $existing = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -ne '0.0.0.0' } |
        Select-Object -First 1
    if (-not $existing) {
        Write-Host "The USB adapter has no address yet - is tethering on? Try again in a moment."
        exit 1
    }
    $ip     = $existing.IPAddress
    $prefix = $existing.PrefixLength
    Write-Host "Adapter already configured statically ($ip/$prefix); skipping the address step."
}

function PrefixToMask([int]$p) {
    $bytes = New-Object byte[] 4
    for ($i = 0; $i -lt 4; $i++) {
        $bits = [Math]::Min(8, [Math]::Max(0, $p - $i * 8))
        $bytes[$i] = [byte]((0xFF -shl (8 - $bits)) -band 0xFF)
    }
    return (($bytes | ForEach-Object { $_.ToString() }) -join '.')
}
$mask = PrefixToMask $prefix

if ($lease) {
    Write-Host "USB link IP: $ip/$prefix - making it static with NO default gateway..."
    # netsh wants name="<adapter>" as a single quoted token.
    $nameTok = 'name="' + $adapter.Name + '"'
    & netsh interface ip set address $nameTok static $ip $mask none | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "netsh failed to set the static address."
        exit 1
    }
}

# Belt and braces: drop any default route through the USB NIC and pin its
# metric so Windows never picks it for internet. The metric is cosmetic
# (there is no gateway left to route through), so failures here are not fatal.
Get-NetRoute -InterfaceIndex $adapter.ifIndex -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
    Remove-NetRoute -Confirm:$false
Set-NetIPInterface -InterfaceIndex $adapter.ifIndex -InterfaceMetric 9000 -ErrorAction SilentlyContinue | Out-Null

# Report.
$routes = Get-NetRoute -InterfaceIndex $adapter.ifIndex -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue
if ($routes) {
    Write-Host "WARNING: the USB adapter still has a default route:" $routes.DestinationPrefix
} else {
    Write-Host "OK: no default route through the USB adapter - internet routing is untouched."
}
Write-Host ""
Write-Host "USB tethering is configured for AudioRouter. This PC's USB IP: $ip"
Write-Host "  - Server:  bin\audiorouter_server.exe   (answers discovery on this interface)"
Write-Host "  - Phone:   ./scripts/termux_run.sh --tether -d agm"
Write-Host "  - AAudio:  ./scripts/termux_run.sh -s $ip -d aaudio"
Write-Host ""
Write-Host "This is a one-time setup: Windows keeps the static config for this adapter."
