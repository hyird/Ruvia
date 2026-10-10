# Test code keeps a compact warning set: useful diagnostics without the noise
# produced by partially initialized fixtures and macro-based registration.
function(ruvia_apply_test_warnings name)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /permissive- /bigobj /FS)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${name} PRIVATE -Wall)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${name} PRIVATE -Wshadow=local -Wno-psabi)
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
    cmake_parse_arguments(test "" "TIMEOUT" "SOURCES" ${ARGN})
    if(NOT test_SOURCES)
        message(FATAL_ERROR "test executable ${name} requires SOURCES")
    endif()
    add_executable(${name} ${test_SOURCES})
    target_link_libraries(${name} PRIVATE ${lib})
    ruvia_apply_test_support_includes(${name} ${lib})
    ruvia_apply_test_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    if(DEFINED test_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${test_TIMEOUT})
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

if(RUVIA_BUILD_CORE)
    ruvia_add_test(ruvia_core_unit_tests ruvia::core TIMEOUT 180 SOURCES
        core/bytes.cpp
        core/connection_scanner.cpp
        core/hex.cpp
        core/integer.cpp
        core/ip_address.cpp
        core/memory_pool.cpp
        core/number_format.cpp
        core/pmr_string.cpp
        core/scoped_operation.cpp
        core/spsc_ring_queue.cpp
        core/stop_token.cpp
        core/worker_queue.cpp
        core/worker_runtime.cpp
        support/test_main.cpp)
    ruvia_add_test(ruvia_asio_task ruvia::core SOURCES core/asio_task.cpp)
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
    ruvia_add_test(ruvia_worker_notification ruvia::core TIMEOUT 60 SOURCES
        core/worker_notification.cpp
        support/test_main.cpp)
endif()

if(RUVIA_BUILD_HTTP)
    ruvia_collect_test_sources(_ruvia_http_unit_sources
        http/unit/*.cpp
        http/http1/*.cpp
        http/http2/*.cpp
        http/http3/*.cpp
        http/websocket/*.cpp)
    list(APPEND _ruvia_http_unit_sources support/test_main.cpp)
    list(SORT _ruvia_http_unit_sources)

    ruvia_add_test(ruvia_http_unit_tests ruvia::http TIMEOUT 180 SOURCES
        ${_ruvia_http_unit_sources})
    if(WIN32)
        target_compile_definitions(ruvia_http_unit_tests PRIVATE NOMINMAX)
        target_link_libraries(ruvia_http_unit_tests PRIVATE ws2_32)
    endif()
    target_link_libraries(ruvia_http_unit_tests PRIVATE ${RUVIA_HTTP_DEPENDENCIES})
endif()

if(RUVIA_BUILD_WEB)
    ruvia_collect_test_sources(_ruvia_web_unit_sources web/unit/*.cpp)
    list(REMOVE_ITEM _ruvia_web_unit_sources
        web/unit/app/route_conflict.cpp
        web/unit/app/startup.cpp
        web/unit/auth/jwt.cpp
        web/unit/http3/client_connection_control.cpp
        web/unit/http3/client_connection_tunnel.cpp
        web/unit/http3/client_connection_worker.cpp
        web/unit/http1/connect.cpp
        web/unit/http1/websocket.cpp
        web/unit/http2/client_tunnel.cpp
        web/unit/http2/connect.cpp
        web/unit/http2/push.cpp
        web/unit/http/udp_tunnel.cpp
        web/unit/db/client.cpp
        web/unit/db/entity.cpp
        web/unit/db/event_loop_client.cpp
        web/unit/db/migration.cpp
        web/unit/db/query.cpp
        web/unit/db/repository.cpp
        web/unit/db/schema.cpp
        web/unit/db/transaction_options.cpp
        web/unit/redis/client.cpp
        web/unit/redis/entity.cpp
        web/unit/redis/event_loop_client.cpp
        web/unit/redis/protocol.cpp
        web/unit/redis/query.cpp
        web/unit/redis/repository.cpp)

    if(RUVIA_ENABLE_JWT)
        list(APPEND _ruvia_web_unit_sources web/unit/auth/jwt.cpp)
    endif()
    if(RUVIA_ENABLE_REDIS)
        list(APPEND _ruvia_web_unit_sources
            web/unit/redis/client.cpp
            web/unit/redis/entity.cpp
            web/unit/redis/event_loop_client.cpp
            web/unit/redis/protocol.cpp
            web/unit/redis/query.cpp
            web/unit/redis/repository.cpp)
    endif()
    if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
        list(APPEND _ruvia_web_unit_sources
            web/unit/db/client.cpp
            web/unit/db/entity.cpp
            web/unit/db/migration.cpp
            web/unit/db/query.cpp
            web/unit/db/repository.cpp
            web/unit/db/schema.cpp
            web/unit/db/transaction_options.cpp)
    endif()
    if(RUVIA_ENABLE_POSTGRESQL)
        list(APPEND _ruvia_web_unit_sources web/unit/db/event_loop_client.cpp)
    endif()

    list(APPEND _ruvia_web_unit_sources support/test_main.cpp)
    list(SORT _ruvia_web_unit_sources)
    # Process-singleton application fixtures run in separate test executables.
    ruvia_add_test(ruvia_app_startup ruvia::web TIMEOUT 30 SOURCES
        web/unit/app/startup.cpp
        support/test_main.cpp)
    target_link_libraries(ruvia_app_startup PRIVATE ${RUVIA_WEB_DEPENDENCIES} ${RUVIA_HTTP_DEPENDENCIES})
    target_compile_definitions(ruvia_app_startup PRIVATE ${RUVIA_OPENSSL_DEFINITIONS})
    # A conflicting route pair makes every application in its executable fail
    # startup, so it cannot share the process-wide controller registry.
    ruvia_add_test(ruvia_route_conflict ruvia::web TIMEOUT 30 SOURCES
        web/unit/app/route_conflict.cpp
        support/test_main.cpp)
    ruvia_add_test(ruvia_http3_client_tests ruvia::web TIMEOUT 60 SOURCES
        web/unit/http3/client_connection_control.cpp
        web/unit/http3/client_connection_tunnel.cpp
        web/unit/http3/client_connection_worker.cpp
        support/test_main.cpp)
    target_link_libraries(ruvia_http3_client_tests PRIVATE
        ${RUVIA_WEB_DEPENDENCIES} ${RUVIA_HTTP_DEPENDENCIES})
    target_compile_definitions(ruvia_http3_client_tests PRIVATE ${RUVIA_OPENSSL_DEFINITIONS})
    ruvia_add_test(ruvia_tcp_client_tests ruvia::web TIMEOUT 60 SOURCES
        web/unit/http1/connect.cpp
        web/unit/http1/websocket.cpp
        web/unit/http2/client_tunnel.cpp
        web/unit/http2/connect.cpp
        web/unit/http2/push.cpp
        web/unit/http/udp_tunnel.cpp
        support/test_main.cpp)
    target_link_libraries(ruvia_tcp_client_tests PRIVATE
        ${RUVIA_WEB_DEPENDENCIES} ${RUVIA_HTTP_DEPENDENCIES})
    target_compile_definitions(ruvia_tcp_client_tests PRIVATE ${RUVIA_OPENSSL_DEFINITIONS})
    ruvia_add_test(ruvia_web_unit_tests ruvia::web TIMEOUT 360 SOURCES
        ${_ruvia_web_unit_sources})
    # These functional fixtures exercise native TLS/coding/backend behavior.
    # Declare their dependencies here instead of exposing implementation headers
    # and compile options through the installed Web target.
    target_link_libraries(ruvia_web_unit_tests PRIVATE
        ${RUVIA_WEB_DEPENDENCIES} ${RUVIA_HTTP_DEPENDENCIES})
    target_compile_definitions(ruvia_web_unit_tests PRIVATE ${RUVIA_OPENSSL_DEFINITIONS})
    if(WIN32)
        target_compile_definitions(ruvia_web_unit_tests PRIVATE NOMINMAX)
        target_compile_definitions(ruvia_http3_client_tests PRIVATE NOMINMAX)
        target_compile_definitions(ruvia_tcp_client_tests PRIVATE NOMINMAX)
    endif()
endif()
