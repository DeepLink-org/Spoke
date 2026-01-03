add_library(spoke
    SHARED
    agent.cpp
    actor.cpp
)

set_target_properties(spoke PROPERTIES EXPORT_NAME core)
add_library(Spoke::core ALIAS spoke)

target_include_directories(spoke PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# [Changed] DLSlime is now required for Aggressive RDMA
find_package(dlslime REQUIRED)
message(STATUS "Spoke: Found DLSlime, enabling RDMA support")
target_link_libraries(spoke PUBLIC dlslime::dlslime)
target_compile_definitions(spoke PUBLIC SPOKE_HAS_RDMA)

install(TARGETS spoke
    EXPORT SpokeTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
