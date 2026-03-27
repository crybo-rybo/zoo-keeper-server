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
    find_program(ZKS_NPM npm REQUIRED)

    set(ZKS_TEST_UI_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/test_ui_app")
    set(ZKS_TEST_UI_DIST_DIR "${CMAKE_CURRENT_BINARY_DIR}/test_ui_dist")
    set(ZKS_TEST_UI_NPM_STAMP "${CMAKE_CURRENT_BINARY_DIR}/test_ui_npm.stamp")
    set(ZKS_TEST_UI_TEMPLATE "${ZKS_TEST_UI_DIST_DIR}/index.html")
    set(ZKS_TEST_UI_CSS "${ZKS_TEST_UI_DIST_DIR}/index.css")
    set(ZKS_TEST_UI_JS "${ZKS_TEST_UI_DIST_DIR}/index.js")
    set(ZKS_TEST_UI_TEMPLATE_CPP "${CMAKE_CURRENT_BINARY_DIR}/test_ui_template_content.cpp")
    set(ZKS_TEST_UI_CSS_CPP "${CMAKE_CURRENT_BINARY_DIR}/test_ui_css_content.cpp")
    set(ZKS_TEST_UI_JS_CPP "${CMAKE_CURRENT_BINARY_DIR}/test_ui_js_content.cpp")

    file(GLOB_RECURSE ZKS_TEST_UI_APP_SOURCES CONFIGURE_DEPENDS
        "${ZKS_TEST_UI_APP_DIR}/src/*"
    )

    add_custom_command(
        OUTPUT "${ZKS_TEST_UI_NPM_STAMP}"
        COMMAND "${ZKS_NPM}" ci --no-audit --no-fund
        COMMAND "${CMAKE_COMMAND}" -E touch "${ZKS_TEST_UI_NPM_STAMP}"
        WORKING_DIRECTORY "${ZKS_TEST_UI_APP_DIR}"
        DEPENDS
            "${ZKS_TEST_UI_APP_DIR}/package.json"
            "${ZKS_TEST_UI_APP_DIR}/package-lock.json"
        COMMENT "Installing test UI frontend dependencies"
        VERBATIM
    )

    add_custom_command(
        OUTPUT
            "${ZKS_TEST_UI_TEMPLATE}"
            "${ZKS_TEST_UI_CSS}"
            "${ZKS_TEST_UI_JS}"
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "ZKS_TEST_UI_OUTDIR=${ZKS_TEST_UI_DIST_DIR}"
            "${ZKS_NPM}" run build
        WORKING_DIRECTORY "${ZKS_TEST_UI_APP_DIR}"
        DEPENDS
            "${ZKS_TEST_UI_NPM_STAMP}"
            "${ZKS_TEST_UI_APP_DIR}/build.mjs"
            "${ZKS_TEST_UI_APP_DIR}/package.json"
            "${ZKS_TEST_UI_APP_DIR}/package-lock.json"
            ${ZKS_TEST_UI_APP_SOURCES}
        COMMENT "Building test UI frontend bundle"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${ZKS_TEST_UI_TEMPLATE_CPP}"
        COMMAND
            ${CMAKE_COMMAND}
            "-DXXD=${ZKS_XXD}"
            "-DINPUT=${ZKS_TEST_UI_TEMPLATE}"
            "-DOUTPUT=${ZKS_TEST_UI_TEMPLATE_CPP}"
            "-DSYMBOL=kTestUiTemplateData"
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedXxd.cmake"
        DEPENDS
            "${ZKS_TEST_UI_TEMPLATE}"
            "${PROJECT_SOURCE_DIR}/cmake/EmbedXxd.cmake"
        COMMENT "Embedding test UI HTML template"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${ZKS_TEST_UI_CSS_CPP}"
        COMMAND
            ${CMAKE_COMMAND}
            "-DXXD=${ZKS_XXD}"
            "-DINPUT=${ZKS_TEST_UI_CSS}"
            "-DOUTPUT=${ZKS_TEST_UI_CSS_CPP}"
            "-DSYMBOL=kTestUiCssData"
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedXxd.cmake"
        DEPENDS
            "${ZKS_TEST_UI_CSS}"
            "${PROJECT_SOURCE_DIR}/cmake/EmbedXxd.cmake"
        COMMENT "Embedding test UI CSS bundle"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${ZKS_TEST_UI_JS_CPP}"
        COMMAND
            ${CMAKE_COMMAND}
            "-DXXD=${ZKS_XXD}"
            "-DINPUT=${ZKS_TEST_UI_JS}"
            "-DOUTPUT=${ZKS_TEST_UI_JS_CPP}"
            "-DSYMBOL=kTestUiJsData"
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedXxd.cmake"
        DEPENDS
            "${ZKS_TEST_UI_JS}"
            "${PROJECT_SOURCE_DIR}/cmake/EmbedXxd.cmake"
        COMMENT "Embedding test UI JavaScript bundle"
        VERBATIM
    )

    target_sources(zks_server_core PRIVATE
        test_ui.cpp
        "${ZKS_TEST_UI_TEMPLATE_CPP}"
        "${ZKS_TEST_UI_CSS_CPP}"
        "${ZKS_TEST_UI_JS_CPP}"
    )
    target_compile_definitions(zks_server_core PUBLIC ZKS_ENABLE_TEST_UI=1)
endif()
