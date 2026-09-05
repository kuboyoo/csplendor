cmake_minimum_required(VERSION 3.13)
project(proof_batch_command_adapter LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
foreach(side IN ITEMS A B)
  add_executable(proof_launcher_${side} ../phase3dp1_proof_launcher_20260905.cpp)
  target_compile_definitions(proof_launcher_${side} PRIVATE
    PYTHON_PATH="${PYTHON_PATH}" PROBE_PATH="${PROBE_PATH}"
    PACKAGE_ROOT="${PACKAGE_${side}}")
endforeach()
