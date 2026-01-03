# examples/CMakeLists.txt
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)

add_executable(smoke_v1 smoke_v1.cpp)
target_link_libraries(smoke_v1 Spoke::core)

add_executable(smoke_v2_agent smoke_v2_agent.cpp)
target_link_libraries(smoke_v2_agent Spoke::daemon Spoke::core)

add_executable(smoke_v2 smoke_v2.cpp)
target_link_libraries(smoke_v2 Spoke::core)

add_executable(smoke_v3 smoke_v3.cpp)
target_link_libraries(smoke_v3 Spoke::core)

add_executable(smoke_v4_agent smoke_v4_agent.cpp)
target_link_libraries(smoke_v4_agent Spoke::daemon Spoke::core)

add_executable(smoke_v4 smoke_v4.cpp)
target_link_libraries(smoke_v4 Spoke::core)

add_executable(smoke_v5_agent smoke_v5_agent.cpp)
target_link_libraries(smoke_v5_agent Spoke::daemon Spoke::core)

add_executable(smoke_v5 smoke_v5.cpp)
target_link_libraries(smoke_v5 Spoke::core)

add_executable(smoke_v6_agent smoke_v6_agent.cpp)
target_link_libraries(smoke_v6_agent Spoke::daemon Spoke::core)

add_executable(smoke_v6 smoke_v6.cpp)
target_link_libraries(smoke_v6 Spoke::core)

# Installation
install(TARGETS
    smoke_v1 smoke_v2 smoke_v3 smoke_v4 smoke_v5 smoke_v6
    smoke_v2_agent smoke_v4_agent smoke_v5_agent smoke_v6_agent
    DESTINATION examples
)
