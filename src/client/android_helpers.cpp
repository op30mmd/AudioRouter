#include "android_helpers.hpp"
#include "../common/logger.hpp"

#include <unistd.h>
#include <sys/stat.h>
#include <grp.h>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>

namespace audiorouter {

namespace {
// Termux home directory; its owning uid/gid is the app user to drop to.
constexpr const char* kTermuxHomeDefault = "/data/data/com.termux/files/home";
} // namespace

bool AndroidHelpers::is_running_as_root() {
#if defined(__linux__) || defined(__ANDROID__)
    return (geteuid() == 0);
#else
    return false;
#endif
}

bool AndroidHelpers::drop_to_termux_user() {
#if defined(__linux__) || defined(__ANDROID__)
    if (geteuid() != 0) return false;  // already a normal user - nothing to drop

    const char* override = std::getenv("AUDIOROUTER_TERMUX_HOME");
    const std::string termux_home =
        (override != nullptr && override[0] != '\0') ? override : kTermuxHomeDefault;

    struct stat st;
    if (::stat(termux_home.c_str(), &st) != 0) {
        LOG_WARN("AndroidHelpers: cannot find the Termux home ('" << termux_home
                 << "') to drop privileges for AAudio; AAudio will be blocked under root "
                    "(rerun AAudio without su, or use -d agm/-d default)");
        return false;
    }
    if (st.st_uid == 0) {
        LOG_WARN("AndroidHelpers: Termux home '" << termux_home
                 << "' is owned by root; cannot determine the app user to drop to");
        return false;
    }

    // Point the AAudio FIFO at the Termux home (writable by the app user).
    ::setenv("HOME", termux_home.c_str(), 1);

    // Drop supplementary groups first, then gid, then uid. The uid drop is
    // permanent; do it only now that everything root-only (socket binding,
    // /dev/snd permissions) is done.
    if (::setgroups(0, nullptr) != 0) {
        LOG_WARN("AndroidHelpers: setgroups failed: " << std::strerror(errno));
        return false;
    }
    if (::setgid(st.st_gid) != 0) {
        LOG_WARN("AndroidHelpers: setgid(" << st.st_gid << ") failed: " << std::strerror(errno));
        return false;
    }
    if (::setuid(st.st_uid) != 0) {
        LOG_WARN("AndroidHelpers: setuid(" << st.st_uid << ") failed: " << std::strerror(errno));
        return false;
    }
    LOG_INFO("AndroidHelpers: dropped privileges to UID " << st.st_uid << " (Termux app user) "
             << "so AAudio can render (Android blocks the AAudio data path for root)");
    return true;
#else
    (void)0;
    return false;
#endif
}

bool AndroidHelpers::fix_snd_permissions() {
#if defined(__linux__) || defined(__ANDROID__)
    if (!is_running_as_root()) {
        LOG_WARN("Cannot fix /dev/snd permissions: not running as root (uid != 0)");
        return false;
    }

    DIR* dir = opendir("/dev/snd");
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string full_path = std::string("/dev/snd/") + entry->d_name;
        chmod(full_path.c_str(), 0666);
    }
    closedir(dir);
    LOG_INFO("Set read/write permissions (0666) on /dev/snd/*");
    return true;
#else
    return true;
#endif
}

std::vector<std::string> AndroidHelpers::get_proc_asound_cards() {
    std::vector<std::string> cards;
    std::ifstream file("/proc/asound/cards");
    if (!file.is_open()) return cards;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            cards.push_back(line);
        }
    }
    return cards;
}

std::vector<std::string> AndroidHelpers::get_dev_snd_nodes() {
    std::vector<std::string> nodes;
#if defined(__linux__) || defined(__ANDROID__)
    DIR* dir = opendir("/dev/snd");
    if (!dir) return nodes;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        nodes.push_back(std::string("/dev/snd/") + entry->d_name);
    }
    closedir(dir);
#endif
    return nodes;
}

std::mutex& AndroidHelpers::subprocess_mutex() {
    static std::mutex m;
    return m;
}

bool AndroidHelpers::run_tinymix_command(const std::string& control_name, const std::string& value) {
#if defined(__linux__) || defined(__ANDROID__)
    std::string cmd = "tinymix \"" + control_name + "\" \"" + value + "\" 2>/dev/null";
    std::lock_guard<std::mutex> lock(subprocess_mutex());
    int ret = std::system(cmd.c_str());
    return (ret == 0);
#else
    return false;
#endif
}

bool AndroidHelpers::apply_speaker_routing() {
#if defined(__linux__) || defined(__ANDROID__)
    // Qualcomm RX_MACRO speaker path (Samsung A05s / Snapdragon 680 verified):
    // route the LPASS digital audio output to the speaker and switch it on.
    struct {
        const char* control;
        const char* value;
    } const routes[] = {
        {"RX_MACRO RX2 MUX", "AIF2_PB"},
        {"RX INT2_1 MIX1 INP0", "RX2"},
        {"AUX_RDAC Switch", "1"},
    };

    bool all_ok = true;
    for (const auto& route : routes) {
        if (!run_tinymix_command(route.control, route.value)) {
            LOG_WARN("AndroidHelpers: tinymix '" << route.control << "' '" << route.value
                     << "' failed (control name may differ on this device)");
            all_ok = false;
        }
    }

    if (all_ok) {
        LOG_INFO("AndroidHelpers: speaker routing applied (RX_MACRO RX2 -> AUX_RDAC)");
    }
    return all_ok;
#else
    return false;
#endif
}

bool AndroidHelpers::speaker_routing_intact() {
#if defined(__linux__) || defined(__ANDROID__)
    struct {
        const char* control;
        const char* expected;  // tinymix marks the current enum value with '>'
    } const checks[] = {
        {"RX_MACRO RX2 MUX", ">AIF2_PB"},
        {"RX INT2_1 MIX1 INP0", ">RX2"},
        {"AUX_RDAC Switch", "On"},
    };

    for (const auto& check : checks) {
        const std::string cmd = std::string("tinymix \"") + check.control + "\" 2>/dev/null";
        std::string out;
        {
            std::lock_guard<std::mutex> lock(subprocess_mutex());
            if (FILE* f = ::popen(cmd.c_str(), "r")) {
                char buf[256];
                while (::fgets(buf, static_cast<int>(sizeof(buf)), f)) out += buf;
                ::pclose(f);
            }
        }
        if (out.find(check.expected) == std::string::npos) {
            LOG_DEBUG("AndroidHelpers: speaker routing lost: '" << check.control << "' is no longer "
                      << check.expected);
            return false;
        }
    }
    return true;
#else
    return true;
#endif
}

void AndroidHelpers::print_android_troubleshooting_tips() {
    LOG_INFO("=== Android ALSA / Termux Root Tips ===");
    LOG_INFO("1. Make sure to run Termux with root: type 'su' or 'sudo' before launching.");
    LOG_INFO("2. If no sound is heard on the phone speaker:");
    LOG_INFO("   - Check '/proc/asound/cards' to see the audio card name.");
    LOG_INFO("   - Check available nodes in '/dev/snd/'. Usually '/dev/snd/pcmC0D0p' is Card 0 Device 0 Playback.");
    LOG_INFO("   - Some Android Qualcomm/MediaTek devices require routing the mixer to speaker:");
    LOG_INFO("     Run: tinymix | grep -i 'speaker' or tinymix 'RX_CDC_DMA_RX_0 Audio Mixer MultiMedia1' 1");
    LOG_INFO("   - If the PCM node hangs or fails to open, Android's 'audioserver' is usually holding it.");
    LOG_INFO("     Free the device (root), then re-run the client:");
    LOG_INFO("       stop audioserver      (re-enable Android audio later with: start audioserver)");
    LOG_INFO("   - If the AGM path (-d agm) cannot dlopen libagmclient.so, the linker namespace blocks");
    LOG_INFO("     vendor libraries. Run with LD_LIBRARY_PATH set (as root):");
    LOG_INFO("       su -c 'LD_LIBRARY_PATH=/vendor/lib64 ./audiorouter_client -s <PC_IP> -d agm'");
    LOG_INFO("3. You can pass a specific ALSA device using: -d direct:/dev/snd/pcmC0D0p or -d hw:0,0");
}

} // namespace audiorouter
