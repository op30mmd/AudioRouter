cmake_minimum_required(VERSION 3.20)
project(AudioRouter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MinGW needs its resource compiler enabled so the server's UAC manifest is
# embedded in the executable. MSVC generates the equivalent manifest through
# its linker options (see src/server/CMakeLists.txt).
if (WIN32 AND NOT MSVC)
    enable_language(RC)
endif()

# Warnings & hardening — keep compatible with original but ensure C++23
if (MSVC)
    add_compile_options(/W4 /EHsc /O2 /permissive-)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DNOMINMAX)
elseif (ANDROID)
    # -Wno-unavailable-declarations: AAudio.h tags newer APIs with availability
    # attributes; the aaudio player guards them by API level, this silences the
    # annotations when targeting android-26 exactly.
    add_compile_options(-Wall -Wextra -O3 -Wno-unavailable-declarations)
else()
    add_compile_options(-Wall -Wextra -O3 -pthread)
endif()

# PulseAudio playback backend (-d pulse), shared by the client and the tests.
# Optional: without libpulse-simple, pulse_player.cpp compiles as a stub whose
# open() fails fast and the client falls back to the other backends. Detected
# once here so both targets agree; turn off with
# -DAUDIOROUTER_ENABLE_PULSEAUDIO=OFF.
option(AUDIOROUTER_ENABLE_PULSEAUDIO "Build the PulseAudio playback backend (needs libpulse-simple)" ON)
set(AUDIOROUTER_PULSE_FOUND OFF)
if (AUDIOROUTER_ENABLE_PULSEAUDIO AND NOT WIN32)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(PULSE_SIMPLE QUIET libpulse-simple)
    endif()
    if (PULSE_SIMPLE_FOUND)
        set(AUDIOROUTER_PULSE_FOUND ON)
        message(STATUS "PulseAudio backend: enabled (libpulse-simple ${PULSE_SIMPLE_VERSION})")
    else()
        message(STATUS "PulseAudio backend: disabled (libpulse-simple not found; install libpulse-dev)")
    endif()
endif()

# Applies the detected PulseAudio flags to a target (no-op when not found).
function(audiorouter_link_pulseaudio target)
    if (AUDIOROUTER_PULSE_FOUND)
        target_compile_definitions(${target} PRIVATE PULSEAUDIO_ENABLED=1)
        target_include_directories(${target} PRIVATE ${PULSE_SIMPLE_INCLUDE_DIRS})
        target_link_directories(${target} PRIVATE ${PULSE_SIMPLE_LIBRARY_DIRS})
        target_link_libraries(${target} PRIVATE ${PULSE_SIMPLE_LIBRARIES})
    endif()
endfunction()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/common)

add_subdirectory(src/client)

if (WIN32 OR (NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android"))
    add_subdirectory(src/server)
endif()

if (NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
