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
    // Drops root privileges to the Termux app user (u0_a...) so AAudio can
    // run as a normal app: Android audio policy blocks the AAudio/MMAP data
    // path for UID 0 (root has no app attribution token), so a root-launched
    // AAudio stream opens but never renders. Call while still root (after
    // the socket binding / -b auto, before opening the audio player); the
    // drop is permanent for the process. Returns true on success, false when
    // already non-root, no Termux install is found, or the drop failed (the
    // caller should then let AAudio's own UID-0 guard fall back). The Termux
    // home is located via AUDIOROUTER_TERMUX_HOME when set (also used by
    // tests), otherwise /data/data/com.termux/files/home.
    static bool drop_to_termux_user();
    static void print_android_troubleshooting_tips();
};

} // namespace audiorouter
