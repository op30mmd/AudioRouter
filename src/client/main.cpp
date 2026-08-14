#include "client.hpp"
#include "alsa_player.hpp"
#include "aaudio_player.hpp"
#include "termux_api_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
    std::atomic<bool> g_shutdown_requested{false};
}

int get_terminal_width() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;
}

// ANSI color for the banner: only when stdout is a terminal and the user has
// not opted out (NO_COLOR / TERM=dumb). When piped or redirected, the banner
// is plain ASCII so logs stay clean.
namespace {
bool banner_use_color() {
    static const bool enabled = [] {
        if (std::getenv("NO_COLOR") != nullptr) return false;
        const char* term = std::getenv("TERM");
        if (term != nullptr && std::strcmp(term, "dumb") == 0) return false;
        return ::isatty(STDOUT_FILENO) == 1;
    }();
    return enabled;
}
const char* banner_cyan()   { return banner_use_color() ? "\x1b[36m" : ""; }
const char* banner_green()  { return banner_use_color() ? "\x1b[32m" : ""; }
const char* banner_bold()   { return banner_use_color() ? "\x1b[1m" : ""; }
const char* banner_reset()  { return banner_use_color() ? "\x1b[0m" : ""; }
}  // namespace

void print_banner() {
    const int term_width = get_terminal_width();
    const char* cyan  = banner_cyan();
    const char* green = banner_green();
    const char* bold  = banner_bold();
    const char* reset = banner_reset();
    const char* tag   = "Windows PC audio -> Android speakers  (AAudio / Termux:API / AGM / ALSA)";

    if (term_width >= 84) {
        std::cout << cyan << R"(
  █████╗ ██╗   ██╗██████╗ ██╗ ██████╗ ██████╗  ██████╗ ██╗   ██╗████████╗███████╗██████╗
 ██╔══██╗██║   ██║██╔══██╗██║██╔═══██╗██╔══██╗██╔═══██╗██║   ██║╚══██╔══╝██╔════╝██╔══██╗
 ███████║██║   ██║██║  ██║██║██║   ██║██████╔╝██║   ██║██║   ██║   ██║   █████╗  ██████╔╝
 ██╔══██║██║   ██║██║  ██║██║██║   ██║██╔══██╗██║   ██║██║   ██║   ██║   ██╔══╝  ██╔══██╗
 ██║  ██║╚██████╔╝██████╔╝██║╚██████╔╝██║  ██║╚██████╔╝╚██████╔╝   ██║   ███████╗██║  ██║
 ╚═╝  ╚═╝ ╚═════╝ ╚═════╝ ╚═╝ ╚═════╝ ╚═╝  ╚═╝ ╚═════╝  ╚═════╝    ╚═╝   ╚══════╝╚═╝  ╚═╝
)" << reset;
    } else if (term_width >= 44) {
        std::cout << cyan << R"(
  ╔═╗╦ ╦╔╦╗╦╔═╗╦═╗╔═╗╦ ╦╔╦╗╔═╗╦═╗
  ╠═╣║ ║║ ║║╠═╣╠╦╝║ ║║ ║ ║║╣╠╦╝
  ╩ ╩╚═╝╚═╝╩╩ ╩╩╚═╚═╝╚═╝ ╩╚═╝╩╚═
)" << reset;
    } else {
        std::cout << cyan << bold << "AudioRouter" << reset;
    }

    // Tagline on its own line, trimmed for narrow terminals.
    std::cout << green << bold;
    if (term_width < 52) {
        std::cout << " - Windows PC audio -> Android speakers";
    } else {
        std::cout << "   " << tag;
    }
    std::cout << reset << "\n\n";
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  -s, --server <ip>         Windows PC Server IP address (e.g. 192.168.43.45 or 192.168.137.1)\n"
              << "  -p, --port <port>         Server UDP port (default: 44100)\n"
              << "  -d, --device <dev>        Audio device (default: 'default'):\n"
              << "                              ALSA: 'default', 'hw:0,0', 'plughw:0,0'\n"
              << "                              Direct kernel: 'direct:/dev/snd/pcmC0D0p' or any '/dev/snd/...'\n"
              << "                              Qualcomm AGM: 'agm' or 'agm:<backend>'\n"
              << "                              AAudio (NO ROOT needed): 'aaudio', 'aaudio:deep', 'aaudio:voip'\n"
              << "                              Termux:API (NO ROOT, needs the Termux:API app):\n"
              << "                                'termux', 'termux-api', 'termux:<seg-ms>' (e.g. termux:1000)\n"
              << "  -l, --latency <ms>        Target Jitter Buffer latency in ms (default: 35ms)\n"
              << "  -b, --bind <iface>        Pin UDP socket to a network interface (bypasses Android VPN tunnels):\n"
              << "                              'auto' = detect physical NIC (e.g. wlan0), or specify e.g. 'wlan0'\n"
              << "  -u, --usb                 Voice over USB: stream over the USB cable via adb reverse (no Wi-Fi).\n"
              << "                              Connect phone by USB (USB debugging on) and on the PC run:\n"
              << "                              'adb reverse tcp:44100 tcp:44100' (or scripts\\usb_setup.bat)\n"
              << "      --discover            Auto-discover server on local Wi-Fi Hotspot subnet\n"
              << "      --dummy               Use dummy audio player instead of ALSA (for testing/benchmarks)\n"
              << "      --list-devices        List detected ALSA and kernel PCM devices and exit\n"
              << "  -v, --verbose             Enable debug logging\n"
              << "  -h, --help                Show this help message\n\n"
              << "Hotspot Connection Instructions:\n"
              << "  Scenario A: PC connected to Android Mobile Hotspot\n"
              << "    1. Turn on Wi-Fi Hotspot on Android.\n"
              << "    2. Connect your Windows PC to the Hotspot.\n"
              << "    3. On Windows PC, run: audiorouter_server.exe\n"
              << "    4. On Android Termux (run 'su' first for root):\n"
              << "       ./audiorouter_client -s <PC_IP_ADDRESS> -p 44100\n\n"
              << "  Scenario B: Android connected to Windows Mobile Hotspot\n"
              << "    1. Turn on Windows Mobile Hotspot on PC (default gateway is usually 192.168.137.1).\n"
              << "    2. Connect Android phone to the PC Hotspot.\n"
              << "    3. On Android Termux: su && ./audiorouter_client -s 192.168.137.1 -p 44100\n\n"
              << "  Scenario C: Voice over USB (no Wi-Fi)\n"
              << "    1. Connect the phone to the PC with a USB cable (USB debugging enabled).\n"
              << "    2. On the PC, set up the USB tunnel: adb reverse tcp:44100 tcp:44100\n"
              << "       (or run scripts\\usb_setup.bat)\n"
              << "    3. Start the server: audiorouter_server.exe --usb\n"
              << "    4. On Android Termux: ./audiorouter_client -u\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    // Install signal handlers FIRST, before anything else, so Ctrl+C / kill
    // always reach the graceful path (and never interrupt a thread mid-
    // shutdown in a way that could crash).
    {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = [](int) { g_shutdown_requested.store(true); };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;  // no SA_RESTART: interrupt blocking calls promptly
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);
        // Terminal closed (or the su session detached): shut down gracefully
        // instead of streaming into the void.
        ::sigaction(SIGHUP, &sa, nullptr);
        struct sigaction sa_ign;
        std::memset(&sa_ign, 0, sizeof(sa_ign));
        sa_ign.sa_handler = SIG_IGN;
        sigemptyset(&sa_ign.sa_mask);
        // The AAudio FIFO's reader can disappear (agmplay restart); a SIGPIPE
        // on a FIFO write must not take the client down.
        ::sigaction(SIGPIPE, &sa_ign, nullptr);
    }

    audiorouter::ClientConfig config;
    bool list_devs = false;
    bool latency_explicit = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--list-devices") {
            list_devs = true;
        } else if (arg == "-v" || arg == "--verbose") {
            audiorouter::Logger::instance().set_level(audiorouter::LogLevel::Debug);
        } else if ((arg == "-s" || arg == "--server") && i + 1 < argc) {
            config.server_ip = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            config.server_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-d" || arg == "--device") && i + 1 < argc) {
            config.device_name = argv[++i];
        } else if ((arg == "-l" || arg == "--latency") && i + 1 < argc) {
            config.target_latency_ms = static_cast<uint32_t>(std::stoi(argv[++i]));
            latency_explicit = true;
        } else if ((arg == "-b" || arg == "--bind") && i + 1 < argc) {
            config.bind_iface = argv[++i];
        } else if (arg == "-u" || arg == "--usb") {
            config.usb_mode = true;
        } else if (arg == "--discover") {
            config.auto_discover = true;
        } else if (arg == "--dummy") {
            config.use_dummy_player = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // The USB tunnel adds two relay threads and the adb reverse forward; its
    // queue depth oscillates slowly (tens to a few hundred ms), starving
    // delivery in slow phases - 35 ms (the LAN default) and 80 ms dry out and
    // underrun. 100 ms measured clean on-device (1 underrun in ~6 min; 0 at
    // 120, 9 in ~4 min at 80). -l/--latency overrides.
    if (config.usb_mode && !latency_explicit) {
        config.target_latency_ms = 100;
        LOG_INFO("USB mode: default jitter buffer target raised to " << config.target_latency_ms
                 << " ms (override with -l)");
    }

    if (list_devs) {
        std::cout << "\nAvailable ALSA Playback Devices:\n";
        auto devs = audiorouter::AlsaPlayer::get_available_devices();
        for (const auto& d : devs) {
            std::cout << "  -> " << d << "\n";
        }
        std::cout << "\nKernel ALSA Nodes in /dev/snd/:\n";
        auto nodes = audiorouter::AndroidHelpers::get_dev_snd_nodes();
        for (const auto& n : nodes) {
            std::cout << "  -> " << n << "\n";
        }
        std::cout << "\n/proc/asound/cards:\n";
        auto cards = audiorouter::AndroidHelpers::get_proc_asound_cards();
        for (const auto& c : cards) {
            std::cout << "  " << c << "\n";
        }
        std::cout << "\nAAudio (no root, Android 8.0+):\n";
        if (audiorouter::AaudioFifoPlayer::is_supported()) {
            std::cout << "  -> available — use -d aaudio (or aaudio:deep / aaudio:voip)\n";
        } else {
            std::cout << "  -> not compiled in this build (needs an Android API 26+ toolchain with libaaudio)\n";
        }
        std::cout << "\nTermux:API media player (no root, needs the Termux:API app):\n";
        if (audiorouter::TermuxApiPlayer::is_supported()) {
            if (audiorouter::TermuxApiPlayer::api_available()) {
                std::cout << "  -> available — use -d termux (or termux:1000 for ~1 s segments)\n";
            } else {
                std::cout << "  -> the Termux:API app (com.termux.api) is not answering — install it from\n"
                          << "     F-Droid, then use -d termux\n";
            }
        } else {
            std::cout << "  -> not available on this platform\n";
        }
        return 0;
    }

    print_banner();

    audiorouter::AudioRouterClient client(config);

    if (!client.start()) {
        LOG_FATAL("Failed to start AudioRouter Client.");
        return 1;
    }

    // Monitoring Loop
    uint64_t last_stats_time = audiorouter::get_time_ms();

    while (!g_shutdown_requested && client.is_running()) {
        audiorouter::sleep_ms(200);

        uint64_t now_ms = audiorouter::get_time_ms();
        if (now_ms - last_stats_time >= 5000 && client.get_state() == audiorouter::ClientState::STREAMING) {
            last_stats_time = now_ms;
            auto stats = client.get_stats();
            double rtt_ms = static_cast<double>(stats.round_trip_time_us) / 1000.0;

            LOG_INFO("Streaming Status: "
                     << "RTT: " << rtt_ms << " ms | "
                     << "Buffer: " << stats.jitter_stats.current_buffer_ms << " ms | "
                     << "Jitter: " << stats.jitter_stats.avg_jitter_ms << " ms | "
                     << "Audio: " << stats.audio_backend_delay_ms << " ms | "
                     << "Lost: " << stats.jitter_stats.packets_lost << " pkts | "
                     << "Underruns: " << stats.jitter_stats.underruns << " | "
                     << "Played: " << stats.frames_played << " frames");
        }
    }

    if (g_shutdown_requested) {
        LOG_INFO("Termination requested, disconnecting gracefully...");
    }

    client.stop();
    return 0;
}
