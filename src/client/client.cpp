#include "client.hpp"
#include "alsa_player.hpp"
#include "direct_alsa.hpp"
#include "agm_fifo_player.hpp"
#include "aaudio_player.hpp"
#include "termux_api_player.hpp"
#include "pulse_player.hpp"
#include "dummy_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"
#include "../common/usb_tunnel.hpp"

#include <cstring>
#include <vector>
#include <functional>
#include <chrono>
#include <algorithm>
#include <limits>
#include <cerrno>
#include <cstdlib>
#include <grp.h>

namespace audiorouter {

namespace {
    // How long to wait for the ALSA/direct audio device to open before giving
    // up and falling back to the dummy sink.
    constexpr uint32_t kPlayerOpenTimeoutMs = 3000;
    // Termux:API device opens probe the app via `am` (cold app processes can
    // take seconds); its budget is deliberately larger so the dummy sink
    // doesn't flash before the hot-swap.
    constexpr uint32_t kTermuxPlayerOpenTimeoutMs = 8000;
    // PulseAudio gets the same generous budget: resuming a suspended sink
    // re-opens the Android HAL stream and can take seconds. With the 3 s
    // default the client flashed the dummy sink (and the ALSA troubleshooting
    // wall) on every run that started more than 5 s after the previous one,
    // even though the connect then succeeded moments later.
    constexpr uint32_t kPulsePlayerOpenTimeoutMs = pulse::kPlayerOpenTimeoutMs;
    // A single open attempt that exceeds this is considered hung in a kernel
    // call and is abandoned (a new attempt is started on a fresh player).
    constexpr uint32_t kDeviceAttemptTimeoutMs = 20000;
    constexpr uint32_t kDeviceAttemptPollMs = 100;
    constexpr uint32_t kDeviceRetryBackoffMs = 3000;
    constexpr uint32_t kDeviceRetryMaxBackoffMs = 30000;
    // Node-based (direct:/dev/... or /dev/...) opens try PCM nodes one by one;
    // stop after this many attempts so a device held by Android's audioserver
    // doesn't stall the client forever.
    constexpr size_t kMaxNodeOpenAttempts = 8;
    // AGM is tried first for "agm"/"agm:<backend>" devices. If the vendor
    // library is absent (many Samsung/entry-level builds ship the classic ALSA
    // HAL instead) those attempts fail fast and the client falls back.
    constexpr size_t kMaxAgmOpenAttempts = 2;
    // AAudio opens are quick user-space calls (no kernel driver to hang on),
    // so a small retry budget is plenty before falling back to the
    // root-requiring backends.
    constexpr size_t kMaxAaudioOpenAttempts = 2;
    // Termux:API opens are an `am` probe round trip; same small budget.
    constexpr size_t kMaxTermuxOpenAttempts = 2;
    // PulseAudio opens are a socket connect to the local daemon: fast, and
    // either it answers or it does not.
    constexpr size_t kMaxPulseOpenAttempts = 2;
    // ...but libpulse does not always honour that. With no daemon running,
    // pa_simple_new() can BLOCK (name resolution, autospawn) instead of
    // returning an error, which used to hold this strategy for the full
    // kDeviceAttemptTimeoutMs (20 s) - far past the caller's open budget, so
    // playback was stranded on the dummy sink and the chain never reached
    // AAudio.
    //
    // The cap must still be generous enough for a legitimate slow connect.
    // PulseAudio's module-suspend-on-idle suspends a sink after 5 s of
    // silence, and resuming it re-opens the Android HAL stream - measured at
    // ~2.5 s on device. A 1500 ms cap turned that into a spurious "hung in a
    // kernel call", so any run starting more than 5 s after the previous one
    // fell back to AAudio while the PulseAudio connect completed moments
    // later and had to be discarded.
    constexpr uint32_t kPulseAttemptTimeoutMs = pulse::kOpenAttemptTimeoutMs;

    // "direct:/dev/snd/pcmC0D0p" or a bare "/dev/snd/..." path opens individual
    // kernel PCM nodes; any other name (default, hw:0,0, plughw:...) goes
    // through the whole-chain AlsaPlayer open (ALSA-lib style).
    bool is_node_based_device(const std::string& device_name) {
        return device_name.rfind("direct:", 0) == 0 || device_name.rfind("/dev/", 0) == 0;
    }

    // "agm" / "agm:..." streams straight into Qualcomm's Audio Graph Manager
    // (vendor libagmclient.so) instead of raw PCM nodes.
    bool is_agm_device(const std::string& device_name) {
        return device_name.rfind("agm:", 0) == 0 || device_name == "agm";
    }

    // "aaudio" / "aaudio:..." plays through Android's native AAudio API (audio
    // HAL / AudioFlinger) — the only backend that needs NO root.
    bool is_aaudio_device(const std::string& device_name) {
        return device_name.rfind("aaudio:", 0) == 0 || device_name == "aaudio";
    }

    // "termux" / "termux-api" / "termux:..." plays through the Termux:API
    // app's media player (Android MediaPlayer). Also needs NO root — just the
    // Termux:API app installed.
    bool is_termux_device(const std::string& device_name) {
        return device_name.rfind("termux:", 0) == 0 || device_name == "termux" ||
               device_name == "termux-api";
    }

    // "pulse" / "pulseaudio" / "pa" (optionally ":<sink>" and/or "@<ms>")
    // plays through a PulseAudio (or PipeWire pulse-shim) daemon. NO root
    // required — the daemon runs as the user.
    bool is_pulse_device(const std::string& device_name) {
        return pulse::is_pulse_device_name(device_name);
    }

    // A PulseAudio daemon is only worth trying when this binary was built with
    // libpulse AND a server socket / PULSE_SERVER is actually present.
    bool pulse_worth_trying() {
        return PulsePlayer::is_supported() && PulsePlayer::server_available();
    }

    std::vector<std::string> build_node_candidates(const std::string& device_name) {
        std::string primary = "/dev/snd/pcmC0D0p";
        if (device_name.rfind("direct:", 0) == 0) {
            primary = device_name.substr(7);
        } else if (device_name.rfind("/dev/", 0) == 0) {
            primary = device_name;
        }
        if (primary.empty()) {
            primary = "/dev/snd/pcmC0D0p";
        }

        std::vector<std::string> candidates;
        candidates.push_back(primary);
        candidates.push_back("/dev/snd/pcmC0D0p");
        for (const auto& dev : DirectAlsaPlayer::enumerate_kernel_pcm_devices()) {
            if (std::find(candidates.begin(), candidates.end(), dev) == candidates.end()) {
                candidates.push_back(dev);
            }
        }
        return candidates;
    }

    // Runs one device open attempt on a fresh player. May block in the kernel
    // for a long time; on success it hot-swaps the real device into
    // open->player. Never touches the AudioRouterClient object, so it remains
    // safe even if the client is destroyed while this thread is stuck.
    void attempt_device_open(std::shared_ptr<DeviceOpenShared> open,
                             std::shared_ptr<IAudioPlayer> device,
                             std::function<bool()> open_fn) {
        bool opened = false;
        if (!open->shutdown.load()) {
            opened = open_fn();
        }

        bool superseded = false;
        std::shared_ptr<IAudioPlayer> displaced;
        if (opened && !open->shutdown.load()) {
            {
                std::lock_guard<std::mutex> lock(open->mutex);
                // A slow attempt that was abandoned can finish AFTER a later
                // strategy already opened a device. Hot-swapping then would
                // leave two engines (e.g. this PulseAudio stream and the
                // AAudio fallback) both feeding the Android HAL. Only take
                // the slot if it is still free.
                //
                // The dummy sink does NOT count as occupied: it is the silent
                // placeholder installed while the open is still in flight.
                // Treating it as a live backend made every real device that
                // opened afterwards get discarded, so the client played
                // silence for the whole session.
                if (open->player && open->player->is_open() &&
                    !open->player->is_placeholder()) {
                    superseded = true;
                } else {
                    displaced = open->player;   // dummy placeholder, if any
                    open->player = device;
                    open->result = true;
                    open->pending = false;
                }
            }
            if (superseded) {
                LOG_INFO("Audio device: a late open attempt succeeded but another backend "
                         "is already playing; discarding the duplicate stream.");
                device->close();
            } else {
                if (displaced) {
                    LOG_INFO("Audio device: real backend took over from the dummy sink.");
                    displaced->close();
                }
                open->cv.notify_all();
                return;
            }
        }
        {
            if (opened && !superseded) device->close();  // shutdown raced the open
            {
                std::lock_guard<std::mutex> lock(open->mutex);
                open->result = false;
                open->pending = false;
            }
            open->cv.notify_all();
        }
    }

    enum class OpenStrategy { PULSE, AAUDIO, TERMUXAPI, AGM, NODES, LEGACY };

    // "aaudio"/"aaudio:<mode>" uses AAudio first (no root, works on stock
    // devices; needs Android 8.0+), then the Termux:API media player (no root
    // either), then the root-requiring backends. "termux"/"termux:<ms>" tries
    // the Termux:API media player first, then AAudio, then the root backends.
    // "agm"/"agm:<backend>" tries Qualcomm AGM first (some builds don't ship
    // libagmclient.so at all), then direct kernel PCM nodes, then ALSA-lib.
    // Node-based names open PCM nodes only; everything else is ALSA-lib only.
    std::vector<OpenStrategy> build_open_strategies(const std::string& device_name) {
        if (is_pulse_device(device_name)) {
            // Explicitly requested: PulseAudio first, then the other rootless
            // backends, then the root-requiring ones.
            return {OpenStrategy::PULSE, OpenStrategy::AAUDIO, OpenStrategy::TERMUXAPI,
                    OpenStrategy::AGM, OpenStrategy::NODES, OpenStrategy::LEGACY};
        }
        if (is_aaudio_device(device_name)) {
            return {OpenStrategy::AAUDIO, OpenStrategy::TERMUXAPI, OpenStrategy::AGM,
                    OpenStrategy::NODES, OpenStrategy::LEGACY};
        }
        if (is_termux_device(device_name)) {
            return {OpenStrategy::TERMUXAPI, OpenStrategy::AAUDIO, OpenStrategy::AGM,
                    OpenStrategy::NODES, OpenStrategy::LEGACY};
        }
        if (is_agm_device(device_name)) {
            return {OpenStrategy::AGM, OpenStrategy::NODES, OpenStrategy::LEGACY};
        }
        if (is_node_based_device(device_name)) {
            return {OpenStrategy::NODES};
        }
        // Plain "default"/"hw:0,0": when a PulseAudio daemon is running it
        // owns the sound card, so a raw ALSA/PCM open would either fail or
        // fight it. Try PulseAudio first in that case; on systems without a
        // daemon (stock Android/Termux) this is skipped entirely and the
        // behaviour is unchanged.
        if (pulse_worth_trying()) {
            // Keep the rootless backends in the chain behind PulseAudio. The
            // daemon socket is only detected passively (a dead daemon leaves
            // its socket file behind), so PULSE can still fail here - and
            // dropping straight to LEGACY would skip AAudio/Termux:API, which
            // usually work when PulseAudio does not.
            return {OpenStrategy::PULSE, OpenStrategy::AAUDIO, OpenStrategy::TERMUXAPI,
                    OpenStrategy::LEGACY};
        }
        return {OpenStrategy::LEGACY};
    }

    const char* strategy_label(OpenStrategy strategy, bool node_based) {
        switch (strategy) {
            case OpenStrategy::PULSE: return "PulseAudio daemon via libpulse (no root)";
            case OpenStrategy::AAUDIO: return "AAudio native audio via Android audio HAL (no root)";
            case OpenStrategy::TERMUXAPI: return "Termux:API media player via com.termux.api (no root)";
            case OpenStrategy::AGM: return "AGM playback via vendor agmplay subprocess (FIFO)";
            case OpenStrategy::NODES: return node_based ? "direct kernel PCM nodes (/dev/snd)" : "direct kernel PCM nodes";
            default: return "ALSA-lib device (default/hw:0,0)";
        }
    }

    // Supervises the retry loop: spawns one attempt thread at a time, abandons
    // it if it hangs in the kernel for kDeviceAttemptTimeoutMs, and retries
    // with backoff. Abandoned attempts keep running and hot-swap the device in
    // automatically if they eventually succeed. Node-based opens try each PCM
    // node in turn so one hung node (e.g. held by Android's audioserver) can't
    // hide the others; when a strategy exhausts its attempt budget the next
    // strategy (if any) takes over.
    void device_open_supervisor(std::shared_ptr<DeviceOpenShared> open,
                                AudioConfig cfg, std::string device_name) {
        const std::vector<OpenStrategy> strategies = build_open_strategies(device_name);
        size_t strategy_index = 0;
        uint32_t backoff_ms = kDeviceRetryBackoffMs;
        int attempt = 1;

        while (!open->shutdown.load() && strategy_index < strategies.size()) {
            const OpenStrategy strategy = strategies[strategy_index];
            const size_t max_attempts = strategy == OpenStrategy::PULSE ? kMaxPulseOpenAttempts
                                      : strategy == OpenStrategy::AAUDIO ? kMaxAaudioOpenAttempts
                                      : strategy == OpenStrategy::TERMUXAPI ? kMaxTermuxOpenAttempts
                                      : strategy == OpenStrategy::AGM ? kMaxAgmOpenAttempts
                                      : strategy == OpenStrategy::NODES ? kMaxNodeOpenAttempts
                                                                        : std::numeric_limits<size_t>::max();
            const bool node_based = strategy == OpenStrategy::NODES;

            std::vector<std::string> candidates;
            size_t candidate_index = 0;
            if (node_based) {
                candidates = build_node_candidates(device_name);
            }

            if (strategy_index > 0) {
                LOG_WARN("Audio device: previous strategy failed, falling back to "
                         << strategy_label(strategy, node_based));
                backoff_ms = kDeviceRetryBackoffMs;
            }

            size_t attempts_in_strategy = 0;
            while (!open->shutdown.load() && attempts_in_strategy < max_attempts) {
                // An earlier, abandoned attempt may have hot-swapped a real
                // device in while we were waiting. Stop churning through the
                // remaining strategies in that case - otherwise the client
                // keeps opening and discarding backends behind a device that
                // is already playing.
                {
                    std::lock_guard<std::mutex> lock(open->mutex);
                    if (open->player && open->player->is_open() &&
                        !open->player->is_placeholder()) {
                        LOG_INFO("Audio device: already playing on "
                                 << open->player->get_device_name()
                                 << "; stopping the open supervisor.");
                        return;
                    }
                }
                ++attempts_in_strategy;

                std::string candidate;
                if (node_based) {
                    candidate = candidates[candidate_index % candidates.size()];
                    ++candidate_index;
                    LOG_INFO("Audio device open attempt " << attempt << ": trying PCM node '" << candidate
                             << "' (node " << ((candidate_index - 1) % candidates.size()) + 1
                             << " of " << candidates.size() << ")");
                }

                std::shared_ptr<IAudioPlayer> device =
                    node_based ? std::shared_ptr<IAudioPlayer>(std::make_shared<DirectAlsaPlayer>())
                    : strategy == OpenStrategy::PULSE ? std::shared_ptr<IAudioPlayer>(std::make_shared<PulsePlayer>())
                    : strategy == OpenStrategy::AAUDIO ? std::shared_ptr<IAudioPlayer>(std::make_shared<AaudioFifoPlayer>())
                    : strategy == OpenStrategy::TERMUXAPI ? std::shared_ptr<IAudioPlayer>(std::make_shared<TermuxApiPlayer>())
                    : strategy == OpenStrategy::AGM ? std::shared_ptr<IAudioPlayer>(std::make_shared<AgmFifoPlayer>())
                                                    : std::shared_ptr<IAudioPlayer>(std::make_shared<AlsaPlayer>());
                auto finished = std::make_shared<std::atomic<bool>>(false);
                std::thread attempt_thread([open, cfg, device_name, candidate, device, finished]() {
                    if (!candidate.empty()) {
                        attempt_device_open(open, device, [&]() {
                            return std::static_pointer_cast<DirectAlsaPlayer>(device)->open_candidate_only(cfg, candidate);
                        });
                    } else {
                        attempt_device_open(open, device, [&]() {
                            return device->open(cfg, device_name);
                        });
                    }
                    finished->store(true);
                });

                const uint32_t attempt_timeout_ms = strategy == OpenStrategy::PULSE
                    ? kPulseAttemptTimeoutMs : kDeviceAttemptTimeoutMs;
                uint32_t waited = 0;
                for (; waited < attempt_timeout_ms && !open->shutdown.load() && !finished->load();
                     waited += kDeviceAttemptPollMs) {
                    sleep_ms(kDeviceAttemptPollMs);
                }

                if (!open->shutdown.load()) {
                    bool ok = false;
                    {
                        std::lock_guard<std::mutex> lock(open->mutex);
                        ok = open->result;
                    }
                    if (finished->load() && ok) {
                        attempt_thread.join();
                        LOG_INFO("Audio device opened successfully on attempt " << attempt
                                 << ": " << device->get_device_name());
                        return;
                    }
                }

                if (open->shutdown.load()) {
                    if (attempt_thread.joinable()) attempt_thread.detach();
                    break;
                }

                if (!finished->load()) {
                    LOG_WARN("Audio device open attempt " << attempt
                             << (candidate.empty() ? "" : " on '" + candidate + "'")
                             << " hung in a kernel call (" << attempt_timeout_ms << "ms). Abandoning it and trying "
                             << (node_based ? "the next PCM node" : "again with a fresh player") << "; if the abandoned "
                             << "attempt later succeeds the real device is hot-swapped in automatically.");
                    attempt_thread.detach();
                } else {
                    attempt_thread.join();
                    LOG_INFO("Audio device open attempt " << attempt
                             << (candidate.empty() ? "" : " on '" + candidate + "'") << " failed; retrying.");
                }

                ++attempt;
                for (waited = 0; waited < backoff_ms && !open->shutdown.load(); waited += kDeviceAttemptPollMs) {
                    sleep_ms(kDeviceAttemptPollMs);
                }
                backoff_ms = std::min(backoff_ms * 2, kDeviceRetryMaxBackoffMs);
            }

            if (!open->shutdown.load()) {
                ++strategy_index;
            }
        }

        // Reached only when every strategy exhausted its budget (a plain
        // node-based device with all nodes hung).
        if (!open->shutdown.load()) {
            LOG_ERROR("Could not open any audio device after exhausting all strategies.");
            LOG_ERROR("If every PCM node hangs or fails while running as root, Android's 'audioserver' is "
                      << "most likely holding the primary PCM device. Run (as root):");
            LOG_ERROR("    stop audioserver");
            LOG_ERROR("then re-run the client. Re-enable Android audio later with: start audioserver");
            // Only now is the troubleshooting wall actually warranted: every
            // backend has been tried and failed. It used to print the moment
            // the first open exceeded its budget, while the supervisor was
            // still working and a device was usually seconds away.
            AndroidHelpers::print_android_troubleshooting_tips();
        }
        LOG_INFO("Audio device open thread exiting.");
    }
}

AudioRouterClient::AudioRouterClient(const ClientConfig& config)
    : config_(config),
      is_running_(false),
      state_(ClientState::STOPPED),
      open_(std::make_shared<DeviceOpenShared>()),
      jitter_buffer_(config.target_latency_ms),
      last_packet_time_ms_(0),
      last_rtt_us_(0) {}

AudioRouterClient::~AudioRouterClient() {
    stop();
}

bool AudioRouterClient::start() {
    if (is_running_) return true;

    LOG_INFO("=================================================");
    LOG_INFO(" Starting AudioRouter Android ALSA Client Engine");
    LOG_INFO("=================================================");

    // -T/--termux-segment overrides the file length of the Termux:API
    // backend (how often the media player switches files; the end-to-end
    // delay is unaffected). It rewrites the device name to the equivalent
    // termux:<ms> form so every code path sees one source of truth.
    if (config_.termux_segment_ms != 0) {
        if (is_termux_device(config_.device_name)) {
            config_.device_name = "termux:" + std::to_string(config_.termux_segment_ms);
            LOG_INFO("-T/--termux-segment: Termux:API file length set to "
                     << config_.termux_segment_ms << " ms (one ~prepare-time switch pause per "
                     << config_.termux_segment_ms / 1000 << " s)");
        } else {
            LOG_WARN("-T/--termux-segment is ignored for device '" << config_.device_name
                     << "' (Termux:API backend only)");
        }
    }

    // Check Android root permissions
    if (AndroidHelpers::is_running_as_root()) {
        LOG_INFO("Running with root privileges (UID 0). Direct ALSA access enabled.");
        AndroidHelpers::fix_snd_permissions();
        if (is_aaudio_device(config_.device_name)) {
            LOG_INFO("AAudio requested while running as root: the AAudio stream is opened "
                     "in-process, exactly like the standalone stream_daemon (which runs as "
                     "root and works). No privilege games needed.");
        }
    } else {
        LOG_WARN("Not running as root. If ALSA device fails to open, run 'su' or 'sudo' in Termux.");
    }

    // Open UDP Socket
    if (!socket_.open()) {
        LOG_ERROR("Failed to open client UDP socket");
        return false;
    }

    socket_.set_buffer_sizes(1024 * 1024, 1024 * 1024);
    socket_.set_qos_priority(true);

    if (config_.usb_mode) {
        // Voice over USB: the PC-side "adb reverse tcp:PORT tcp:PORT" tunnel
        // exposes the server at the phone's loopback. adb cannot forward UDP,
        // so this client runs a local relay: the protocol engine sends UDP to
        // usb_relay_udp_ (a loopback socket), and the relay re-frames each
        // datagram over the TCP tunnel. The Wi-Fi/VPN interface machinery
        // below does not apply to lo.
        if (!config_.bind_iface.empty()) {
            LOG_WARN("--usb active: -b/--bind (" << config_.bind_iface << ") is ignored; the stream runs over the USB cable via loopback");
        }
        if (config_.auto_discover) {
            LOG_WARN("--usb active: --discover is ignored; connecting straight to the adb reverse tunnel");
        }

        if (!usb_relay_udp_.open()) {
            LOG_ERROR("USB mode: failed to open relay UDP socket");
            socket_.close();
            return false;
        }
        if (!usb_relay_udp_.bind(0, "127.0.0.1")) {
            LOG_ERROR("USB mode: failed to bind relay UDP socket on loopback");
            socket_.close();
            return false;
        }
        // The protocol engine targets the relay's loopback address; the relay
        // forwards those datagrams over the USB TCP tunnel.
        server_addr_ = usb_relay_udp_.get_local_address();
        if (!server_addr_.is_valid()) {
            LOG_ERROR("USB mode: cannot determine relay UDP address");
            socket_.close();
            return false;
        }
        LOG_INFO("USB mode: engine -> loopback relay at " << server_addr_.to_string()
                 << ", tunnel -> 127.0.0.1:" << config_.server_port
                 << " (start 'adb reverse tcp:" << config_.server_port
                 << " tcp:" << config_.server_port << "' on the PC)");

        // Relay must be up before the handshake below: its first CONNECT_REQ
        // is what the tunnel carries. The loop exits on stop_requested_ only
        // (is_running_ is still false here), so failure paths stay clean.
        usb_relay_thread_ = std::thread(&AudioRouterClient::usb_relay_thread_fn, this);
    } else {
        // Optional VPN bypass: pin the socket to the physical Wi-Fi interface so
        // an Android VPN tunnel (tun0) cannot swallow the LAN traffic to the PC.
        // Requires root (SO_BINDTODEVICE / CAP_NET_RAW); best-effort otherwise.
        if (!config_.bind_iface.empty()) {
            std::string iface = config_.bind_iface;
            if (iface == "auto") {
                iface = UdpSocket::pick_physical_interface();
                if (iface.empty()) {
                    LOG_WARN("No physical interface found to bind to; continuing with default routing");
                } else {
                    LOG_INFO("Auto-selected physical interface '" << iface << "' for VPN bypass");
                }
            }
            if (!iface.empty()) {
                socket_.bind_to_interface(iface);
            }
        } else {
            auto ifaces = UdpSocket::get_local_interfaces();
            bool vpn_active = false;
            for (const auto& info : ifaces) {
                if (info.is_loopback || !info.is_up) continue;
                if (info.name.rfind("tun", 0) == 0 || info.name.rfind("ppp", 0) == 0) vpn_active = true;
            }
            if (vpn_active) {
                LOG_WARN("VPN tunnel detected but no -b/--bind given. If the handshake fails, rerun with "
                         << "'-b auto' (or '-b wlan0') to bypass the VPN: e.g. -b auto");
            }
        }

        // Auto-discovery if requested
        if (config_.auto_discover) {
            state_ = ClientState::DISCOVERING;
            if (!discover_server(server_addr_)) {
                LOG_ERROR("Server auto-discovery failed. Please specify server IP manually: -s <IP>");
                socket_.close();
                return false;
            }
        } else {
            server_addr_ = SocketAddress(config_.server_ip, config_.server_port);
            if (!server_addr_.is_valid()) {
                LOG_ERROR("Invalid server address: " << config_.server_ip << ":" << config_.server_port);
                socket_.close();
                return false;
            }
        }
    }

    LOG_INFO("Target Server: " << server_addr_.to_string());

    // Termux:API playback must run as the Termux app user: its listen-socket
    // protocol only accepts that uid and com.termux.api reads the segment
    // files from the Termux sandbox. The interface binding above (-b/--bind)
    // needed root; now that the socket is pinned, drop to the Termux app
    // user so the backend runs in-process with working sandbox access.
    // AAudio also renders as that user; the root backends (AGM/ALSA/direct)
    // are given up by the drop.
    // The PulseAudio daemon is a per-user service: its socket lives in the
    // Termux user's runtime dir and is owned by that uid, so a root client
    // cannot connect to it (pa_simple_new -> "Connection refused", preceded by
    // libpulse trying to create //.config/pulse because root's HOME is "/").
    // The same privilege drop the Termux:API backend uses fixes it: bind the
    // socket as root for -b/--bind, then become the Termux app user before the
    // backend opens.
    const bool pulse_needs_drop =
        is_pulse_device(config_.device_name) && AndroidHelpers::is_running_as_root();
    if (pulse_needs_drop) {
        LOG_INFO("PulseAudio backend: running as root, but the daemon is a per-user "
                 "service; dropping to the Termux app user after the socket binding.");
    }

    if ((is_termux_device(config_.device_name) || pulse_needs_drop) &&
        AndroidHelpers::is_running_as_root()) {
        // Pre-create the segment cache — including the SELinux labels the
        // Termux:API app needs to read it — while we still have root: after
        // setuid() the process cannot relabel files, and the player only
        // recycles these pre-labeled pool inodes.
        if (is_termux_device(config_.device_name)) {
            TermuxApiPlayer::prepare_cache_as_root();
        }
        uid_t termux_uid = 0;
        gid_t termux_gid = 0;
        std::string termux_home;
        const char* backend_label = pulse_needs_drop ? "PulseAudio backend" : "Termux:API backend";
        if (!AndroidHelpers::termux_user(&termux_uid, &termux_gid, &termux_home)) {
            LOG_WARN(backend_label << ": Termux app user not found; continuing as root "
                     "(the PulseAudio daemon will likely refuse the connection)");
        } else if (::setgroups(0, nullptr) != 0 || ::setgid(termux_gid) != 0 ||
                   ::setuid(termux_uid) != 0) {
            LOG_WARN(backend_label << ": could not drop to the Termux app user ("
                     << std::strerror(errno) << "); continuing as root");
        } else {
            ::setenv("HOME", termux_home.c_str(), 1);
            // libpulse locates the daemon through XDG_RUNTIME_DIR / HOME, which
            // still describe root's view until rewritten here.
            //
            // Do NOT invent an XDG_RUNTIME_DIR: Termux's PulseAudio keeps its
            // runtime dir under $HOME/.config/pulse/<machine-id>-runtime, not
            // in $PREFIX/var/run. Pointing XDG_RUNTIME_DIR at the latter made
            // libpulse look in an empty directory and miss a daemon that was
            // running perfectly well. Clearing it lets libpulse fall back to
            // its HOME-based lookup, which is what the Termux daemon uses.
            if (pulse_needs_drop) {
                ::unsetenv("XDG_RUNTIME_DIR");
            }
            LOG_INFO(backend_label << ": socket bound as root; dropped privileges to the "
                     "Termux app user (uid " << termux_uid << ", gid " << termux_gid
                     << ") so the per-user audio service is reachable");
        }
    }

    // Create Audio Player (ALSA or Dummy)
    {
        std::lock_guard<std::mutex> lock(open_->mutex);
        open_->player = config_.use_dummy_player ? std::make_shared<DummyPlayer>() : nullptr;
    }

    // Connect & Handshake with Windows Server
    state_ = ClientState::CONNECTING;
    if (!perform_handshake()) {
        if (config_.usb_mode) {
            LOG_ERROR("Failed to connect to the USB tunneled server at 127.0.0.1:" << config_.server_port);
            LOG_ERROR("Check that on the PC the tunnel is up and the server is listening:");
            LOG_ERROR("    adb reverse tcp:" << config_.server_port << " tcp:" << config_.server_port);
            LOG_ERROR("    audiorouter_server.exe --usb");
        } else {
            LOG_ERROR("Failed to connect to Windows AudioRouter Server at " << server_addr_.to_string());
        }
        socket_.close();
        return false;
    }

    // Configure Jitter Buffer with negotiated stream parameters
    jitter_buffer_.configure(audio_config_, config_.target_latency_ms);

    is_running_ = true;
    state_ = ClientState::STREAMING;
    last_packet_time_ms_ = get_time_ms();

    // Start worker threads BEFORE opening the audio device. This keeps the
    // server heartbeat alive (its watchdog would otherwise disconnect the
    // client) and keeps Ctrl+C responsive even if the device open hangs.
    net_thread_ = std::thread(&AudioRouterClient::network_receive_thread, this);
    playback_thread_ = std::thread(&AudioRouterClient::audio_playback_thread, this);
    heartbeat_thread_ = std::thread(&AudioRouterClient::heartbeat_thread, this);

    // AAudio runs in-process (like the standalone stream_daemon, which is
    // proven to work as root), so no privilege handling is needed here: the
    // socket binding (-b auto) and the AGM/ALSA fallback keep working in the
    // same process either way.

    // Open the audio player on a bounded, cancellable path: a hung ALSA /
    // kernel driver must never block the main thread or stall shutdown.
    // The Termux:API backend needs a larger budget: its open() probes the
    // app over `am broadcast`, which takes ~2-4 s when the app process is
    // cold (the 3 s ALSA budget made it flash the dummy sink on device).
    open_player_with_timeout(config_.device_name,
                             is_termux_device(config_.device_name)
                                 ? kTermuxPlayerOpenTimeoutMs
                             : is_pulse_device(config_.device_name)
                                 ? kPulsePlayerOpenTimeoutMs
                                 : kPlayerOpenTimeoutMs);

    LOG_INFO("AudioRouter Client connected and streaming directly to Android speakers!");
    return true;
}

void AudioRouterClient::open_player_with_timeout(const std::string& device_name, uint32_t timeout_ms) {
    if (config_.use_dummy_player) {
        std::lock_guard<std::mutex> lock(open_->mutex);
        open_->player->open(audio_config_, device_name);
        return;
    }

    // Route the codec to the speaker before any open attempt so a successful
    // open has an audible path right away. Best effort: some devices name the
    // mixer controls differently. NOT for AAudio or Termux:API: the audio HAL
    // (AudioFlinger) owns the mixer for those streams, and force-routing the
    // codec from outside (tinymix) can jam the HAL's session start - the
    // stream opens but never renders.
    // Also skipped for PulseAudio: the daemon owns the mixer for its sinks,
    // and force-routing the codec underneath it can wedge the sink.
    if (!is_aaudio_device(device_name) && !is_termux_device(device_name) &&
        !is_pulse_device(device_name) && !pulse_worth_trying()) {
        AndroidHelpers::apply_speaker_routing();
    }

    {
        std::lock_guard<std::mutex> lock(open_->mutex);
        open_->pending = true;
        open_->result = false;
    }

    // The supervisor owns its own references to the device-open state and
    // never touches this object, so a hung kernel call cannot block shutdown.
    device_thread_ = std::thread(device_open_supervisor, open_, audio_config_, device_name);

    bool completed = false;
    bool opened_ok = false;
    {
        std::unique_lock<std::mutex> lock(open_->mutex);
        completed = open_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                       [this]() { return !open_->pending; });
        opened_ok = completed && open_->result;
    }

    if (opened_ok) {
        std::lock_guard<std::mutex> lock(open_->mutex);
        LOG_INFO("Audio device opened successfully: " << open_->player->get_device_name());
        return;
    }

    // Do NOT install a dummy sink here. The open supervisor is still running in
    // the background and a real backend usually lands a few seconds later; a
    // placeholder in the slot only invites the "is something already playing?"
    // ambiguity that previously discarded real devices, and it silently
    // consumes frames (and prints a full ALSA troubleshooting wall) while the
    // real open is still in flight.
    //
    // Leaving open_->player empty is safe: the playback thread already treats a
    // null player as "nothing to write to yet" and paces itself, so audio
    // simply starts the moment the supervisor hot-swaps a device in.
    if (!completed) {
        LOG_WARN("Audio device '" << device_name << "' has not opened yet after "
                 << timeout_ms << " ms; still trying in the background. Playback starts "
                 "as soon as a backend opens.");
    } else {
        LOG_WARN("Could not open audio device '" << device_name
                 << "' yet; still trying in the background. Playback starts as soon as a "
                 "backend opens.");
    }
}

void AudioRouterClient::stop() {
    if (state_ == ClientState::STOPPED && !is_running_) return;  // idempotent

    stop_requested_ = true;
    is_running_ = false;
    open_->shutdown.store(true);

    // Send DISCONNECT_REQ to Windows server so it un-mutes PC speaker immediately
    if (server_addr_.is_valid()) {
        std::vector<uint8_t> dis_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::DisconnectPayload));
        auto* dis_hdr = reinterpret_cast<protocol::CommonHeader*>(dis_buf.data());
        auto* dis_pay = reinterpret_cast<protocol::DisconnectPayload*>(dis_buf.data() + sizeof(protocol::CommonHeader));

        dis_hdr->magic = protocol::MAGIC;
        dis_hdr->version = protocol::CURRENT_VERSION;
        dis_hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::DISCONNECT_REQ);
        dis_hdr->flags = protocol::FLAG_NONE;
        dis_hdr->seq_num = 0;
        dis_hdr->timestamp_us = get_time_us();
        dis_hdr->payload_size = sizeof(protocol::DisconnectPayload);

        dis_pay->reason_code = 0;
        std::strncpy(dis_pay->reason, "Client closed", sizeof(dis_pay->reason) - 1);

        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (socket_.is_open()) {
            socket_.send_to(dis_buf.data(), dis_buf.size(), server_addr_);
        }
    }

    // Close UDP socket to unblock network receive thread
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        socket_.close();
    }

    // Close audio player to unblock playback thread immediately
    {
        std::lock_guard<std::mutex> lock(open_->mutex);
        if (open_->player) {
            open_->player->close();
        }
    }

    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }
    if (net_thread_.joinable()) {
        net_thread_.join();
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    if (usb_relay_thread_.joinable()) {
        // The relay's select()/read waits are time-bounded (<= 50 ms), so it
        // exits promptly once stop_requested_ is set.
        usb_relay_thread_.join();
    }
    if (device_thread_.joinable()) {
        // The device open thread may be stuck in a kernel call; never join it.
        // It self-terminates on stop_requested_ and closes whatever it opened.
        device_thread_.detach();
    }

    state_ = ClientState::STOPPED;
    LOG_INFO("AudioRouter Client stopped successfully.");
}

bool AudioRouterClient::is_running() const {
    return is_running_;
}

ClientState AudioRouterClient::get_state() const {
    return state_.load();
}

ClientStats AudioRouterClient::get_stats() const {
    auto j_stats = jitter_buffer_.get_stats();
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ClientStats s = stats_;
    s.jitter_stats = j_stats;
    s.round_trip_time_us = last_rtt_us_.load();
    return s;
}

bool AudioRouterClient::discover_server(SocketAddress& out_server_addr) {
    LOG_INFO("Discovering Windows AudioRouter server on network...");
    socket_.set_broadcast(true);
    socket_.set_receive_timeout_ms(500);

    std::vector<uint8_t> disc_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::DiscoveryReqPayload));
    auto* hdr = reinterpret_cast<protocol::CommonHeader*>(disc_buf.data());
    auto* pay = reinterpret_cast<protocol::DiscoveryReqPayload*>(disc_buf.data() + sizeof(protocol::CommonHeader));

    hdr->magic = protocol::MAGIC;
    hdr->version = protocol::CURRENT_VERSION;
    hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::DISCOVERY_REQ);
    hdr->flags = protocol::FLAG_NONE;
    hdr->seq_num = 0;
    hdr->timestamp_us = get_time_us();
    hdr->payload_size = sizeof(protocol::DiscoveryReqPayload);

    std::strncpy(pay->client_name, "AudioRouter-Android", sizeof(pay->client_name) - 1);
    pay->client_version = protocol::CURRENT_VERSION;

    // Send discovery broadcast and targeted probes to hotspot defaults
    std::vector<std::string> probe_ips = {
        "255.255.255.255",
        "192.168.43.1",   // Android hotspot gateway
        "192.168.137.1",  // Windows mobile hotspot gateway
        "192.168.1.1",
        "192.168.0.1",
        "127.0.0.1"
    };

    std::vector<uint8_t> recv_buf(4096);

    for (int attempt = 0; attempt < 3; ++attempt) {
        for (const auto& ip : probe_ips) {
            SocketAddress target(ip, config_.server_port);
            socket_.send_to(disc_buf.data(), disc_buf.size(), target);
        }

        SocketAddress responder;
        int bytes = socket_.receive_from(recv_buf.data(), recv_buf.size(), responder);
        if (bytes >= static_cast<int>(sizeof(protocol::CommonHeader) + sizeof(protocol::DiscoveryRespPayload))) {
            const auto* resp_hdr = reinterpret_cast<const protocol::CommonHeader*>(recv_buf.data());
            if (protocol::is_valid_header(*resp_hdr, static_cast<size_t>(bytes)) &&
                resp_hdr->msg_type == static_cast<uint8_t>(protocol::MsgType::DISCOVERY_RESP)) {
                const auto* resp_pay = reinterpret_cast<const protocol::DiscoveryRespPayload*>(recv_buf.data() + sizeof(protocol::CommonHeader));
                out_server_addr = responder;
                LOG_INFO("Discovered server '" << resp_pay->server_name << "' at " << responder.to_string()
                         << " (PC Muted: " << (resp_pay->pc_muted ? "Yes" : "No") << ")");
                return true;
            }
        }
        sleep_ms(200);
    }

    return false;
}

bool AudioRouterClient::perform_handshake() {
    if (stop_requested_) return false;

    LOG_INFO("Initiating handshake with server at " << server_addr_.to_string() << "...");

    std::vector<uint8_t> req_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::ConnectReqPayload));
    auto* req_hdr = reinterpret_cast<protocol::CommonHeader*>(req_buf.data());
    auto* req_pay = reinterpret_cast<protocol::ConnectReqPayload*>(req_buf.data() + sizeof(protocol::CommonHeader));

    req_hdr->magic = protocol::MAGIC;
    req_hdr->version = protocol::CURRENT_VERSION;
    req_hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::CONNECT_REQ);
    req_hdr->flags = protocol::FLAG_NONE;
    req_hdr->seq_num = 0;
    req_hdr->timestamp_us = get_time_us();
    req_hdr->payload_size = sizeof(protocol::ConnectReqPayload);

    std::strncpy(req_pay->client_name, "AudioRouter-Android-Termux", sizeof(req_pay->client_name) - 1);
    req_pay->preferred_sample_rate = 48000;
    req_pay->preferred_channels = 2;
    req_pay->preferred_format = static_cast<uint8_t>(AudioSampleFormat::PCM_S16LE);
    req_pay->target_latency_ms = static_cast<uint16_t>(config_.target_latency_ms);

    socket_.set_receive_timeout_ms(1000);
    std::vector<uint8_t> recv_buf(4096);

    for (int retry = 0; retry < 5; ++retry) {
        if (stop_requested_) return false;

        LOG_DEBUG("Sending CONNECT_REQ (attempt " << (retry + 1) << "/5)...");
        socket_.send_to(req_buf.data(), req_buf.size(), server_addr_);

        SocketAddress from;
        int bytes = socket_.receive_from(recv_buf.data(), recv_buf.size(), from);

        if (bytes >= static_cast<int>(sizeof(protocol::CommonHeader))) {
            const auto* hdr = reinterpret_cast<const protocol::CommonHeader*>(recv_buf.data());
            if (!protocol::is_valid_header(*hdr, static_cast<size_t>(bytes))) continue;

            if (hdr->msg_type == static_cast<uint8_t>(protocol::MsgType::CONNECT_ACK)) {
                if (bytes >= static_cast<int>(sizeof(protocol::CommonHeader) + sizeof(protocol::ConnectAckPayload))) {
                    const auto* ack = reinterpret_cast<const protocol::ConnectAckPayload*>(recv_buf.data() + sizeof(protocol::CommonHeader));
                    
                    audio_config_.sample_rate = ack->sample_rate;
                    audio_config_.channels = ack->channels;
                    audio_config_.format = static_cast<AudioSampleFormat>(ack->format);
                    audio_config_.frames_per_packet = ack->frames_per_packet;

                    LOG_INFO("-------------------------------------------------");
                    LOG_INFO(" CONNECT_ACK Received from Windows Server!");
                    LOG_INFO(" Stream Format: " << audio_config_.to_string());
                    LOG_INFO(" PC Speaker Silenced: " << (ack->pc_speaker_muted ? "YES (Audio routed to Android)" : "NO"));
                    LOG_INFO(" Server Message: " << ack->status_msg);
                    LOG_INFO("-------------------------------------------------");
                    return true;
                }
            } else if (hdr->msg_type == static_cast<uint8_t>(protocol::MsgType::CONNECT_NAK)) {
                if (bytes >= static_cast<int>(sizeof(protocol::CommonHeader) + sizeof(protocol::ConnectNakPayload))) {
                    const auto* nak = reinterpret_cast<const protocol::ConnectNakPayload*>(recv_buf.data() + sizeof(protocol::CommonHeader));
                    LOG_ERROR("Server rejected connection: " << nak->reason);
                    return false;
                }
            }
        }

        sleep_ms(300);
    }

    return false;
}

void AudioRouterClient::network_receive_thread() {
    std::vector<uint8_t> recv_buf(65536);
    socket_.set_receive_timeout_ms(200);

    while (is_running_ && !stop_requested_) {
        SocketAddress sender;
        int bytes;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (!is_running_ || stop_requested_) break;
            bytes = socket_.receive_from(recv_buf.data(), recv_buf.size(), sender);
        }

        if (bytes <= 0) {
            if (!is_running_) break;
            continue;
        }

        if (static_cast<size_t>(bytes) < sizeof(protocol::CommonHeader)) continue;

        const auto* hdr = reinterpret_cast<const protocol::CommonHeader*>(recv_buf.data());
        if (!protocol::is_valid_header(*hdr, static_cast<size_t>(bytes))) continue;

        last_packet_time_ms_ = get_time_ms();

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.packets_received++;
            stats_.bytes_received += bytes;
        }

        auto msg_type = static_cast<protocol::MsgType>(hdr->msg_type);

        if (msg_type == protocol::MsgType::AUDIO_DATA) {
            if (static_cast<size_t>(bytes) >= sizeof(protocol::AudioPacketHeader)) {
                const auto* audio_hdr = reinterpret_cast<const protocol::AudioPacketHeader*>(recv_buf.data());
                if (protocol::validate_audio_header(*audio_hdr, static_cast<size_t>(bytes))) {
                    const void* pcm_data = recv_buf.data() + sizeof(protocol::AudioPacketHeader);

                    jitter_buffer_.push_packet(
                        audio_hdr->common.seq_num,
                        audio_hdr->common.timestamp_us,
                        pcm_data,
                        audio_hdr->num_frames
                    );
                } else {
                    LOG_WARN("Discarded invalid/malformed audio packet.");
                }
            }
        } else if (msg_type == protocol::MsgType::HEARTBEAT_PONG) {
            if (static_cast<size_t>(bytes) >= sizeof(protocol::CommonHeader) + sizeof(protocol::HeartbeatPayload)) {
                const auto* pong = reinterpret_cast<const protocol::HeartbeatPayload*>(recv_buf.data() + sizeof(protocol::CommonHeader));
                uint64_t now_us = get_time_us();
                if (now_us > pong->orig_timestamp_us) {
                    uint32_t rtt = static_cast<uint32_t>(now_us - pong->orig_timestamp_us);
                    last_rtt_us_ = rtt;
                }
            }
        } else if (msg_type == protocol::MsgType::DISCONNECT_ACK || msg_type == protocol::MsgType::DISCONNECT_REQ) {
            LOG_INFO("Received disconnect notification from server.");
            is_running_ = false;
            break;
        }
    }
}

void AudioRouterClient::audio_playback_thread() {
    const size_t period_frames = audio_config_.frames_per_packet > 0 ? audio_config_.frames_per_packet : 240;
    std::vector<int16_t> play_buffer(period_frames * audio_config_.channels);
    const uint32_t frame_duration_ms = static_cast<uint32_t>((period_frames * 1000) / (audio_config_.sample_rate > 0 ? audio_config_.sample_rate : 48000));
    const uint32_t fallback_sleep_ms = (frame_duration_ms > 0) ? frame_duration_ms : 2;

    while (is_running_) {
        // Read frames from jitter buffer
        size_t frames = jitter_buffer_.pop_frames(play_buffer.data(), period_frames);

        if (frames > 0) {
            std::shared_ptr<IAudioPlayer> player;
            {
                std::lock_guard<std::mutex> lock(open_->mutex);
                player = open_->player;
            }
            if (player && player->is_open()) {
                size_t written = player->write_frames(play_buffer.data(), frames);
                if (written > 0) {
                    uint32_t backend_ms = 0;
                    {
                        std::lock_guard<std::mutex> lock(stats_mutex_);
                        stats_.frames_played += written;
                        // Sample the real backend delay (pipe + in-stream buffers)
                        // so the status line shows the actual audio latency on
                        // top of the jitter buffer.
                        const uint32_t rate = audio_config_.sample_rate > 0 ? audio_config_.sample_rate : 48000;
                        stats_.audio_backend_delay_ms =
                            static_cast<uint32_t>((player->get_buffer_delay_frames() * 1000ULL) / rate);
                        backend_ms = stats_.audio_backend_delay_ms;
                    }
                    // Self-pace against the device for FIFO-style backends
                    // (AAudio, AGM): their pipe can otherwise accumulate a
                    // full backlog (the jitter prefill burst + stalls), which
                    // shows up as a constant multi-hundred-ms audio delay.
                    // Sleep so the backend drains back toward ~40 ms. Direct
                    // backends (ALSA, PulseAudio) pace themselves on blocking
                    // writes and report their true buffered delay, so they are
                    // unaffected.
                    //
                    // The gate is the backend's own reported delay, NOT the
                    // requested device name: after a fallback (e.g. -d pulse
                    // failing over to AAudio) get_device_name() still returns
                    // the name the user asked for, so a name-based check
                    // silently disabled pacing for the backend that actually
                    // ran - the AAudio pipe then sat at a constant ~358 ms.
                    if (backend_ms > 60 && player->needs_playback_pacing()) {
                        sleep_ms(backend_ms - 40);
                    }
                } else {
                    // Audio device returned 0 frames written or was busy; pace the thread
                    sleep_ms(fallback_sleep_ms);
                }
            } else {
                sleep_ms(fallback_sleep_ms);
            }
        } else {
            sleep_ms(fallback_sleep_ms);
        }
    }
}

void AudioRouterClient::heartbeat_thread() {
    std::vector<uint8_t> ping_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::HeartbeatPayload));
    auto* ping_hdr = reinterpret_cast<protocol::CommonHeader*>(ping_buf.data());
    auto* ping_pay = reinterpret_cast<protocol::HeartbeatPayload*>(ping_buf.data() + sizeof(protocol::CommonHeader));

    while (is_running_ && !stop_requested_) {
        auto j_stats = jitter_buffer_.get_stats();

        ping_hdr->magic = protocol::MAGIC;
        ping_hdr->version = protocol::CURRENT_VERSION;
        ping_hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::HEARTBEAT_PING);
        ping_hdr->flags = protocol::FLAG_NONE;
        ping_hdr->seq_num = 0;
        ping_hdr->timestamp_us = get_time_us();
        ping_hdr->payload_size = sizeof(protocol::HeartbeatPayload);

        ping_pay->orig_timestamp_us = get_time_us();
        ping_pay->client_buffer_level_frames = static_cast<uint32_t>(jitter_buffer_.available_frames());
        ping_pay->packets_received = static_cast<uint32_t>(stats_.packets_received);
        ping_pay->packets_lost = static_cast<uint32_t>(j_stats.packets_lost);
        ping_pay->buffer_underruns = static_cast<uint32_t>(j_stats.underruns);
        ping_pay->buffer_overruns = static_cast<uint32_t>(j_stats.overruns);

        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (!is_running_ || stop_requested_) break;
            socket_.send_to(ping_buf.data(), ping_buf.size(), server_addr_);
        }

        // Watchdog check for server connection timeout
        uint64_t now_ms = get_time_ms();
        uint64_t last_packet = last_packet_time_ms_.load();
        if (!stop_requested_ && last_packet > 0 && (now_ms - last_packet) > config_.reconnect_timeout_ms) {
            LOG_WARN("Server packet stream timeout (" << (now_ms - last_packet) << "ms). Attempting to re-handshake...");
            perform_handshake();
            last_packet_time_ms_ = get_time_ms();
        }

        sleep_ms(1000);
    }
}

void AudioRouterClient::usb_relay_thread_fn() {
    const SocketAddress tunnel_addr("127.0.0.1", config_.server_port);
    uint8_t datagram_buf[tunnel::kMaxFramePayload];

    while (!stop_requested_) {
        // (Re)establish the TCP leg of the tunnel. adb reverse rebinds the
        // device-side listener automatically, so retrying here is enough to
        // ride through unplug/replug and server restarts.
        if (!usb_tcp_.is_open()) {
            if (usb_tcp_.connect(tunnel_addr, 200)) {
                usb_tcp_.set_tcp_nodelay(true);
                usb_tcp_.set_non_blocking(true);
                // Bound the tunnel's kernel buffers (~320 ms of audio) so a
                // stalled hop cannot pile up seconds of stale audio (see the
                // server relay's matching cap).
                usb_tcp_.set_buffer_sizes(64 * 1024, 64 * 1024);
                usb_rx_buf_.clear();  // stale partial frame from a previous session
                usb_relay_connected_.store(true);
                LOG_INFO("USB tunnel: connected to adb reverse at 127.0.0.1:" << config_.server_port);
            } else {
                usb_relay_connected_.store(false);
                usb_tcp_.close();  // failed connect may leave the socket dead; retry clean
                sleep_ms(300);
                continue;
            }
        }

        // Only select the UDP leg while the TCP tunnel is up: with the tunnel
        // down, engine datagrams are left queued in the relay socket's kernel
        // buffer and are flushed as soon as the TCP leg (re)connects, so the
        // first CONNECT_REQ survives a tunnel that comes up late.
        int m = tunnel::select2(&usb_tcp_, usb_tcp_.is_open() ? &usb_relay_udp_ : nullptr, 50);
        if (m & 1) {
            auto r = tunnel::read_frame(usb_tcp_, usb_rx_buf_, usb_frame_);
            if (r == tunnel::RecvResult::Frame) {
                if (usb_engine_peer_.is_valid()) {
                    usb_relay_udp_.send_to(usb_frame_.data(), usb_frame_.size(), usb_engine_peer_);
                }
            } else if (r == tunnel::RecvResult::Closed || r == tunnel::RecvResult::Error) {
                LOG_WARN("USB tunnel: server connection closed; reconnecting...");
                usb_tcp_.close();
                usb_relay_connected_.store(false);
            }
        }
        if (m & 2) {
            SocketAddress from;
            int n = usb_relay_udp_.receive_from(datagram_buf, sizeof(datagram_buf), from);
            if (n > 0) {
                usb_engine_peer_ = from;  // the protocol engine's loopback address
                if (!tunnel::write_frame(usb_tcp_, datagram_buf, static_cast<size_t>(n))) {
                    usb_tcp_.close();
                    usb_relay_connected_.store(false);
                }
            }
        }
    }

    usb_tcp_.close();
    usb_relay_connected_.store(false);
}

bool AudioRouterClient::send_pc_mute_command(bool mute) {
    if (!server_addr_.is_valid()) return false;

    std::vector<uint8_t> cmd_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::ControlCmdPayload));
    auto* hdr = reinterpret_cast<protocol::CommonHeader*>(cmd_buf.data());
    auto* pay = reinterpret_cast<protocol::ControlCmdPayload*>(cmd_buf.data() + sizeof(protocol::CommonHeader));

    hdr->magic = protocol::MAGIC;
    hdr->version = protocol::CURRENT_VERSION;
    hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::CONTROL_CMD);
    hdr->flags = protocol::FLAG_NONE;
    hdr->seq_num = 0;
    hdr->timestamp_us = get_time_us();
    hdr->payload_size = sizeof(protocol::ControlCmdPayload);

    pay->cmd_id = mute ? 1 : 2; // 1 = Mute, 2 = Unmute
    pay->param_float = 0.0f;
    pay->param_int = 0;

    return socket_.send_to(cmd_buf.data(), cmd_buf.size(), server_addr_) > 0;
}

bool AudioRouterClient::send_pc_volume_command(float volume_0_to_1) {
    if (!server_addr_.is_valid()) return false;

    std::vector<uint8_t> cmd_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::ControlCmdPayload));
    auto* hdr = reinterpret_cast<protocol::CommonHeader*>(cmd_buf.data());
    auto* pay = reinterpret_cast<protocol::ControlCmdPayload*>(cmd_buf.data() + sizeof(protocol::CommonHeader));

    hdr->magic = protocol::MAGIC;
    hdr->version = protocol::CURRENT_VERSION;
    hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::CONTROL_CMD);
    hdr->flags = protocol::FLAG_NONE;
    hdr->seq_num = 0;
    hdr->timestamp_us = get_time_us();
    hdr->payload_size = sizeof(protocol::ControlCmdPayload);

    pay->cmd_id = 3; // Set Volume
    pay->param_float = volume_0_to_1;
    pay->param_int = 0;

    return socket_.send_to(cmd_buf.data(), cmd_buf.size(), server_addr_) > 0;
}

} // namespace audiorouter
