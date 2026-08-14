# AudioRouter

High-performance, ultra-low-latency C++ audio routing engine that captures Windows
system audio output (WASAPI loopback) and streams it to an Android device over UDP,
where it is rendered through one of several pluggable playback backends (ALSA, direct
kernel PCM, Qualcomm AGM, Android AAudio, or the Termux:API media player — the last
two need no root). The server automatically mutes the PC
speakers while a client is attached and restores the previous volume state on
disconnect. The stream runs over Wi-Fi (hotspot) by default; a **Voice over USB**
mode carries the same stream over the USB cable via `adb reverse` — no Wi-Fi at all.

```
┌──────────────────────────── Windows PC (server) ────────────────────────────┐
│                                                                             │
│  WASAPI Loopback ──► Packetizer ──► UDP socket          IAudioEndpointVolume │
│  (IAudioCaptureClient,  (5 ms chunks,   (port 44100,     (mute PC speaker   │
│   shared-mode, 48 kHz   240 frames,     QoS priority,    while streaming;   │
│   stereo S16)            seq + ts)      MTU-safe)        restore on exit)   │
└───────────────────────────────────┬─────────────────────────────────────────┘
                                    │ UDP: DISCOVERY / CONNECT / AUDIO_DATA /
                                    │       HEARTBEAT / DISCONNECT / CONTROL
┌───────────────────────────────────▼─────────────────────────────────────────┐
│                        Android device (client, Termux)                      │
│                                                                             │
│  UDP receiver ──► Adaptive Jitter Buffer ──► Audio Player (pluggable)       │
│  (reorder + PLC  (256-slot ring, RFC3550   ├─ AAudio   (no root, in-process │
│   + NAT keepalive) jitter EMA, prefill     │  like stream_daemon)           │
│                  + stability gate)         ├─ TermuxAPI(no root, WAV        │
│                                             │  segments via the Termux:API  │
│                                             │  app's media player)          │
│                                             ├─ AGM      (vendor agmplay +    │
│                                             │           FIFO, root)         │
│                                             ├─ ALSA     (libasound, root)   │
│                                             └─ direct   (/dev/snd ioctl,    │
│                                                         root)               │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 1. Protocol

All packets are binary, little-endian, and start with a packed `CommonHeader`
(`#pragma pack(1)`):

```
offset  size  field
0       4     magic       0x41554452 ("AUDR")
4       1     version     CURRENT_VERSION (1)
5       1     msg_type    MsgType
6       2     flags       PacketFlags (reserved; FLAG_NONE)
8       4     seq_num     uint32 (per-message; audio packets carry the audio sequence)
12      8     timestamp_us uint64 monotonic source timestamp
20      4     payload_size
```

Message types (`protocol::MsgType`):

| Type              | ID   | Direction      | Payload                                    |
|-------------------|------|----------------|--------------------------------------------|
| `DISCOVERY_REQ`   | 0x01 | client → server| `DiscoveryReqPayload` (client name, version)|
| `DISCOVERY_RESP`  | 0x02 | server → client| `DiscoveryRespPayload` (server name, port, busy, muted) |
| `CONNECT_REQ`     | 0x10 | client → server| `ConnectReqPayload` (preferred rate/channels/format, target latency) |
| `CONNECT_ACK`     | 0x11 | server → client| `ConnectAckPayload` (negotiated format, frames/packet, PC muted, status) |
| `CONNECT_NAK`     | 0x12 | server → client| `ConnectNakPayload` (error code, reason)   |
| `DISCONNECT_REQ`  | 0x20 | either         | `DisconnectPayload` (reason)               |
| `DISCONNECT_ACK`  | 0x21 | server → client| `DisconnectPayload`                         |
| `AUDIO_DATA`      | 0x30 | server → client| `AudioPacketHeader` + interleaved PCM       |
| `HEARTBEAT_PING`  | 0x40 | client → server| `HeartbeatPayload` (orig ts, buffer level, loss stats) |
| `HEARTBEAT_PONG`  | 0x41 | server → client| `HeartbeatPayload` (echoes orig ts → RTT)   |
| `CONTROL_CMD`     | 0x50 | client → server| `ControlCmdPayload` (mute / volume commands)|

`AudioPacketHeader` extends `CommonHeader` with `sample_rate`, `channels`, `format`
and `num_frames`; PCM follows as interleaved S16LE (48 kHz, stereo, negotiated at
connect). `SAFE_PAYLOAD_MTU = 1400` bounds the audio payload so a single packet never
fragments over Wi-Fi (default 240 frames = 5 ms = 960 bytes).

Validation helpers (`is_valid_header`, `validate_audio_header`, `validate`) enforce
magic/version/type and payload-size consistency on every receive; malformed packets
are dropped.

## 2. Server (Windows)

`src/server/` — WASAPI loopback capture, packetization, endpoint-volume control,
and a watchdog:

- **Capture** (`wasapi_capture.cpp`): `IAudioClient` in `AUDCLNT_SHAREMODE_SHARED` +
  `AUDCLNT_STREAMFLAGS_LOOPBACK`, `IAudioCaptureClient::GetBuffer` on a dedicated
  thread. The device mix format is queried and the client is told the negotiated
  format in `CONNECT_ACK`. The Windows executable embeds a
  `requireAdministrator` UAC manifest so its integrity level matches programs
  started with **Run as administrator** and their audio is included in capture.
- **Packetization** (`server.cpp::on_audio_captured`): captured frames are chunked to
  `frames_per_packet` (default 240 = 5 ms @ 48 kHz, single-MTU), stamped with a
  monotonically increasing `seq_num` and a µs timestamp, and sent over a
  QoS-prioritized UDP socket.
- **Endpoint control** (`audio_endpoint_control.cpp`): on client connect the current
  master volume/mute is saved and the PC speaker is silenced (`IAudioEndpointVolume`),
  using either `mute`, `zero` (volume 0) or `both` (`--mute-mode`); on disconnect the
  saved state is restored.
- **Watchdog** (`server.cpp::watchdog_thread`): if no client traffic arrives within
  `client_timeout_ms` (8 s), the client is disconnected and the speaker restored.
- **Test tone** (`dummy_capture.cpp`): `-t` replaces loopback with a 440 Hz sine
  generator for network/pipeline testing without a sound source.

Options: `-p/--port`, `-b/--bind`, `-r/--rate`, `-f/--frames`, `--no-mute`,
`--mute-mode`, `-t/--test-tone`, `-l/--list-if`, `--usb`.

### 2.1 Voice over USB

Two USB transports exist; **USB tethering is the recommended one** (native
UDP over the cable, ~0.25 ms delivery jitter measured on-device vs ~7 ms
through the adb relay).

#### 2.1.1 USB tethering (RNDIS — lowest latency)

The phone turns the USB connection into a network link; the client then uses
the plain UDP protocol over it, exactly like Wi-Fi:

```bat
REM on the PC (ONE-TIME, run while the phone's USB tethering is active):
scripts\usb_tether_setup.bat   :: keeps the USB link off your internet routing
bin\audiorouter_server.exe     :: normal server, no flags needed
```
```bash
# on the phone (root backends; switches USB to RNDIS and back automatically):
./scripts/termux_run.sh --tether -d agm
```

`usb_tether_setup.bat` addresses the one wart of USB tethering: the phone's
DHCP hands the PC a default gateway, so Windows can start routing internet
traffic through the phone. The script converts the DHCP lease into a static
address with **no default gateway** (and pins the interface metric low), so
the cable is reachable for AudioRouter while your internet routing stays
exactly as it was. It persists per adapter — one run, then every tethering
session comes up internet-safe.

`--tether` runs `svc usb setFunctions rndis` (root), waits for `rndis0`,
launches the client with `--discover -b rndis0` (the interface pin keeps the
discovery broadcast off a VPN's default route), and restores the adb USB
function on exit. The server answers discovery on the RNDIS interface — no IPs
to type. The RNDIS link still has a slow ~130 ms transit oscillation, so the
tethering default jitter buffer is 100 ms (measured on-device: 21 underruns at
60 ms, 0 at 100 ms over 45 s); pass `-l <ms>` to override. AAudio cannot use
`--tether` (the pin needs root, and AAudio does not render as root); enable
tethering in the phone Settings and pass the PC's USB IP instead:
`./scripts/termux_run.sh -s <PC-USB-IP> -d aaudio` (the server prints its USB
interface IP on startup; unicast routes on-link via rndis0, no root needed).

#### 2.1.2 adb reverse tunnel (fallback, no tethering)

`--usb` binds the server to loopback (`127.0.0.1`) only and sets up an
`adb reverse tcp:<port> tcp:<port>` tunnel (best effort; falls back to printed
instructions if `adb` is missing or no device is connected). adb cannot forward
UDP, so the server runs a relay thread: it accepts the tunnel's TCP connection
and carries each UDP datagram as a length-prefixed frame (`uint32 LE length |
payload`), so the WASAPI engine above it still speaks plain UDP. The phone's
traffic then travels over the USB cable straight into the PC's loopback — no
hotspot, no Wi-Fi, no VPN issues. Any client that connects to `127.0.0.1:<port>`
is treated like any other client (same mute/heartbeat/watchdog logic).

In USB mode the client's default jitter buffer target rises from 35 ms to
100 ms (measured on-device: the two relays plus the adb forward deliver in
slow oscillating bursts that smaller buffers cannot ride out); pass
`-l <ms>` to override. Both relays cap their TCP socket buffers at 64 KB so a
stalled hop (adb/USB/CPU) drops fast and reconnects to live audio instead of
queuing and replaying seconds of stale audio. FIFO-style backends (AAudio,
AGM) self-pace so their pipe backlog stays near ~40 ms instead of
accumulating the full pipe capacity as constant delay.

## 3. Client (Android / Termux)

`src/client/` — UDP receiver, adaptive jitter buffer, pluggable playback backends,
and a supervised device-open/retry machinery.

### 3.1 Network receive thread
Drains the UDP socket (512 KB buffers, QoS priority) and dispatches by type. Audio
packets go to the jitter buffer; `HEARTBEAT_PONG` computes RTT from the echoed
`orig_timestamp_us`; `DISCONNECT_*` tears down the session. Optional `-b auto`
pins the socket to the physical Wi-Fi interface via `SO_BINDTODEVICE` (bypasses
Android VPN tunnels; requires root).

### 3.2 Adaptive jitter buffer (`jitter_buffer.cpp`)
- **256-slot ring** (`MAX_SLOTS`), indexed by `seq_num % 256`.
- **Reorder + loss detection**: out-of-window late packets are dropped;
  a gap ≥ `MAX_SLOTS` resyncs the play pointer (stats `overruns`).
- **Jitter estimate**: RFC 3550-style exponential moving average over inter-arrival
  transit-time deltas (`jitter_estimate_us_ += (d - jitter_estimate_us_) / 16`).
- **Prefill / stability gate**: first fill targets `max(120 ms, 3 × target)`
  (capped 500 ms); the playhead starts only after 24 consecutive gap-free arrivals,
  so it does not restart into a delivery stall. Re-buffers use the configured
  `target_latency_ms` (default 35 ms).
- **PLC**: on a missing slot, one packet's worth of silence is emitted and the
  sequence is advanced (stats `packets_lost`, `underruns`).

### 3.3 Playback backends
All implement `IAudioPlayer` (`open/close/is_open/write_frames/get_buffer_delay_frames/flush`).
The device name selects both the backend and the fallback chain:

| `-d` value | Backend | Privilege | Notes |
|------------|---------|-----------|-------|
| `aaudio` / `aaudio:deep` / `aaudio:voip` | **AAudio** (NDK, in-process) | none | Byte-for-byte mirror of `stream_daemon`: 64 KB (≈341 ms) `O_RDWR` FIFO, ~20 ms pump chunks, 500 ms write timeout, no usage hints, no readiness probe; lenient watchdog (20 failed writes → recreate + drain the FIFO, deep-buffer mode on 2nd rebuild, fall back after 3). FIFO at `/data/local/tmp` as root, `$HOME` otherwise. |
| `termux` / `termux-api` / `termux:<ms>` | **Termux:API** media player (Android `MediaPlayer` via `com.termux.api`) | none | Needs the Termux:API app (F-Droid). MediaPlayer cannot play a pipe or a growing file, so the stream is laid out as a **file ring buffer**: each segment file is created at its full length (exact sizes in the header, the data region a sparse hole that reads as silence) and the stream is overwritten into it sequentially. The player is handed a file once a ~600 ms prefill is recorded and plays it at 1x while the recorder keeps filling just ahead — the end-to-end delay is **prefill + command latency (~0.7-1.6 s)**, independent of the file length (`<ms>`, default 600000 ms = 10 min, only sets how often the player switches files — every switch costs the app a stop/reset/prepare/start cycle, ~0.3-1 s of silence). Files come from a recycled pool under the Termux home (pre-labeled with the app-data SELinux context before the privilege drop). All IPC runs on a dedicated issuer thread — the playback thread never blocks (see §3.3.1). Commands use the Termux:API listen-socket protocol when available, otherwise `am broadcast` with the client's result sockets (the official termux-api mechanism on Android 14+, where the app process freezes); every play is confirmed by the app's own reply. |
| `agm` / `agm:<backend>` | **AGM** via vendor `agmplay` subprocess + WAV-over-FIFO | root | Default backend `CODEC_DMA-LPAIF_RXTX-RX-1`; auto-recover: FIFO-stall detection, logcat/mixer preemption watcher, HAL restart. |
| `direct:/dev/snd/pcmC0D0p` or `/dev/snd/...` | **Direct kernel PCM** (ioctl) | root | No ALSA userspace deps; enumerates `/dev/snd` nodes; per-node retry with hang detection. |
| `default`, `hw:0,0`, `plughw:0,0` | **ALSA** via `libasound.so` | root | dlopen-based, optional direct fallback. |
| `dummy` | DummyPlayer | — | Benchmarking / CI. |

Fallback chains (`build_open_strategies`): `aaudio*` → `AAUDIO → TERMUXAPI → AGM →
NODES → LEGACY`; `termux*` → `TERMUXAPI → AAUDIO → AGM → NODES → LEGACY`;
`agm*` → `AGM → NODES → LEGACY`; node paths → `NODES`; everything else → `LEGACY`.
Each attempt runs on its own thread with a 20 s hang timeout; abandoned attempts
hot-swap the device in if they later succeed.

#### 3.3.1 How the Termux:API backend streams complete files (file ring buffer)

Android's `MediaPlayer` sizes its source at `prepare()` time, so neither a
named pipe nor a growing file can carry a live stream into it. The backend
lays the stream out as a **file ring buffer**, so the file-completeness
requirement no longer sets the delay:

- each segment file is created at its **full length** right away: the header
  carries the exact sizes and the data region is a sparse hole (ftruncate)
  that reads back as silence. The stream is then overwritten into the file
  sequentially as it arrives — the file IS the buffer, and the player can
  never read past the recording head (an underrun degrades to silence, never
  to an error);
- the file is handed to the media player once its first **prefill (600 ms)**
  of real audio is recorded **and** the wall clock has advanced that far
  (the wall gate stops a jitter-buffer prefill burst from skipping audio
  ahead of real time). The player starts ~command-latency later and plays
  the file at 1x while the recorder keeps filling the region just ahead —
  the **end-to-end delay is `prefill + command latency` (~0.7 s over the
  socket protocol, ~1.6 s over `am broadcast`), independent of the segment
  length**;
- `write_frames()` paces itself at real time (like `DummyPlayer`), records a
  file until its window is complete, and only then rotates to the next pool
  file — the issue cadence IS the file length (`-T/--termux-segment <ms>`,
  or the `-d termux:<ms>` device suffix; default 600000 ms = **10 minutes**).
  The file length therefore only sets how often
  the player switches files: each switch makes the app stop/reset/prepare/
  start the new track, an unavoidable **~0.3-1 s pause per boundary** (the
  app's MediaPlayer has no gapless transition). Long files make that pause
  rare — roughly once every 10 minutes at the default — at the cost of disk
  (≈12 MB per minute of 48 kHz stereo S16; with the result channel the
  previous file is truncated after each confirmed switch, so disk stays at
  ~2 files);
- every play runs on a dedicated **issuer thread**: the playback thread only
  records, paces and hands off, so a slow `am broadcast` (up to seconds on a
  cold/frozen app) can never stall the jitter-buffer pops. A busy handoff
  slot means a command outlasted a whole segment: the pending segment is
  replaced by the fresher one (logged rate-limited) and playback resumes at
  live audio;
- `L` (the command latency) is tracked with an EMA of the measured command
  duration. Commands prefer the Termux:API listen-socket protocol; where it
  is unavailable they go through `am broadcast` **carrying the client's own
  result-socket extras** — the app then writes its result back after
  `prepare()+start()` (the exact mechanism the official termux-api binary
  uses on Android 14+, where the app's own listen socket freezes). Every
  play is confirmed by the app's own "Now Playing" reply, which surfaces
  silent failures in the log; in blind mode (no result channel) the EMA
  tracks the am time plus a prepare bias;
- a watchdog (whenever the app has a result channel) polls the player status
  once a second, resumes a paused player, and restarts a dead one at live
  audio. Failed plays likewise discard the partial recording segment and
  re-issue after the prefill, so the stream resumes within ~prefill +
  latency instead of waiting for the next boundary (the same stale-audio
  policy as the FIFO backends).

### 3.4 Latency budget

End-to-end audio delay = jitter buffer + backend delay:

| Component | Typical | Worst case | Bound by |
|-----------|---------|------------|----------|
| Jitter buffer | target `-l` (default 35 ms); excess drained back to ~target+25 ms after bursts | startup prefill 120–500 ms (one-time, protected by a 3 s grace) | prefill / stability gate + drain-to-target |
| FIFO (AAudio pipe) | ~40 ms (playback thread self-paced against the backend; stale backlog discarded on stall recovery / rebuild) | **341 ms** transient (64 KB @ 192 KB/s, only while a stall is active) | `kFifoSizeBytes` + playback pacing + pump drain-on-recovery |
| AAudio in-stream | ≤ 40 ms (capacity capped at 1920 frames on LOW_LATENCY); deep mode uses the HAL default | 40 ms | `setBufferCapacityInFrames` |
| Termux:API ring buffer | `prefill (600 ms) + command latency` (~0.7 s socket protocol, ~1.6 s am broadcast) — the file ring pre-sizes each segment, so the player only needs the prefill before starting | + segment length after a player death until the next switch | prefill gate + issuer thread + latency EMA (§3.3.1) |
| Network / UDP | RTT + jitter (see status line) | — | — |

The 5 s status line reports the backend portion separately as
`Audio: <ms>` (`ClientStats::audio_backend_delay_ms`, sampled from
`IAudioPlayer::get_buffer_delay_frames()` = FIFO bytes + AAudio
written−read frames), so the real audio delay is observable on top of
the jitter `Buffer: <ms>` figure.

Three client-specific latency controls keep the AAudio backend tight:
- **Bounded AAudio buffer (1920 frames ≈ 40 ms)**: some vendor HALs hand out
  huge AAudio buffers even in LOW_LATENCY mode (the reference device
  reported ~16.8 k frames = 350 ms of in-flight audio, a constant
  multi-hundred-ms source→speaker delay). `setBufferCapacityInFrames` caps
  the in-stream backlog to 40 ms, so the device paces the pump through
  write back-pressure instead of absorbing a big backlog.
- **Bounded FIFO (64 KB ≈ 341 ms)**: the pipe is a thread-decoupling
  buffer, not a burst absorber — the jitter buffer (≤ ~1.28 s) owns
  network burst absorption. The stream_daemon keeps its original 1 MB
  for external `cat`/ffmpeg feeders; the client's smaller pipe caps how
  much stale audio can be queued during a stall.
- **Playback self-pacing**: the client's playback thread is otherwise
  unpaced — it dumps the jitter prefill burst into the pipe faster than
  real time, and the pipe stays permanently full (the writer refills at
  exactly the pump's drain rate), which showed up as a constant
  `Audio: 350 ms` on device. The thread now sleeps when the backend
  reports > 60 ms buffered (AAudio only), holding the pipe near ~40 ms.
- **Jitter drain-to-target**: a burst (startup prefill, a stall that let the
  buffer accumulate, a reconnect) can leave the jitter buffer well above the
  configured `-l` target — the level then stays high forever, adding constant
  latency. After a 3 s startup grace, `pop_frames()` sheds the excess back to
  ~target + 25 ms in small 5 ms chunks, at most one per 400 ms, so the
  catch-up is a few near-inaudible skips instead of one gap or permanent
  added delay (`JitterBufferStats::drained_frames` counts what was shed).
- **FIFO drain on recovery**: whenever the pump transitions from stalled
  to consuming — the startup ramp (writes block while the stream is
  STARTING, priming the pipe full), a mid-stream stall, or a stream
  rebuild after a disconnect — everything queued in the pipe is
  discarded so playback resumes at live audio.

### 3.5 Heartbeat / keep-alive
A 1 s heartbeat thread sends `HEARTBEAT_PING` with buffer level and loss counters
(keeps NAT mappings open, feeds server-side stats), and re-handshakes when the
server goes silent beyond `reconnect_timeout_ms`.

### 3.6 Standalone tool: `stream_daemon`
`src/tools/stream_daemon.cpp` — continuous real-time AAudio FIFO daemon. Creates
`/data/local/tmp/audio_pipe` (or `$HOME/audio_pipe`), opens it `O_RDWR` (never EOF),
expands it to 1 MB, and pumps 20 ms chunks into a low-latency AAudio stream with
500 ms write timeouts. The client's `aaudio` backend is its in-process twin.

```bash
# build (NDK, or scripts/build_stream_daemon.sh):
clang++ -O2 -Wno-unavailable-declarations -target aarch64-linux-android30 \
    -o stream_daemon src/tools/stream_daemon.cpp -laaudio -lm
# run, then feed raw interleaved S16 stereo PCM:
./stream_daemon
ffmpeg -re -f lavfi -i "sine=frequency=440:sample_rate=48000" \
    -f s16le -ac 2 -fflags nobuffer -y /data/local/tmp/audio_pipe
```

## 4. Build

### Windows server
```bat
scripts\build_server_msvc.bat     :: MSVC + CMake
scripts\build_server_mingw.bat    :: MinGW (requires g++ and windres)
```
Both build paths embed an elevation manifest. Windows displays a UAC prompt whenever
`audiorouter_server.exe` starts; approve it so WASAPI can capture audio from elevated
as well as ordinary programs.

### Android client (Termux)
```bash
./scripts/termux_setup.sh         # clang, make, alsa-utils, optional ndk-sysroot
make client                       # probes: Android target, libaaudio (sysroot or
                                  #   /system/lib64 absolute path), defines AAUDIO_ENABLED
```
The Makefile probes the toolchain (`--target=<triple>30` for AAudio, matching the
proven stream_daemon build) and only compiles the AAudio backend when `libaaudio` is
linkable; otherwise the player compiles as a stub and `-d aaudio` fails fast into the
fallback chain. `make stream-daemon` builds the standalone tool.

### NDK / CMake
```bash
cmake -B build-android -DCMAKE_TOOLCHAIN_FILE=$NDK/toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release
cmake --build build-android --target audiorouter_client stream_daemon
```

### Host tests
```bash
make test          # 10 suites: protocol, ring buffer, jitter buffer/PLC,
                   # socket, conversion, Termux:API segment scheduling,
                   # thread/type/memory safety
```

### Binary-only release (no source)
The GitHub release artifact ships `audiorouter_client`, `stream_daemon` and the
scripts without a source tree. The scripts detect this layout: `termux_setup.sh`
installs runtime tools only (`alsa-utils` for `tinymix`) and never tries to
compile; `termux_run.sh` uses the prebuilt binary next to it. Both exit with a
clear message pointing at the release download when the binary is missing. The
build scripts (`build_client.sh`, `build_stream_daemon.sh`) refuse with an
explicit "source checkout required" error in this layout.

## 5. Usage

### Server
```bat
bin\audiorouter_server.exe                  :: loopback capture, port 44100
bin\audiorouter_server.exe -t               :: 440 Hz test tone
bin\audiorouter_server.exe --usb            :: Voice over USB (binds loopback, sets up adb reverse)
```

### Client
```bash
# root backends (AGM recommended on Qualcomm devices):
su
./bin/audiorouter_client -s 192.168.43.45 -d agm
# no-root AAudio (in-process, like stream_daemon):
./bin/audiorouter_client -s 192.168.43.45 -d aaudio
# no-root Termux:API media player (needs the Termux:API app from F-Droid):
./scripts/termux_run.sh -s 192.168.43.45 -d termux
#   one ~prepare-time switch pause per file (10 min default); the delay is
#   the same (~0.6 s + command latency) no matter the file length:
./scripts/termux_run.sh -s 192.168.43.45 -d termux -T 300000   # pause every 5 min
# with interface binding (root; bypasses VPN):
./scripts/termux_run.sh -s 192.168.43.45 -d agm -b auto
```

### Voice over USB (no Wi-Fi)
Phone plugged into the PC with USB debugging enabled; server and client speak
over the USB cable only:
```bat
REM on the PC:
scripts\usb_setup.bat            :: adb reverse tcp:44100 tcp:44100 (or let the server do it)
bin\audiorouter_server.exe --usb
```
```bash
# on the phone (no hotspot, no root needed for the transport):
./bin/audiorouter_client -u -d aaudio
```
`-u/--usb` routes the client through a loopback relay: the protocol engine
sends UDP to a local relay socket, and the relay frames each datagram over the
adb TCP tunnel to `127.0.0.1:<port>`. Wi-Fi/VPN interface handling is disabled;
`--discover` and `-b` are ignored in USB mode. Works with every playback
backend.

`scripts/termux_run.sh` resolves the binary to an absolute path and launches it via
`su -c` for root backends (the AAudio build links `/system/lib64/libaaudio.so` by
absolute path — only the system linker resolves it); `-b auto` runs via `su` and the
client keeps root for `SO_BINDTODEVICE` while AAudio runs in-process.

### Client options
```
-s, --server <ip>     server IP
-p, --port <port>     server port (default 44100)
-d, --device <dev>    backend/device (see table above; default 'default')
-T, --termux-segment <ms>
                      Termux:API file length in ms (default 600000 = one
                      ~prepare-time switch pause per 10 min; 2000..3600000).
                      The end-to-end delay is unaffected by it; overrides
                      -d termux:<ms>. Ignored for other backends.
-l, --latency <ms>    target jitter-buffer latency (default 35 ms)
-b, --bind <iface>    pin UDP socket to an interface ('auto' = physical NIC)
-u, --usb             Voice over USB: stream over the USB cable via adb reverse
                      (targets 127.0.0.1; ignored with -b/--discover)
    --discover        auto-discover the server on the local hotspot subnet
    --dummy           DummyPlayer (benchmarks)
    --list-devices    list ALSA nodes, kernel PCM nodes, AAudio availability
```

## 6. Hotspot topologies

- **A: PC → Android hotspot**: PC connects to the phone's Wi-Fi hotspot; server
  prints its IP (`192.168.43.x`); client connects to it.
- **B: Android → Windows mobile hotspot**: PC hotspot gateway is typically
  `192.168.137.1`; client connects there. If a VPN tunnel is active, use
  `-b auto` to bypass it.
- **C: USB cable (Voice over USB)**: no Wi-Fi at all. Phone connected by USB
  with USB debugging on; PC runs `adb reverse udp:44100 udp:44100` (server
  `--usb` does it automatically) and the client uses `-u`. The link is the
  USB cable, so RTT drops to sub-ms and interference/hotspot stalls vanish.

## 7. Troubleshooting / operational notes

- **Audio from an administrator program is silent**: use a newly rebuilt server and
  approve its UAC prompt. If the prompt does not appear, the executable does not
  contain the required elevation manifest; rebuild it with one of the supported
  Windows scripts. Protected/DRM audio and applications using an exclusive output
  path can still be unavailable to WASAPI shared-mode loopback.
- **AAudio does not render on some vendor HALs**: the stream opens but never starts
  consuming (watchdog logs `AAudioStream_write wrote X of Y frames (state=..., read=...)`).
  The client falls back to the Termux:API player and then AGM/ALSA automatically; use
  `-d agm` directly for the best experience on such devices (or `-d termux` when the
  device is not rooted).
- **Termux:API backend** (`-d termux`, no root):
  - needs the **Termux:API app** (`com.termux.api`) installed — F-Droid → Termux:API.
    `--list-devices` probes it; `open()` fails fast into the fallback chain when the
    app does not answer;
  - runs **as the Termux app user**: the app only accepts its own uid and reads the
    segment files from the Termux home. `termux_run.sh` launches `-d termux` without
    su by default — this is the recommended mode;
  - with `-b/--bind` (which needs root) the client binds the socket as root,
    pre-labels the segment pool and captures the app's SELinux context, then drops
    to the Termux app user in-process — its result sockets carry the app's own
    context so the confined app can reply. If the log still reports `the app
    returned no result`, the root domain is being denied anyway: run without
    `-b` (no su), or check `su -c 'logcat -d -s ResultReturner TermuxApiReceiver'`;
  - on **Android 14+** the app's listen socket freezes, so commands go through
    `am broadcast` with the client's result sockets — still fully confirmed by the
    app, just slower (the startup log prints the estimated end-to-end delay, which is
    `~0.6 s + command latency` either way; raise `-T <ms>` / `-d termux:<ms>` only
    to make the per-switch pause rarer);
  - **not a real-time transport**: the pipeline delay is ~0.7-1.6 s and every file
    switch costs the app a stop/reset/prepare/start cycle (~0.3-1 s pause; once per
    10 min at the default). For true real-time playback use `-d aaudio` (no root,
    in-process, low latency) or `-d agm` / ALSA (root) — Termux:API exists for
    devices where none of those render;
  - every play is logged with the app's own reply (`Now Playing: seg_pX.wav`).
    If the reply is an error or the log reports `the app returned no result`,
    the app could not play the segment — check `logcat -s MediaPlayerAPI` for its
    error. If the app confirms every play but no sound comes out, the segment path
    is fine and the issue is device-side: media volume or the media output route;
  - playback uses Android's media volume, not the call volume.
- **`/dev/snd` nodes hang**: Android's `audioserver` usually holds them.
  `su -c "stop audioserver"` frees them (re-enable with `start audioserver`).
- **AGM preempted by notifications**: the AGM player watches logcat + mixer state
  and respawns `agmplay`; if the HAL is wedged it restarts `vendor.audio-hal`.
- **Mixer routing** (`scripts/android_mixer_setup.sh`): Qualcomm/MediaTek devices may
  need `tinymix` speaker-path setup when no Android audio has played yet.
- **Runtime linking**: the AAudio build needs the system linker (run via
  `termux_run.sh` / `su -c "<abs path>"`), not a Termux-shell exec.
- **Voice over USB does not connect**: enable **USB debugging** on the phone
  (Developer options) and confirm `adb devices` lists it as `device` (not
  `unauthorized` — authorize the RSA prompt). The tunnel needs platform-tools
  ≥ 31 for `udp:` reverse sockets; `scripts/usb_setup.bat` checks and sets
  everything up. Because the server runs elevated (UAC), its `adb` call talks
  to the adb server already running in your user session; if none is running it
  starts one that uses your normal `%USERPROFILE%\.android\adbkey`, so the
  device stays authorized. If the tunnel never confirms, run
  `adb reverse udp:44100 udp:44100` from a normal terminal and retry.
- **Ctrl+C under `su`**: Magisk's `su` can put the client in its own session,
  so the terminal's Ctrl+C never reaches it. `termux_run.sh` runs su in the
  background and bridges INT/TERM to the client (matching it by absolute
  path), escalating INT → TERM → KILL; the client also shuts down on
  SIGHUP (terminal closed).

## 8. Repository layout

```
src/common/    protocol.hpp, socket_util.*, audio_types.hpp, ring_buffer.hpp,
               logger.hpp, time_util.hpp, span/expected/thread compat
src/server/    main.cpp, server.*, wasapi_capture.*, dummy_capture.*,
               audio_endpoint_control.*, audio_capture.hpp, UAC manifest/resource
src/client/    main.cpp, client.*, jitter_buffer.*, audio_player.hpp,
               aaudio_player.*, termux_api_player.*, agm_fifo_player.*,
               alsa_player.*, direct_alsa.*, dummy_player.*, android_helpers.*
src/tools/     stream_daemon.cpp
scripts/       termux_setup.sh, termux_run.sh, build_client.sh,
               build_stream_daemon.sh, android_mixer_setup.sh,
               build_server_msvc.bat, build_server_mingw.bat, usb_setup.bat
tests/         protocol, ring buffer, jitter buffer/PLC, socket,
               conversion, Termux:API segments/scheduling/protocol,
               thread/type/memory safety
```

## 9. License

Apache License 2.0 — see `LICENSE`.
