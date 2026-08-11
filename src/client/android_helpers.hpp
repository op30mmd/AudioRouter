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
    static void print_android_troubleshooting_tips();
};

} // namespace audiorouter
