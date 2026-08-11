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
- Visual Studio 2022 (17) or 2019 (16) with C++23 support for MSVC build, or MinGW-w64 with `g++`.
- Firewall rule allowing UDP inbound on configured port (default 44100).

### Android Client
- Android device with root access (Magisk / KernelSU / `su`).
- Termux from F-Droid.
- Packages: `clang`, `make`, `alsa-lib`, `alsa-utils`, `sudo` (installed by `termux_setup.sh`).
- Kernel must expose `/dev/snd/` PCM nodes. For Qualcomm AGM devices, vendor binary `agmplay` should be present in `/vendor/bin` or PATH.
- For `tinymix` mixer setup: root package `tinyalsa` or vendor `tinymix`.

### Linux (Testing / Development)
- GCC 13+ or Clang 16+ with C++23 support.
- `libasound2-dev` optional for libasound backend.
- `make` or `cmake` 3.20+.

## Quick Start

### Network Topology Options

**Scenario A: PC Connected to Android Wi-Fi Hotspot (Recommended for lowest latency)**

1. Enable Personal Hotspot / Wi-Fi Hotspot on Android.
2. Connect Windows PC to Android hotspot.
3. On Windows, run `audiorouter_server.exe` or `bin/audiorouter_server -l` to list interface IPs. Note the PC IP (e.g., `192.168.43.45`).
4. On Android in Termux, connect via PC IP.

**Scenario B: Android Connected to Windows Mobile Hotspot**

1. Enable Mobile Hotspot in Windows Settings (Settings > Network > Mobile Hotspot).
2. Connect Android phone to PC hotspot. Gateway is typically `192.168.137.1`.
3. On Android in Termux, connect to `192.168.137.1`.

### Building and Running

#### Windows Server - MSVC (Recommended)

```bat
scripts\build_server_msvc.bat
bin\audiorouter_server.exe
```

The script generates `build_msvc/` with Visual Studio 2022 (fallback to 2019) and copies the executable to `bin\audiorouter_server.exe`.

#### Windows Server - MinGW

```bat
scripts\build_server_mingw.bat
bin\audiorouter_server.exe
```

#### Linux / Android Client - Makefile

```bash
# Server and client and tests
make all
# Server only
make server
# Client only (Linux / Termux)
make client
# Run unit tests
make test
```

Build system auto-detects platform (Windows vs Linux) and links `ws2_32`, `iphlpapi`, `avrt`, `ole32` on Windows, `pthread`/`dl` elsewhere. Binaries output to `bin/`.

#### CMake Build

```bash
# Linux / general
cmake -B build -DCMAKE_BUILD_TYPE=Release
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

#### Termux Setup

In Termux:

```bash
git clone https://github.com/op30mmd/AudioRouter.git
cd AudioRouter
chmod +x scripts/*.sh
./scripts/termux_setup.sh
```

`termux_setup.sh` updates package lists, installs `clang`, `make`, `alsa-lib`, `alsa-utils`, `sudo`, and compiles `bin/audiorouter_client` if not present.

Then run with root:

```bash
su
./bin/audiorouter_client -s 192.168.43.45 -p 44100
```

Or automated helper (handles `chmod 666 /dev/snd/*` and `LD_LIBRARY_PATH` setup for AGM vendor libraries):

```bash
./scripts/termux_run.sh 192.168.43.45
./scripts/termux_run.sh 192.168.43.45 44100 --verbose
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

Example:

```bat
audiorouter_server.exe -p 44100 -b 0.0.0.0 -f 240 --mute-mode both
audiorouter_server.exe -l
audiorouter_server.exe -t --freq 1000
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

Examples:

```bash
# List devices
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
   - Creates a named pipe (FIFO) and spawns vendor `agmplay` subprocess. Client writes 44-byte WAV header + S16LE PCM into FIFO; `agmplay` owns AGM graph registration via HIDL binder and ADSP session management.
   - Input is downmixed to mono because AGM speaker graph is mono-oriented.
   - Example backends: `CODEC_DMA-LPAIF_RXTX-RX-1`, auto-detected default.

4. **DummyPlayer** - `--dummy`
   - Simulates audio sink for benchmarking and headless CI, calculates buffer level without hardware.

### Mixer Routing

On many Qualcomm and MediaTek devices, the audio HAL powers down mixer paths when no Android Java MediaPlayer is active. If no audio is audible despite successful streaming:

```bash
su
chmod 666 /dev/snd/*
./scripts/android_mixer_setup.sh
# Manual controls example:
tinymix "Speaker Function" "On"
tinymix "RX_CDC_DMA_RX_0 Audio Mixer MultiMedia1" 1
tinymix "PRI_MI2S_RX Audio Mixer MultiMedia1" 1
tinymix "RX1 Digital Volume" 84
tinymix "RX2 Digital Volume" 84
```

`android_mixer_setup.sh` performs permission fix, dumps `/proc/asound/cards`, and attempts common routing controls.

## Networking and Configuration

- **Port:** Default 44100 UDP. Both server and client must agree.
- **QoS:** `UdpSocket::set_qos_priority()` sets DSCP/TOS for low-latency audio (IP_TOS / traffic class).
- **Buffer Sizes:** Socket receive/send buffers increased for bursty Wi-Fi.
- **Socket Binding:** Supports binding to `0.0.0.0` or specific IP. On Android, `SO_BINDTODEVICE` support to pin socket to physical interface (`wlan0`) and bypass VPN interface (`tun0`). Requires root / `CAP_NET_RAW`, handled via `bind_to_interface()` and `pick_physical_interface()`.
- **NAT Keep-Alive:** Heartbeat every ~500-1000 ms maintains Wi-Fi hotspot NAT table entry and computes RTT from timestamp echo.
- **MTU Tuning:** Default frames per packet 240 at 48 kHz stereo S16LE = 960 byte payload + 36 byte header ~996 byte UDP payload = MTU-safe. Configurable via `-f/--frames`.

## Testing

Unit test suite covers protocol serialization, ring buffer, jitter buffer with packet loss concealment and reordering, socket address parsing, audio conversion/downmixing, thread safety, memory safety, and type safety.

Makefile:

```bash
make test
# Sanitizer build
make sanitize DEBUG=1 SANITIZE=address,undefined
```

CMake / CTest:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
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
└── scripts/
    ├── build_server_msvc.bat      # MSVC x64 build
    ├── build_server_mingw.bat     # MinGW build
    ├── build_client.sh            # Linux / Termux client build
    ├── termux_setup.sh            # Termux env setup (pkg install + build)
    ├── termux_run.sh              # Termux root runner (chmod, LD_LIBRARY_PATH, connect)
    └── android_mixer_setup.sh     # ALSA mixer routing helper for speaker path
```

## Troubleshooting

**No audio on Android but client shows streaming:**
- Verify root: `id` should show `uid=0`. Run `su` first.
- Check `/dev/snd` permissions: `ls -l /dev/snd/` and run `chmod 666 /dev/snd/*`.
- Run `android_mixer_setup.sh` or `tinymix` controls listed above.
- Try alternative device: `direct:/dev/snd/pcmC0D0p`, `hw:0,0`, `agm`.
- List devices: `./bin/audiorouter_client --list-devices`.
- Check if another app holds PCM device exclusively.

**Windows server build fails:**
- Ensure C++23 capable compiler. For MSVC, VS 2022 17.6+. For MinGW, GCC 13+.
- For MSVC, run from x64 Native Tools Prompt or rely on `build_server_msvc.bat` which attempts VS2022 then VS2019.

**Client cannot connect / discovery finds nothing:**
- Verify both devices on same hotspot subnet.
- Check firewall on Windows: allow UDP 44100 inbound (or your custom port).
- Use explicit IP (`-s 192.168.x.x`) instead of `--discover`.
- List server interfaces (`audiorouter_server -l`) and client routing (`ip addr` in Termux).
- VPN on Android can hijack routing: use `-b auto` or `-b wlan0`.

**High latency or frequent underruns:**
- Reduce jitter buffer (`-l 35`) for good Wi-Fi, increase (`-l 80` or higher) for lossy hotspot.
- Reduce frames per packet (`-f 240` = 5 ms) to lower serialization delay, but increases packet rate.
- Check RTT logs; >50 ms RTT suggests hotspot congestion.
- Use 5 GHz hotspot if device supports it.

**Android audio is distorted or channels swapped:**
- Verify sample rate matches (server default 48 kHz). Use `-r 48000`.
- Direct ALSA may need period size adjustment (handled internally). Try libasound backend as fallback.

## CI/CD and Releases

GitHub Actions workflow `.github/workflows/ci.yml`:

- **Linux Build & Test:** Ubuntu latest, installs `aarch64-linux-gnu` toolchain, builds host binaries with CMake C++23, runs CTest.
- **Android ARM64 Client:** If NDK present, builds via `android.toolchain.cmake` for `arm64-v8a` API 24. Otherwise cross-compiles with `aarch64-linux-gnu-g++`. Packages client binary plus setup/run/mixer scripts and docs into artifact `audiorouter_client_android_arm64`. Staged as `audiorouter-<tag>-android-termux-arm64.tar.gz`.
- **Windows Server:** Windows latest, CMake MSVC x64, builds `audiorouter_server.exe`, packages into `audiorouter_server_windows_x64` artifact and `audiorouter-<tag>-windows-x64.zip`.
- **Release:** On push to `main` (not PRs) and manual dispatch, generates tag `v1.0.<run_number>` and publishes GitHub Release with both archives, using `softprops/action-gh-release`.

## Security Considerations

- Root on Android is required for direct ALSA access; Termux running as root bypasses Android sandboxing. Only run trusted binaries.
- UDP audio is unencrypted. Do not use on untrusted public networks without VPN.
- `SO_BINDTODEVICE` pinning requires `CAP_NET_RAW`; using `su` satisfies this but implies full device access.
- Server intentionally mutes local speakers; ensure `--no-mute` or `--mute-mode` is understood when debugging on shared systems.

## License

Licensed under Apache License 2.0. See `LICENSE` for details.

```
Copyright 2024 AudioRouter Contributors
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
```
