cmake_minimum_required(VERSION 3.10)
project(SpokeSystem)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(csrc)

add_subdirectory(csrc/sdk)
add_subdirectory(csrc/spoke)

add_subdirectory(tests)

