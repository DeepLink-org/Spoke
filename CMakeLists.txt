cmake_minimum_required(VERSION 3.10)
project(SpokeSystem)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(./)

add_subdirectory(spoke/csrc)
add_subdirectory(spoke/sdk)
add_subdirectory(spoke/tools)

add_subdirectory(tests)

