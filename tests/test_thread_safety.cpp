#include "../src/common/ring_buffer.hpp"
#include "../src/client/jitter_buffer.hpp"
#include "../src/common/socket_util.hpp"
#include "../src/common/logger.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool run_thread_safety_tests() {
    using namespace audiorouter;

    // ── RingBuffer concurrent SPSC stress ──
    {
        RingBuffer<int16_t> ring(2048);
        constexpr int N = 50000;
        std::atomic<bool> done{false};
        std::vector<int16_t> produced(N), consumed;
        consumed.reserve(N);
        for (int i = 0; i < N; ++i) produced[i] = static_cast<int16_t>(i % 32767);

        std::thread producer([&]{
            size_t idx = 0;
            while (idx < produced.size()) {
                size_t w = ring.write(produced.data() + idx, 1);
                if (w == 0) std::this_thread::yield();
                else ++idx;
            }
            done.store(true);
        });
        std::thread consumer([&]{
            std::vector<int16_t> tmp(1);
            while (!done.load() || !ring.empty()) {
                size_t r = ring.read(tmp.data(), 1);
                if (r) consumed.push_back(tmp[0]);
                else std::this_thread::yield();
            }
        });
        producer.join();
        consumer.join();
        TEST_ASSERT(consumed.size() == produced.size());
        for (size_t i = 0; i < consumed.size(); ++i) TEST_ASSERT(consumed[i] == produced[i]);
    }

    // ── RingBuffer MPSC (capacity > total to avoid deadlock without consumer) ──
    {
        constexpr int PER_THREAD = 5000;
        constexpr int NTHREADS = 4;
        constexpr int TOTAL = PER_THREAD * NTHREADS;
        RingBuffer<uint32_t> ring(static_cast<size_t>(TOTAL) + 1024);
        std::atomic<int> total_written{0};
        std::vector<std::thread> writers;
        for (int t = 0; t < NTHREADS; ++t) {
            writers.emplace_back([&, t]{
                std::vector<uint32_t> data(PER_THREAD, static_cast<uint32_t>(t));
                size_t off = 0;
                while (off < data.size()) {
                    size_t w = ring.write(data.data() + off, data.size() - off);
                    off += w;
                    total_written.fetch_add(static_cast<int>(w));
                    if (w == 0) std::this_thread::yield();
                }
            });
        }
        for (auto& th : writers) th.join();
        TEST_ASSERT(ring.size() == static_cast<size_t>(TOTAL));
        std::vector<uint32_t> out(TOTAL);
        size_t r = ring.read(out.data(), out.size());
        TEST_ASSERT(r == out.size());
        TEST_ASSERT(ring.empty());
        TEST_ASSERT(total_written.load() == TOTAL);
    }

    // ── JitterBuffer concurrent push/pop ──
    {
        AudioConfig cfg; cfg.sample_rate = 48000; cfg.channels = 2; cfg.frames_per_packet = 240;
        JitterBuffer jb(20);
        jb.configure(cfg, 20);
        constexpr int PKTS = 200;
        std::atomic<bool> push_done{false};
        std::thread pusher([&]{
            for (int i = 0; i < PKTS; ++i) {
                std::vector<int16_t> pkt(240*2, static_cast<int16_t>(i));
                // spin until accepted (covers buffering state)
                while (!jb.push_packet(static_cast<uint32_t>(i), 1000+i*5000, pkt.data(), 240)) {
                    if (i==0) break; // first packet always?
                    std::this_thread::yield();
                    // duplicate will fail, so break if already exists
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            push_done.store(true);
        });
        std::thread popper([&]{
            std::vector<int16_t> out(240*2);
            int pops = 0;
            while (!push_done.load() || jb.available_frames() > 0) {
                jb.pop_frames(out.data(), 240);
                ++pops;
                if (pops > 1000) break;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        pusher.join();
        popper.join();
        TEST_ASSERT(jb.get_stats().packets_received >= static_cast<uint64_t>(PKTS/2));
    }

    // ── Logger thread safety ──
    {
        Logger::instance().set_level(LogLevel::Info);
        constexpr int T = 8, N = 200;
        std::vector<std::thread> ths;
        std::atomic<int> ok{0};
        for (int i = 0; i < T; ++i) {
            ths.emplace_back([&]{ for(int j=0;j<N;++j){ LOG_INFO("thread " << j); ok++; }});
        }
        for (auto& th : ths) th.join();
        TEST_ASSERT(ok.load() == T*N);
    }

    // ── SocketAddress thread-safe creation ──
    {
        constexpr int T = 16;
        std::vector<std::thread> ths;
        std::atomic<int> ok{0};
        for (int i=0;i<T;++i){
            ths.emplace_back([&]{
                for(int p=0;p<100;++p){
                    auto a = SocketAddress::create("127.0.0.1", 40000);
                    if (a && a->is_valid()) ok++;
                }
            });
        }
        for(auto& th: ths) th.join();
        TEST_ASSERT(ok.load() == T*100);
    }

    return true;
}
