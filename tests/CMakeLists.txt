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


add_executable(smoke_v4_agent
    smoke_v4_agent.cpp
)

target_link_libraries(smoke_v4_agent spoke_agent _spoke)

add_executable(smoke_v4
    smoke_v4.cpp
)

target_link_libraries(smoke_v4 _spoke)

add_executable(smoke_v5_agent
    smoke_v5_agent.cpp
)

target_link_libraries(smoke_v5_agent spoke_agent _spoke)

add_executable(smoke_v5
    smoke_v5.cpp
)

target_link_libraries(smoke_v5 _spoke)
