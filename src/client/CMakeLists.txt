set(CLIENT_SOURCES
    main.cpp
    client.cpp
    alsa_player.cpp
    direct_alsa.cpp
    agm_fifo_player.cpp
    aaudio_player.cpp
    dummy_player.cpp
    jitter_buffer.cpp
    android_helpers.cpp
    ../common/socket_util.cpp
)

set(CLIENT_HEADERS
    client.hpp
    audio_player.hpp
    alsa_player.hpp
    direct_alsa.hpp
    agm_fifo_player.hpp
    aaudio_player.hpp
    dummy_player.hpp
    jitter_buffer.hpp
    android_helpers.hpp
    ../common/protocol.hpp
    ../common/socket_util.hpp
    ../common/audio_types.hpp
    ../common/ring_buffer.hpp
    ../common/logger.hpp
    ../common/time_util.hpp
)

add_executable(audiorouter_client ${CLIENT_SOURCES} ${CLIENT_HEADERS})

target_include_directories(audiorouter_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

if (WIN32)
    target_link_libraries(audiorouter_client PRIVATE
        ws2_32
        iphlpapi
    )
elseif (ANDROID)
    target_link_libraries(audiorouter_client PRIVATE
        dl
        log
    )
    # AAudio (no-root playback) requires API 26+; on older platform targets
    # aaudio_player.cpp compiles as a stub and nothing extra is linked.
    if (ANDROID_PLATFORM MATCHES "^android-(2[6-9]|[3-9][0-9])$" OR
        ANDROID_PLATFORM MATCHES "^(2[6-9]|[3-9][0-9])$")
        target_link_libraries(audiorouter_client PRIVATE aaudio)
        # Standalone AAudio FIFO daemon (see src/tools/stream_daemon.cpp).
        add_executable(stream_daemon ../tools/stream_daemon.cpp)
        target_link_libraries(stream_daemon PRIVATE aaudio)
    endif()
else()
    target_link_libraries(audiorouter_client PRIVATE
        pthread
        dl
    )
endif()
