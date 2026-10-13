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
            ${CMAKE_CURRENT_SOURCE_DIR}/web/support)
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

# Web functional fixtures exercise native TLS/coding/backend behavior. Declare
# those dependencies here instead of exposing implementation headers and compile
# options through the installed Web target.
function(ruvia_add_web_test name timeout)
    ruvia_add_test(${name} ruvia::web TIMEOUT ${timeout} SOURCES ${ARGN})
    target_link_libraries(${name} PRIVATE ${RUVIA_WEB_DEPENDENCIES} ${RUVIA_HTTP_DEPENDENCIES})
    target_compile_definitions(${name} PRIVATE ${RUVIA_OPENSSL_DEFINITIONS})
    if(WIN32)
        target_compile_definitions(${name} PRIVATE NOMINMAX)
    endif()
endfunction()

# Web suites that need their own process-wide application or peer build into
# separate executables; their sources are excluded from ruvia_web_unit_tests.
function(ruvia_add_web_standalone_test name timeout)
    ruvia_add_web_test(${name} ${timeout} ${ARGN} support/test_main.cpp)
    set(_ruvia_web_standalone_sources ${_ruvia_web_standalone_sources} ${ARGN}
        PARENT_SCOPE)
endfunction()

if(RUVIA_BUILD_CORE)
    ruvia_add_test(ruvia_core_unit_tests ruvia::core TIMEOUT 180 SOURCES
        core/bytes.cpp
        core/error_text.cpp
        core/failure_report.cpp
        core/ip_address.cpp
        core/local_ring_queue.cpp
        core/memory_pool.cpp
        core/scoped_operation.cpp
        core/stop_token.cpp
        core/worker_queue.cpp
        core/worker_runtime.cpp
        support/test_main.cpp)
    ruvia_add_test(ruvia_asio_task ruvia::core SOURCES core/asio_task.cpp)
    ruvia_add_test(ruvia_event_loop_pool ruvia::core SOURCES core/runtime_worker.cpp)
    ruvia_add_test(ruvia_operation_deadline ruvia::core SOURCES core/operation_deadline.cpp)
    ruvia_add_test(ruvia_pool_lease_scheduler ruvia::core TIMEOUT 10 SOURCES
        core/pool_lease_scheduler.cpp)
    ruvia_add_test(ruvia_task_scope ruvia::core SOURCES core/task_scope.cpp)
    ruvia_add_test(ruvia_task_completion ruvia::core SOURCES core/task_completion.cpp)
    ruvia_add_test(ruvia_blocking_pool ruvia::core TIMEOUT 30 SOURCES
        core/blocking_pool.cpp support/test_main.cpp)
    ruvia_add_test(ruvia_channel ruvia::core SOURCES core/channel.cpp)
    ruvia_add_test(ruvia_one_shot ruvia::core SOURCES core/one_shot.cpp)
    ruvia_add_test(ruvia_worker_timer ruvia::core SOURCES core/worker_timer.cpp)
endif()

if(RUVIA_BUILD_HTTP)
    ruvia_collect_test_sources(_ruvia_http_unit_sources
        http/unit/*.cpp
        http/http1/*.cpp
        http/websocket/*.cpp)
    list(APPEND _ruvia_http_unit_sources support/test_main.cpp)
    list(SORT _ruvia_http_unit_sources)

    ruvia_add_test(ruvia_http_unit_tests ruvia::http TIMEOUT 180 SOURCES
        ${_ruvia_http_unit_sources})
    target_link_libraries(ruvia_http_unit_tests PRIVATE ${RUVIA_HTTP_DEPENDENCIES})
endif()

if(RUVIA_BUILD_WEB)
    # Process-singleton application fixtures run in separate test executables.
    ruvia_add_web_standalone_test(ruvia_app_startup 30 web/app/startup.cpp)
    ruvia_add_web_standalone_test(ruvia_app_lifecycle 60 web/app/lifecycle.cpp)
    ruvia_add_web_standalone_test(ruvia_app_tls_paths 30 web/app/tls_paths.cpp)
    # A conflicting route pair makes every application in its executable fail
    # startup, so it cannot share the process-wide controller registry.
    ruvia_add_web_standalone_test(ruvia_route_conflict 30 web/app/route_conflict.cpp)
    ruvia_add_web_standalone_test(ruvia_tcp_client_tests 60
        web/http1/connect.cpp
        web/http1/websocket.cpp
        web/http/udp_tunnel.cpp)
    # Owns an application with an app-wide rate limit, so it cannot share the
    # process-wide application of the other server suites.
    ruvia_add_web_standalone_test(ruvia_server_admission_tests 60
        web/http/server_admission.cpp)
    # Owns an application with an app-wide CORS policy and access-log hook, so
    # it cannot share the process-wide application of the other server suites.
    ruvia_add_web_standalone_test(ruvia_response_policy_tests 60
        web/http1/response_policy.cpp)
    # Owns an application with an app-wide compression policy, which would
    # otherwise encode the bodies other server suites compare.
    ruvia_add_web_standalone_test(ruvia_response_compression_tests 60
        web/http1/response_compression.cpp)
    # Owns an application with small inbound budgets and replaces the global
    # aligned operator new/delete to observe the worker's idle trim.
    ruvia_add_web_standalone_test(ruvia_inbound_memory_tests 60
        web/http1/inbound_memory.cpp)
    # Owns a TLS application with a short write timeout, which must not time
    # the handler cases of the other server suites.
    ruvia_add_web_standalone_test(ruvia_interim_phase_timeout_tests 30
        web/http1/interim_phase_timeout.cpp)
    # Owns an application with an application-wide error handler that must
    # observe no peer reset.
    ruvia_add_web_standalone_test(ruvia_peer_reset_tests 60
        web/http1/peer_reset.cpp)
    # Owns an application with a document root shadowing a guarded route.
    ruvia_add_web_standalone_test(ruvia_document_root_tests 60
        web/http/document_root.cpp)
    # Owns a multi-worker application whose document root changes under it.
    ruvia_add_web_standalone_test(ruvia_document_root_refresh_tests 60
        web/http/document_root_refresh.cpp)

    # Feature directories join the unit suite only when their feature is built.
    ruvia_collect_test_sources(_ruvia_web_unit_sources web/*.cpp)
    list(FILTER _ruvia_web_unit_sources EXCLUDE REGEX "^web/(auth|db|redis)/")
    list(REMOVE_ITEM _ruvia_web_unit_sources ${_ruvia_web_standalone_sources})
    if(RUVIA_ENABLE_JWT)
        ruvia_collect_test_sources(_ruvia_web_feature_sources web/auth/*.cpp)
        list(APPEND _ruvia_web_unit_sources ${_ruvia_web_feature_sources})
    endif()
    if(RUVIA_ENABLE_REDIS)
        ruvia_collect_test_sources(_ruvia_web_feature_sources web/redis/*.cpp)
        list(APPEND _ruvia_web_unit_sources ${_ruvia_web_feature_sources})
    endif()
    if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
        ruvia_collect_test_sources(_ruvia_web_feature_sources web/db/*.cpp)
        # The event-loop DB client is PostgreSQL-only.
        if(NOT RUVIA_ENABLE_POSTGRESQL)
            list(REMOVE_ITEM _ruvia_web_feature_sources web/db/event_loop_client.cpp)
        endif()
        list(APPEND _ruvia_web_unit_sources ${_ruvia_web_feature_sources})
    endif()
    list(APPEND _ruvia_web_unit_sources support/test_main.cpp)
    list(SORT _ruvia_web_unit_sources)

    ruvia_add_web_test(ruvia_web_unit_tests 360 ${_ruvia_web_unit_sources})
endif()
