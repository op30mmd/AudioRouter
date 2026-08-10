set(CLIENT_SOURCES
    main.cpp
    client.cpp
    alsa_player.cpp
    direct_alsa.cpp
    agm_player.cpp
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
    agm_player.hpp
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
else()
    target_link_libraries(audiorouter_client PRIVATE
        pthread
        dl
    )
endif()
