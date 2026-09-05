cmake_minimum_required(VERSION 3.13)
project(native_proof_batch_adapter LANGUAGES CXX)
set(CSPLENDOR_BUILD_PYTHON_MODULE OFF CACHE BOOL "" FORCE)
set(CSPLENDOR_BUILD_NATIVE_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${ENGINE_SOURCE_ROOT}" engine)
add_executable(native_proof_batch ../phase3dp2_native_proof_batch_20260905.cpp)
target_link_libraries(native_proof_batch PRIVATE csplendor_core csplendor_core_config)
target_compile_definitions(native_proof_batch PRIVATE
    CSPLENDOR_EXISTING_BENCHMARK="${ENGINE_SOURCE_ROOT}/scripts/benchmark_engine_hotpaths.cpp")
