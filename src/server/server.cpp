#include "server.hpp"
#include "wasapi_capture.hpp"
#include "dummy_capture.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"
#include "../common/usb_tunnel.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace audiorouter {

AudioRouterServer::AudioRouterServer(const ServerConfig& config)
    : config_(config),
      is_running_(false),
      state_(ServerState::STOPPED),
      last_client_activity_time_ms_(0),
      sequence_number_(0) {}

AudioRouterServer::~AudioRouterServer() {
    stop();
}

bool AudioRouterServer::start() {
    if (is_running_) return true;

    LOG_INFO("=================================================");
    LOG_INFO(" Starting AudioRouter Windows Server Engine");
    LOG_INFO("=================================================");

    // Initialize Endpoint Volume Control (for PC speaker muting)
    if (config_.auto_mute_pc_speaker) {
        if (!endpoint_control_.init()) {
            LOG_WARN("Failed to initialize Windows endpoint volume controller. PC speaker muting might be unavailable.");
        }
    }

    // Open UDP Socket
    if (!socket_.open()) {
        LOG_ERROR("Failed to open UDP socket");
        return false;
    }

    // Bind UDP Socket
    if (!socket_.bind(config_.port, config_.bind_ip)) {
        LOG_ERROR("Failed to bind UDP socket to " << config_.bind_ip << ":" << config_.port);
        socket_.close();
        return false;
    }

    // Enable high socket buffer sizes and low-latency QoS priority
    socket_.set_buffer_sizes(1024 * 1024, 1024 * 1024);
    socket_.set_qos_priority(true);

    // List available network interfaces for user convenience (not in USB
    // mode: the stream comes in over the loopback, not the LAN)
    if (!config_.usb_mode) {
        auto ifaces = UdpSocket::get_local_interfaces();
        LOG_INFO("Available Network Interfaces for Android Client Connection:");
        for (const auto& iface : ifaces) {
            if (iface.is_up && !iface.is_loopback) {
                LOG_INFO("  -> " << iface.ip_address << " (" << iface.name << ")");
            }
        }
    }
    LOG_INFO("Listening for Android client on UDP port " << config_.port << "...");

    // Create Audio Capture Engine
    if (config_.use_test_tone) {
        capture_engine_ = std::make_unique<DummyCapture>(config_.test_tone_freq);
    } else {
#if defined(_WIN32)
        capture_engine_ = std::make_unique<WasapiCapture>();
#else
        LOG_WARN("Non-Windows host detected: using synthetic audio generator");
        capture_engine_ = std::make_unique<DummyCapture>(config_.test_tone_freq);
#endif
    }

    // Wire up audio capture callback
    capture_engine_->set_audio_callback(
        [this](const void* data, size_t num_frames, const AudioConfig& config) {
            this->on_audio_captured(data, num_frames, config);
        }
    );

    // Build the VST3 plugin chain. If no plugins are configured, or all
    // configured plugins failed to load, this is a BypassPluginChain
    // (no-op) and the audio path stays exactly as before. We construct
    // it before start() of the capture engine because prepare() needs
    // to know the sample rate, which the capture engine has not yet
    // resolved; the chain is therefore prepared lazily in
    // on_audio_captured() the first time audio arrives, by which point
    // actual_audio_config_ is known.
    plugin_chain_ = make_plugin_chain(config_.vst3_plugins);
    if (plugin_chain_->num_stages() > 0) {
        LOG_INFO("VST3 plugin chain: " << plugin_chain_->describe());
    } else if (!config_.vst3_plugins.empty()) {
        LOG_WARN("VST3 plugin chain is empty: no plugins were loaded successfully.");
    }

    is_running_ = true;
    state_ = ServerState::LISTENING;

    // Start background threads
    net_thread_ = std::thread(&AudioRouterServer::network_receive_thread, this);
    watchdog_thread_ = std::thread(&AudioRouterServer::watchdog_thread, this);

    // Voice over USB: relay thread accepts the adb reverse TCP connection and
    // bridges length-prefixed frames to/from this engine's UDP socket. Started
    // before any client can connect so the tunnel is live on first handshake.
    if (config_.usb_mode) {
        usb_relay_thread_ = std::thread(&AudioRouterServer::usb_relay_thread, this);
    }

    LOG_INFO("AudioRouter Server ready. Waiting for Android client connection.");
    return true;
}

void AudioRouterServer::stop() {
    is_running_ = false;

    // Disconnect active client and unmute PC speaker
    disconnect_client("Server shutdown", true);

    // Stop audio capture
    if (capture_engine_) {
        capture_engine_->stop();
    }

    // Close socket to unblock receive thread
    socket_.close();

    if (net_thread_.joinable()) {
        net_thread_.join();
    }
    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }
    if (usb_relay_thread_.joinable()) {
        // Time-bounded select() waits let it notice is_running_ == false
        // within ~100 ms without needing to close sockets cross-thread.
        usb_relay_thread_.join();
    }

    // Ensure endpoint volume is restored
    if (config_.auto_mute_pc_speaker) {
        endpoint_control_.unmute_pc_speaker();
        endpoint_control_.shutdown();
    }

    state_ = ServerState::STOPPED;
    LOG_INFO("AudioRouter Server stopped successfully.");
}

bool AudioRouterServer::is_running() const {
    return is_running_;
}

ServerState AudioRouterServer::get_state() const {
    return state_.load();
}

ServerStats AudioRouterServer::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

SocketAddress AudioRouterServer::get_active_client() const {
    std::lock_guard<std::mutex> lock(client_mutex_);
    return active_client_;
}

void AudioRouterServer::network_receive_thread() {
    std::vector<uint8_t> recv_buf(65536);
    socket_.set_receive_timeout_ms(200);

    while (is_running_) {
        SocketAddress sender;
        int bytes_read = socket_.receive_from(recv_buf.data(), recv_buf.size(), sender);

        if (bytes_read <= 0) {
            if (!is_running_) break;
            sleep_ms(1);
            continue;
        }

        if (static_cast<size_t>(bytes_read) < sizeof(protocol::CommonHeader)) {
            continue;
        }

        const auto* hdr = reinterpret_cast<const protocol::CommonHeader*>(recv_buf.data());
        if (!protocol::is_valid_header(*hdr, static_cast<size_t>(bytes_read))) {
            continue; // Not an AudioRouter packet or invalid
        }

        const uint8_t* payload = recv_buf.data() + sizeof(protocol::CommonHeader);
        size_t payload_len = bytes_read - sizeof(protocol::CommonHeader);

        auto msg_type = static_cast<protocol::MsgType>(hdr->msg_type);

        switch (msg_type) {
            case protocol::MsgType::DISCOVERY_REQ:
                handle_discovery_req(*hdr, sender);
                break;
            case protocol::MsgType::CONNECT_REQ:
                handle_connect_req(*hdr, payload, payload_len, sender);
                break;
            case protocol::MsgType::DISCONNECT_REQ:
                handle_disconnect_req(*hdr, sender);
                break;
            case protocol::MsgType::HEARTBEAT_PING:
                handle_heartbeat_ping(*hdr, payload, payload_len, sender);
                break;
            case protocol::MsgType::CONTROL_CMD:
                handle_control_cmd(*hdr, payload, payload_len, sender);
                break;
            default:
                break;
        }
    }
}

void AudioRouterServer::handle_discovery_req(const protocol::CommonHeader& hdr, const SocketAddress& sender) {
    (void)hdr;
    LOG_INFO("Received Discovery Probe from " << sender.to_string());

    std::vector<uint8_t> resp_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::DiscoveryRespPayload));
    auto* resp_hdr = reinterpret_cast<protocol::CommonHeader*>(resp_buf.data());
    auto* resp_pay = reinterpret_cast<protocol::DiscoveryRespPayload*>(resp_buf.data() + sizeof(protocol::CommonHeader));

    resp_hdr->magic = protocol::MAGIC;
    resp_hdr->version = protocol::CURRENT_VERSION;
    resp_hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::DISCOVERY_RESP);
    resp_hdr->flags = protocol::FLAG_NONE;
    resp_hdr->seq_num = 0;
    resp_hdr->timestamp_us = get_time_us();
    resp_hdr->payload_size = sizeof(protocol::DiscoveryRespPayload);

    std::strncpy(resp_pay->server_name, "AudioRouter-PC-Server", sizeof(resp_pay->server_name) - 1);
    resp_pay->server_version = protocol::CURRENT_VERSION;
    resp_pay->server_port = config_.port;
    resp_pay->is_busy = (state_ == ServerState::STREAMING) ? 1 : 0;
    resp_pay->pc_muted = endpoint_control_.is_currently_silenced_by_us() ? 1 : 0;

    socket_.send_to(resp_buf.data(), resp_buf.size(), sender);
}

void AudioRouterServer::handle_connect_req(const protocol::CommonHeader& hdr, const uint8_t* payload,
                                           size_t len, const SocketAddress& sender) {
    (void)hdr;
    (void)payload;
    (void)len;
    std::lock_guard<std::mutex> lock(client_mutex_);

    LOG_INFO("Connection request received from client: " << sender.to_string());

    // If already streaming to another client, check if previous client is still active
    if (state_ == ServerState::STREAMING && active_client_.is_valid() && active_client_ != sender) {
        uint64_t now_ms = get_time_ms();
        if (now_ms - last_client_activity_time_ms_ < config_.client_timeout_ms) {
            // Reject new client with NAK
            LOG_WARN("Rejecting connection from " << sender.to_string() << ": Busy with " << active_client_.to_string());
            std::vector<uint8_t> nak_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::ConnectNakPayload));
            auto* nak_hdr = reinterpret_cast<protocol::CommonHeader*>(nak_buf.data());
            auto* nak_pay = reinterpret_cast<protocol::ConnectNakPayload*>(nak_buf.data() + sizeof(protocol::CommonHeader));

            nak_hdr->magic = protocol::MAGIC;
            nak_hdr->version = protocol::CURRENT_VERSION;
            nak_hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::CONNECT_NAK);
            nak_hdr->flags = protocol::FLAG_NONE;
            nak_hdr->seq_num = 0;
            nak_hdr->timestamp_us = get_time_us();
            nak_hdr->payload_size = sizeof(protocol::ConnectNakPayload);

            nak_pay->error_code = 1;
            std::strncpy(nak_pay->reason, "Server is currently streaming to another client", sizeof(nak_pay->reason) - 1);
            socket_.send_to(nak_buf.data(), nak_buf.size(), sender);
            return;
        }
    }

    // Set active client
    active_client_ = sender;
    last_client_activity_time_ms_ = get_time_ms();
    sequence_number_ = 0;

    // Desired audio format configuration
    AudioConfig desired_config;
    desired_config.sample_rate = config_.sample_rate;
    desired_config.channels = config_.channels;
    desired_config.format = AudioSampleFormat::PCM_S16LE;
    desired_config.frames_per_packet = config_.frames_per_packet;

    // Start WASAPI audio capture
    if (!capture_engine_->is_capturing()) {
        if (!capture_engine_->start(desired_config, actual_audio_config_)) {
            LOG_ERROR("Failed to start audio capture engine");
            return;
        }
    }

    // Prepare the plugin chain now that we know the actual sample rate
    // and channel layout. max_frames_per_block is the largest single
    // block the chain's process() will ever be asked to handle. The
    // capture engine delivers audio in its own quantum (typically
    // 10 ms = 480 frames at 48 kHz on shared-mode WASAPI), which is
    // generally larger than the on-wire packet size (frames_per_packet,
    // default 5 ms = 240). We size max_frames_per_block to cover the
    // capture quantum: the chain processes whatever the capture
    // engine hands it, and the server's packetizer then re-chunks
    // the chain's output into frames_per_packet-sized UDP packets
    // for transmission. 2048 frames is a generous upper bound
    // (covers up to ~43 ms at 48 kHz) and matches the VST3 SDK's
    // common "maxSamplesPerBlock" recommendation.
    constexpr uint32_t kMaxPluginBlockFrames = 2048;
    if (plugin_chain_) {
        if (!plugin_chain_->prepare(actual_audio_config_.sample_rate,
                                    actual_audio_config_.channels,
                                    kMaxPluginBlockFrames)) {
            LOG_WARN("Plugin chain prepare() failed; continuing without effects");
        }
    }

    // MUTE PC SPEAKER as requested: PC goes quiet, audio routes directly to client!
    bool muted_ok = false;
    if (config_.auto_mute_pc_speaker) {
        LOG_INFO("Routing audio to client -> Silencing PC speaker...");
        muted_ok = endpoint_control_.mute_pc_speaker(config_.mute_method);
        if (muted_ok) {
            LOG_INFO("PC Speaker successfully silenced.");
        } else {
            LOG_WARN("Could not silence PC speaker via endpoint volume.");
        }
    }

    state_ = ServerState::STREAMING;

    // Send CONNECT_ACK
    std::vector<uint8_t> ack_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::ConnectAckPayload));
    auto* ack_hdr = reinterpret_cast<protocol::CommonHeader*>(ack_buf.data());
    auto* ack_pay = reinterpret_cast<protocol::ConnectAckPayload*>(ack_buf.data() + sizeof(protocol::CommonHeader));

    ack_hdr->magic = protocol::MAGIC;
    ack_hdr->version = protocol::CURRENT_VERSION;
    ack_hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::CONNECT_ACK);
    ack_hdr->flags = protocol::FLAG_NONE;
    ack_hdr->seq_num = 0;
    ack_hdr->timestamp_us = get_time_us();
    ack_hdr->payload_size = sizeof(protocol::ConnectAckPayload);

    ack_pay->status_code = 0;
    ack_pay->sample_rate = actual_audio_config_.sample_rate;
    ack_pay->channels = actual_audio_config_.channels;
    ack_pay->format = static_cast<uint8_t>(actual_audio_config_.format);
    ack_pay->frames_per_packet = static_cast<uint16_t>(actual_audio_config_.frames_per_packet);
    ack_pay->pc_speaker_muted = muted_ok ? 1 : 0;
    std::strncpy(ack_pay->status_msg, "Connected: Audio routed to client", sizeof(ack_pay->status_msg) - 1);

    socket_.send_to(ack_buf.data(), ack_buf.size(), sender);
    LOG_INFO("Client connected! Streaming audio to " << sender.to_string()
             << " (" << actual_audio_config_.to_string() << ")");
}

void AudioRouterServer::handle_disconnect_req(const protocol::CommonHeader& hdr, const SocketAddress& sender) {
    (void)hdr;
    std::lock_guard<std::mutex> lock(client_mutex_);
    if (sender == active_client_) {
        LOG_INFO("Client " << sender.to_string() << " requested disconnect.");
        disconnect_client("Client disconnected gracefully", true);
    }
}

void AudioRouterServer::handle_heartbeat_ping(const protocol::CommonHeader& hdr, const uint8_t* payload,
                                              size_t len, const SocketAddress& sender) {
    if (sender != active_client_) return;

    last_client_activity_time_ms_ = get_time_ms();

    uint64_t client_orig_time = 0;
    if (len >= sizeof(protocol::HeartbeatPayload)) {
        const auto* ping_pay = reinterpret_cast<const protocol::HeartbeatPayload*>(payload);
        client_orig_time = ping_pay->orig_timestamp_us;

        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.client_buffer_level = ping_pay->client_buffer_level_frames;
        stats_.client_lost_packets = ping_pay->packets_lost;
    }

    // Send PONG response
    std::vector<uint8_t> pong_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::HeartbeatPayload));
    auto* pong_hdr = reinterpret_cast<protocol::CommonHeader*>(pong_buf.data());
    auto* pong_pay = reinterpret_cast<protocol::HeartbeatPayload*>(pong_buf.data() + sizeof(protocol::CommonHeader));

    pong_hdr->magic = protocol::MAGIC;
    pong_hdr->version = protocol::CURRENT_VERSION;
    pong_hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::HEARTBEAT_PONG);
    pong_hdr->flags = protocol::FLAG_NONE;
    pong_hdr->seq_num = hdr.seq_num;
    pong_hdr->timestamp_us = get_time_us();
    pong_hdr->payload_size = sizeof(protocol::HeartbeatPayload);

    pong_pay->orig_timestamp_us = client_orig_time;
    pong_pay->client_buffer_level_frames = 0;
    pong_pay->packets_received = static_cast<uint32_t>(stats_.packets_sent);
    pong_pay->packets_lost = 0;

    socket_.send_to(pong_buf.data(), pong_buf.size(), sender);
}

void AudioRouterServer::handle_control_cmd(const protocol::CommonHeader& hdr, const uint8_t* payload,
                                           size_t len, const SocketAddress& sender) {
    (void)hdr;
    if (sender != active_client_) return;
    if (len < sizeof(protocol::ControlCmdPayload)) return;

    const auto* cmd = reinterpret_cast<const protocol::ControlCmdPayload*>(payload);
    LOG_INFO("Control command received: cmd=" << static_cast<int>(cmd->cmd_id));

    switch (cmd->cmd_id) {
        case 1: // MUTE PC
            endpoint_control_.set_mute(true);
            break;
        case 2: // UNMUTE PC
            endpoint_control_.set_mute(false);
            break;
        case 3: // SET PC VOLUME
            endpoint_control_.set_volume(cmd->param_float);
            break;
        default:
            break;
    }
}

void AudioRouterServer::usb_relay_thread() {
    if (!usb_listener_.listen(config_.port, "127.0.0.1", 1)) {
        LOG_ERROR("USB tunnel: cannot listen on tcp:127.0.0.1:" << config_.port);
        return;
    }
    LOG_INFO("USB tunnel: waiting for adb reverse connection on tcp:127.0.0.1:" << config_.port);

    // UDP peer: the engine's UDP socket (bound to 127.0.0.1:port in USB mode)
    // sees this relay as a normal remote client; the engine's replies come
    // back here and are re-framed onto the TCP tunnel.
    if (!usb_relay_udp_.open() || !usb_relay_udp_.bind(0, "127.0.0.1")) {
        LOG_ERROR("USB tunnel: cannot open relay UDP socket");
        return;
    }
    usb_relay_udp_.set_non_blocking(true);
    const SocketAddress engine_udp("127.0.0.1", config_.port);

    uint8_t datagram_buf[tunnel::kMaxFramePayload];
    std::vector<uint8_t> frame;
    std::vector<uint8_t> rx;  // frame reassembly buffer (this thread only)

    while (is_running_) {
        TcpSocket client;
        if (!usb_listener_.accept(client, 200)) {
            if (!is_running_) break;
            continue;  // timeout: re-check shutdown, keep accepting
        }
        client.set_tcp_nodelay(true);
        client.set_non_blocking(true);
        // Bound the tunnel's kernel buffers (~320 ms of audio each way at
        // 48 kHz stereo S16). A stalled hop (adb/USB/CPU) then cannot pile up
        // seconds of queued audio: once the bounded buffer fills, the relay's
        // write fails fast, the tunnel reconnects, and playback resumes live
        // instead of replaying a multi-second stale backlog after the stall.
        client.set_buffer_sizes(64 * 1024, 64 * 1024);
        rx.clear();  // stale partial frame from a previous tunnel session
        // Drop datagrams left over from the previous tunnel (e.g. a
        // DISCONNECT_ACK sent while the old connection was dying) so the new
        // session never receives stale control traffic.
        {
            SocketAddress stale_from;
            while (usb_relay_udp_.receive_from(datagram_buf, sizeof(datagram_buf), stale_from) > 0) {}
        }
        LOG_INFO("USB tunnel: adb reverse connected; relaying tcp <-> udp");

        bool tunnel_closed = false;
        while (is_running_ && !tunnel_closed) {
            int m = tunnel::select2(&client, &usb_relay_udp_, 50);
            if (m & 1) {
                auto r = tunnel::read_frame(client, rx, frame);
                if (r == tunnel::RecvResult::Frame) {
                    usb_relay_udp_.send_to(frame.data(), frame.size(), engine_udp);
                } else if (r == tunnel::RecvResult::Closed || r == tunnel::RecvResult::Error) {
                    tunnel_closed = true;
                }
            }
            if (m & 2) {
                SocketAddress from;
                int n = usb_relay_udp_.receive_from(datagram_buf, sizeof(datagram_buf), from);
                if (n > 0) {
                    if (!tunnel::write_frame(client, datagram_buf, static_cast<size_t>(n))) {
                        tunnel_closed = true;
                    }
                }
            }
        }

        if (is_running_ && tunnel_closed) {
            LOG_WARN("USB tunnel: client disconnected; restoring speaker state");
            // Same path as a Wi-Fi client dropping: unmutes the PC speaker and
            // clears the session so a reconnected tunnel starts clean.
            disconnect_client("USB tunnel disconnected", true);
        }
        client.close();
    }
}

void AudioRouterServer::disconnect_client(const std::string& reason, bool send_ack) {
    if (state_ == ServerState::STREAMING && active_client_.is_valid()) {
        LOG_INFO("Disconnecting client " << active_client_.to_string() << ": " << reason);

        if (send_ack) {
            std::vector<uint8_t> dis_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::DisconnectPayload));
            auto* dis_hdr = reinterpret_cast<protocol::CommonHeader*>(dis_buf.data());
            auto* dis_pay = reinterpret_cast<protocol::DisconnectPayload*>(dis_buf.data() + sizeof(protocol::CommonHeader));

            dis_hdr->magic = protocol::MAGIC;
            dis_hdr->version = protocol::CURRENT_VERSION;
            dis_hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::DISCONNECT_ACK);
            dis_hdr->flags = protocol::FLAG_NONE;
            dis_hdr->seq_num = 0;
            dis_hdr->timestamp_us = get_time_us();
            dis_hdr->payload_size = sizeof(protocol::DisconnectPayload);

            dis_pay->reason_code = 0;
            std::strncpy(dis_pay->reason, reason.c_str(), sizeof(dis_pay->reason) - 1);
            socket_.send_to(dis_buf.data(), dis_buf.size(), active_client_);
        }

        // Restore PC speaker when client disconnects
        if (config_.auto_mute_pc_speaker) {
            LOG_INFO("Client disconnected -> Restoring PC speaker volume/unmute...");
            endpoint_control_.unmute_pc_speaker();
        }

        // Flip state to LISTENING *before* un-preparing the chain so the
        // capture thread's on_audio_captured() bails on the state check
        // and never tries to process() an unprepared chain. Without this
        // ordering the audio thread can race past the state check while
        // we're in the middle of un-preparing the chain, which produces
        // "received N frames but was prepared for 0" warnings and (in the
        // worst case) a call into an unprepared plugin.
        active_client_ = SocketAddress();
        state_ = ServerState::LISTENING;

        // Tear down the plugin chain so a re-connect with a different
        // sample rate re-prepares correctly. If we keep the chain
        // prepared across disconnects, a subsequent CONNECT with a
        // different preferred_sample_rate would mismatch the plugin's
        // internal sample rate and either drop audio or refuse to
        // process.
        if (plugin_chain_) {
            plugin_chain_->unprepare();
        }

        LOG_INFO("Server back in LISTENING state. Waiting for next client.");
    }
}

void AudioRouterServer::watchdog_thread() {
    while (is_running_) {
        sleep_ms(500);

        if (state_ == ServerState::STREAMING) {
            uint64_t now_ms = get_time_ms();
            uint64_t last_activity = last_client_activity_time_ms_.load();

            if (last_activity > 0 && (now_ms - last_activity) > config_.client_timeout_ms) {
                std::lock_guard<std::mutex> lock(client_mutex_);
                LOG_WARN("Client connection heartbeat timeout ("
                         << (now_ms - last_activity) << "ms > "
                         << config_.client_timeout_ms << "ms).");
                disconnect_client("Heartbeat timeout", false);
            }
        }
    }
}

void AudioRouterServer::on_audio_captured(const void* data, size_t num_frames, const AudioConfig& config) {
    if (state_ != ServerState::STREAMING) return;
    if (!active_client_.is_valid()) return;

    const size_t bytes_per_frame = config.bytes_per_frame();
    const size_t total_input_bytes = num_frames * bytes_per_frame;
    const size_t target_chunk_frames = config_.frames_per_packet > 0 ? config_.frames_per_packet : 240;
    const size_t target_chunk_bytes = target_chunk_frames * bytes_per_frame;

    // The audio pipeline now runs: capture (S16LE) -> float32 -> plugin
    // chain -> float32 -> S16LE -> packetize. The float-side buffers
    // are reused across calls to avoid per-block heap traffic; they
    // are sized to the maximum number of frames we will ever receive
    // in a single capture callback, which is bounded by the
    // capture engine's quantum (a few ms at 48 kHz).
    std::vector<float>       float_buf(num_frames * config.channels);
    std::vector<int16_t>     pcm_buf;
    // Only allocate the converted-PCM vector when we actually have
    // plugins to run; the bypass path is allocation-free.
    const bool run_plugins = plugin_chain_ && plugin_chain_->num_stages() > 0;
    if (run_plugins) {
        pcm_buf.resize(num_frames * config.channels);
    }

    // Convert the capture-engine-native format into float32 in place.
    // The capture engine in practice emits S16LE (the only format the
    // server negotiates on connect); we handle it generically so a
    // future float capture backend is also supported without code
    // changes here.
    if (config.format == AudioSampleFormat::PCM_S16LE) {
        const auto* in = reinterpret_cast<const int16_t*>(data);
        for (size_t i = 0; i < float_buf.size(); ++i) {
            float_buf[i] = static_cast<float>(in[i]) / 32768.0f;
        }
    } else if (config.format == AudioSampleFormat::PCM_FLOAT32LE) {
        std::memcpy(float_buf.data(), data, float_buf.size() * sizeof(float));
    } else {
        // Unsupported format; fall back to treating the raw bytes as
        // silence so the chain cannot crash on bogus input.
        std::memset(float_buf.data(), 0, float_buf.size() * sizeof(float));
    }

    if (run_plugins) {
        // Run the configured VST3 (or future) plugin chain on the
        // float buffer. The chain processes in place; if it has zero
        // stages (e.g. all plugins failed to load and the chain is a
        // bypass) this is a no-op.
        plugin_chain_->process(float_buf.data(), num_frames);

        // Convert float -> S16LE for the on-wire format.
        for (size_t i = 0; i < pcm_buf.size(); ++i) {
            float v = std::clamp(float_buf[i], -1.0f, 1.0f);
            pcm_buf[i] = static_cast<int16_t>(v * 32767.0f);
        }
    }

    std::lock_guard<std::mutex> lock(chunk_mutex_);
    const uint8_t* byte_ptr = run_plugins
        ? reinterpret_cast<const uint8_t*>(pcm_buf.data())
        : reinterpret_cast<const uint8_t*>(data);

    // Append incoming captured audio bytes into chunking buffer
    chunk_buffer_.insert(chunk_buffer_.end(), byte_ptr, byte_ptr + total_input_bytes);

    // Packet buffer pre-allocated
    std::vector<uint8_t> packet_buf(sizeof(protocol::AudioPacketHeader) + target_chunk_bytes);

    while (chunk_buffer_.size() >= target_chunk_bytes) {
        auto* hdr = reinterpret_cast<protocol::AudioPacketHeader*>(packet_buf.data());

        hdr->common.magic = protocol::MAGIC;
        hdr->common.version = protocol::CURRENT_VERSION;
        hdr->common.msg_type = static_cast<uint8_t>(protocol::MsgType::AUDIO_DATA);
        hdr->common.flags = protocol::FLAG_NONE;
        hdr->common.seq_num = sequence_number_.fetch_add(1);
        hdr->common.timestamp_us = get_time_us();
        hdr->common.payload_size = static_cast<uint32_t>(sizeof(protocol::AudioPacketHeader) - sizeof(protocol::CommonHeader) + target_chunk_bytes);

        hdr->sample_rate = config.sample_rate;
        hdr->channels = config.channels;
        hdr->format = static_cast<uint8_t>(config.format);
        hdr->reserved = 0;
        hdr->num_frames = static_cast<uint32_t>(target_chunk_frames);

        // Copy chunk PCM payload
        std::memcpy(packet_buf.data() + sizeof(protocol::AudioPacketHeader),
                    chunk_buffer_.data(), target_chunk_bytes);

        // Erase chunked bytes from buffer
        chunk_buffer_.erase(chunk_buffer_.begin(), chunk_buffer_.begin() + target_chunk_bytes);

        // Send packet over UDP to Android client
        int sent = socket_.send_to(packet_buf.data(), packet_buf.size(), active_client_);
        if (sent > 0) {
            std::lock_guard<std::mutex> s_lock(stats_mutex_);
            stats_.packets_sent++;
            stats_.bytes_sent += sent;
            stats_.audio_frames_captured += target_chunk_frames;
        }
    }
}

} // namespace audiorouter
