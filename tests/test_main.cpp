#include "../src/common/logger.hpp"
#include <iostream>
#include <vector>
#include <functional>
#include <string>

namespace test {
    struct TestCase {
        std::string name;
        std::function<bool()> test_fn;
    };

    static std::vector<TestCase>& get_tests() {
        static std::vector<TestCase> tests;
        return tests;
    }

    void register_test(const std::string& name, std::function<bool()> test_fn) {
        get_tests().push_back({name, test_fn});
    }
}

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

// Declare test suites
bool run_protocol_tests();
bool run_ring_buffer_tests();
bool run_jitter_buffer_tests();
bool run_socket_tests();
bool run_conversion_tests();

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    audiorouter::Logger::instance().set_level(audiorouter::LogLevel::Info);

    std::cout << "\n=========================================\n";
    std::cout << " Running AudioRouter Comprehensive Tests\n";
    std::cout << "=========================================\n\n";

    test::register_test("Protocol & Packet Serialization", run_protocol_tests);
    test::register_test("Audio Ring Buffer", run_ring_buffer_tests);
    test::register_test("Adaptive Jitter Buffer & PLC", run_jitter_buffer_tests);
    test::register_test("Socket & Network Address Parsing", run_socket_tests);
    test::register_test("Audio Converter & Downmixing", run_conversion_tests);

    int passed = 0;
    int failed = 0;

    for (const auto& test_case : test::get_tests()) {
        std::cout << "[ RUN      ] " << test_case.name << std::endl;
        bool ok = false;
        try {
            ok = test_case.test_fn();
        } catch (const std::exception& ex) {
            std::cerr << "Exception: " << ex.what() << "\n";
            ok = false;
        }

        if (ok) {
            std::cout << "[       OK ] " << test_case.name << "\n";
            passed++;
        } else {
            std::cout << "[  FAILED  ] " << test_case.name << "\n";
            failed++;
        }
    }

    std::cout << "\n=========================================\n";
    std::cout << " Test Summary: " << passed << " Passed, " << failed << " Failed\n";
    std::cout << "=========================================\n\n";

    return (failed == 0) ? 0 : 1;
}
