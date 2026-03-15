cmake_minimum_required(VERSION 3.18)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
endif()

project(zoo-keeper-server VERSION 0.0.4 LANGUAGES CXX)

include(CTest)
include(FetchContent)
include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ProjectOptions)
include(Dependencies)

add_subdirectory(extern/zoo-keeper)
add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(DIRECTORY config/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zoo-keeper-server/config
    FILES_MATCHING PATTERN "*.json")
