set(SERVER_SOURCES
    main.cpp
    server.cpp
    wasapi_capture.cpp
    dummy_capture.cpp
    audio_endpoint_control.cpp
    plugin_chain.cpp
    vst3_host_stub.cpp
    ../common/socket_util.cpp
)

set(SERVER_HEADERS
    server.hpp
    audio_capture.hpp
    wasapi_capture.hpp
    dummy_capture.hpp
    audio_endpoint_control.hpp
    plugin_chain.hpp
    vst3_host.hpp
    ../common/protocol.hpp
    ../common/socket_util.hpp
    ../common/audio_types.hpp
    ../common/ring_buffer.hpp
    ../common/logger.hpp
    ../common/time_util.hpp
)

# VST3 plugin support: optional. The plugin chain abstraction
# (plugin_chain.{hpp,cpp}) is always compiled so the rest of the audio
# path always works. The "real" VST3 host (vst3_host.cpp) is only
# compiled when AUDIOROUTER_ENABLE_VST3 is ON AND the VST3 public SDK
# headers (third_party/vst3) are present; otherwise --vst3 at runtime
# is a no-op (the chain is a BypassPluginChain that logs a warning and
# forwards audio unchanged). The stub (vst3_host_stub.cpp) provides
# the make_vst3_stage() symbol in that case so the rest of the code
# always links.
option(AUDIOROUTER_ENABLE_VST3 "Build the VST3 audio effects host" OFF)
set(AUDIOROUTER_VST3_FOUND OFF)
set(AUDIOROUTER_VST3_INCLUDE_DIR "" CACHE PATH
    "Path to the VST3 public SDK headers (parent of pluginterfaces/vst/). "
    "Leave empty to use the vendored third_party/vst3/ tree when AUDIOROUTER_ENABLE_VST3=ON.")
if (AUDIOROUTER_ENABLE_VST3)
    if (AUDIOROUTER_VST3_INCLUDE_DIR)
        # Caller pointed us at an explicit SDK location. Trust it.
        if (EXISTS "${AUDIOROUTER_VST3_INCLUDE_DIR}/pluginterfaces/vst/ivstcomponent.h")
            set(AUDIOROUTER_VST3_FOUND ON)
            message(STATUS "VST3 host: enabled (SDK headers at ${AUDIOROUTER_VST3_INCLUDE_DIR})")
        else()
            message(FATAL_ERROR
                "AUDIOROUTER_VST3_INCLUDE_DIR is set to '${AUDIOROUTER_VST3_INCLUDE_DIR}' "
                "but does not contain the VST3 public SDK (expected "
                "${AUDIOROUTER_VST3_INCLUDE_DIR}/pluginterfaces/vst/ivstcomponent.h). "
                "Either point it at the directory that contains 'pluginterfaces/' "
                "or unset it to fall back to the vendored third_party/vst3/ tree.")
        endif()
    elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/vst3/pluginterfaces/vst/ivstcomponent.h")
        # Vendored SDK headers under third_party/vst3/. The public SDK
        # is BSD-licensed; a copy of the public headers is committed
        # there for reproducible builds.
        set(AUDIOROUTER_VST3_FOUND ON)
        message(STATUS "VST3 host: enabled (vendored headers under third_party/vst3/)")
    else()
        message(STATUS "VST3 host: disabled (third_party/vst3/ headers not found and "
                       "AUDIOROUTER_VST3_INCLUDE_DIR is not set; the public SDK is "
                       "BSD-licensed; place headers there or set "
                       "-DAUDIOROUTER_VST3_INCLUDE_DIR=<path> to enable the VST3 host)")
    endif()
endif()

# When the VST3 host is enabled we link vst3_host.cpp, NOT the stub.
# The stub is the symbol provider only for the no-SDK case. Removing
# the stub from the source list when VST3 is found avoids a duplicate
# make_vst3_stage() symbol at link time. This must happen before
# add_executable() so the source list is final.
if (AUDIOROUTER_VST3_FOUND)
    list(REMOVE_ITEM SERVER_SOURCES vst3_host_stub.cpp)
    list(APPEND SERVER_SOURCES vst3_host.cpp)
endif()

add_executable(audiorouter_server ${SERVER_SOURCES} ${SERVER_HEADERS})

target_include_directories(audiorouter_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

# Apply VST3-specific compile and link configuration to the target now
# that it exists. The plugin chain abstraction is always built; the
# VST3 host source was already added to SERVER_SOURCES above. The
# compile definition is what makes plugin_chain.cpp's #if branch
# resolve to the right call site.
if (AUDIOROUTER_VST3_FOUND)
    target_compile_definitions(audiorouter_server PRIVATE AUDIOROUTER_ENABLE_VST3=1)
    if (AUDIOROUTER_VST3_INCLUDE_DIR)
        target_include_directories(audiorouter_server PRIVATE "${AUDIOROUTER_VST3_INCLUDE_DIR}")
    else()
        target_include_directories(audiorouter_server PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/vst3"
        )
    endif()
    if (WIN32)
        # VST3 plugins on Windows link against the MS COM runtime.
        target_link_libraries(audiorouter_server PRIVATE ole32)
    endif()
else()
    # No SDK: the stub symbol (vst3_host_stub.cpp, still in
    # SERVER_SOURCES) is the one that ends up in the binary. Define
    # the macro to 0 so plugin_chain.cpp's #if branch is unambiguous.
    target_compile_definitions(audiorouter_server PRIVATE AUDIOROUTER_ENABLE_VST3=0)
endif()

if (WIN32)
    target_link_libraries(audiorouter_server PRIVATE
        ws2_32
        iphlpapi
        avrt
        ole32
    )

    # WASAPI loopback capture must run at the same integrity level as audio
    # producers. Without elevation, sessions owned by programs started with
    # "Run as administrator" can be absent from the captured mix.
    if (MSVC)
        # Let link.exe generate and embed the UAC section in its normal
        # manifest. Keeping this as a linker option avoids conflicting with
        # the default asInvoker manifest generated by MSVC/CMake.
        target_link_options(audiorouter_server PRIVATE
            "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
        )
    else()
        # GNU ld has no /MANIFESTUAC equivalent. Compile the same setting as a
        # Win32 RT_MANIFEST resource with windres.
        target_sources(audiorouter_server PRIVATE audiorouter_server.rc)
        set_property(SOURCE audiorouter_server.rc APPEND PROPERTY
            OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/audiorouter_server.manifest"
        )
    endif()

    # Do not accept a successful Windows link if the toolchain silently drops
    # the requested execution level. That would reintroduce silent capture for
    # elevated programs while leaving an otherwise healthy executable.
    add_custom_command(TARGET audiorouter_server POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DEXECUTABLE=$<TARGET_FILE:audiorouter_server>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_windows_manifest.cmake"
        VERBATIM
    )
else()
    target_link_libraries(audiorouter_server PRIVATE
        pthread
    )
endif()
