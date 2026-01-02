add_library(spoke_agent SHARED daemon_main.cpp)

target_link_libraries(spoke_agent PUBLIC _spoke)
