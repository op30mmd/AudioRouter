#include "server.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <atomic>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace {
    std::atomic<bool> g_shutdown_requested{false};

    void signal_handler(int sig) {
        (void)sig;
        g_shutdown_requested.store(true);
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
    std::cout << R"(
  █████╗ ██╗   ██╗██████╗ ██╗ ██████╗ ██████╗  ██████╗ ██╗   ██╗████████╗███████╗██████╗ 
 ██╔══██╗██║   ██║██╔══██╗██║██╔═══██╗██╔══██╗██╔═══██╗██║   ██║╚══██╔══╝██╔════╝██╔══██╗
 ███████║██║   ██║██║  ██║██║██║   ██║██████╔╝██║   ██║██║   ██║   ██║   █████╗  ██████╔╝
 ██╔══██║██║   ██║██║  ██║██║██║   ██║██╔══██╗██║   ██║██║   ██║   ██║   ██╔══╝  ██╔══██╗
 ██║  ██║╚██████╔╝██████╔╝██║╚██████╔╝██║  ██║╚██████╔╝╚██████╔╝   ██║   ███████╗██║  ██║
 ╚═╝  ╚═╝ ╚═════╝ ╚═════╝ ╚═╝ ╚═════╝ ╚═╝  ╚═╝ ╚═════╝  ╚═════╝    ╚═╝   ╚══════╝╚═╝  ╚═╝
                       [Windows Audio Router Server -> Android ALSA]
    )" << std::endl;
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
              << "  4. PC speaker will automatically go quiet and audio will play on Android!\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    setup_console();
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

    // Register signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif

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
                     << " | Lost (Client reported): " << stats.client_lost_packets);
        }
    }

    if (g_shutdown_requested) {
        LOG_INFO("Termination requested, shutting down server...");
    }

    server.stop();
    return 0;
}
