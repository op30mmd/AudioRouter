# AudioRouter

High-performance, low-latency audio routing engine that streams Windows system audio output to an Android device over UDP and plays it directly through the phone speakers via ALSA in Termux (root required).

When an Android client connects to the Windows server, the server saves the current master volume and mute state, silences the local PC output, and routes the captured system audio stream to the Android device. When the client disconnects, times out, or the connection is lost, the PC speaker state is restored automatically.

## Overview

AudioRouter is designed for mobile hotspot and local Wi-Fi environments where traditional Bluetooth audio or network audio solutions introduce excessive latency or require additional drivers. It uses:

- **Server (Windows):** WASAPI Loopback capture for bit-perfect system audio capture without third-party virtual drivers, plus `IAudioEndpointVolume` control for automatic muting.
- **Client (Android / Linux):** Native ALSA playback with multiple backends, including dynamic `libasound.so` loading, direct kernel ioctl to `/dev/snd/pcmC*D*p`, and Qualcomm AGM FIFO playback via the vendor `agmplay` binary. Playback path bypasses higher-level Android audio frameworks.
- **Transport:** Custom binary UDP protocol with monotonic sequence numbers, microsecond timestamps, QoS marking (DSCP/TOS), adaptive jitter buffering, packet loss concealment, and bidirectional heartbeat to maintain NAT bindings on Wi-Fi hotspots.

Typical end-to-end latency of 35-80 ms depending on Wi-Fi conditions and jitter buffer configuration, with packet sizes tuned to avoid IP fragmentation (default 240 frames = 5 ms, ~960 bytes at 48 kHz stereo S16LE).

## Features

- **WASAPI Loopback Capture:** Direct capture of Windows system mix via Windows Audio Session API. No virtual audio driver required. Optional synthetic test tone generator for debugging.
- **Automatic PC Speaker Control:** Saves previous mute/volume state on client connect and restores on disconnect. Supports three mute strategies: endpoint mute, volume zero, or both.
- **Low-Latency UDP Transport:** Custom protocol (magic `0x41554452` / "AUDR", version 1) with packed binary headers. MTU-safe payload sizing (default 1400 byte safe MTU).
- **Resilient Wi-Fi Handling:** Sequence-based reordering, adaptive jitter buffer with startup prefill and hole-free-run gate, loss concealment, clock drift compensation, NAT keep-alive, and RTT measurement.
- **Network Discovery and Enumeration:** Interface listing (`--list-if`), broadcast-based discovery probes, gateway hotspot probing, and support for binding to a specific interface to bypass VPN tunnels on Android.
- **Multiple Android Playback Backends:**
  - `libasound` dynamic loading via `dlopen` (requires `alsa-lib` in Termux)
  - Direct ALSA kernel driver (`ioctl` on `/dev/snd/pcmC*D*p`) with zero dynamic dependencies
  - AGM FIFO player that streams PCM through Qualcomm's `agmplay` vendor binary (for devices with AGM audio HAL)
  - Dummy player for benchmarking and CI
- **Root and Permission Handling:** Automatic `chmod 666 /dev/snd/*` attempt, root verification helper, and mixer routing scripts for Qualcomm Snapdragon and MediaTek platforms via `tinymix`.
- **Safety and Quality:** C++23 codebase with span-based APIs, `expected<T,E>` error handling, strict header validation, overflow checks, hardening flags (`-fstack-protector-strong`, `_FORTIFY_SOURCE=2`, PIE), and sanitizer support.
- **Cross-Platform Build:** CMake and Makefile support for MSVC, MinGW, Linux, and Termux (Clang). CI builds host binaries, Android ARM64 client (NDK or aarch64 cross-compiler fallback), and Windows x64 server.

## Architecture

```
+-------------------------------------------------------------+
|                 Windows PC (Server / Sender)                |
|                                                             |
|   +---------------------+        +----------------------+   |
|   |   WASAPI Loopback   | -----> |   IAudioEndpoint     |   |
|   |   Audio Capture     |        |   Volume Mute        |   |
|   +----------+----------+        +----------------------+   |
|              |                                              |
|              v                                              |
|   +---------------------+                                   |
|   |  Packetizer / QoS   |                                   |
|   |  Low-Latency UDP    |                                   |
|   +----------+----------+                                   |
+--------------|----------------------------------------------+
               |
               |  UDP Packets (Port 44100)
               |  [DISCOVERY_REQ/RESP, CONNECT_REQ/ACK/NAK,
               |   AUDIO_DATA, HEARTBEAT_PING/PONG,
               |   DISCONNECT_REQ/ACK, CONTROL_CMD]
               v
+-------------------------------------------------------------+
|               Android Device (Client / Receiver)            |
|                   Termux (Root Privileges)                  |
|                                                             |
|   +---------------------+                                   |
|   |  UDP Receiver &     |                                   |
|   |  NAT Heartbeat Ping |                                   |
|   +----------+----------+                                   |
|              |                                              |
|              v                                              |
|   +---------------------+                                   |
|   | Adaptive Jitter     | (Reordering, PLC, Drift Comp.)    |
|   | Buffer & Ring Queue |                                   |
|   +----------+----------+                                   |
|              |                                              |
|              v                                              |
|   +---------------------+        +----------------------+   |
|   |  ALSA Audio Player  | -----> |  ALSA Hardware       |   |
|   |  libasound /        |        |  /dev/snd/pcmC0D0p / |   |
|   |  direct ioctl / AGM |        |  CODEC_DMA-LPAIF_*   |   |
|   +---------------------+        +----------------------+   |
+-------------------------------------------------------------+
```

Server responsibilities:
- `AudioRouterServer`: UDP socket management, client session state, watchdog for client timeout (default 8000 ms), network receive thread.
- `WASAPI Capture` / `Dummy Capture`: Audio source implementation behind `IAudioCapture` interface.
- `AudioEndpointControl`: COM wrapper around `IAudioEndpointVolume` for mute/volume state save/restore.

Client responsibilities:
- `AudioRouterClient`: Handshake, discovery, receive thread, playback thread, heartbeat thread, device-open supervisor thread with timeout.
- `JitterBuffer`: Fixed-capacity slot array (256 slots), startup prefill, jitter estimation from transit time variation, continuity tracking to avoid repeated underruns.
- `IAudioPlayer` implementations: `AlsaPlayer`, `DirectAlsaPlayer`, `AgmFifoPlayer`, `DummyPlayer`.

## Protocol

All packets start with a packed `CommonHeader` (24 bytes):

```cpp
struct CommonHeader {
    uint32_t magic;         // 0x41554452 = "AUDR"
    uint8_t  version;       // 1
    uint8_t  msg_type;      // MsgType enum
    uint16_t flags;         // FLAG_DISCONTINUITY, FLAG_SILENCE, FLAG_KEYFRAME, FLAG_LAST_PACKET
    uint32_t seq_num;
    uint64_t timestamp_us;  // monotonic sender time
    uint32_t payload_size;
};
```

Message types:
- `DISCOVERY_REQ` (0x01) / `DISCOVERY_RESP` (0x02): Broadcast discovery.
- `CONNECT_REQ` (0x10) / `CONNECT_ACK` (0x11) / `CONNECT_NAK` (0x12): Session negotiation including client name, preferred sample rate/channels/format, target latency.
- `DISCONNECT_REQ` (0x20) / `DISCONNECT_ACK` (0x21): Graceful teardown.
- `AUDIO_DATA` (0x30): Contains `AudioPacketHeader` (36 bytes total, extends `CommonHeader` with sample_rate, channels, format, num_frames) followed by PCM payload (default S16LE stereo 48 kHz).
- `HEARTBEAT_PING` (0x40) / `HEARTBEAT_PONG` (0x41): Bidirectional keep-alive carrying `HeartbeatPayload` with original timestamp, buffer level, packet counters, underrun/overrun stats for RTT calculation.
- `CONTROL_CMD` (0x50): Remote control commands (e.g., PC mute/volume control from Android).

Validation includes magic/version checks, message type allow-list, payload size bounds (MAX_UDP 65507, safe MTU 1400 * 8), flag mask validation, and for audio packets, sample rate (0-192 kHz), channel count (0-32), frame count (0-8192), and payload size consistency.

Audio configuration uses `AudioConfig` with sample rate, channels, format (`PCM_S16LE`, `PCM_FLOAT32LE`, `PCM_S24LE`, `PCM_S32LE`), and frames per packet, plus helpers for byte size calculation and validation.

## Requirements

### Windows Server
- Windows 10/11 with WASAPI support.
- Visual Studio 2022 (17) or 2019 (16) with C++23 support for MSVC build, or MinGW-w64 with `g++` (GCC 13+).
- CMake 3.20+ recommended (required for MSVC build scripts).
- Firewall rule allowing UDP inbound on configured port (default 44100).

### Android Client
- Android device with root access (Magisk / KernelSU / `su`).
- Termux from F-Droid.
- Packages: `clang`, `make`, `alsa-lib`, `alsa-utils`, `sudo` (installed by `termux_setup.sh`).
- Kernel must expose `/dev/snd/` PCM nodes. For Qualcomm AGM devices, vendor binary `agmplay` should be present in `/vendor/bin` or PATH.
- For `tinymix` mixer setup: `tinyalsa` / `alsa-utils` (provides `tinymix`).

### Linux (Testing / Development)
- GCC 13+ or Clang 16+ with C++23 support.
- `libasound2-dev` optional for libasound backend.
- `make` or `cmake` 3.20+.

## Quick Start

### Network Topology Options

**Scenario A: PC Connected to Android Wi-Fi Hotspot (Recommended for lowest latency)**

1. Enable Personal Hotspot / Wi-Fi Hotspot on Android.
2. Connect Windows PC to Android hotspot.
3. On Windows, run the server (via launcher or directly) and note the PC IP from interface list (e.g., `192.168.43.45`).
4. On Android in Termux, connect via PC IP.

**Scenario B: Android Connected to Windows Mobile Hotspot**

1. Enable Mobile Hotspot in Windows Settings (Settings > Network > Mobile Hotspot).
2. Connect Android phone to PC hotspot. Gateway is typically `192.168.137.1`.
3. On Android in Termux, connect to `192.168.137.1`.

### Building and Running - Quick Commands

**Windows ( easiest - unified ):**
```bat
scripts\build_all.bat
scripts\start_server.bat
:: or PowerShell:
powershell -ExecutionPolicy Bypass -File scripts\start_server.ps1
```

**Linux / Termux ( easiest - unified ):**
```bash
./scripts/build_all.sh
./scripts/build_all.sh --tests
./scripts/check_env.sh
```

**Termux full flow:**
```bash
git clone https://github.com/op30mmd/AudioRouter.git
cd AudioRouter
chmod +x scripts/*.sh
./scripts/termux_setup.sh
./scripts/android_diagnose.sh   # optional but recommended
./scripts/termux_run.sh 192.168.43.45
```

## Building - Detailed

### Windows Server - Unified Build

```bat
:: Clean, configure with VS2022 (fallback VS2019), build all targets
scripts\build_all.bat
scripts\build_all.bat --clean --server-only
scripts\build_all.bat --debug
```

Artifacts are copied to `bin\audiorouter_server.exe`, `bin\audiorouter_client.exe`, `bin\audiorouter_tests.exe`.

### Windows Server - MSVC (Focused)

```bat
scripts\build_server_msvc.bat
scripts\build_server_msvc.bat --clean
scripts\build_server_msvc.bat --debug
bin\audiorouter_server.exe --list-if
bin\audiorouter_server.exe --help
```

The script checks for `cmake`, creates `build_msvc/`, tries VS2022 then VS2019 generators, builds `Release` by default, and copies the executable to `bin\`.

### Windows Server - MinGW

```bat
scripts\build_server_mingw.bat
scripts\build_server_mingw.bat --clean --debug
bin\audiorouter_server.exe
```

Updated to C++23, with hardening flags, and proper g++ version check.

### Linux / Android - Unified Build

```bash
# Build server + client + tests
./scripts/build_all.sh
./scripts/build_all.sh --clean
./scripts/build_all.sh --server-only
./scripts/build_all.sh --client-only --compiler clang++
./scripts/build_all.sh --tests-only
./scripts/build_all.sh --cmake --verbose

# Direct Makefile still available
make all
make server
make client
make test
make sanitize DEBUG=1 SANITIZE=address,undefined
```

Detects `clang++` or `g++`, verifies C++23 support, and uses Makefile with hardening flags. With `--cmake`, uses CMake build system.

### Linux / Android - Client Only

```bash
./scripts/build_client.sh
./scripts/build_client.sh --clean --tests --verbose
./scripts/build_client.sh --compiler clang++ --debug
```

Builds `bin/audiorouter_client` only, with optional tests and sanitizer support.

### CMake Build (Cross-Platform)

```bash
# Linux / general
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel

# Windows MSVC x64 explicitly
cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target audiorouter_server

# Android ARM64 via NDK
cmake -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_LATEST_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android --target audiorouter_client --parallel
```

### Termux Setup and Diagnostics

In Termux:

```bash
git clone https://github.com/op30mmd/AudioRouter.git
cd AudioRouter
chmod +x scripts/*.sh

# Install dependencies and build
./scripts/termux_setup.sh
./scripts/termux_setup.sh --verbose --force
./scripts/termux_setup.sh --no-build   # only install packages
./scripts/termux_setup.sh --tests

# Check environment
./scripts/check_env.sh

# Detailed Android audio diagnostics (requires root)
su
./scripts/android_diagnose.sh
./scripts/android_mixer_setup.sh
./scripts/android_mixer_setup.sh --list --dry-run
```

`termux_setup.sh` now:
- Detects Termux environment, checks for `pkg`
- Installs `clang`, `make`, `alsa-lib`, `alsa-utils`, `sudo`, `termux-tools`, `pkg-config`
- Verifies compiler C++23 support
- Builds client unless `--no-build`
- Supports `--force` to rebuild existing binary

## Scripts Reference

All helper scripts are in `scripts/` and designed to be user-friendly with `--help` flags and professional logging.

| Script | Platform | Purpose |
|--------|----------|---------|
| `build_all.sh` | Linux, Termux, macOS | Unified build: server, client, tests. Supports `--clean`, `--debug`, `--sanitize`, `--server-only`, `--client-only`, `--tests-only`, `--cmake`, `--compiler`, `--verbose`. Detects compiler and verifies C++23. |
| `build_all.bat` | Windows | Unified Windows build via CMake. Tries VS2022 then VS2019. Supports `--clean`, `--debug`, `--server-only`, `--client-only`, `--tests-only`. Copies artifacts to `bin\`. |
| `build_server_msvc.bat` | Windows | Focused MSVC build using CMake. Robust generator fallback, config selection (`Release`/`Debug`), clean support, helpful error messages if `cmake` missing. |
| `build_server_mingw.bat` | Windows | MinGW direct `g++` build with C++23 and hardening flags. Checks g++ version, supports `--clean`, `--debug`. Updated from C++17 to C++23. |
| `build_client.sh` | Linux, Termux | Client-only build wrapper. Auto-detects `clang++`/`g++`, checks C++23, supports `--clean`, `--tests`, `--sanitize`, `--debug`, `--verbose`, `--compiler`. |
| `termux_setup.sh` | Termux | Termux environment setup. Updates `pkg`, installs all dependencies, builds client if missing. Options: `--no-build`, `--force`, `--verbose`, `--tests`. Improved from original with better detection and logging. |
| `termux_run.sh` | Termux | User-friendly client runner. Auto-discovers or builds binary, fixes `/dev/snd/*` permissions via `su`, sets `LD_LIBRARY_PATH` for vendor libs and clears `LD_PRELOAD` to fix `libtermux-exec.so` linking error for `agmplay` children. Supports full client options: `-s IP`, `-p PORT`, `-d DEVICE`, `-l LATENCY`, `-b IFACE`, `--discover`, `--verbose`, `--list-devices`. Detects `tun0` VPN and `wlan0 DOWN` and warns with `-b auto` bypass suggestion. Interactive IP prompt if needed. Updated with Bengal-friendly examples. |
| `check_env.sh` | Linux, Termux, Android | Environment diagnostics. Checks: OS/arch, compiler C++23 support, cmake/make, alsa-lib, Termux/Root, `/dev/snd` nodes and permissions, `tinymix`/`agmplay` (now with clean env `env -i LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib` to avoid Termux `libtermux-exec.so` false failure), network interfaces with `tun0` VPN and `wlan0 DOWN` detection plus bypass suggestion. Color-coded OK/WARN/FAIL. Updated with Bengal awareness. |
| `android_diagnose.sh` | Android (root) | Comprehensive Android audio diagnostics. Checks root, `tinymix`/`agmplay` via clean vendor env (fixes libtermux-exec linking error), `/dev/snd` PCM nodes with tailored analysis for Bengal 7-node no-D0p case, `/proc/asound/cards` with Bengal detection, SoC from `/proc/cpuinfo`, mixer dump with expanded filter (RX_MACRO, RX MIX, RDAC etc.) and manual routing suggestions for Bengal, libasound presence, network with `tun0` VPN + `wlan0 DOWN` tailored recommendations, and prints actionable steps. Most detailed script - use first for debugging. |
| `android_mixer_setup.sh` | Android (root) | ALSA speaker routing helper. Must run as root. Fixes permissions, detects SoC (Qualcomm/Bengal SD662/680 via `bengal-idp-snd-card`, MediaTek, or forced via `--qualcomm`/`--bengal`/`--mediatek`), lists cards, applies common routing plus Bengal-specific routing (RX_MACRO RX0 MUX->AIF1_PB, RX MIX->RX0, INT MIX, RDAC Switches, volumes) with `--list`, `--dry-run`, `--qualcomm`/`--bengal`. Improved with more controls and detailed logging. Now includes tailored Bengal workaround from real diagnostic. |
| `start_server.bat` | Windows | Interactive server launcher. Lists interfaces via `audiorouter_server --list-if` (fallback to `ipconfig`), prompts for port and mute mode, then runs server with chosen args. Supports `-p PORT -b IP` non-interactively. Also prints firewall hint. |
| `start_server.ps1` | Windows | PowerShell version of interactive launcher. Uses `Get-NetIPAddress` fallback, prompts for port/mute/test tone, shows firewall rule creation hint (`New-NetFirewallRule`), supports parameters `-Port`, `-Bind`, `-MuteMode`, `-NoMute`, `-TestTone`, `-Freq`. |

Make all shell scripts executable:

```bash
chmod +x scripts/*.sh
```

On Windows, run `.bat` files directly in CMD, or `.ps1` via PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\start_server.ps1 -Port 44100 -MuteMode both
```

## Usage

### Server CLI

```
Usage: audiorouter_server [options]
  -p, --port <port>         UDP listening port (default: 44100)
  -b, --bind <ip>           Bind IP address (default: 0.0.0.0)
  -r, --rate <hz>           Sample rate in Hz (default: 48000)
  -f, --frames <count>      Audio frames per UDP packet (default: 240 = 5ms)
      --no-mute             Do not mute PC speaker when client connects (debug)
      --mute-mode <mode>    Mute method: 'mute' (default), 'zero' (volume 0), 'both'
  -t, --test-tone           Generate test sine tone instead of WASAPI loopback
      --freq <hz>           Test tone frequency in Hz (default: 440.0)
  -l, --list-if             List all available network interfaces and exit
  -v, --verbose             Enable debug logging
  -h, --help                Show this help message
```

Server prints banner, binds UDP, enters listening state, then streaming state after `CONNECT_REQ` / `CONNECT_ACK`. Periodic stats logged every 5 seconds during streaming (packets sent, bytes, client-reported loss). Clean shutdown restores volume.

**Via launcher scripts (recommended for Windows beginners):**

```bat
:: Interactive prompts for port/mute mode, lists interfaces
scripts\start_server.bat
scripts\start_server.bat --port 44100 --bind 0.0.0.0

:: PowerShell version
powershell -ExecutionPolicy Bypass -File scripts\start_server.ps1
powershell -ExecutionPolicy Bypass -File scripts\start_server.ps1 -Port 44100 -MuteMode both
```

**Direct examples:**

```bat
audiorouter_server.exe -p 44100 -b 0.0.0.0 -f 240 --mute-mode both
audiorouter_server.exe -l
audiorouter_server.exe -t --freq 1000 --no-mute
```

### Client CLI

```
Usage: audiorouter_client [options]
  -s, --server <ip>         Windows PC Server IP address
  -p, --port <port>         Server UDP port (default: 44100)
  -d, --device <dev>        ALSA device (default: 'default')
                              'hw:0,0', 'direct:/dev/snd/pcmC0D0p',
                              'agm' or 'agm:<backend>' for Qualcomm AGM
  -l, --latency <ms>        Target jitter buffer latency in ms (default: 35)
  -b, --bind <iface>        Pin UDP socket to interface (bypass Android VPN):
                              'auto' = detect physical NIC (e.g., wlan0), or specify 'wlan0'
      --discover            Auto-discover server on local hotspot subnet
      --dummy               Use dummy audio player (benchmark/testing)
      --list-devices        List detected ALSA and kernel PCM devices and exit
  -v, --verbose             Enable debug logging
  -h, --help                Show this help message
```

**Via Termux runner (recommended for Android):**

```bash
# Interactive IP prompt if not provided
./scripts/termux_run.sh
./scripts/termux_run.sh 192.168.43.45
./scripts/termux_run.sh -s 192.168.43.45 -p 44100 -d direct:/dev/snd/pcmC0D0p -l 35 -b auto --verbose
./scripts/termux_run.sh --discover --verbose
./scripts/termux_run.sh --list-devices

# On Bengal (bengal-idp-snd-card, SD662/680) default is now AGM named pipe (agmplay FIFO)
# So you can just run without -d and it will auto-select AGM:
./scripts/termux_run.sh 192.168.43.45 -b auto -v
# Which is equivalent to:
./scripts/termux_run.sh -s 192.168.43.45 -d agm:CODEC_DMA-LPAIF_RXTX-RX-1 -b auto -v
```

**Direct examples:**

```bash
# List devices (diagnostic, no server needed)
./bin/audiorouter_client --list-devices

# Default libasound path
./bin/audiorouter_client -s 192.168.43.45

# Direct kernel driver (most reliable for rooted devices)
./bin/audiorouter_client -s 192.168.43.45 -d direct:/dev/snd/pcmC0D0p

# AGM backend via vendor agmplay
./bin/audiorouter_client -s 192.168.43.45 -d agm
./bin/audiorouter_client -s 192.168.43.45 -d agm:CODEC_DMA-LPAIF_RXTX-RX-1

# Increase jitter buffer for lossy Wi-Fi
./bin/audiorouter_client -s 192.168.43.45 -l 80

# Bypass VPN tunnel that forces traffic over tun0
./bin/audiorouter_client -s 192.168.43.45 -b auto
./bin/audiorouter_client -s 192.168.43.45 -b wlan0

# Auto-discovery on hotspot subnet
./bin/audiorouter_client --discover -p 44100
```

Client logs include RTT, current buffer duration, average jitter, packet loss, underruns, and frames played.

## Android Audio Backends

The client selects playback backend based on device string and runtime fallback:

1. **AlsaPlayer (libasound dynamic)** - `default`, `hw:0,0`, etc.
   - Loads `libasound.so` via `dlopen` at runtime (installed via `pkg install alsa-lib`).
   - Falls back to `DirectAlsaPlayer` if libasound open fails.
   - `get_available_devices()` enumerates ALSA cards.

2. **DirectAlsaPlayer (direct kernel ioctl)** - `direct:/dev/snd/pcmC0D0p` or explicit PCM node.
   - Direct `open/ioctl/write` on `/dev/snd/pcmC*D*p` nodes.
   - No external library dependency, most reliable with root.
   - Handles permission fix (`chmod 666 /dev/snd/*`) in Termux runner scripts.
   - Tries candidate list: `pcmC0D0p`, `pcmC0D1p`, `pcmC0D2p` etc. if plain `direct:` specified.

3. **AgmFifoPlayer (Qualcomm AGM)** - `agm` or `agm:<backend>`
   - Used on devices with AGM audio HAL (card 100, device 100).
   - Creates a named pipe (FIFO) and spawns vendor `agmplay` subprocess via `fork()` + `execve()` with clean environment (`LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib:/system/lib64:/system/lib`, no `LD_PRELOAD`) to fix Termux `libtermux-exec.so not accessible` linker namespace issue. Previously running `/vendor/bin/agmplay` directly in Termux shell failed with `CANNOT LINK EXECUTABLE agmplay: library libtermux-exec.so...`. Fixed in this update by clean env spawn.
   - Client writes 44-byte WAV header + S16LE PCM into FIFO; `agmplay` owns AGM graph registration via HIDL binder and ADSP session management.
   - Input is downmixed to mono because AGM speaker graph is mono-oriented.
   - Example backends for Bengal (SD662/680) observed in diagnostic: `CODEC_DMA-LPAIF_RXTX-RX-0`, `RX-1`, `RX-2`, `RX-3` (controls 164-167 in tinymix). Try each if default fails.
   - Manual clean-env test:
     ```
     su
     env -i LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib:/system/lib64:/system/lib PATH=/vendor/bin:/system/bin /vendor/bin/agmplay --help
     ```

4. **DummyPlayer** - `--dummy`
   - Simulates audio sink for benchmarking and headless CI, calculates buffer level without hardware.

### Mixer Routing

On many Qualcomm and MediaTek devices, the audio HAL powers down mixer paths when no Android Java MediaPlayer is active. If no audio is audible despite successful streaming:

```bash
su
chmod 666 /dev/snd/*
./scripts/android_mixer_setup.sh
./scripts/android_mixer_setup.sh --list
./scripts/android_mixer_setup.sh --qualcomm --dry-run

# Manual controls example:
tinymix "Speaker Function" "On"
tinymix "RX_CDC_DMA_RX_0 Audio Mixer MultiMedia1" 1
tinymix "PRI_MI2S_RX Audio Mixer MultiMedia1" 1
tinymix "RX1 Digital Volume" 84
tinymix "RX2 Digital Volume" 84
```

`android_mixer_setup.sh` now supports `--list` (only list controls), `--dry-run` (show what would be done), `--qualcomm`/`--mediatek` forcing, and more comprehensive controls (including QUAT, PRI, SEC, TERT MI2S, SpkrLeft/Right, Ext Spk).

For full diagnostics:

```bash
su
./scripts/android_diagnose.sh
./scripts/check_env.sh
```

## Networking and Configuration

- **Port:** Default 44100 UDP. Both server and client must agree.
- **QoS:** `UdpSocket::set_qos_priority()` sets DSCP/TOS for low-latency audio (IP_TOS / traffic class).
- **Buffer Sizes:** Socket receive/send buffers increased for bursty Wi-Fi.
- **Socket Binding:** Supports binding to `0.0.0.0` or specific IP. On Android, `SO_BINDTODEVICE` support to pin socket to physical interface (`wlan0`) and bypass VPN interface (`tun0`). Requires root / `CAP_NET_RAW`, handled via `bind_to_interface()` and `pick_physical_interface()`.
- **NAT Keep-Alive:** Heartbeat every ~500-1000 ms maintains Wi-Fi hotspot NAT table entry and computes RTT from timestamp echo.
- **MTU Tuning:** Default frames per packet 240 at 48 kHz stereo S16LE = 960 byte payload + 36 byte header ~996 byte UDP payload = MTU-safe. Configurable via `-f/--frames`.

## Testing

Unit test suite covers protocol serialization, ring buffer, jitter buffer with packet loss concealment and reordering, socket address parsing, audio conversion/downmixing, thread safety, memory safety, and type safety.

**Via unified script:**

```bash
./scripts/build_all.sh --tests-only
./scripts/build_all.sh --tests-only --sanitize address
```

**Via Makefile:**

```bash
make test
make sanitize DEBUG=1 SANITIZE=address,undefined
```

**Via CMake / CTest:**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

**Via check script:**

```bash
./scripts/check_env.sh   # verifies toolchain before building
```

Expected output:

```
=========================================
 Running AudioRouter Comprehensive Tests
=========================================

[ RUN      ] Protocol & Packet Serialization
[       OK ] Protocol & Packet Serialization
[ RUN      ] Audio Ring Buffer
[       OK ] Audio Ring Buffer
[ RUN      ] Adaptive Jitter Buffer & PLC
[       OK ] Adaptive Jitter Buffer & PLC
[ RUN      ] Socket & Network Address Parsing
[       OK ] Socket & Network Address Parsing
[ RUN      ] Audio Converter & Downmixing
[       OK ] Audio Converter & Downmixing

=========================================
 Test Summary: 5 Passed, 0 Failed
=========================================
```

Tests are also executed in CI on Ubuntu.

## Project Structure

```
AudioRouter/
├── CMakeLists.txt                 # Root CMake (C++23, subdirs for client/server/tests)
├── Makefile                       # Universal Makefile (server, client, tests, sanitizers)
├── README.md
├── LICENSE                        # Apache 2.0
├── .github/workflows/ci.yml       # CI: Linux host tests + Android ARM64 cross-build + Windows x64 + release
├── src/
│   ├── common/                    # Shared networking, protocol, DSP utilities
│   │   ├── protocol.hpp           # Packed binary protocol, validation, factory helpers
│   │   ├── socket_util.hpp/.cpp   # Cross-platform UDP socket wrapper, iface enumeration
│   │   ├── audio_types.hpp        # AudioConfig, AudioSampleFormat, AudioConverter
│   │   ├── ring_buffer.hpp        # Thread-safe lock-free circular buffer
│   │   ├── logger.hpp             # Leveled colored timestamp logger
│   │   ├── time_util.hpp          # Monotonic clocks, microsecond timers
│   │   ├── expected_compat.hpp    # std::expected polyfill
│   │   ├── span_compat.hpp        # std::span polyfill
│   │   └── thread_compat.hpp      # JThread / threading compat
│   ├── server/                    # Windows PC Audio Server
│   │   ├── CMakeLists.txt
│   │   ├── main.cpp               # CLI, signal handling, stats loop
│   │   ├── server.hpp/.cpp        # Streaming engine, handshake, watchdog, chunking
│   │   ├── wasapi_capture.hpp/.cpp# WASAPI Loopback capture (Windows)
│   │   ├── audio_endpoint_control.hpp/.cpp # IAudioEndpointVolume mute/volume save/restore
│   │   ├── dummy_capture.hpp/.cpp # Synthetic sine tone generator
│   │   └── audio_capture.hpp      # IAudioCapture interface
│   └── client/                    # Android Termux ALSA Client (also Linux)
│       ├── CMakeLists.txt
│       ├── main.cpp               # CLI, signal handling, monitoring loop
│       ├── client.hpp/.cpp        # Receiver engine, discovery, heartbeat, device supervisor
│       ├── alsa_player.hpp/.cpp   # Dynamic libasound loader with direct fallback
│       ├── direct_alsa.hpp/.cpp   # Direct /dev/snd/pcm* ioctl driver
│       ├── agm_fifo_player.hpp/.cpp # Qualcomm AGM via agmplay subprocess + FIFO
│       ├── dummy_player.hpp/.cpp  # Simulated audio sink
│       ├── jitter_buffer.hpp/.cpp # Adaptive jitter buffer with PLC
│       ├── android_helpers.hpp/.cpp # Root check, /dev/snd enumeration, proc parsing
│       └── audio_player.hpp       # IAudioPlayer interface
├── tests/                         # Unit test suite
│   ├── CMakeLists.txt
│   ├── test_main.cpp
│   ├── test_protocol.cpp
│   ├── test_ring_buffer.cpp
│   ├── test_jitter_buffer.cpp
│   ├── test_socket.cpp
│   ├── test_conversion.cpp
│   ├── test_thread_safety.cpp
│   ├── test_type_safety.cpp
│   └── test_memory_safety.cpp
└── scripts/                       # Setup, build, run, and diagnostic scripts
    ├── build_all.sh               # Unified build for Linux/Termux/macOS (server+client+tests, --clean --debug --sanitize etc.)
    ├── build_all.bat              # Unified Windows build via CMake (VS2022/2019 fallback)
    ├── build_server_msvc.bat      # Focused MSVC build (professional rewrite, C++23 check)
    ├── build_server_mingw.bat     # Focused MinGW build (updated to C++23, hardening flags)
    ├── build_client.sh            # Client-only build (auto-detects clang++/g++, C++23 verification)
    ├── termux_setup.sh            # Termux env setup (pkg update, deps, build - improved with --force --verbose)
    ├── termux_run.sh              # Termux runner (binary discovery, perm fix, LD_LIBRARY_PATH, full CLI --discover --bind etc.)
    ├── check_env.sh               # NEW: Environment diagnostics (compiler, cmake, ALSA libs, root, /dev/snd, network, binaries)
    ├── android_diagnose.sh        # NEW: Deep Android audio diagnostics (PCM nodes, cards, mixer dump, AGM, libasound, VPN)
    ├── android_mixer_setup.sh     # Mixer routing helper (now with --list, --dry-run, --qualcomm/--mediatek, more controls)
    ├── start_server.bat           # NEW: Windows interactive server launcher (lists ifaces, prompts port/mute, firewall hint)
    └── start_server.ps1           # NEW: PowerShell launcher (Get-NetIPAddress fallback, -Port -MuteMode -TestTone params)
```

## Troubleshooting

**No audio on Android but client shows streaming:**
- Run full diagnostics (as root):
  ```bash
  su
  ./scripts/check_env.sh
  ./scripts/android_diagnose.sh
  ./scripts/android_mixer_setup.sh --qualcomm   # or --bengal for SD662/680
  ```
- Verify root: `id` should show `uid=0`. Run `su` first.
- Check `/dev/snd` permissions: `ls -l /dev/snd/` and run `chmod 666 /dev/snd/*` or `./scripts/android_mixer_setup.sh`.
- Try detailed mixer modes: `./scripts/android_mixer_setup.sh --list`, `--qualcomm`, `--bengal`, `--dry-run`.
- Try alternative ALSA devices: `direct:/dev/snd/pcmC0D0p`, `hw:0,0`, `agm`, or specific nodes like `direct:/dev/snd/pcmC0D1p`.
- List devices: `./bin/audiorouter_client --list-devices` or `./scripts/termux_run.sh --list-devices`.
- Check if another app holds PCM device exclusively: `stop audioserver` (test, then `start audioserver` to restore Android audio).

**Bengal device (bengal-idp-snd-card, SD662/SD680, WCD937x, 7 playback nodes, no pcmC0D0p):**
- This device family (diagnostic example from real device) has no `pcmC0D0p` playback node. `DirectAlsaPlayer` automatically tries fallback candidates, but you should explicitly test each:
  ```bash
  su
  ./bin/audiorouter_client --list-devices
  ./bin/audiorouter_client -s <PC_IP> -d direct:/dev/snd/pcmC0D1p -b auto -v
  ./bin/audiorouter_client -s <PC_IP> -d direct:/dev/snd/pcmC0D5p -b auto -v
  ./bin/audiorouter_client -s <PC_IP> -d direct:/dev/snd/pcmC0D6p -b auto -v
  ./bin/audiorouter_client -s <PC_IP> -d direct:/dev/snd/pcmC0D14p -b auto -v
  ./bin/audiorouter_client -s <PC_IP> -d agm:CODEC_DMA-LPAIF_RXTX-RX-1 -b auto -v
  ./bin/audiorouter_client -s <PC_IP> -d agm:CODEC_DMA-LPAIF_RXTX-RX-0 -b auto -v
  ```
- Mixer routing for Bengal requires more than generic Qualcomm controls. Use `./scripts/android_mixer_setup.sh --bengal` which applies:
  ```
  RX_MACRO RX0/RX1/RX2 MUX -> AIF1_PB/AIF2_PB
  RX MIX TX0/1/2 MUX -> RX0/1/2
  RX INT0_1 MIX1 INP0 -> RX0 etc.
  HPHL_RDAC, HPHR_RDAC, AUX_RDAC, EAR_RDAC Switch -> 1
  RX_RX0/RX1/RX2 Mix Digital Volume -> 84
  ```
- Manual test:
  ```bash
  su
  tinymix "RX_MACRO RX0 MUX" "AIF1_PB"
  tinymix "RX MIX TX0 MUX" "RX0"
  tinymix "RX INT0_1 MIX1 INP0" "RX0"
  tinymix "HPHL_RDAC Switch" 1
  tinymix "AUX_RDAC Switch" 1
  tinyplay /system/media/audio/ui/camera_click.ogg  # or test.wav
  ```
- The ALSA card has 184 controls (seen via `tinymix`). If `android_mixer_setup.sh` doesn't enable speaker, run `./scripts/android_diagnose.sh` which prints tailored suggestions for Bengal.

**VPN tun0 active (e.g., 10.183.115.4/32, MTU 1300) and wlan0 DOWN:**
- The diagnostic you posted shows `tun0: POINTOPOINT,UP,LOWER_UP mtu 1300` and `wlan0: BROADCAST,MULTICAST mtu 1500 DOWN`. Means VPN app active and WiFi/hotspot off, connected via mobile data `rmnet_data0`/`rmnet_data2`.
- AudioRouter needs local hotspot, not mobile data:
  - **Scenario A (recommended):** Android hotspot ON, PC connected to phone (192.168.43.x). `wlan0` or `swlan0` should be UP as AP.
  - **Scenario B:** Windows hotspot ON, Android connected to PC (192.168.137.x). `wlan0` should be UP with 192.168.137.x.
- VPN routes all traffic through tunnel, adds latency and causes MTU fragmentation (MTU 1300 vs default server ~996 byte packets still OK but RTT high). Solutions:
  - Disable VPN temporarily for testing
  - Or keep VPN but bypass local traffic: use client bind option `-b auto` or `-b wlan0` to force packet out physical WiFi interface:
    ```bash
    ./scripts/termux_run.sh 192.168.43.45 -- -b auto -v
    ./bin/audiorouter_client -s 192.168.43.45 -d direct:/dev/snd/pcmC0D1p -b auto -v
    ```
  - Some VPN apps support split-tunnel: exclude local subnet 192.168.0.0/16
- `check_env.sh` and `android_diagnose.sh` now detect `tun0` and `wlan0 DOWN` and warn with remediation steps (fixed in this update).

**agmplay linking error `CANNOT LINK EXECUTABLE agmplay: library libtermux-exec.so not accessible`:**
- This occurs when running `/vendor/bin/agmplay` directly inside Termux shell because Termux sets `LD_PRELOAD=libtermux-exec.so` which lives in Termux private data dir, not accessible to vendor linker namespace (default). The error is expected if you run agmplay from Termux shell directly.
- **Fix applied in this update:**
  - `AgmFifoPlayer` now spawns agmplay via `fork()` + `execve()` with clean `envp` containing only `LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib:/system/lib64:/system/lib` and `PATH=/vendor/bin:/system/bin`, no `LD_PRELOAD`. This clears `libtermux-exec.so` and vendor binary loads correctly.
  - `android_diagnose.sh` and `check_env.sh` now test agmplay with `env -i LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib:/system/lib64:/system/lib PATH=/vendor/bin:/system/bin /vendor/bin/agmplay --help` to avoid false failure.
  - `termux_run.sh` now explicitly sets `LD_PRELOAD=""` in its `su -c` command and adds vendor lib paths.
- To manually test AGM backend after fix:
  ```bash
  su
  env -i LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib:/system/lib64:/system/lib PATH=/vendor/bin:/system/bin /vendor/bin/agmplay --help
  ./bin/audiorouter_client -s <PC_IP> -d agm:CODEC_DMA-LPAIF_RXTX-RX-1 -b auto -v
  ./bin/audiorouter_client -s <PC_IP> -d agm:CODEC_DMA-LPAIF_RXTX-RX-0 -b auto -v
  ```
- If AGM still fails, fallback to direct ALSA nodes listed above.

**Windows server build fails:**
- Run `./scripts/check_env.sh` on Linux or `scripts/build_all.bat --clean` on Windows.
- Ensure C++23 capable compiler: VS 2022 17.6+ or MinGW GCC 13+. Run `g++ --version` or check Visual Studio Installer - C++ Desktop Workload.
- Ensure CMake 3.20+ in PATH: `cmake --version`.
- For MSVC, run from x64 Native Tools Prompt or use `scripts/build_server_msvc.bat` which auto-fallbacks VS2022->VS2019.
- Check Windows SDK headers for WASAPI (`audioclient.h`).

**Client cannot connect / discovery finds nothing:**
- Run `./scripts/check_env.sh` and `./scripts/android_diagnose.sh` to verify network.
- Verify both devices on same hotspot subnet (not mobile data).
- Check firewall on Windows: allow UDP 44100 inbound. Launcher hints: `New-NetFirewallRule -DisplayName AudioRouter -Direction Inbound -Protocol UDP -LocalPort 44100 -Action Allow`.
- Use explicit IP (`-s 192.168.x.x`) instead of `--discover`.
- List server interfaces: `audiorouter_server -l` or `scripts/start_server.bat` shows them.
- List client routing: `ip addr` in Termux, or `check_env.sh` output.
- VPN on Android can hijack routing: use `-b auto` or `-b wlan0` via `./scripts/termux_run.sh -b auto`.
- If `wlan0` DOWN (as in your diagnostic), enable hotspot/WiFi first.

**High latency or frequent underruns:**
- Check diagnostics RTT: `./scripts/termux_run.sh --verbose` shows RTT.
- Reduce jitter buffer for good Wi-Fi (`-l 35`), increase (`-l 80` or higher) for lossy hotspot or mobile data+VPN.
- Reduce frames per packet server side (`-f 240` = 5 ms) to lower serialization delay, but increases packet rate.
- Use 5 GHz hotspot if device supports it (your device shows `wlan0` DOWN, so currently on mobile data - switch to hotspot).
- Check VPN interference via `check_env.sh` tun0 detection.

**Android audio is distorted or channels swapped:**
- Verify sample rate matches (server default 48 kHz). Use `-r 48000`.
- Direct ALSA may need period size adjustment (handled internally). Try libasound backend as fallback.
- Run `./scripts/android_diagnose.sh` to see mixer volume controls (RX Digital Volume 84 is safe, >90 may clip on WCD937x).
- On Bengal, distorted may mean wrong RDAC: try only `AUX_RDAC Switch 1` without HPH RDAC, or vice-versa.

## CI/CD and Releases

GitHub Actions workflow `.github/workflows/ci.yml`:

- **Linux Build & Test:** Ubuntu latest, installs `aarch64-linux-gnu` toolchain, builds host binaries with CMake C++23, runs CTest.
- **Android ARM64 Client:** If NDK present, builds via `android.toolchain.cmake` for `arm64-v8a` API 24. Otherwise cross-compiles with `aarch64-linux-gnu-g++`. Packages client binary plus setup/run/mixer scripts and docs into artifact `audiorouter_client_android_arm64`. Staged as `audiorouter-<tag>-android-termux-arm64.tar.gz`.
- **Windows Server:** Windows latest, CMake MSVC x64, builds `audiorouter_server.exe`, packages into `audiorouter_server_windows_x64` artifact and `audiorouter-<tag>-windows-x64.zip`.
- **Release:** On push to `main` (not PRs) and manual dispatch, generates tag `v1.0.<run_number>` and publishes GitHub Release with both archives, using `softprops/action-gh-release`.

New helper scripts are included in release archives:
- `check_env.sh` for environment verification
- `android_diagnose.sh` for deep Android diagnostics
- `start_server.bat` / `start_server.ps1` for easy Windows launching
- `build_all.sh` / `build_all.bat` for unified builds

## Security Considerations

- Root on Android is required for direct ALSA access; Termux running as root bypasses Android sandboxing. Only run trusted binaries.
- UDP audio is unencrypted. Do not use on untrusted public networks without VPN (or use `-b` binding carefully).
- `SO_BINDTODEVICE` pinning requires `CAP_NET_RAW`; using `su` satisfies this but implies full device access.
- Server intentionally mutes local speakers; ensure `--no-mute` or `--mute-mode` is understood when debugging on shared systems. Launcher scripts prompt for mute mode.
- Diagnostic scripts (`check_env.sh`, `android_diagnose.sh`) only read system state and do not modify audio routing unless explicitly running `android_mixer_setup.sh`.

## License

Licensed under Apache License 2.0. See `LICENSE` for details.

```
Copyright 2024 AudioRouter Contributors
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
```
