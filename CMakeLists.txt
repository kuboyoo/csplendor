cmake_minimum_required(VERSION 3.13)
project(csplendor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CSPLENDOR_BUILD_PYTHON_MODULE "Build the pybind11 extension" ON)
option(CSPLENDOR_BUILD_NATIVE_TESTS "Build native C++ tests" OFF)
option(CSPLENDOR_BUILD_PARALLEL_BENCHMARK "Build native MCTS benchmark" OFF)
set(CSPLENDOR_CPU_TARGET "portable" CACHE STRING
    "CPU target: portable or native")
set_property(CACHE CSPLENDOR_CPU_TARGET PROPERTY STRINGS portable native)
set(CSPLENDOR_SANITIZER "none" CACHE STRING
    "Sanitizer configuration: none, thread, or address-undefined")
set_property(CACHE CSPLENDOR_SANITIZER PROPERTY STRINGS
             none thread address-undefined)

find_package(Threads REQUIRED)

add_library(csplendor_core_config INTERFACE)
target_include_directories(
    csplendor_core_config
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(csplendor_core_config INTERFACE cxx_std_17)

set(CSPLENDOR_CORE_SOURCES
    src/game_snapshot.cpp
    src/mcts_parallel_trace.cpp)
add_library(csplendor_core STATIC EXCLUDE_FROM_ALL ${CSPLENDOR_CORE_SOURCES})
set_target_properties(csplendor_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(csplendor_warnings INTERFACE)
if(MSVC)
  target_compile_options(csplendor_warnings INTERFACE /W4 /WX /permissive-)
else()
  target_compile_options(csplendor_warnings INTERFACE
                         -Wall -Wextra -Wpedantic -Werror)
endif()

if(CSPLENDOR_CPU_TARGET STREQUAL "portable")
  message(STATUS "csplendor CPU target: portable")
elseif(CSPLENDOR_CPU_TARGET STREQUAL "native")
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
     NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$" OR
     NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$" OR
     CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
            "CSPLENDOR_CPU_TARGET=native currently requires an Apple "
            "Silicon host and a native macOS build. Use portable for "
            "distributed or cross-compiled builds.")
  endif()
  if(CMAKE_OSX_ARCHITECTURES AND
     NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR
            "CSPLENDOR_CPU_TARGET=native requires an arm64-only build; "
            "universal2 and cross-architecture builds must use portable.")
  endif()

  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-mcpu=native" CSPLENDOR_HAS_MCPU_NATIVE)
  if(NOT CSPLENDOR_HAS_MCPU_NATIVE)
    message(FATAL_ERROR
            "The selected C++ compiler does not support -mcpu=native")
  endif()

  target_compile_options(csplendor_core_config INTERFACE -mcpu=native)
  message(STATUS "csplendor CPU target: native (-mcpu=native)")
  message(WARNING
          "The native CPU target creates a host-specific local binary. "
          "Do not distribute it as a generic arm64 wheel.")
else()
  message(FATAL_ERROR
          "Unsupported CSPLENDOR_CPU_TARGET=${CSPLENDOR_CPU_TARGET}; "
          "expected portable or native")
endif()

add_library(csplendor_sanitizer INTERFACE)
if(CSPLENDOR_SANITIZER STREQUAL "none")
  # No instrumentation.
elseif(CSPLENDOR_SANITIZER STREQUAL "thread")
  target_compile_options(
      csplendor_sanitizer INTERFACE -O1 -g -fno-omit-frame-pointer
                                    -fsanitize=thread)
  target_link_options(csplendor_sanitizer INTERFACE -fsanitize=thread)
elseif(CSPLENDOR_SANITIZER STREQUAL "address-undefined")
  target_compile_options(
      csplendor_sanitizer INTERFACE -O1 -g -fno-omit-frame-pointer
                                    -fsanitize=address,undefined)
  target_link_options(csplendor_sanitizer INTERFACE
                      -fsanitize=address,undefined)
else()
  message(FATAL_ERROR
          "Unsupported CSPLENDOR_SANITIZER=${CSPLENDOR_SANITIZER}")
endif()

target_link_libraries(csplendor_core_config INTERFACE csplendor_sanitizer)
target_link_libraries(
    csplendor_core PUBLIC csplendor_core_config PRIVATE csplendor_warnings)

if(NOT CSPLENDOR_SANITIZER STREQUAL "none" AND
   CSPLENDOR_BUILD_PYTHON_MODULE)
  message(FATAL_ERROR
          "Sanitized builds are native-only; disable the Python module")
endif()

if(CSPLENDOR_BUILD_PYTHON_MODULE)
  set(PYBIND11_FINDPYTHON ON)
  find_package(pybind11 REQUIRED)

  pybind11_add_module(
      _csplendor
      ${CSPLENDOR_CORE_SOURCES}
      src/bindings.cpp
      src/bindings_domain.cpp
      src/bindings_rules.cpp
      src/bindings_encoding.cpp
      src/bindings_mcts.cpp
      src/bindings_solvers.cpp)
  target_link_libraries(
      _csplendor PRIVATE csplendor_core_config Threads::Threads)
endif()

if(CSPLENDOR_BUILD_NATIVE_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CSPLENDOR_BUILD_PARALLEL_BENCHMARK)
  add_executable(benchmark_mcts_parallel scripts/benchmark_mcts_parallel.cpp)
  target_link_libraries(
      benchmark_mcts_parallel PRIVATE csplendor_core csplendor_sanitizer
                                      csplendor_warnings Threads::Threads)
  add_executable(benchmark_mcts_optimizations
                 scripts/benchmark_mcts_optimizations.cpp)
  target_compile_definitions(
      benchmark_mcts_optimizations
      PRIVATE CSPLENDOR_ACTION_ENCODER_REFERENCE=1)
  target_link_libraries(
      benchmark_mcts_optimizations
      PRIVATE csplendor_core csplendor_sanitizer csplendor_warnings
              Threads::Threads)
endif()
