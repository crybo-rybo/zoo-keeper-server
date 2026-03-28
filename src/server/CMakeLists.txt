configure_file(version.hpp.in
    "${PROJECT_BINARY_DIR}/src/server/version.hpp" @ONLY)

add_library(zks_server_core
    api_types.cpp
    api_json.cpp
    api_routes.cpp
    chat_service.cpp
    completion_controller.cpp
    command_tools.cpp
    config.cpp
    executor.cpp
    health.cpp
    http_server.cpp
    metrics.cpp
    route_utils.cpp
    runtime.cpp
    session_manager.cpp
    streaming.cpp
    zoo_adapter.cpp
)

target_compile_features(zks_server_core PUBLIC cxx_std_23)
target_include_directories(zks_server_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)
target_link_libraries(zks_server_core PUBLIC
    Drogon::Drogon
    ZooKeeper::zoo
)

if(ZKS_ENABLE_TEST_UI)
    find_program(ZKS_XXD xxd REQUIRED)

    set(ZKS_TEST_UI_HTML_CPP "${CMAKE_CURRENT_BINARY_DIR}/test_ui_html_content.cpp")
    set(ZKS_TEST_UI_JS_CPP "${CMAKE_CURRENT_BINARY_DIR}/test_ui_js_content.cpp")

    add_custom_command(
        OUTPUT "${ZKS_TEST_UI_HTML_CPP}"
        COMMAND
            ${CMAKE_COMMAND}
            "-DXXD=${ZKS_XXD}"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/test_ui.html"
            "-DOUTPUT=${ZKS_TEST_UI_HTML_CPP}"
            "-DSYMBOL=kTestUiHtmlData"
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedXxd.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/test_ui.html"
            "${PROJECT_SOURCE_DIR}/cmake/EmbedXxd.cmake"
        COMMENT "Embedding test_ui.html"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${ZKS_TEST_UI_JS_CPP}"
        COMMAND
            ${CMAKE_COMMAND}
            "-DXXD=${ZKS_XXD}"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/test_ui.js"
            "-DOUTPUT=${ZKS_TEST_UI_JS_CPP}"
            "-DSYMBOL=kTestUiJsData"
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedXxd.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/test_ui.js"
            "${PROJECT_SOURCE_DIR}/cmake/EmbedXxd.cmake"
        COMMENT "Embedding test_ui.js"
        VERBATIM
    )

    target_sources(zks_server_core PRIVATE
        test_ui.cpp
        "${ZKS_TEST_UI_HTML_CPP}"
        "${ZKS_TEST_UI_JS_CPP}"
    )
    target_compile_definitions(zks_server_core PUBLIC ZKS_ENABLE_TEST_UI=1)
endif()
