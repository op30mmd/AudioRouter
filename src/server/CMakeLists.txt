set(SERVER_SOURCES
    main.cpp
    server.cpp
    wasapi_capture.cpp
    dummy_capture.cpp
    audio_endpoint_control.cpp
    ../common/socket_util.cpp
)

set(SERVER_HEADERS
    server.hpp
    audio_capture.hpp
    wasapi_capture.hpp
    dummy_capture.hpp
    audio_endpoint_control.hpp
    ../common/protocol.hpp
    ../common/socket_util.hpp
    ../common/audio_types.hpp
    ../common/ring_buffer.hpp
    ../common/logger.hpp
    ../common/time_util.hpp
)

add_executable(audiorouter_server ${SERVER_SOURCES} ${SERVER_HEADERS})

target_include_directories(audiorouter_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_compile_features(audiorouter_server PRIVATE cxx_std_23)
set_target_properties(audiorouter_server PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

if (WIN32)
    target_link_libraries(audiorouter_server PRIVATE
        ws2_32
        iphlpapi
        avrt
        ole32
    )
else()
    target_link_libraries(audiorouter_server PRIVATE
        pthread
    )
endif()
