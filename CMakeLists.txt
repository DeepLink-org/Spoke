cmake_minimum_required(VERSION 3.10)
project(Spoke VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Set build output layout
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/sdk)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Header files installation
file(GLOB SPOKE_HEADERS 
    "spoke/csrc/*.h"
)
install(FILES ${SPOKE_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/spoke)

add_subdirectory(spoke/csrc)
add_subdirectory(spoke/sdk)
add_subdirectory(spoke/tools)
add_subdirectory(examples)

# --- CMake Package Export ---

set(INCLUDE_INSTALL_DIR ${CMAKE_INSTALL_INCLUDEDIR})
set(BIN_INSTALL_DIR sdk)

configure_package_config_file(
    "cmake/SpokeConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SpokeConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Spoke
    PATH_VARS INCLUDE_INSTALL_DIR BIN_INSTALL_DIR
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/SpokeConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT SpokeTargets
    FILE SpokeTargets.cmake
    NAMESPACE Spoke::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Spoke
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/SpokeConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/SpokeConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Spoke
)

