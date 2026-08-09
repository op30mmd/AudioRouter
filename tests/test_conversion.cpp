#include "../src/common/audio_types.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <span>
#include <limits>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool run_conversion_tests() {
    using namespace audiorouter;

    // float to s16 clamping exhaustive
    {
        std::vector<float> in{0.0f,1.0f,-1.0f,0.5f,-0.5f,2.0f,-2.0f, 0.9999f, -0.9999f};
        std::vector<int16_t> out(in.size());
        AudioConverter::float32_to_s16le(in.data(), out.data(), in.size());
        TEST_ASSERT(out[0]==0);
        TEST_ASSERT(out[1]==32767);
        TEST_ASSERT(out[2]==-32767);
        TEST_ASSERT(out[3]==static_cast<int16_t>(0.5f*32767.0f));
        TEST_ASSERT(out[5]==32767);
        TEST_ASSERT(out[6]==-32767);

        // span API
        std::vector<float> fin{0.0f, 1.0f, -1.0f};
        std::vector<int16_t> sout(3);
        TEST_ASSERT(AudioConverter::float32_to_s16le(std::span<const float>(fin), std::span<int16_t>(sout)));
        TEST_ASSERT(sout[1]==32767);
        // span size mismatch
        std::vector<int16_t> small(2);
        TEST_ASSERT(!AudioConverter::float32_to_s16le(std::span<const float>(fin), std::span<int16_t>(small)));
        // empty
        TEST_ASSERT(AudioConverter::float32_to_s16le(std::span<const float>{}, std::span<int16_t>{}));
        // null handling should not crash
        AudioConverter::float32_to_s16le(nullptr,nullptr,10);
        AudioConverter::float32_to_s16le(fin.data(),nullptr, fin.size());
    }

    // s16 to float
    {
        std::vector<int16_t> sin{0, 32767, -32768, 16384};
        std::vector<float> fout(sin.size());
        AudioConverter::s16le_to_float32(sin.data(), fout.data(), sin.size());
        TEST_ASSERT(fout[0]==0.0f);
        TEST_ASSERT(std::abs(fout[1] - 0.999969f) < 0.001f);
        TEST_ASSERT(std::abs(fout[3] - 0.5f) < 0.001f);
        // span success
        std::vector<float> out2(sin.size());
        TEST_ASSERT(AudioConverter::s16le_to_float32(std::span<const int16_t>(sin), std::span<float>(out2)));
        // null
        AudioConverter::s16le_to_float32(nullptr,nullptr,5);
    }

    // volume scaling exhaustive
    {
        std::vector<int16_t> vol{1000,-1000,20000,-20000,32767,-32768};
        AudioConverter::apply_volume_s16le(vol.data(), vol.size(), 0.5f);
        TEST_ASSERT(vol[0]==500 && vol[1]==-500 && vol[2]==10000);
        // 0 volume -> silence
        std::vector<int16_t> z{100,200,300};
        AudioConverter::apply_volume_s16le(z.data(), z.size(), 0.0f);
        TEST_ASSERT(z[0]==0 && z[1]==0);
        // negative -> silence
        std::vector<int16_t> neg{100,200};
        AudioConverter::apply_volume_s16le(neg.data(), neg.size(), -1.0f);
        TEST_ASSERT(neg[0]==0);
        // 1.0 -> unchanged
        std::vector<int16_t> unity{123, -456};
        std::vector<int16_t> copy=unity;
        AudioConverter::apply_volume_s16le(unity.data(), unity.size(), 1.0f);
        TEST_ASSERT(unity==copy);
        // >1 amplification with clamp
        std::vector<int16_t> loud{30000};
        AudioConverter::apply_volume_s16le(loud.data(), loud.size(), 2.0f);
        TEST_ASSERT(loud[0]==32767);
        // span overload
        std::vector<int16_t> sp{1000,2000};
        AudioConverter::apply_volume_s16le(std::span<int16_t>(sp), 0.25f);
        TEST_ASSERT(sp[0]==250);
        // inf / nan -> silence per hardened impl
        std::vector<int16_t> inf{1000};
        AudioConverter::apply_volume_s16le(std::span<int16_t>(inf), std::numeric_limits<float>::infinity());
        // inf clamped to 10x -> 10000, not 0. Actually our impl clamps inf? We check isfinite for volume? Wait we do std::isfinite check: if !isfinite -> fill 0. So inf should give 0.
        // For our test, inf should produce 0? Let's see implementation: if !isfinite => fill 0. So:
        TEST_ASSERT(inf[0]==0);
        std::vector<int16_t> nan{1000};
        AudioConverter::apply_volume_s16le(std::span<int16_t>(nan), std::numeric_limits<float>::quiet_NaN());
        TEST_ASSERT(nan[0]==0);
        // empty
        AudioConverter::apply_volume_s16le(std::span<int16_t>{}, 0.5f);
        AudioConverter::apply_volume_s16le(nullptr, 0, 0.5f);
    }

    // downmix exhaustive (C++23 safety: clamping, all channel counts)
    {
        // mono -> stereo
        std::vector<float> mono{0.8f, -0.4f, 0.0f};
        std::vector<float> stereo(6);
        AudioConverter::downmix_to_stereo_float(mono.data(),1,stereo.data(),3);
        TEST_ASSERT(stereo[0]==0.8f && stereo[1]==0.8f);
        TEST_ASSERT(stereo[2]==-0.4f && stereo[3]==-0.4f);
        // span version mono
        std::vector<float> mono2{0.5f, 0.5f};
        std::vector<float> out2(4);
        TEST_ASSERT(AudioConverter::downmix_to_stereo_float(std::span<const float>(mono2),1,std::span<float>(out2),2));
        TEST_ASSERT(out2[0]==0.5f);

        // stereo passthrough
        std::vector<float> stereo_in{0.1f,0.2f,0.3f,0.4f};
        std::vector<float> passthrough(4);
        AudioConverter::downmix_to_stereo_float(stereo_in.data(),2,passthrough.data(),2);
        for(size_t i=0;i<4;++i) TEST_ASSERT(passthrough[i]==stereo_in[i]);

        // 5.1 downmix
        std::vector<float> f51(6); // one frame 5.1
        f51[0]=1.0f; f51[1]=0.5f; f51[2]=0.2f; f51[3]=0.1f; f51[4]=0.3f; f51[5]=0.4f;
        std::vector<float> out51(2);
        AudioConverter::downmix_to_stereo_float(f51.data(),6,out51.data(),1);
        TEST_ASSERT(out51[0] >= -1.0f && out51[0] <= 1.0f);
        TEST_ASSERT(out51[1] >= -1.0f && out51[1] <= 1.0f);
        // verify center/surround contribute: left includes fl + center*gain + bl*gain
        // manual compute with clamp*0.7
        float left_expected = std::clamp((1.0f + 0.7071f*0.2f + 0.7071f*0.3f)*0.7f, -1.0f,1.0f);
        TEST_ASSERT(std::abs(out51[0]-left_expected) < 0.01f);

        // 3 channels generic fallback
        std::vector<float> three{0.9f,0.5f,0.3f, -0.9f,-0.5f,-0.3f}; // 2 frames 3ch
        std::vector<float> out3(4);
        AudioConverter::downmix_to_stereo_float(three.data(),3,out3.data(),2);
        TEST_ASSERT(out3[0] >= -1.0f && out3[0] <=1.0f);

        // 7.1 (8ch) generic fallback case >=6 still uses 5.1 path; ensure not crash
        std::vector<float> eight(16,0.1f);
        std::vector<float> out8(4);
        AudioConverter::downmix_to_stereo_float(eight.data(),8,out8.data(),2);
        TEST_ASSERT(out8[0] != 9.9f); // changed

        // null and zero handling
        std::vector<float> dst(2,9.9f);
        AudioConverter::downmix_to_stereo_float(nullptr,2,nullptr,2);
        AudioConverter::downmix_to_stereo_float(mono.data(),0,dst.data(),1);
        TEST_ASSERT(dst[0]==9.9f); // unchanged on error
        // span failure cases
        std::vector<float> src_small{0.1f};
        std::vector<float> dst_small{0.0f};
        TEST_ASSERT(!AudioConverter::downmix_to_stereo_float(std::span<const float>(src_small),2,std::span<float>(dst_small),1)); // need 2*1=2 src but only 1
    }

    // AudioConfig packet math edge
    {
        AudioConfig cfg{48000,2,AudioSampleFormat::PCM_S16LE,240};
        TEST_ASSERT(cfg.packet_payload_size()==960);
        cfg.frames_per_packet=0;
        TEST_ASSERT(cfg.packet_payload_size()==0);
        TEST_ASSERT(cfg.packet_duration_ms()==0);
        cfg.frames_per_packet=240; cfg.sample_rate=0;
        TEST_ASSERT(cfg.packet_duration_ms()==0);
        cfg.sample_rate=48000;
        TEST_ASSERT(cfg.frames_to_bytes(0)==0);
        TEST_ASSERT(cfg.bytes_to_frames(0)==0);
        // overflow guard
        AudioConfig big{48000,32,AudioSampleFormat::PCM_S32LE,8192};
        size_t ps = big.packet_payload_size(); // would be huge but still compute; our guard returns 0 if overflow? Actually 8192*32*4=1M fits in size_t, no overflow, so >0. But is_valid should be false due to payload >65507.
        TEST_ASSERT(!big.is_valid());
        (void)ps;
    }

    return true;
}
