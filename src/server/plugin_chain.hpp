#pragma once

#include "../common/audio_types.hpp"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>

namespace audiorouter {

// Abstract effect stage. Implementations transform interleaved float32 audio
// in place. The server always hands audio to a chain in float32 form
// regardless of the on-wire S16LE format, because VST3 plugins are
// float32-only at the API level.
class IPluginStage {
public:
    virtual ~IPluginStage() = default;

    // One-time configuration. Must be called before any process() call.
    // The chain is responsible for opening and initialising the plugin
    // (allocating buffers, telling it the sample rate and channel layout).
    // Returns true on success.
    virtual bool prepare(uint32_t sample_rate, uint16_t channels, uint32_t max_frames_per_block) = 0;

    // Release any resources held by the plugin.
    virtual void unprepare() = 0;

    // Process interleaved float32 audio in place. num_frames is the number of
    // multichannel frames (i.e. buffer size = num_frames * channels).
    virtual void process(float* interleaved, size_t num_frames) = 0;

    // Human-readable name for logs / status output.
    virtual std::string name() const = 0;
};

// A chain of zero or more IPluginStages that processes audio in series.
// The server owns one of these; if VST3 is not compiled in or no plugins
// are configured, a BypassPluginChain (which does nothing) is used so the
// rest of the pipeline does not need to special-case the absence of
// effects.
class IPluginChain {
public:
    virtual ~IPluginChain() = default;

    // Prepare all configured stages. After this, process() may be called
    // from any thread but the chain itself is internally serialised (the
    // audio callback is real-time and must not block; the chain uses a
    // mutex to make setup-thread calls to set_sample_rate() and
    // add_plugin() safe to interleave with process()).
    virtual bool prepare(uint32_t sample_rate, uint16_t channels, uint32_t max_frames_per_block) = 0;
    virtual void unprepare() = 0;

    // Process interleaved float32 audio in place. num_frames is per block;
    // the chain is responsible for keeping the blocks sized appropriately
    // for the host's packet size.
    virtual void process(float* interleaved, size_t num_frames) = 0;

    // Number of stages in the chain (0 = bypass).
    virtual size_t num_stages() const = 0;

    // Human-readable list of stages, one per line, for log output.
    virtual std::string describe() const = 0;
};

// Factory: construct the appropriate chain for the given list of plugin
// file paths. An empty list returns a BypassPluginChain. If VST3 support
// is not compiled in and paths are non-empty, the factory still returns
// a BypassPluginChain and logs a warning, so the server can keep running
// without effects.
std::unique_ptr<IPluginChain> make_plugin_chain(const std::vector<std::string>& vst3_paths);

} // namespace audiorouter
