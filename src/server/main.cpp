#include "server.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <cstdio>
#include <csignal>
#include <atomic>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
    #include <windows.h>
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace {
    std::atomic<bool> g_shutdown_requested{false};

    void signal_handler(int sig) {
        (void)sig;
        g_shutdown_requested.store(true);
    }

    // Runs a shell command and captures its stdout/stderr. Empty on failure.
    // Windows toolchains (MSVC, MinGW) spell these _popen/_pclose; POSIX
    // systems use popen/pclose.
    std::string run_command(const std::string& cmd) {
        std::string result;
#if defined(_WIN32)
        FILE* pipe = _popen(cmd.c_str(), "r");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        if (!pipe) return result;
        char buf[256];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf) - 1, pipe)) > 0) {
            buf[n] = '\0';
            result += buf;
        }
#if defined(_WIN32)
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        return result;
    }

    // Voice over USB: "adb reverse tcp:<port> tcp:<port>" tunnels the phone's
    // loopback TCP port over the USB cable into this PC's loopback. adb cannot
    // forward UDP, so the server relays datagrams as length-prefixed frames
    // over that TCP connection (see AudioRouterServer::usb_relay_thread).
    // Best effort from the server; usb_setup.bat is the manual equivalent.
    void setup_usb_tunnel(uint16_t port) {
        std::ostringstream rev;
        // 2>&1: adb reports diagnostics on stderr; popen/_popen capture only
        // stdout, so fold stderr in or the "no devices" check would miss it.
        rev << "adb reverse tcp:" << port << " tcp:" << port << " 2>&1";
        std::ostringstream list;
        list << "adb reverse --list 2>&1";

        LOG_INFO("Voice over USB: setting up the USB tunnel...");
        LOG_INFO("  " << rev.str());

        std::string out = run_command(rev.str());
        if (!out.empty()) {
            // adb writes diagnostics to stderr which popen still captures.
            LOG_DEBUG("adb reverse output: " << out);
        }

        std::string listed = run_command(list.str());
        if (listed.find("tcp:" + std::to_string(port)) != std::string::npos) {
            LOG_INFO("USB tunnel active: tcp:" << port << " (phone loopback) <-> this PC's loopback over USB");
        } else {
            if (listed.find("no devices") != std::string::npos) {
                LOG_WARN("adb reports no connected device. Make sure the phone is plugged in with USB debugging enabled.");
            } else {
                LOG_WARN("adb reverse did not confirm the tunnel.");
            }
            LOG_WARN("Set it up manually in another terminal and restart with --usb:");
            LOG_WARN("    adb reverse tcp:" << port << " tcp:" << port);
        }
    }

#if defined(_WIN32)
    BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
        switch (ctrl_type) {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
            case CTRL_CLOSE_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                g_shutdown_requested.store(true);
                return TRUE;
            default:
                return FALSE;
        }
    }

    // The banner and the logger use UTF-8 box-drawing/block characters and
    // ANSI color escapes. The Windows console renders those only when the
    // output codepage is UTF-8 and virtual terminal (VT) processing is on;
    // without this the logo shows as mojibake in Windows Terminal/cmd.
    void setup_console() {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h_out != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(h_out, &mode)) {
                SetConsoleMode(h_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
    }
#endif
}

void print_banner() {
    // ANSI color only for a real console (the setup_console() call on
    // Windows already enables VT processing); plain when redirected/piped.
    const bool color = ::isatty(1) == 1 && std::getenv("NO_COLOR") == nullptr;
    const char* cyan  = color ? "\x1b[36m" : "";
    const char* green = color ? "\x1b[32m" : "";
    const char* bold  = color ? "\x1b[1m" : "";
    const char* reset = color ? "\x1b[0m" : "";

    std::cout << cyan << R"(
  █████╗ ██╗   ██╗██████╗ ██╗ ██████╗ ██████╗  ██████╗ ██╗   ██╗████████╗███████╗██████╗ 
 ██╔══██╗██║   ██║██╔══██╗██║██╔═══██╗██╔══██╗██╔═══██╗██║   ██║╚══██╔══╝██╔════╝██╔══██╗
 ███████║██║   ██║██║  ██║██║██║   ██║██████╔╝██║   ██║██║   ██║   ██║   █████╗  ██████╔╝
 ██╔══██║██║   ██║██║  ██║██║██║   ██║██╔══██╗██║   ██║██║   ██║   ██║   ██╔══╝  ██╔══██╗
 ██║  ██║╚██████╔╝██████╔╝██║╚██████╔╝██║  ██║╚██████╔╝╚██████╔╝   ██║   ███████╗██║  ██║
 ╚═╝  ╚═╝ ╚═════╝ ╚═════╝ ╚═╝ ╚═════╝ ╚═╝  ╚═╝ ╚═════╝  ╚═════╝    ╚═╝   ╚══════╝╚═╝  ╚═╝
)" << reset;
    std::cout << green << bold
              << "   Windows PC audio loopback -> Android  (WASAPI / UDP)" << reset
              << "\n\n";
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  -p, --port <port>         UDP listening port (default: 44100)\n"
              << "  -b, --bind <ip>           Bind IP address (default: 0.0.0.0)\n"
              << "  -r, --rate <hz>           Sample rate in Hz (default: 48000)\n"
              << "  -f, --frames <count>      Audio frames per UDP packet (default: 240 = 5ms)\n"
              << "      --no-mute             Do not mute PC speaker when client connects (debug)\n"
              << "      --mute-mode <mode>    Mute method: 'mute' (default), 'zero' (volume 0), 'both'\n"
              << "  -t, --test-tone           Generate test sine tone instead of WASAPI loopback\n"
              << "      --freq <hz>           Test tone frequency in Hz (default: 440.0)\n"
              << "      --usb                 Voice over USB: bind to loopback and stream over the USB cable\n"
              << "                              via 'adb reverse tcp:<port> tcp:<port>' (no Wi-Fi)\n"
              << "      --vst3 <path>         Load a VST3 plugin (.vst3) and apply it to the audio. May be\n"
              << "                              repeated to chain multiple effects in series (left to right).\n"
              << "                              The on-wire format stays S16LE; audio is converted to\n"
              << "                              float32 internally for the plugins. Requires a build with\n"
              << "                              -DAUDIOROUTER_ENABLE_VST3=ON and the VST3 SDK.\n"
              << "  -l, --list-if             List all available network interfaces and exit\n"
              << "  -v, --verbose             Enable debug logging\n"
              << "  -h, --help                Show this help message\n\n"
              << "Hotspot Quick Start:\n"
              << "  1. If PC is connected to Android Wi-Fi Hotspot:\n"
              << "     Note your PC's IP address (e.g. 192.168.43.x) from the interface list.\n"
               << "  2. Start this server on Windows:\n"
               << "     " << prog << "\n"
               << "  3. In Termux on Android with root privileges, run:\n"
               << "     su\n"
               << "     ./audiorouter_client -s <PC_IP> -p 44100\n"
               << "  4. PC speaker will automatically go quiet and audio will play on Android!\n\n"
               << "Voice over USB (no Wi-Fi):\n"
               << "  1. Connect the phone by USB (USB debugging on).\n"
               << "  2. " << prog << " --usb   (sets up 'adb reverse tcp:44100 tcp:44100' automatically)\n"
               << "  3. On the phone: ./audiorouter_client -u\n"
               << std::endl;
}

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    setup_console();
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    // Install signal handlers FIRST so Ctrl+C / kill always take the graceful
    // path (sigaction: reliable handler + no SA_RESTART so blocking calls
    // return promptly).
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);
#endif
    audiorouter::ServerConfig config;
    bool list_ifaces_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-l" || arg == "--list-if") {
            list_ifaces_only = true;
        } else if (arg == "-v" || arg == "--verbose") {
            audiorouter::Logger::instance().set_level(audiorouter::LogLevel::Debug);
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            config.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-b" || arg == "--bind") && i + 1 < argc) {
            config.bind_ip = argv[++i];
        } else if ((arg == "-r" || arg == "--rate") && i + 1 < argc) {
            config.sample_rate = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if ((arg == "-f" || arg == "--frames") && i + 1 < argc) {
            config.frames_per_packet = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--no-mute") {
            config.auto_mute_pc_speaker = false;
        } else if (arg == "--mute-mode" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "mute") config.mute_method = audiorouter::MuteMethod::EndpointMute;
            else if (mode == "zero") config.mute_method = audiorouter::MuteMethod::VolumeZero;
            else if (mode == "both") config.mute_method = audiorouter::MuteMethod::Both;
        } else if (arg == "-t" || arg == "--test-tone") {
            config.use_test_tone = true;
        } else if (arg == "--freq" && i + 1 < argc) {
            config.test_tone_freq = std::stod(argv[++i]);
        } else if (arg == "--usb") {
            config.usb_mode = true;
        } else if (arg == "--vst3" && i + 1 < argc) {
            config.vst3_plugins.emplace_back(argv[++i]);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (list_ifaces_only) {
        auto ifaces = audiorouter::UdpSocket::get_local_interfaces();
        std::cout << "\nAvailable Network Interfaces:\n";
        for (const auto& iface : ifaces) {
            std::cout << "  [" << (iface.is_up ? "UP" : "DOWN") << "] "
                      << iface.name << ": " << iface.ip_address
                      << (iface.is_loopback ? " (Loopback)" : "") << "\n";
        }
        return 0;
    }

    print_banner();

    if (config.usb_mode) {
        if (config.bind_ip != "0.0.0.0" && config.bind_ip != "127.0.0.1") {
            LOG_WARN("--usb active: -b/--bind (" << config.bind_ip << ") is ignored; binding loopback for the USB tunnel");
        }
        config.bind_ip = "127.0.0.1";
        setup_usb_tunnel(config.port);
    }

    audiorouter::AudioRouterServer server(config);

    if (!server.start()) {
        LOG_FATAL("Failed to start AudioRouter Server.");
        return 1;
    }

    // Main event / stats display loop
    uint64_t last_stats_print = audiorouter::get_time_ms();

    while (!g_shutdown_requested && server.is_running()) {
        audiorouter::sleep_ms(200);

        uint64_t now_ms = audiorouter::get_time_ms();
        if (now_ms - last_stats_print >= 5000 && server.get_state() == audiorouter::ServerState::STREAMING) {
            last_stats_print = now_ms;
            auto stats = server.get_stats();
            auto client = server.get_active_client();
            LOG_INFO("Status: Streaming to " << client.to_string()
                     << " | Sent: " << stats.packets_sent << " pkts ("
                     << (stats.bytes_sent / 1024) << " KB)"
                     << " | Lost (Client reported): " << stats.client_lost_packets
                     << " | FX: " << (config.vst3_plugins.empty()
                                        ? std::string("none")
                                        : std::to_string(config.vst3_plugins.size()) + " plugin(s)"));
        }
    }

    if (g_shutdown_requested) {
        LOG_INFO("Termination requested, shutting down server...");
    }

    server.stop();
    return 0;
}
