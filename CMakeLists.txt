cmake_minimum_required(VERSION 3.20)
project(AudioRouter VERSION 1.0.0 LANGUAGES CXX)

# C++23 required — all targets must opt into the latest standard.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for tooling / clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Hardening & warning flags
if (MSVC)
    add_compile_options(/W4 /EHsc /O2 /permissive- /std:c++latest)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
    # Control Flow Guard + Spectre mitigations where available
    add_compile_options(/guard:cf)
    add_link_options(/guard:cf)
else()
    # Baseline warnings — keep -Werror off for portability but be very noisy.
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnull-dereference
                        -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wnon-virtual-dtor
                        -Woverloaded-virtual -Wunused -Wold-style-cast -Wcast-align)
    # Hardening (non-Android cross builds get stack protector + fortify)
    if (NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
        add_compile_options(-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE)
        add_link_options(-pie)
    endif()
    # Optimization — respect CMAKE_BUILD_TYPE
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-O0 -g -fno-omit-frame-pointer)
        # Optionally enable sanitizers in Debug (ON by default for Debug on non-Windows)
        option(ENABLE_SANITIZERS "Enable ASan+UBSan in Debug" ON)
        if (ENABLE_SANITIZERS AND NOT WIN32 AND NOT ANDROID)
            add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
            add_link_options(-fsanitize=address,undefined)
        endif()
        option(ENABLE_TSAN "Enable ThreadSanitizer (mutually exclusive with ASan)" OFF)
        if (ENABLE_TSAN)
            add_compile_options(-fsanitize=thread)
            add_link_options(-fsanitize=thread)
        endif()
    else()
        add_compile_options(-O3)
    endif()
    add_compile_options(-pthread)
    add_link_options(-pthread)
endif()

# Request C++23 features explicitly on every target via interface
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/common)

# Subdirectories
add_subdirectory(src/client)

if (WIN32 OR (NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android"))
    add_subdirectory(src/server)
endif()

if (NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    enable_testing()
    add_subdirectory(tests)
endif()

# Print summary
message(STATUS "AudioRouter C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
