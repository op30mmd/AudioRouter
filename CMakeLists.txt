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
