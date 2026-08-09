cmake_minimum_required(VERSION 3.20)
project(AudioRouter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings & hardening — keep compatible with original but ensure C++23
if (MSVC)
    add_compile_options(/W4 /EHsc /O2 /permissive-)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DNOMINMAX)
elseif (ANDROID)
    add_compile_options(-Wall -Wextra -O3)
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

message(WARNING "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(WARNING "ENV CXXFLAGS: $ENV{CXXFLAGS}")
message(WARNING "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(WARNING "CMAKE_CXX_FLAGS_RELEASE: ${CMAKE_CXX_FLAGS_RELEASE}")
message(STATUS "AudioRouter C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
