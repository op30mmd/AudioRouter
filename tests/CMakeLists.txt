cmake_minimum_required(VERSION 3.16)
project(audiorouter_tests CXX)

set(TEST_SOURCES
    test_main.cpp
    test_protocol.cpp
    test_ring_buffer.cpp
    test_jitter_buffer.cpp
    test_socket.cpp
    test_usb_tunnel.cpp
    test_conversion.cpp
    test_termux_api.cpp
    test_pulse.cpp
    test_thread_safety.cpp
    test_type_safety.cpp
    test_memory_safety.cpp
    ../src/client/jitter_buffer.cpp
    ../src/client/termux_api_player.cpp
    ../src/client/pulse_player.cpp
    ../src/client/dummy_player.cpp
    ../src/client/aaudio_player.cpp
    ../src/client/android_helpers.cpp
    ../src/common/socket_util.cpp
)

add_executable(audiorouter_tests ${TEST_SOURCES})

# Mirror the client's optional PulseAudio backend so the tests exercise the
# same build of pulse_player.cpp (stub or real).
audiorouter_link_pulseaudio(audiorouter_tests)

target_include_directories(audiorouter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/client
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/server
)

if (WIN32)
    target_link_libraries(audiorouter_tests PRIVATE
        ws2_32
        iphlpapi
    )
else()
    target_link_libraries(audiorouter_tests PRIVATE
        pthread
        dl
    )
endif()

add_test(NAME AllTests COMMAND audiorouter_tests)
