#include "../src/client/termux_api_player.hpp"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

namespace {

uint32_t le32(const std::vector<uint8_t>& buf, size_t off) {
    return static_cast<uint32_t>(buf[off]) |
           (static_cast<uint32_t>(buf[off + 1]) << 8) |
           (static_cast<uint32_t>(buf[off + 2]) << 16) |
           (static_cast<uint32_t>(buf[off + 3]) << 24);
}

uint16_t le16(const std::vector<uint8_t>& buf, size_t off) {
    return static_cast<uint16_t>(buf[off]) | (static_cast<uint16_t>(buf[off + 1]) << 8);
}

} // namespace

bool run_termux_api_tests() {
    using namespace audiorouter::termux_api;

    // ---- WAV header layout (PCM16, stereo 48 kHz) ----
    {
        const auto h = build_wav_header(48000, 2, /*float=*/false, 0xFFFFFFFFu, 0xFFFFFFFFu);
        TEST_ASSERT(h.size() == 44);
        TEST_ASSERT(std::string(reinterpret_cast<const char*>(&h[0]), 4) == "RIFF");
        TEST_ASSERT(std::string(reinterpret_cast<const char*>(&h[8]), 4) == "WAVE");
        TEST_ASSERT(std::string(reinterpret_cast<const char*>(&h[12]), 4) == "fmt ");
        TEST_ASSERT(std::string(reinterpret_cast<const char*>(&h[36]), 4) == "data");
        TEST_ASSERT(le32(h, 4) == 0xFFFFFFFFu);   // streamed riff size
        TEST_ASSERT(le32(h, 16) == 16);           // fmt chunk size
        TEST_ASSERT(le16(h, 20) == 1);            // PCM
        TEST_ASSERT(le16(h, 22) == 2);            // stereo
        TEST_ASSERT(le32(h, 24) == 48000);        // sample rate
        TEST_ASSERT(le32(h, 28) == 192000);       // byte rate (48k * 2ch * 2B)
        TEST_ASSERT(le16(h, 32) == 4);            // block align
        TEST_ASSERT(le16(h, 34) == 16);           // bits per sample
        TEST_ASSERT(le32(h, 40) == 0xFFFFFFFFu);  // streamed data size
    }

    // ---- WAV header layout (IEEE float, mono 44.1 kHz, exact sizes) ----
    {
        const auto h = build_wav_header(44100, 1, /*float=*/true, 36u + 176400u, 176400u);
        TEST_ASSERT(h.size() == 44);
        TEST_ASSERT(le16(h, 20) == 3);        // IEEE float
        TEST_ASSERT(le16(h, 22) == 1);        // mono
        TEST_ASSERT(le32(h, 24) == 44100);
        TEST_ASSERT(le32(h, 28) == 176400);   // 44100 * 1ch * 4B
        TEST_ASSERT(le16(h, 32) == 4);
        TEST_ASSERT(le16(h, 34) == 32);       // float bits
        TEST_ASSERT(le32(h, 4) == 36u + 176400u);
        TEST_ASSERT(le32(h, 40) == 176400u);
    }

    // ---- Segment issue gate ----
    // 48 kHz; segment 2000 ms, latency estimate 300 ms, lead 100 ms
    // -> target = 1600 ms = 76800 frames.
    {
        constexpr uint32_t kRate = 48000;
        constexpr uint32_t kSegMs = 2000;
        constexpr double kLat = 300.0;
        constexpr uint32_t kLead = 100;
        constexpr size_t kTargetFrames = 76800;  // 1600 ms @ 48 kHz

        // Content below target: never ready, even with plenty of wall time.
        SegmentClock clock{1000, kTargetFrames - 480, kRate};
        TEST_ASSERT(!segment_ready(clock, 1000 + 10000, kSegMs, kLat, kLead));

        // Content met but the wall clock has not (jitter prefill burst):
        // the gate must hold so audio is not skipped ahead of real time.
        SegmentClock early{1000, kTargetFrames + 4800, kRate};
        TEST_ASSERT(!segment_ready(early, 1000 + 1200, kSegMs, kLat, kLead));

        // Wall met but content not (network stall): hold.
        SegmentClock stalled{1000, kTargetFrames - 1, kRate};
        TEST_ASSERT(!segment_ready(stalled, 1000 + 2000, kSegMs, kLat, kLead));

        // Both met: ready exactly at the target.
        SegmentClock ready{1000, kTargetFrames, kRate};
        TEST_ASSERT(segment_ready(ready, 1000 + 1600, kSegMs, kLat, kLead));

        // A larger latency estimate shortens the content target.
        SegmentClock bigger_lat{1000, kTargetFrames - 10 * 480, kRate};  // 400 ms less
        TEST_ASSERT(segment_ready(bigger_lat, 1000 + 1600, kSegMs, kLat + 400.0, kLead));

        // The target never drops below the 200 ms floor (tiny segments /
        // zero latency must not produce unplayable 0-byte segments), but it
        // also never fires below that floor.
        SegmentClock floor_met{1000, 200 * kRate / 1000, kRate};
        TEST_ASSERT(segment_ready(floor_met, 1000 + 200, 300, 0.0, kLead));
        SegmentClock floor_miss{1000, 199 * kRate / 1000, kRate};
        TEST_ASSERT(!segment_ready(floor_miss, 1000 + 100000, 300, 0.0, kLead));

        // Sustain term: when the command latency is large (am broadcast to a
        // frozen app), the target becomes L + margin so the single issuer
        // thread keeps up and playback stays continuous. With L = 5000 ms
        // the target is 5200 ms regardless of the segment length.
        SegmentClock sustain_met{1000, 5200 * kRate / 1000, kRate};
        TEST_ASSERT(segment_ready(sustain_met, 1000 + 5200, 2000, 5000.0, kLead));
        SegmentClock sustain_miss{1000, 5199 * kRate / 1000, kRate};
        TEST_ASSERT(!segment_ready(sustain_miss, 1000 + 100000, 2000, 5000.0, kLead));
        // The wall gate still applies on the sustain path.
        SegmentClock sustain_wall_miss{1000, 5200 * kRate / 1000, kRate};
        TEST_ASSERT(!segment_ready(sustain_wall_miss, 1000 + 100, 2000, 5000.0, kLead));
    }

    // ---- Steady-state chaining simulation ----
    // Feed audio in real time (240 frames per 5 ms) and issue segments via
    // the gate. Consecutive issues must land ~(S - L - lead) apart and every
    // issued segment must hold ~the target amount of audio, which is what
    // makes the chaining gapless (constant ~S end-to-end delay).
    {
        constexpr uint32_t kRate = 48000;
        constexpr uint32_t kSegMs = 2000;
        constexpr double kLat = 300.0;
        constexpr uint32_t kLead = 100;
        constexpr double kTargetMs = kSegMs - kLat - kLead;  // 1600 ms

        uint64_t wall = 0;
        uint64_t seg_start = 0;
        size_t frames = 0;
        std::vector<uint64_t> issue_times;
        std::vector<size_t> issue_frames;

        for (uint64_t step = 0; step < 2000; ++step) {  // 10 s of audio
            frames += 240;  // 5 ms of 48 kHz
            wall += 5;
            if (segment_ready(SegmentClock{seg_start, frames, kRate}, wall, kSegMs, kLat,
                              kLead)) {
                issue_times.push_back(wall - seg_start);
                issue_frames.push_back(frames);
                // Rotate: the new segment starts recording at the issue instant.
                frames = 0;
                seg_start = wall;
            }
        }
        TEST_ASSERT(issue_times.size() >= 5);
        const double target_frames = kTargetMs * kRate / 1000.0;
        for (size_t i = 0; i < issue_times.size(); ++i) {
            // Wall spacing and content length both sit at the target
            // (+ one 5 ms arrival step of slack).
            TEST_ASSERT(issue_times[i] >= static_cast<uint64_t>(kTargetMs));
            TEST_ASSERT(issue_times[i] <= static_cast<uint64_t>(kTargetMs) + 5);
            TEST_ASSERT(issue_frames[i] >= static_cast<size_t>(target_frames));
            TEST_ASSERT(issue_frames[i] <= static_cast<size_t>(target_frames) + 240);
        }
    }

    // ---- Command line construction ----
    // The wire payload is extras-only: the app's socket parser extracts the
    // quoted --es values and the -a action, and fails the command when any
    // other token is left over.
    {
        const std::string play = build_command_line("play", "/data/local/tmp/x.wav", "", "");
        TEST_ASSERT(play.find("--es api_method \"MediaPlayer\" -a play") != std::string::npos);
        TEST_ASSERT(play.find("--es file \"/data/local/tmp/x.wav\"") != std::string::npos);
        TEST_ASSERT(play.find("socket_input") == std::string::npos);
        TEST_ASSERT(play.find("socket_output") == std::string::npos);
        // No leftover non-extra tokens for the parser to reject.
        TEST_ASSERT(play.find("am") == std::string::npos);
        TEST_ASSERT(play.find("broadcast") == std::string::npos);

        const std::string info = build_command_line("info", "", "", "");
        TEST_ASSERT(info.find("--es api_method \"MediaPlayer\" -a info") != std::string::npos);
        TEST_ASSERT(info.find("--es file") == std::string::npos);

        const std::string sock = build_command_line("play", "/x.wav", "in_addr", "out_addr");
        TEST_ASSERT(sock.find("--es socket_input \"in_addr\"") != std::string::npos);
        TEST_ASSERT(sock.find("--es socket_output \"out_addr\"") != std::string::npos);
        TEST_ASSERT(sock.find("--es api_method \"MediaPlayer\" -a play") != std::string::npos);
        TEST_ASSERT(sock.find("--es file \"/x.wav\"") != std::string::npos);
    }

    return true;
}
