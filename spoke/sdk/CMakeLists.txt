add_library(spoke_daemon SHARED daemon.cpp)
target_link_libraries(spoke_daemon PUBLIC spoke)

add_executable(spoke_agent daemon_main.cpp)
target_link_libraries(spoke_agent PRIVATE spoke_daemon)

install(TARGETS spoke_daemon spoke_agent
    EXPORT SpokeTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
