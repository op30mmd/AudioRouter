#include "../src/common/protocol.hpp"
#include "../src/common/audio_types.hpp"
#include "../src/common/expected_compat.hpp"
#include <iostream>
#include <vector>
#include <type_traits>
#include <cmath>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool test_protocol_layout_safety() {
    using namespace audiorouter::protocol;

    // Check size requirements for serialization compatibility
    TEST_ASSERT(sizeof(CommonHeader) == 24);
    TEST_ASSERT(sizeof(AudioPacketHeader) == 36);

    // Verify layout and trivial copyability properties
    TEST_ASSERT(std::is_standard_layout_v<CommonHeader>);
    TEST_ASSERT(std::is_trivially_copyable_v<CommonHeader>);
    TEST_ASSERT(std::is_standard_layout_v<AudioPacketHeader>);

    return true;
}

bool test_audio_converter_clamping() {
    using namespace audiorouter;

    // Float to Int16 Conversion Clamping
    std::vector<float> input_floats = {1.5f, -2.0f, 0.5f, -0.5f, 0.0f};
    std::vector<int16_t> output_shorts(5);

    bool res = AudioConverter::float32_to_s16le(input_floats, output_shorts);
    TEST_ASSERT(res);

    TEST_ASSERT(output_shorts[0] == 32767);  // clamped from 1.5f
    TEST_ASSERT(output_shorts[1] == -32767); // clamped from -2.0f (using 32767.0f scaling)
    TEST_ASSERT(output_shorts[2] == 16383);  // 0.5 * 32767
    TEST_ASSERT(output_shorts[3] == -16383); // -0.5 * 32767
    TEST_ASSERT(output_shorts[4] == 0);

    // Int16 to Float Conversion Clamping / Scaling
    std::vector<int16_t> input_shorts = {32767, -32768, 0};
    std::vector<float> output_floats(3);

    res = AudioConverter::s16le_to_float32(input_shorts, output_floats);
    TEST_ASSERT(res);

    TEST_ASSERT(std::abs(output_floats[0] - (32767.0f / 32768.0f)) < 0.0001f);
    TEST_ASSERT(output_floats[1] == -1.0f); // -32768 / 32768
    TEST_ASSERT(output_floats[2] == 0.0f);

    return true;
}

bool test_audio_volume_safety() {
    using namespace audiorouter;

    std::vector<int16_t> samples = {1000, -1000, 20000, -20000};

    // Extreme amplification should clamp safely to max/min int16 values without wrap-around
    AudioConverter::apply_volume_s16le(samples, 10.0f);
    TEST_ASSERT(samples[0] == 10000);
    TEST_ASSERT(samples[1] == -10000);
    TEST_ASSERT(samples[2] == 32767);  // clamped from 200000
    TEST_ASSERT(samples[3] == -32768); // clamped from -200000

    // Zero volume should mute entirely
    AudioConverter::apply_volume_s16le(samples, 0.0f);
    for (auto s : samples) {
        TEST_ASSERT(s == 0);
    }

    // Invalid volume parameter (e.g. NaN or infinite) should fail-safe to zero (muted)
    samples = {500, -500};
    AudioConverter::apply_volume_s16le(samples, std::nanf(""));
    TEST_ASSERT(samples[0] == 0);
    TEST_ASSERT(samples[1] == 0);

    return true;
}

bool test_expected_type_wrapper() {
    using namespace audiorouter;

    // Success path
    expected<int, std::string> success_exp(42);
    TEST_ASSERT(success_exp.has_value());
    TEST_ASSERT((bool)success_exp == true);
    TEST_ASSERT(success_exp.value() == 42);
    TEST_ASSERT(*success_exp == 42);

    // Error path
    expected<int, std::string> error_exp = unexpected<std::string>("operation failed");
    TEST_ASSERT(!error_exp.has_value());
    TEST_ASSERT((bool)error_exp == false);
    TEST_ASSERT(error_exp.error() == "operation failed");

    // Void success path
    expected<void, std::string> void_success;
    TEST_ASSERT(void_success.has_value());

    // Void error path
    expected<void, std::string> void_error = unexpected<std::string>("failed void");
    TEST_ASSERT(!void_error.has_value());
    TEST_ASSERT(void_error.error() == "failed void");

    return true;
}

bool run_type_safety_tests() {
    TEST_ASSERT(test_protocol_layout_safety());
    TEST_ASSERT(test_audio_converter_clamping());
    TEST_ASSERT(test_audio_volume_safety());
    TEST_ASSERT(test_expected_type_wrapper());
    return true;
}
