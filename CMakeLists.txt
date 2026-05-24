cmake_minimum_required(VERSION 3.12)
project(csplendor)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(pybind11 REQUIRED)

pybind11_add_module(_csplendor
    src/bindings.cpp
)

target_include_directories(_csplendor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
