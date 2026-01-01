add_executable(smoke_v1
    smoke_v1.cpp
)

target_link_libraries(smoke_v1 _spoke)

add_executable(smoke_v2_agent
    smoke_v2_agent.cpp
)

target_link_libraries(smoke_v2_agent spoke_agent _spoke)

add_executable(smoke_v2
    smoke_v2.cpp
)

target_link_libraries(smoke_v2 _spoke)

add_executable(smoke_v3
    smoke_v3.cpp
)

target_link_libraries(smoke_v3 _spoke)
