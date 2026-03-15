add_subdirectory(server)

add_executable(zoo_keeper_server main.cpp)
target_link_libraries(zoo_keeper_server PRIVATE zks_server_core)

add_executable(zoo_keeper_link_smoke link_smoke.cpp)
target_compile_features(zoo_keeper_link_smoke PRIVATE cxx_std_23)
target_link_libraries(zoo_keeper_link_smoke PRIVATE ZooKeeper::zoo)

add_executable(zoo_keeper_live_smoke live_smoke.cpp)
target_compile_features(zoo_keeper_live_smoke PRIVATE cxx_std_23)
target_link_libraries(zoo_keeper_live_smoke PRIVATE ZooKeeper::zoo)

install(TARGETS zoo_keeper_server
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
