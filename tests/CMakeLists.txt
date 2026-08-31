# Test code keeps a compact warning set: useful diagnostics without the noise
# produced by partially initialized fixtures and macro-based registration.
function(ruvia_apply_test_warnings name)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /permissive- /bigobj /FS)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${name} PRIVATE -Wall)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${name} PRIVATE -Wshadow=local -Wno-dangling-reference)
        endif()
    endif()
endfunction()

function(ruvia_apply_test_support_includes name lib)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support)
    if("${lib}" STREQUAL "ruvia::http")
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/http/support)
    elseif("${lib}" STREQUAL "ruvia::web")
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/http/support
            ${CMAKE_CURRENT_SOURCE_DIR}/web/unit/support)
    elseif(NOT "${lib}" STREQUAL "ruvia::core")
        message(FATAL_ERROR "unknown Ruvia test library for ${name}: ${lib}")
    endif()
endfunction()

function(ruvia_add_test name lib)
    cmake_parse_arguments(T "" "TIMEOUT" "SOURCES" ${ARGN})
    if(NOT T_SOURCES)
        message(FATAL_ERROR "test executable ${name} requires SOURCES")
    endif()
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE ${lib})
    ruvia_apply_test_support_includes(${name} ${lib})
    ruvia_apply_test_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    if(DEFINED T_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${T_TIMEOUT})
    endif()
endfunction()

function(ruvia_collect_test_sources out)
    set(_patterns "")
    foreach(_pattern IN LISTS ARGN)
        list(APPEND _patterns "${CMAKE_CURRENT_SOURCE_DIR}/${_pattern}")
    endforeach()
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" ${_patterns})
    list(SORT _sources)
    set(${out} ${_sources} PARENT_SCOPE)
endfunction()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/support/verify_repository_boundaries.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/verify_repository_boundaries.cmake
    @ONLY)
add_test(
    NAME ruvia_guard_repository_boundaries
    COMMAND ${CMAKE_COMMAND}
        -P ${CMAKE_CURRENT_BINARY_DIR}/verify_repository_boundaries.cmake)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/support/verify_package_consumer.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/verify_package_consumer.cmake
    @ONLY)
add_test(
    NAME ruvia_guard_package_consumer
    COMMAND ${CMAKE_COMMAND}
        -DRUVIA_TEST_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_BINARY_DIR}/verify_package_consumer.cmake)
set_tests_properties(ruvia_guard_package_consumer PROPERTIES TIMEOUT 180)

if(RUVIA_BUILD_CORE)
    ruvia_add_test(ruvia_core_unit_tests ruvia::core TIMEOUT 180 SOURCES
        core/base64.cpp
        core/pool_waiter_queue.cpp
        core/scoped_operation.cpp
        core/stop_token.cpp
        support/test_main.cpp)

    ruvia_add_test(ruvia_asio_task ruvia::core SOURCES core/asio_task.cpp)
    ruvia_add_test(ruvia_smoke_core_target ruvia::core SOURCES
        core/smoke_core_target.cpp)
    target_compile_definitions(ruvia_smoke_core_target PRIVATE
        RUVIA_EXPECTED_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        RUVIA_EXPECTED_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        RUVIA_EXPECTED_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        RUVIA_EXPECTED_VERSION_STRING="${PROJECT_VERSION}")
    ruvia_add_test(ruvia_connection_scanner_lifetime ruvia::core SOURCES
        core/connection_scanner_lifetime.cpp)
    ruvia_add_test(ruvia_event_loop_pool ruvia::core SOURCES core/runtime_worker.cpp)
    ruvia_add_test(ruvia_operation_deadline ruvia::core SOURCES core/operation_deadline.cpp)
    ruvia_add_test(ruvia_pool_lease_scheduler ruvia::core TIMEOUT 10 SOURCES
        core/pool_lease_scheduler.cpp)
    ruvia_add_test(ruvia_task_scope ruvia::core SOURCES core/task_scope.cpp)
    ruvia_add_test(ruvia_task_completion ruvia::core SOURCES core/task_completion.cpp)
    ruvia_add_test(ruvia_blocking_pool ruvia::core SOURCES core/blocking_pool.cpp)
    ruvia_add_test(ruvia_channel ruvia::core SOURCES core/channel.cpp)
    ruvia_add_test(ruvia_one_shot ruvia::core SOURCES core/one_shot.cpp)
    ruvia_add_test(ruvia_worker_timer ruvia::core SOURCES core/worker_timer.cpp)
    ruvia_add_test(ruvia_worker_dispatch_failure ruvia::core SOURCES
        core/worker_dispatch_failure.cpp)
    ruvia_add_test(ruvia_worker_async_await ruvia::core SOURCES core/asio_await.cpp)
endif()

if(RUVIA_BUILD_HTTP)
    ruvia_collect_test_sources(_ruvia_http_unit_sources
        http/unit/*.cpp
        http/http1/*.cpp
        http/http2/*.cpp
        http/websocket/*.cpp)
    list(REMOVE_ITEM _ruvia_http_unit_sources http/unit/smoke.cpp)
    list(APPEND _ruvia_http_unit_sources support/test_main.cpp)
    list(SORT _ruvia_http_unit_sources)

    ruvia_add_test(ruvia_http_unit_tests ruvia::http TIMEOUT 180 SOURCES
        ${_ruvia_http_unit_sources})
    ruvia_add_test(ruvia_smoke_http_target ruvia::http SOURCES http/unit/smoke.cpp)
endif()

if(RUVIA_BUILD_WEB)
    ruvia_collect_test_sources(_ruvia_web_unit_sources web/unit/*.cpp)
    list(REMOVE_ITEM _ruvia_web_unit_sources
        web/unit/auth/jwt.cpp
        web/unit/db/api_surface.cpp
        web/unit/db/migration.cpp
        web/unit/db/postgresql.cpp
        web/unit/db/sql.cpp
        web/unit/redis/api_surface.cpp
        web/unit/redis/protocol.cpp)

    if(RUVIA_ENABLE_JWT)
        list(APPEND _ruvia_web_unit_sources web/unit/auth/jwt.cpp)
    endif()
    if(RUVIA_ENABLE_REDIS)
        list(APPEND _ruvia_web_unit_sources
            web/unit/redis/api_surface.cpp
            web/unit/redis/protocol.cpp)
    endif()
    if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
        list(APPEND _ruvia_web_unit_sources
            web/unit/db/api_surface.cpp
            web/unit/db/migration.cpp)
    endif()
    if(RUVIA_ENABLE_MARIADB)
        list(APPEND _ruvia_web_unit_sources web/unit/db/sql.cpp)
    endif()
    if(RUVIA_ENABLE_POSTGRESQL)
        list(APPEND _ruvia_web_unit_sources web/unit/db/postgresql.cpp)
    endif()

    list(APPEND _ruvia_web_unit_sources support/test_main.cpp)
    list(SORT _ruvia_web_unit_sources)
    ruvia_add_test(ruvia_web_unit_tests ruvia::web TIMEOUT 180 SOURCES
        ${_ruvia_web_unit_sources})

    if(RUVIA_ENABLE_POSTGRESQL)
        ruvia_add_test(ruvia_postgresql_integration ruvia::web TIMEOUT 60 SOURCES
            web/server/postgresql_integration.cpp)
        set_tests_properties(ruvia_postgresql_integration PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    if(RUVIA_ENABLE_MARIADB)
        ruvia_add_test(ruvia_mariadb_cancellation ruvia::web TIMEOUT 15 SOURCES
            web/server/mariadb_cancellation.cpp)
        ruvia_add_test(ruvia_mariadb_connect_failure ruvia::web TIMEOUT 15 SOURCES
            web/server/mariadb_connect_failure.cpp)
        ruvia_add_test(ruvia_mariadb_integration ruvia::web TIMEOUT 60 SOURCES
            web/server/mariadb_integration.cpp)
        set_tests_properties(ruvia_mariadb_integration PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
        ruvia_add_test(ruvia_db_client_lifecycle ruvia::web TIMEOUT 30 SOURCES
            web/server/db_client.cpp)
        ruvia_add_test(ruvia_db_migration_timeout ruvia::web TIMEOUT 30 SOURCES
            web/server/db_migration_timeout.cpp)
    endif()

    ruvia_add_test(ruvia_app_startup_retry ruvia::web TIMEOUT 60 SOURCES
        web/server/app_startup_retry.cpp)
    ruvia_add_test(ruvia_app_web_worker ruvia::web TIMEOUT 60 SOURCES
        web/server/app_web_worker.cpp)
    ruvia_add_test(ruvia_server_immediate_shutdown ruvia::web TIMEOUT 30 SOURCES
        web/server/immediate_shutdown.cpp)
    ruvia_add_test(ruvia_server_keepalive_idle_timeout ruvia::web TIMEOUT 60 SOURCES
        web/server/keepalive_idle_timeout.cpp)
    ruvia_add_test(ruvia_server_max_connections ruvia::web TIMEOUT 60 SOURCES
        web/server/max_connections.cpp)
    ruvia_add_test(ruvia_server_pipeline_request_views ruvia::web TIMEOUT 60 SOURCES
        web/server/pipeline_request_views.cpp)
    ruvia_add_test(ruvia_server_connection_failure ruvia::web TIMEOUT 60 SOURCES
        web/server/connection_failure.cpp)
    ruvia_add_test(ruvia_server_write_timeout ruvia::web TIMEOUT 60 SOURCES
        web/server/write_timeout.cpp)
    ruvia_add_test(ruvia_server_notfound_keepalive ruvia::web TIMEOUT 60 SOURCES
        web/server/notfound_keepalive.cpp)
    ruvia_add_test(ruvia_server_buffered_body_limit ruvia::web TIMEOUT 30 SOURCES
        web/server/buffered_body_limit.cpp)
    ruvia_add_test(ruvia_server_buffered_response_write ruvia::web TIMEOUT 60 SOURCES
        web/server/buffered_response_write.cpp
        support/test_main.cpp)
    ruvia_add_test(ruvia_server_blocking_pool ruvia::web TIMEOUT 60 SOURCES
        web/server/blocking_pool.cpp)
    ruvia_add_test(ruvia_server_state_tests ruvia::web TIMEOUT 60 SOURCES
        web/server/access_log.cpp
        web/server/client_close_state.cpp
        web/server/connection_read_buffer.cpp
        web/server/http_server_request_state.cpp
        web/server/http_server_worker_completion.cpp
        web/server/response_state.cpp
        support/test_main.cpp)
    ruvia_add_test(ruvia_server_deadline_body_limit_wire ruvia::web TIMEOUT 30 SOURCES
        web/server/deadline_body_limit_wire.cpp)
    ruvia_add_test(ruvia_server_deadline_error_handler_wire ruvia::web TIMEOUT 30 SOURCES
        web/server/deadline_error_handler_wire.cpp)
    ruvia_add_test(ruvia_server_error_static_file ruvia::web TIMEOUT 30 SOURCES
        web/server/error_static_file.cpp)
    ruvia_add_test(ruvia_server_head_document_root ruvia::web TIMEOUT 30 SOURCES
        web/server/head_document_root.cpp)
    ruvia_add_test(ruvia_server_head_stream_route ruvia::web TIMEOUT 30 SOURCES
        web/server/head_stream_route.cpp)
    ruvia_add_test(ruvia_server_static_root_refresh ruvia::web TIMEOUT 60 SOURCES
        web/server/static_root_refresh.cpp)
    ruvia_add_test(ruvia_server_stream_body_limit ruvia::web TIMEOUT 30 SOURCES
        web/server/stream_body_limit.cpp)
    ruvia_add_test(ruvia_server_http2_connection_failure ruvia::web TIMEOUT 60 SOURCES
        web/server/http2_connection_failure.cpp)
    ruvia_add_test(ruvia_server_http2_document_root ruvia::web TIMEOUT 60 SOURCES
        web/server/http2_document_root.cpp)
    ruvia_add_test(ruvia_server_http2_head_stream_route ruvia::web TIMEOUT 60 SOURCES
        web/server/http2_head_stream_route.cpp)

    add_executable(ruvia_http2_conformance_server web/server/http2_conformance_server.cpp)
    target_link_libraries(ruvia_http2_conformance_server PRIVATE ruvia::web)
    ruvia_apply_test_warnings(ruvia_http2_conformance_server)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME ruvia_http2_conformance
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/http/conformance/http2_wire_conformance.py
            --server $<TARGET_FILE:ruvia_http2_conformance_server>)
    set_tests_properties(ruvia_http2_conformance PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

    ruvia_add_test(ruvia_server_websocket ruvia::web TIMEOUT 60 SOURCES
        web/server/websocket.cpp)
    ruvia_add_test(ruvia_standalone_http_client ruvia::web TIMEOUT 30 SOURCES
        web/server/standalone_http_client.cpp)
    ruvia_add_test(ruvia_websocket_client ruvia::web TIMEOUT 30 SOURCES
        web/server/websocket_client.cpp)
    ruvia_add_test(ruvia_server_http_client ruvia::web TIMEOUT 60 SOURCES
        web/server/http_client.cpp)
    ruvia_add_test(ruvia_server_http_client_limits ruvia::web TIMEOUT 60 SOURCES
        web/server/http_client_limits.cpp)
    ruvia_add_test(ruvia_server_tls_keepalive_deadline ruvia::web TIMEOUT 60 SOURCES
        web/server/tls_keepalive_deadline.cpp)
    ruvia_add_test(ruvia_server_web_worker ruvia::web TIMEOUT 60 SOURCES
        web/server/worker.cpp)
    ruvia_add_test(ruvia_server_worker_state ruvia::web TIMEOUT 60 SOURCES
        web/server/worker_state.cpp)
endif()
