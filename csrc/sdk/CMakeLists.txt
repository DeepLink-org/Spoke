add_library(spoke_agent daemon_main.cpp)

target_link_libraries(spoke_agent PUBLIC _spoke)

add_executable(spoke_hub hub_main.cpp)

target_link_libraries(spoke_hub PUBLIC _spoke)
