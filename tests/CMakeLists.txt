set(TEST_SOURCES
    test_main.cpp
    test_protocol.cpp
    test_ring_buffer.cpp
    test_jitter_buffer.cpp
    test_socket.cpp
    test_conversion.cpp
    test_thread_safety.cpp
    test_type_safety.cpp
    test_memory_safety.cpp
    ../src/client/jitter_buffer.cpp
    ../src/common/socket_util.cpp
)

add_executable(audiorouter_tests ${TEST_SOURCES})

target_include_directories(audiorouter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/client
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/server
)

target_compile_features(audiorouter_tests PRIVATE cxx_std_23)
set_target_properties(audiorouter_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

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
