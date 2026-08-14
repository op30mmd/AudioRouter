#pragma once

#include "audio_player.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace audiorouter {

// Pure, unit-testable helpers for the PulseAudio backend (no libpulse
// dependency; compiled and exercised on host builds too).
namespace pulse {

// Result of parsing a "-d pulse..." device string.
struct DeviceSpec {
    // Sink to connect to. Empty = let the PulseAudio daemon pick the default
    // sink (equivalent to @DEFAULT_SINK@).
    std::string sink;
    // Requested playback buffer target in ms. 0 = derive it from the stream
    // configuration (see compute_buffer_attr).
    uint32_t latency_ms = 0;
};

// Accepted device spellings (everything is case-sensitive, like the other
// backends):
//   pulse                      -> default sink, auto latency
//   pulseaudio / pa            -> aliases of "pulse"
//   pulse:<sink>               -> named sink (e.g. pulse:alsa_output.pci-0000_00_1f.3.analog-stereo)
//   pulse@<ms>                 -> default sink, explicit buffer target
//   pulse:<sink>@<ms>          -> named sink + explicit buffer target
// A sink of "default" or "@DEFAULT_SINK@" is normalised to "" (daemon default).
// Anything that is not a pulse device name yields sink="" / latency_ms=0.
DeviceSpec parse_device_spec(const std::string& device_name);

// True when `device_name` selects this backend.
bool is_pulse_device_name(const std::string& device_name);

// The four fields of pa_buffer_attr that matter for a low-latency playback
// stream, in BYTES (as libpulse wants them). Kept as a plain struct so the
// sizing logic is testable without libpulse headers.
struct BufferAttr {
    uint32_t maxlength = 0;  // hard upper bound of the stream buffer
    uint32_t tlength = 0;    // target fill level == the latency we ask for
    uint32_t prebuf = 0;     // bytes required before playback starts
    uint32_t minreq = 0;     // smallest request the server may make (one period)
};

// Sizes the playback buffer. `latency_ms` of 0 means "derive from the stream":
// four packets' worth of audio, clamped into [kMinLatencyMs, kMaxLatencyMs].
// The jitter buffer already absorbs network jitter, so this buffer only has to
// cover scheduler jitter between the playback thread and the daemon.
BufferAttr compute_buffer_attr(uint32_t sample_rate, size_t bytes_per_frame,
                               uint32_t frames_per_packet, uint32_t latency_ms);

// Bounds applied by compute_buffer_attr to the derived/requested latency.
inline constexpr uint32_t kMinLatencyMs = 10;
inline constexpr uint32_t kMaxLatencyMs = 500;

} // namespace pulse

// PulseAudio playback backend.
//
// Plays the stream through a PulseAudio (or PipeWire-with-pulse-shim) daemon
// using the synchronous `pa_simple` API. Needs NO root: the daemon runs as the
// user, which makes this the natural desktop-Linux backend and a rootless
// alternative to ALSA in Termux (`pkg install pulseaudio`).
//
// Behaviour notes:
//   - pa_simple_write() blocks until the daemon has taken the samples, so the
//     backend paces the playback thread by itself (like the ALSA backend and
//     unlike the FIFO-based AAudio/AGM backends, which the client self-paces).
//   - the requested buffer target (pa_buffer_attr::tlength) is derived from
//     the negotiated packet size, or taken from the -d pulse:<sink>@<ms>
//     suffix, and PulseAudio's ADJUST_LATENCY flag (set by pa_simple) makes
//     the daemon honour it as the end-to-end latency of the stream.
//   - a broken connection (daemon restart, sink removal) is detected on write
//     and reconnected in place, rate-limited, so a PulseAudio restart does not
//     take the client down.
//
// When the binary is built without libpulse (PULSEAUDIO_ENABLED undefined) the
// class compiles as a stub whose open() fails fast, letting the client's
// strategy fallback pick ALSA / direct PCM instead.
class PulsePlayer : public IAudioPlayer {
public:
    PulsePlayer();
    ~PulsePlayer() override;

    bool open(const AudioConfig& config, const std::string& device_name = "pulse") override;
    void close() override;
    bool is_open() const override;

    size_t write_frames(const void* pcm_data, size_t num_frames) override;
    size_t get_buffer_delay_frames() const override;
    void flush() override;
    std::string get_device_name() const override;

    // True when this binary was compiled against libpulse.
    static bool is_supported();

    // Cheap, non-blocking guess at whether a PulseAudio daemon is reachable:
    // PULSE_SERVER is set, or a native socket exists in one of the usual
    // places ($XDG_RUNTIME_DIR/pulse/native, /run/user/<uid>/pulse/native,
    // $PREFIX/var/run/pulse/native on Termux, ~/.pulse/... ). Used by the
    // client so a machine without PulseAudio never pays the cost of a
    // connection attempt.
    static bool server_available();

    // The server address of the daemon this client will actually talk to, in
    // libpulse's "unix:/path/to/native" form (or the raw PULSE_SERVER value).
    // Empty when no live daemon was found.
    //
    // server_available() is just `!resolve_server_address().empty()`. The
    // resolved address is passed EXPLICITLY to pa_simple_new(): letting
    // libpulse rediscover the server itself made it connect somewhere other
    // than the socket that was probed (on Termux it missed the daemon's
    // $HOME/.config/pulse/<machine-id>-runtime socket and got ECONNREFUSED).
    static std::string resolve_server_address();

    // Sink names reported by `pactl list short sinks`, best effort (empty when
    // pactl is missing or no daemon answers). Used by --list-devices only.
    static std::vector<std::string> get_available_sinks();

private:
#if defined(PULSEAUDIO_ENABLED)
    // Creates the pa_simple connection from config_/spec_. Caller holds mutex_.
    bool connect_locked();
    void disconnect_locked();
#endif

    AudioConfig config_;
    pulse::DeviceSpec spec_;
    std::string device_name_ = "pulse";
    // pa_simple* — opaque here so the header compiles without libpulse.
    void* stream_ = nullptr;
    // Serializes stream_ across write/close/reconnect.
    mutable std::mutex mutex_;
    std::atomic<bool> is_open_{false};
    // Effective buffer target actually requested from the daemon.
    uint32_t effective_latency_ms_ = 0;
    // Monotonic ms of the last reconnect attempt and the last logged write
    // error, so a dead daemon cannot spin or flood the log.
    uint64_t last_reconnect_ms_ = 0;
    uint64_t last_error_log_ms_ = 0;
    int consecutive_write_failures_ = 0;
};

} // namespace audiorouter
