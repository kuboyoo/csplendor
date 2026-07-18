cmake_minimum_required(VERSION 3.13)
project(csplendor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CSPLENDOR_BUILD_PYTHON_MODULE "Build the pybind11 extension" ON)
option(CSPLENDOR_BUILD_NATIVE_TESTS "Build native C++ tests" OFF)
option(CSPLENDOR_BUILD_PARALLEL_BENCHMARK "Build native MCTS benchmark" OFF)
set(CSPLENDOR_SANITIZER "none" CACHE STRING
    "Sanitizer configuration: none, thread, or address-undefined")
set_property(CACHE CSPLENDOR_SANITIZER PROPERTY STRINGS
             none thread address-undefined)

find_package(Threads REQUIRED)

add_library(csplendor_core INTERFACE)
target_include_directories(
    csplendor_core INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(csplendor_core INTERFACE cxx_std_17)

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

if(NOT CSPLENDOR_SANITIZER STREQUAL "none" AND
   CSPLENDOR_BUILD_PYTHON_MODULE)
  message(FATAL_ERROR
          "Sanitized builds are native-only; disable the Python module")
endif()

if(CSPLENDOR_BUILD_PYTHON_MODULE)
  set(PYBIND11_FINDPYTHON ON)
  find_package(pybind11 REQUIRED)

  pybind11_add_module(_csplendor src/bindings.cpp)
  target_link_libraries(
      _csplendor PRIVATE csplendor_core csplendor_sanitizer Threads::Threads)
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
                                      Threads::Threads)
  target_compile_options(benchmark_mcts_parallel PRIVATE -Wall -Wextra
                                                        -Wpedantic -Werror)
endif()
