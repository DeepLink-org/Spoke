# examples/CMakeLists.txt
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)

add_executable(smoke_v1 smoke_v1.cpp)
target_link_libraries(smoke_v1 spoke)

add_executable(smoke_v2_agent smoke_v2_agent.cpp ../spoke/sdk/daemon_main.cpp)
target_link_libraries(smoke_v2_agent spoke_daemon spoke)

add_executable(smoke_v2 smoke_v2.cpp)
target_link_libraries(smoke_v2 spoke)

add_executable(smoke_v3 smoke_v3.cpp)
target_link_libraries(smoke_v3 spoke)

add_executable(smoke_v4_agent smoke_v4_agent.cpp ../spoke/sdk/daemon_main.cpp)
target_link_libraries(smoke_v4_agent spoke_daemon spoke)

add_executable(smoke_v4 smoke_v4.cpp)
target_link_libraries(smoke_v4 spoke)

add_executable(smoke_v5_agent smoke_v5_agent.cpp ../spoke/sdk/daemon_main.cpp)
target_link_libraries(smoke_v5_agent spoke_daemon spoke)

add_executable(smoke_v5 smoke_v5.cpp)
target_link_libraries(smoke_v5 spoke)

# Installation
install(TARGETS 
    smoke_v1 smoke_v2 smoke_v3 smoke_v4 smoke_v5
    smoke_v2_agent smoke_v4_agent smoke_v5_agent
    DESTINATION examples
)
