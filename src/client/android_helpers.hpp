#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace audiorouter {

class AndroidHelpers {
public:
    // Serializes every subprocess we spawn (tinymix, getprop, kill): the
    // logcat watcher thread runs tinymix probes every second while the
    // playback thread is inside recover() doing getprop/kill, and concurrent
    // popen/system calls corrupt FILE state. All helpers below must run under
    // this lock.
    static std::mutex& subprocess_mutex();
    static bool is_running_as_root();
    static bool fix_snd_permissions();
    static std::vector<std::string> get_proc_asound_cards();
    static std::vector<std::string> get_dev_snd_nodes();
    static bool run_tinymix_command(const std::string& control_name, const std::string& value);
    static bool apply_speaker_routing();
    // Checks whether the speaker path controls still match what
    // apply_speaker_routing() sets. Another sound (notification, UI tone)
    // reconfigures the codec when it preempts us, so a cleared route is the
    // early sign that the AGM graph is about to go silent.
    static bool speaker_routing_intact();
    // Locates the Termux app user: the owning uid/gid of the Termux home
    // (/data/data/com.termux/files/home, overridable via
    // AUDIOROUTER_TERMUX_HOME for tests). Returns false when Termux is not
    // installed (or the home is root-owned). Pure lookup - no logging, no
    // side effects - so it is safe to call from a forked child process.
    static bool termux_user(uid_t* out_uid, gid_t* out_gid, std::string* out_home);
    static void print_android_troubleshooting_tips();
};

} // namespace audiorouter
