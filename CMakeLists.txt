cmake_minimum_required(VERSION 3.16)
project(NexusStream VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NEXUS_ENABLE_FFMPEG "Enable FFmpeg (libavformat/libavcodec) integration if available" ON)
option(NEXUS_BUILD_TESTS "Build unit and integration tests" ON)

add_compile_options(-Wall -Wextra -Wpedantic -O2)

find_package(Threads REQUIRED)

# Optional FFmpeg detection via PkgConfig
set(NEXUS_HAS_FFMPEG FALSE)
if(NEXUS_ENABLE_FFMPEG)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
            libavformat
            libavcodec
            libavutil
            libswscale
            libswresample
        )
        if(FFMPEG_FOUND)
            set(NEXUS_HAS_FFMPEG TRUE)
            message(STATUS "[NexusStream] FFmpeg found via pkg-config: enabling libav* RAII bridge")
        else()
            message(STATUS "[NexusStream] FFmpeg dev libraries not found: building with native C++20 codec/protocol engine")
        endif()
    endif()
endif()

# Core library sources
set(NEXUS_LIB_SOURCES
    src/codec/h264_parser.cpp
    src/codec/aac_parser.cpp
    src/codec/test_stream_generator.cpp
    src/protocol/rtp_packet.cpp
    src/protocol/sdp.cpp
    src/protocol/rtsp_message.cpp
    src/protocol/flv_muxer.cpp
    src/net/socket_utils.cpp
    src/net/event_loop.cpp
    src/net/tcp_server.cpp
    src/media/gop_cache.cpp
    src/media/media_hub.cpp
    src/media/rtsp_server.cpp
    src/media/http_flv_server.cpp
)

add_library(nexus_core STATIC ${NEXUS_LIB_SOURCES})
target_include_directories(nexus_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(nexus_core PUBLIC Threads::Threads)

if(NEXUS_HAS_FFMPEG)
    target_compile_definitions(nexus_core PUBLIC NEXUS_WITH_FFMPEG=1)
    target_link_libraries(nexus_core PUBLIC PkgConfig::FFMPEG)
endif()

# 1. Main Media Server executable (RTSP Server + HTTP-FLV Server + Web Dashboard)
add_executable(nexus_server src/apps/nexus_server_main.cpp)
target_link_libraries(nexus_server PRIVATE nexus_core)

# 2. Stream Tool / Pusher / FLV Muxer CLI executable
add_executable(nexus_pusher src/apps/nexus_pusher_main.cpp)
target_link_libraries(nexus_pusher PRIVATE nexus_core)

# 3. End-to-End Benchmark & Live Demo executable
add_executable(nexus_bench src/apps/nexus_bench_main.cpp)
target_link_libraries(nexus_bench PRIVATE nexus_core)

# 4. Unit & Integration Test Suite
if(NEXUS_BUILD_TESTS)
    enable_testing()
    add_executable(nexus_tests tests/test_main.cpp)
    target_link_libraries(nexus_tests PRIVATE nexus_core)
    add_test(NAME NexusUnitAndIntegrationTests COMMAND nexus_tests)
endif()
