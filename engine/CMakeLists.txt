cmake_minimum_required(VERSION 3.20)
project(payment_journey_tracker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The live Redis reader needs hiredis (a C library). It is easy to install on Linux
# (apt install libhiredis-dev) and is built inside Docker, so it is off by default.
# Everything else (engine, tests, file replay) builds anywhere, including Windows.
option(PJT_WITH_REDIS "Build pjt_stream, the live Redis Streams reader" OFF)
set(PJT_SANITIZE "" CACHE STRING "Build with a sanitizer: address, thread, or empty for none")

if(PJT_SANITIZE)
  add_compile_options(-fsanitize=${PJT_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${PJT_SANITIZE})
  if(PJT_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
  endif()
endif()

include(FetchContent)

# JSON reading/writing (header only), pinned by SHA-256.
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# The engine itself, as a library the programs and tests share.
add_library(pjt_core
  src/stage.cpp
  src/event.cpp
  src/records.cpp
  src/config.cpp
  src/slowdown.cpp
  src/tracker.cpp
  src/json_sink.cpp
  src/util.cpp)
target_include_directories(pjt_core PUBLIC include)
target_link_libraries(pjt_core PUBLIC nlohmann_json::nlohmann_json)
if(MSVC)
  target_compile_options(pjt_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(pjt_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(pjt_replay src/replay_main.cpp)
target_link_libraries(pjt_replay PRIVATE pjt_core)

if(PJT_WITH_REDIS)
  find_path(HIREDIS_HEADER hiredis REQUIRED)
  find_library(HIREDIS_LIB hiredis REQUIRED)
  set(REDIS_PLUS_PLUS_BUILD_TEST OFF CACHE BOOL "" FORCE)
  set(REDIS_PLUS_PLUS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(REDIS_PLUS_PLUS_CXX_STANDARD 20 CACHE STRING "" FORCE)
  # redis-plus-plus 1.3.13, pinned to its exact commit.
  FetchContent_Declare(redis_plus_plus
    GIT_REPOSITORY https://github.com/sewenew/redis-plus-plus.git
    GIT_TAG 3257aa87ca2591f0ede5d6293c47f47c6e0828bd)
  FetchContent_MakeAvailable(redis_plus_plus)
  add_executable(pjt_stream src/stream_main.cpp)
  # redis-plus-plus keeps one generated header in its build folder; make it visible.
  target_include_directories(pjt_stream PRIVATE ${HIREDIS_HEADER} ${redis_plus_plus_BINARY_DIR}/src)
  target_link_libraries(pjt_stream PRIVATE pjt_core redis++_static ${HIREDIS_LIB})
endif()

# Tests
option(PJT_BUILD_TESTS "Build the unit tests" ON)
if(PJT_BUILD_TESTS)
  # GoogleTest 1.15.2, pinned to its exact commit.
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # needed on Windows
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(pjt_tests
    tests/test_stage.cpp
    tests/test_event.cpp
    tests/test_config.cpp
    tests/test_tracker.cpp
    tests/test_ordering.cpp
    tests/test_stuck.cpp
    tests/test_slowdown.cpp
    tests/test_snapshot.cpp)
  target_link_libraries(pjt_tests PRIVATE pjt_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(pjt_tests DISCOVERY_TIMEOUT 60)
endif()
