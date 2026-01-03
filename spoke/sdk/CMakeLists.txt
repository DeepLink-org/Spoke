add_library(spoke_daemon SHARED daemon.cpp daemon_main.cpp)
target_link_libraries(spoke_daemon PUBLIC spoke)

set_target_properties(spoke_daemon PROPERTIES EXPORT_NAME daemon)
add_library(Spoke::daemon ALIAS spoke_daemon)

target_include_directories(spoke_daemon PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

install(TARGETS spoke_daemon
    EXPORT SpokeTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
