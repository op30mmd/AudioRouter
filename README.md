# AudioRouter 🎵

High-performance, ultra-low-latency C++ audio routing engine that streams PC audio output from **Windows** to an **Android** device over **UDP**, playing directly through the phone speakers using **ALSA** in **Termux with root privileges**.

When the Android client connects to the Windows server via `IP:PORT`, the server automatically makes the PC speakers go quiet (mutes/silences master output) and routes the system audio stream directly to the Android speakers. When the client disconnects or times out, the PC speaker volume is immediately restored to its original state.

---

## 🌟 Key Features

- **⚡ Ultra-Low Latency & High Performance**:
  - Direct Windows Audio Session API (**WASAPI Loopback Capture**) for bit-perfect audio capture without third-party drivers.
  - Native **ALSA (Advanced Linux Sound Architecture)** playback on Android with zero intermediate framework layers (AudioFlinger bypassed).
  - Packet chunking tuned to MTU (5ms - 10ms frame packets, ~960 bytes) preventing IP fragmentation over Wi-Fi.

- **🔇 Automatic PC Speaker Silencing**:
  - When the Android client connects, the server saves the PC's volume and mute state, then makes the PC speakers quiet (`IAudioEndpointVolume`).
  - Seamlessly restores PC volume when the client exits, disconnects, or drops off Wi-Fi.

- **📶 Built for Mobile Hotspots & Wi-Fi Jitter**:
  - Custom binary protocol with monotonic sequence numbering, microsecond timestamps, and packet loss concealment (PLC).
  - **Adaptive Jitter Buffer** smoothing out bursty Wi-Fi packet arrivals, reordering out-of-order UDP packets, and handling clock drift.
  - Bidirectional UDP keep-alive heartbeats to keep Wi-Fi NAT routing tables open and compute real-time Round-Trip Time (RTT).
  - Automatic network interface enumeration and auto-discovery probes across hotspot subnets.

- **📱 Rooted Android & Termux Support**:
  - Dual ALSA backend: supports both dynamic `libasound.so` (`pkg install alsa-lib`) and direct kernel ioctl driver (`/dev/snd/pcmC*D*p`) with zero external dynamic dependencies.
  - Mixer setup scripts (`tinymix`) for Qualcomm Snapdragon and MediaTek Android hardware.

- **🔊 AAudio Backend — No Root Required**:
  - Plays through Android's native AAudio API (NDK `<aaudio/AAudio.h>`, API 26+): the stream is owned by AudioFlinger / the audio HAL, so it works on **stock, non-rooted devices** — no `/dev/snd`, no ALSA, no `tinymix`.
  - PCM is pumped through a 1 MB FIFO into a low-latency AAudio stream in ~20 ms chunks with automatic underrun/pause recovery and stream recreation on routing changes (headphone unplug, Bluetooth switch).
  - Three profiles: `aaudio` (media + low latency), `aaudio:deep` (power saver / deep buffer), `aaudio:voip` (call-style routing).
  - Includes the standalone `stream_daemon` tool: a continuous real-time AAudio FIFO daemon (`/data/local/tmp/audio_pipe`) that any process can feed raw S16 stereo PCM into.

---

## 🏗️ Architecture & Protocol

```
+-------------------------------------------------------------+
|                 Windows PC (Server / Sender)                |
|                                                             |
|   +---------------------+        +----------------------+   |
|   |   WASAPI Loopback   | -----> |   IAudioEndpoint     |   |
|   |   Audio Capture     |        |   Volume Mute / Quiet|   |
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
               |  [CONNECT / ACK / AUDIO_DATA / HEARTBEAT]
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
|   | Adaptive Jitter     | (Loss Concealment & Reordering)   |
|   | Buffer & Ring Queue |                                   |
|   +----------+----------+                                   |
|              |                                              |
|              v                                              |
|   +---------------------+        +----------------------+   |
|   |  ALSA Audio Player  | -----> |  /dev/snd/pcmC0D0p   |   |
|   |  (libasound / ioctl)|        |  Phone Speakers      |   |
|   +---------------------+        +----------------------+   |
+-------------------------------------------------------------+
```

### Packet Structure
All packets use packed binary headers:
- `MAGIC` (`0x41554452` = "AUDR")
- `version` (uint8_t)
- `msg_type` (`CONNECT_REQ`, `CONNECT_ACK`, `AUDIO_DATA`, `HEARTBEAT_PING`, `HEARTBEAT_PONG`, `DISCONNECT_REQ`, `DISCONNECT_ACK`, `DISCOVERY_REQ`, `DISCOVERY_RESP`)
- `seq_num` (uint32_t)
- `timestamp_us` (uint64_t)
- `payload_size` (uint32_t) + PCM Payload (16-bit signed integer stereo, 48kHz).

---

## 🚀 Quick Start

### 1. Hotspot Scenarios

#### **Scenario A: PC Connected to Android Wi-Fi Hotspot** (Recommended)
1. Turn on **Personal Hotspot / Wi-Fi Hotspot** on your Android phone.
2. Connect your Windows PC to the Android hotspot.
3. Start the server on Windows (see below). The server will display your PC's IP address (e.g. `192.168.43.45`).
4. On Android in Termux, connect using the PC's IP.

#### **Scenario B: Android Connected to Windows Mobile Hotspot**
1. Turn on **Mobile Hotspot** in Windows Settings.
2. Connect your Android phone to the PC's hotspot.
3. The PC's gateway IP is typically `192.168.137.1`.
4. On Android in Termux, connect to `192.168.137.1`.

---

### 2. Building & Running the Windows Server

#### Option A: Build with CMake & Visual Studio (MSVC)
```bat
scripts\build_server_msvc.bat
```

#### Option B: Build with MinGW (g++)
```bat
scripts\build_server_mingw.bat
```

#### Running the Server
```bat
bin\audiorouter_server.exe
```

Server Options:
```
Usage: audiorouter_server [options]
  -p, --port <port>         UDP listening port (default: 44100)
  -b, --bind <ip>           Bind IP address (default: 0.0.0.0)
  -r, --rate <hz>           Sample rate in Hz (default: 48000)
  -f, --frames <count>      Audio frames per UDP packet (default: 240 = 5ms)
      --no-mute             Keep PC speaker unmuted during streaming (debug)
      --mute-mode <mode>    'mute' (default), 'zero' (volume 0), or 'both'
  -t, --test-tone           Generate test sine tone instead of loopback
  -l, --list-if             List network interface IPs and exit
```

---

### 3. Building & Running the Android Client (Termux)

#### Setup Termux
Inside Termux on your Android phone:
```bash
# 1. Clone repository
git clone https://github.com/op30mmd/AudioRouter.git
cd AudioRouter

# 2. Run automated setup script (installs clang, make, alsa-lib, sudo)
chmod +x scripts/*.sh
./scripts/termux_setup.sh
```

#### Run with Root Privileges
```bash
# Request root
su

# Start client connecting to Windows Server IP
./bin/audiorouter_client -s 192.168.43.45 -p 44100
```
Or use the automated helper:
```bash
./scripts/termux_run.sh 192.168.43.45
```

Client Options:
```
Usage: audiorouter_client [options]
  -s, --server <ip>         Windows PC Server IP address
  -p, --port <port>         Server UDP port (default: 44100)
  -d, --device <dev>        Audio device (default: 'default'):
                              ALSA:  'default', 'hw:0,0', 'plughw:0,0'
                              Direct kernel: 'direct:/dev/snd/pcmC0D0p' or any '/dev/snd/...'
                              Qualcomm AGM: 'agm' or 'agm:<backend>'
                              AAudio (NO ROOT needed): 'aaudio', 'aaudio:deep', 'aaudio:voip'
  -l, --latency <ms>        Target Jitter Buffer latency in ms (default: 35ms)
      --discover            Auto-discover server on local hotspot subnet
      --dummy               Use simulated audio player (benchmarking/testing)
      --list-devices        List detected ALSA, kernel PCM nodes and AAudio availability
```

#### No-Root Quick Start (AAudio)

```bash
# On a stock (non-rooted) device, Android 8.0+ — no 'su', no ALSA setup:
./bin/audiorouter_client -s 192.168.43.45 -d aaudio

# With the interface bind (needs root for -b auto; AAudio still runs as the
# Termux user because the client drops privileges for it):
./scripts/termux_run.sh -s 192.168.43.45 -d aaudio -b auto
```

---

## 🛠️ Android ALSA & Speaker Routing Details

Android's Linux kernel provides raw ALSA hardware devices located in `/dev/snd/`:
- `/dev/snd/pcmC0D0p`: Card 0, Device 0 Playback (Primary Speaker / DAC)
- `/dev/snd/pcmC0D1p`: Deep buffer playback / secondary stream

### Permissions & Mixer Routing
When running in Termux with root (`su`):
1. **Device Permissions**: AudioRouter automatically applies `chmod 666 /dev/snd/*` on launch.
2. **Audio Hardware Unmuting**: On Qualcomm Snapdragon and MediaTek devices, Android's audio HAL might sleep mixer paths when no Android Java MediaPlayer is active. If no sound is audible, run:
   ```bash
   su
   ./scripts/android_mixer_setup.sh
   ```
   Or set mixer controls directly with `tinymix`:
   ```bash
   tinymix "Speaker Function" "On"
   tinymix "RX_CDC_DMA_RX_0 Audio Mixer MultiMedia1" 1
   ```

---

## 🔊 AAudio Playback (No Root Required)

AAudio is the NDK's native audio API (Android 8.0+). Unlike the ALSA / direct
`/dev/snd` / AGM backends, an AAudio stream is owned by **AudioFlinger and the
audio HAL**, so playback goes through the normal Android audio policy —
speaker, Bluetooth, USB — **without root and without touching the mixer**.
This makes it the best option for stock, non-rooted devices, or whenever
another app's audio should keep working alongside AudioRouter.

> ⚠️ **AAudio must run as a normal (non-root) user.** Android's audio policy
> blocks the AAudio/MMAP data path for **UID 0 (root)**: root processes have
> no app attribution token, so the stream opens and reaches STARTED but never
> actually renders (every `AAudioStream_write` returns 0). AAudio is designed
> to run as the standard Termux app user (`u0_a...`). The client handles this
> automatically: when started as root it **forks a helper process that drops
> to the Termux app user and owns the AAudio stream**, while the main process
> keeps root for the socket binding (`-b auto`) and the AGM/ALSA fallback.
>
> ```bash
> # -b auto needs root (SO_BINDTODEVICE); AAudio runs as the Termux user:
> ./scripts/termux_run.sh -s 192.168.43.45 -d aaudio -b auto
> #   -> termux_run.sh starts the client via su; the client keeps root for
> #      the socket, and the AAudio stream lives in the forked helper as
> #      u0_a... (PCM flows into it over the FIFO).
> # No root needed at all (no -b):
> ./bin/audiorouter_client -s 192.168.43.45 -d aaudio
> ```
>
> If no Termux app user is available, the helper fails fast and the client
> falls back to the root-capable backends (AGM/ALSA) instead of silence.

### How it works

- The client writes PCM into a FIFO under the Termux user's home —
  `$HOME/audiorouter_aaudio.fifo` (falling back to
  `/data/local/tmp/audiorouter_aaudio.fifo` when `$HOME` is unset). The
  home is writable by the non-root app user, `/data/local/tmp` is not.
  The FIFO is opened `O_RDWR` with the capacity expanded to 1 MB ≈ 5 s.
- A background pump thread reads the FIFO (never EOF) and feeds
  `AAudioStream_write()` in ~20 ms chunks with a 200 ms timeout — the same
  engine as the standalone `stream_daemon`.
- The FIFO provides natural back-pressure: if the network delivers faster than
  the device consumes, the pipe fills and the jitter buffer drains instead of
  dropping packets.
- **Underrun/pause recovery**: if the stream is PAUSED/STOPPED or a write
  fails, the stream is restarted automatically. If it is DISCONNECTED
  (headphones unplugged, Bluetooth reconnect, USB audio removed), the stream
  is recreated from scratch (rate-limited to once per second).

### Device profiles

| `-d` value         | Profile                                                     |
|--------------------|-------------------------------------------------------------|
| `aaudio`           | `USAGE_MEDIA` + `LOW_LATENCY` — default, lowest latency      |
| `aaudio:deep`      | `USAGE_MEDIA` + `PERFORMANCE_MODE_NONE` — power saver / deep buffer, slightly higher latency, very stable |
| `aaudio:voip`      | `USAGE_VOICE_COMMUNICATION` + `LOW_LATENCY` — call-style routing (useful if the stock policy ducks your stream) |

### Building

The AAudio backend is compiled in automatically when the toolchain targets
Android API 26+ and `libaaudio` is linkable:

- **Termux**: `pkg install ndk-sysroot` (provides the Android platform stub
  libraries), then `make client` as usual. The Makefile probes for
  `-laaudio`; on plain Linux/CI hosts the player compiles as a harmless stub.
- **NDK / CMake**: set `-DANDROID_PLATFORM=android-26` (or newer); the
  `aaudio` library is linked and the `stream_daemon` tool is built too.
- If the probe finds no `libaaudio`, the client still builds — `-d aaudio`
  simply fails fast and falls back to the other backends.

### Standalone stream_daemon tool

`src/tools/stream_daemon.cpp` is the standalone version of the same engine:
it exposes a FIFO (default `$HOME/audio_pipe`, falling back to
`/data/local/tmp/audio_pipe` when `$HOME` is unset; 48 kHz stereo S16) and
plays whatever is written into it through AAudio. Useful when another
process owns the PCM and you do not want the full client. Like the client
backend, it must run as a normal (non-root) user — it prints a warning if
started as root.

```bash
# Build with the NDK (or: ./scripts/build_stream_daemon.sh)
clang++ -O2 -Wno-unavailable-declarations -target aarch64-linux-android30 \
    -o stream_daemon src/tools/stream_daemon.cpp -laaudio -lm

# Run it (no su!), then feed it raw interleaved S16 stereo PCM:
./stream_daemon
cat audio.raw > ~/audio_pipe
```

---

## 🧪 Testing & Verification

The project includes an automated unit test suite covering protocol packing, ring buffers, adaptive jitter buffers with loss concealment, and UDP networking:

```bash
make test
```

Expected Output:
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

---

## 📂 Project Structure

```
AudioRouter/
├── CMakeLists.txt                 # Root CMake build configuration
├── Makefile                       # Universal GNU Makefile (builds client, server, tests)
├── README.md                      # Comprehensive documentation
├── LICENSE                        # Apache 2.0 License
├── src/
│   ├── common/                    # Shared networking, protocol, and DSP
│   │   ├── protocol.hpp           # Binary packet formats and message types
│   │   ├── socket_util.hpp/.cpp   # Cross-platform UDP socket wrapper
│   │   ├── audio_types.hpp        # Format descriptors and SIMD sample converters
│   │   ├── ring_buffer.hpp        # Thread-safe lock-free circular audio buffer
│   │   ├── logger.hpp             # Leveled colored timestamp logging
│   │   └── time_util.hpp          # Monotonic clocks & microsecond timers
│   ├── server/                    # Windows PC Audio Server
│   │   ├── CMakeLists.txt
│   │   ├── main.cpp               # Server CLI and signal handling
│   │   ├── server.hpp/.cpp        # Server streaming engine & keep-alive
│   │   ├── wasapi_capture.hpp/.cpp# Windows WASAPI Loopback Capture
│   │   ├── audio_endpoint_control.hpp/.cpp # IAudioEndpointVolume PC speaker mute
│   │   ├── dummy_capture.hpp/.cpp # Synthetic tone generator (Linux/fallback)
│   │   └── audio_capture.hpp      # Audio capture interface
│   ├── client/                    # Android Termux ALSA Client
│   │   ├── CMakeLists.txt
│   │   ├── main.cpp               # Client CLI and signal handling
│   │   ├── client.hpp/.cpp        # Client receiver engine & NAT heartbeat
│   │   ├── alsa_player.hpp/.cpp   # ALSA player (dynamic libasound)
│   │   ├── direct_alsa.hpp/.cpp   # Direct kernel /dev/snd/pcmC0D0p ioctl driver
│   │   ├── agm_fifo_player.hpp/.cpp # AGM playback via vendor agmplay subprocess + FIFO
│   │   ├── aaudio_player.hpp/.cpp # AAudio FIFO player (no root, Android 8.0+)
│   │   ├── dummy_player.hpp/.cpp  # Simulated audio sink (benchmarks/CI)
│   │   ├── jitter_buffer.hpp/.cpp # Adaptive Jitter Buffer with PLC
│   │   ├── android_helpers.hpp/.cpp # Root verification & tinymix helpers
│   │   └── audio_player.hpp       # Audio player interface
│   └── tools/
│       └── stream_daemon.cpp      # Standalone AAudio FIFO daemon (no root)
├── tests/                         # Unit tests suite
│   ├── CMakeLists.txt
│   ├── test_main.cpp
│   ├── test_protocol.cpp
│   ├── test_ring_buffer.cpp
│   ├── test_jitter_buffer.cpp
│   ├── test_socket.cpp
│   └── test_conversion.cpp
└── scripts/                       # Setup and execution scripts
    ├── build_server_msvc.bat      # Windows MSVC build script
    ├── build_server_mingw.bat     # Windows MinGW build script
    ├── build_client.sh            # Linux / Android build script
    ├── build_stream_daemon.sh     # NDK/Termux build of the AAudio stream daemon
    ├── termux_setup.sh            # Termux environment setup script
    ├── termux_run.sh              # Termux root execution script
    └── android_mixer_setup.sh     # Android ALSA mixer speaker routing helper
```

---

## 📄 License
This project is licensed under the Apache License 2.0. See `LICENSE` for details.
