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
endif()
