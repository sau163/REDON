# CMakeLists.txt — build configuration for Redon.
#
# Produces two executables (redon-server, redon-cli) plus two test programs.
# Works with Visual Studio 2022 on Windows and GCC/Clang on Linux/macOS.
#
#   cmake -S . -B build
#   cmake --build build --config Release
#   ctest --test-dir build -C Release        # run the tests
cmake_minimum_required(VERSION 3.15)
project(Redon LANGUAGES CXX)

# Require C++17 (we use std::lock_guard, structured code, etc.) with no
# compiler-specific extensions, for portability.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a Release build for single-config generators (Makefiles, Ninja) if
# the user didn't pick one. Multi-config generators (Visual Studio) ignore this
# and choose with --config instead.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Reasonable warning levels (not treated as errors, so a stray warning never
# blocks a learning build).
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

# Find the system threads library (maps to -pthread on Linux/macOS; a no-op on
# MSVC). Needed because Phase 2 uses std::thread / std::mutex / condition_variable.
find_package(Threads REQUIRED)

# An INTERFACE target carrying the src/ include path and, on Windows, the
# Winsock library. Anything that links it can #include "net.h" and use sockets.
add_library(redon_net INTERFACE)
target_include_directories(redon_net INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
  target_link_libraries(redon_net INTERFACE ws2_32)
endif()

# The core engine: storage + protocol + server + thread pool. Reused by the
# server exe and by the tests so we test exactly the code that ships.
add_library(redon_core STATIC
  src/storage.cpp
  src/command.cpp
  src/server.cpp
  src/thread_pool.cpp
  src/wal.cpp
  src/replication.cpp
  src/raft.cpp
  src/router.cpp
  src/disk_store.cpp
  src/metrics.cpp
)
target_link_libraries(redon_core PUBLIC redon_net Threads::Threads)

# The server executable.
add_executable(redon-server src/main.cpp)
target_link_libraries(redon-server PRIVATE redon_core)

# The interactive client (only needs the socket header, not the engine).
add_executable(redon-cli src/client_main.cpp)
target_link_libraries(redon-cli PRIVATE redon_net)

# A concurrent load generator / benchmark (uses threads + sockets, not the engine).
add_executable(redon-bench bench/redon_bench.cpp)
target_link_libraries(redon-bench PRIVATE redon_net Threads::Threads)

# ---- Tests --------------------------------------------------------------
enable_testing()

add_executable(test_storage tests/test_storage.cpp)
target_link_libraries(test_storage PRIVATE redon_core)
target_include_directories(test_storage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME storage COMMAND test_storage)

add_executable(test_command tests/test_command.cpp)
target_link_libraries(test_command PRIVATE redon_core)
target_include_directories(test_command PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME command COMMAND test_command)

add_executable(test_thread_pool tests/test_thread_pool.cpp)
target_link_libraries(test_thread_pool PRIVATE redon_core)
target_include_directories(test_thread_pool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME thread_pool COMMAND test_thread_pool)

add_executable(test_wal tests/test_wal.cpp)
target_link_libraries(test_wal PRIVATE redon_core)
target_include_directories(test_wal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME wal COMMAND test_wal)

add_executable(test_raft tests/test_raft.cpp)
target_link_libraries(test_raft PRIVATE redon_core)
target_include_directories(test_raft PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME raft COMMAND test_raft)

add_executable(test_router tests/test_router.cpp)
target_link_libraries(test_router PRIVATE redon_core)
target_include_directories(test_router PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME router COMMAND test_router)

add_executable(test_disk_store tests/test_disk_store.cpp)
target_link_libraries(test_disk_store PRIVATE redon_core)
target_include_directories(test_disk_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME disk_store COMMAND test_disk_store)

add_executable(test_metrics tests/test_metrics.cpp)
target_link_libraries(test_metrics PRIVATE redon_core)
target_include_directories(test_metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME metrics COMMAND test_metrics)
