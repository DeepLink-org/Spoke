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

install(TARGETS spoke
    EXPORT SpokeTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
