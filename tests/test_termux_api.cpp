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

    // ---- Segment issue gate (file ring buffer) ----
    // The segment is handed over once it holds the prefill of real audio AND
    // the wall clock has advanced that far. The rest of the file is already
    // readable as silence, so the end-to-end delay is prefill + command
    // latency — the segment length does not enter the gate at all.
    {
        constexpr uint32_t kRate = 48000;
        constexpr uint32_t kPrefillMs = 600;
        constexpr size_t kPrefillFrames = static_cast<size_t>(kRate) * kPrefillMs / 1000;

        // Content below the prefill: never ready, even with plenty of wall time.
        SegmentClock clock{1000, kPrefillFrames - 240, kRate};
        TEST_ASSERT(!segment_ready(clock, 1000 + 10000, kPrefillMs));

        // Content met but the wall clock has not (jitter prefill burst):
        // the gate must hold so audio is not skipped ahead of real time.
        SegmentClock early{1000, kPrefillFrames + 4800, kRate};
        TEST_ASSERT(!segment_ready(early, 1000 + 300, kPrefillMs));

        // Wall met but content not (network stall): hold.
        SegmentClock stalled{1000, kPrefillFrames - 1, kRate};
        TEST_ASSERT(!segment_ready(stalled, 1000 + 2000, kPrefillMs));

        // Both met: ready exactly at the prefill.
        SegmentClock ready{1000, kPrefillFrames, kRate};
        TEST_ASSERT(segment_ready(ready, 1000 + 600, kPrefillMs));
    }

    // ---- Steady-state chaining simulation ----
    // Feed audio in real time (240 frames per 5 ms). Each file records its
    // full segment window; it is issued ONCE, when its content crosses the
    // prefill, and the next file starts only when the current one is full
    // (the rotation). The issue cadence is therefore the segment length, and
    // every issued file holds ~the prefill of content - the delay is
    // prefill + command latency, not the segment length.
    {
        constexpr uint32_t kRate = 48000;
        constexpr uint32_t kPrefillMs = 600;
        constexpr uint32_t kSegMs = 2000;
        constexpr size_t kSegFrames = static_cast<size_t>(kRate) * kSegMs / 1000;

        uint64_t wall = 0;
        uint64_t seg_start = 0;
        size_t frames = 0;
        bool issued = false;
        std::vector<uint64_t> issue_times;   // segment-local wall time
        std::vector<size_t> issue_frames;    // content at issue
        std::vector<uint64_t> rotate_times;

        for (uint64_t step = 0; step < 2400; ++step) {  // 12 s of audio
            frames += 240;  // 5 ms of 48 kHz
            wall += 5;
            if (!issued &&
                segment_ready(SegmentClock{seg_start, frames, kRate}, wall, kPrefillMs)) {
                issue_times.push_back(wall - seg_start);
                issue_frames.push_back(frames);
                issued = true;
            }
            if (frames >= kSegFrames) {
                // Rotation: the file's window is complete; the next file
                // starts recording now.
                rotate_times.push_back(wall - seg_start);
                frames = 0;
                seg_start = wall;
                issued = false;
            }
        }
        TEST_ASSERT(issue_times.size() >= 5);
        TEST_ASSERT(rotate_times.size() >= 5);

        // Every issue lands right at the prefill of its file's window.
        const size_t prefill_frames = static_cast<size_t>(kRate) * kPrefillMs / 1000;
        for (size_t i = 0; i < issue_times.size(); ++i) {
            TEST_ASSERT(issue_times[i] >= kPrefillMs);
            TEST_ASSERT(issue_times[i] <= kPrefillMs + 5);
            TEST_ASSERT(issue_frames[i] >= prefill_frames);
            TEST_ASSERT(issue_frames[i] <= prefill_frames + 240);
        }
        // Rotations land exactly one segment length apart.
        for (uint64_t r : rotate_times) {
            TEST_ASSERT(r >= kSegMs);
            TEST_ASSERT(r <= kSegMs + 5);
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
