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

# 2. Run automated setup script (installs clang, make, alsa-lib, tsu)
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
  -d, --device <dev>        ALSA device ('default', 'hw:0,0', 'direct:/dev/snd/pcmC0D0p')
  -l, --latency <ms>        Target Jitter Buffer latency in ms (default: 35ms)
      --discover            Auto-discover server on local hotspot subnet
      --dummy               Use simulated audio player (benchmarking/testing)
      --list-devices        List detected ALSA and kernel PCM nodes
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
│   └── client/                    # Android Termux ALSA Client
│       ├── CMakeLists.txt
│       ├── main.cpp               # Client CLI and signal handling
│       ├── client.hpp/.cpp        # Client receiver engine & NAT heartbeat
│       ├── alsa_player.hpp/.cpp   # ALSA player (dynamic libasound)
│       ├── direct_alsa.hpp/.cpp   # Direct kernel /dev/snd/pcmC0D0p ioctl driver
│       ├── dummy_player.hpp/.cpp  # Simulated audio sink (benchmarks/CI)
│       ├── jitter_buffer.hpp/.cpp # Adaptive Jitter Buffer with PLC
│       ├── android_helpers.hpp/.cpp # Root verification & tinymix helpers
│       └── audio_player.hpp       # Audio player interface
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
    ├── termux_setup.sh            # Termux environment setup script
    ├── termux_run.sh              # Termux root execution script
    └── android_mixer_setup.sh     # Android ALSA mixer speaker routing helper
```

---

## 📄 License
This project is licensed under the Apache License 2.0. See `LICENSE` for details.
