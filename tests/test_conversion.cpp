#include "../src/common/audio_types.hpp"
#include <iostream>
#include <vector>
#include <cmath>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool run_conversion_tests() {
    using namespace audiorouter;

    // Test float to s16le conversion and clamping
    std::vector<float> float_in = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f, -2.0f };
    std::vector<int16_t> int_out(float_in.size());

    AudioConverter::float32_to_s16le(float_in.data(), int_out.data(), float_in.size());

    TEST_ASSERT(int_out[0] == 0);
    TEST_ASSERT(int_out[1] == 32767);
    TEST_ASSERT(int_out[2] == -32767);
    TEST_ASSERT(int_out[3] == static_cast<int16_t>(0.5f * 32767.0f));
    TEST_ASSERT(int_out[5] == 32767);  // Clamped
    TEST_ASSERT(int_out[6] == -32767); // Clamped

    // Test volume scaling
    std::vector<int16_t> vol_test = { 1000, -1000, 20000 };
    AudioConverter::apply_volume_s16le(vol_test.data(), vol_test.size(), 0.5f);
    TEST_ASSERT(vol_test[0] == 500);
    TEST_ASSERT(vol_test[1] == -500);
    TEST_ASSERT(vol_test[2] == 10000);

    // Test downmixing
    std::vector<float> mono_in = { 0.8f, -0.4f };
    std::vector<float> stereo_out(4);
    AudioConverter::downmix_to_stereo_float(mono_in.data(), 1, stereo_out.data(), 2);
    TEST_ASSERT(stereo_out[0] == 0.8f && stereo_out[1] == 0.8f);
    TEST_ASSERT(stereo_out[2] == -0.4f && stereo_out[3] == -0.4f);

    return true;
}
