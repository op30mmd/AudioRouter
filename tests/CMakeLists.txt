set(TEST_SOURCES
    test_main.cpp
    test_protocol.cpp
    test_ring_buffer.cpp
    test_jitter_buffer.cpp
    test_socket.cpp
    test_conversion.cpp
    ../src/client/jitter_buffer.cpp
    ../src/common/socket_util.cpp
)

add_executable(audiorouter_tests ${TEST_SOURCES})

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
