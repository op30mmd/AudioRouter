#include "plugin_chain.hpp"
#include "vst3_host.hpp"
#include "../common/logger.hpp"

namespace audiorouter {

// A no-op chain used when no plugins are configured or VST3 support is
// disabled at build time. The server treats it identically to a chain
// with effects; it just forwards audio unchanged.
class BypassPluginChain : public IPluginChain {
public:
    bool prepare(uint32_t /*sample_rate*/, uint16_t /*channels*/, uint32_t /*max_frames_per_block*/) override {
        return true;
    }

    void unprepare() override {}

    void process(float* /*interleaved*/, size_t /*num_frames*/) override {
        // pass through
    }

    size_t num_stages() const override { return 0; }

    std::string describe() const override { return "<bypass>"; }
};

// A chain that applies a fixed set of stages in series. The stages are
// stored in shared_ptr so add_plugin() can be called while another
// thread is in process() without invalidating the live vector.
class SeriesPluginChain : public IPluginChain {
public:
    explicit SeriesPluginChain(std::vector<std::shared_ptr<IPluginStage>> stages)
        : stages_(std::move(stages)) {}

    bool prepare(uint32_t sample_rate, uint16_t channels, uint32_t max_frames_per_block) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sample_rate_ = sample_rate;
        channels_ = channels;
        max_frames_per_block_ = max_frames_per_block;
        prepared_ = false;
        for (auto& s : stages_) {
            if (!s->prepare(sample_rate, channels, max_frames_per_block)) {
                LOG_ERROR("Plugin stage '" << s->name() << "' failed to prepare; aborting chain setup");
                for (auto& t : stages_) t->unprepare();
                return false;
            }
        }
        prepared_ = true;
        return true;
    }

    void unprepare() override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& s : stages_) s->unprepare();
        prepared_ = false;
    }

    void process(float* interleaved, size_t num_frames) override {
        // Snapshot the stages under the lock so a concurrent add_plugin()
        // does not race with our iteration. The plugin objects themselves
        // are reference-counted, so they stay alive after the snapshot.
        std::vector<std::shared_ptr<IPluginStage>> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!prepared_) return;
            snapshot = stages_;
        }
        for (auto& s : snapshot) {
            s->process(interleaved, num_frames);
        }
    }

    size_t num_stages() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return stages_.size();
    }

    std::string describe() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stages_.empty()) return "<empty>";
        std::string out;
        for (size_t i = 0; i < stages_.size(); ++i) {
            if (i) out += " -> ";
            out += stages_[i]->name();
        }
        return out;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IPluginStage>> stages_;
    uint32_t sample_rate_ = 0;
    uint16_t channels_ = 0;
    uint32_t max_frames_per_block_ = 0;
    bool prepared_ = false;
};

std::unique_ptr<IPluginChain> make_plugin_chain(const std::vector<std::string>& vst3_paths) {
    if (vst3_paths.empty()) {
        return std::make_unique<BypassPluginChain>();
    }

    std::vector<std::shared_ptr<IPluginStage>> stages;
#if AUDIOROUTER_ENABLE_VST3
    for (const auto& path : vst3_paths) {
        auto stage = make_vst3_stage(path);
        if (!stage) {
            LOG_WARN("make_plugin_chain: skipping unloadable plugin '" << path << "'");
            continue;
        }
        stages.push_back(std::move(stage));
    }
#else
    LOG_WARN("make_plugin_chain: VST3 support not compiled in ("
             "rebuild with -DAUDIOROUTER_ENABLE_VST3=ON and the VST3 SDK); "
             "ignoring " << vst3_paths.size() << " requested plugin(s).");
    (void)stages;
#endif

    if (stages.empty()) {
        // All requested plugins failed to load (or VST3 is disabled). Fall
        // back to bypass so the rest of the audio pipeline keeps working;
        // the user has already been warned per-plugin above.
        return std::make_unique<BypassPluginChain>();
    }

    return std::make_unique<SeriesPluginChain>(std::move(stages));
}

} // namespace audiorouter
