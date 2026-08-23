# Test code gets a warning set chosen for signal, not the library's full one.
# Measured against this suite, -Wextra and -Wpedantic are ~80% noise here: a
# designated initializer that names only the fields under test reads as a
# missing initializer, and the macro-based test registration leaves a stray
# semicolon at every use. Those would have to be silenced case by case, and a
# warning that is routinely silenced stops being read at all.
#
# What is left still catches the failures that matter in test code: an unused
# helper is a check that was written and never run, and a shadowed local is how
# an assertion quietly starts reading the wrong variable.
#
# -Wdangling-reference is excluded deliberately. GCC 13 flags any const-ref
# binding to a function that took a temporary, with no aliasing analysis, so
# `const auto& m = context.req().valid<T>()` trips it even though the reference
# points into the Context and outlives the temporary facade.
function(ruvia_apply_test_warnings name)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /permissive- /bigobj /FS)
        # std::getenv and friends are standard C++; MSVC's C4996 "unsafe" notice
        # on them is the same curated-out noise as -Wextra, not a real finding.
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

function(ruvia_expected_test_source_prefix out lib)
    if("${lib}" STREQUAL "ruvia::core")
        set(_prefix "core/")
    elseif("${lib}" STREQUAL "ruvia::http")
        set(_prefix "http/")
    elseif("${lib}" STREQUAL "ruvia::web")
        set(_prefix "web/")
    else()
        message(FATAL_ERROR "unknown Ruvia test library: ${lib}")
    endif()
    set(${out} "${_prefix}" PARENT_SCOPE)
endfunction()

function(ruvia_validate_test_sources name lib)
    ruvia_expected_test_source_prefix(_expected_prefix ${lib})
    foreach(_source IN LISTS ARGN)
        if(IS_ABSOLUTE "${_source}")
            set(_absolute_source "${_source}")
        else()
            get_filename_component(_absolute_source "${_source}" ABSOLUTE
                BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
        file(RELATIVE_PATH _relative_source
            "${CMAKE_CURRENT_SOURCE_DIR}" "${_absolute_source}")
        if("${_relative_source}" STREQUAL "support/test_main.cpp")
            continue()
        endif()
        if(NOT _relative_source MATCHES "^${_expected_prefix}")
            message(FATAL_ERROR
                "test target ${name} links ${lib} but uses source "
                "${_relative_source}; expected tests/${_expected_prefix}**")
        endif()
    endforeach()
endfunction()

# One registration helper for every standalone test executable. It requires an
# explicit target library, exposes only the shared support and target-allowed
# support include directories, applies the MSVC large-object flags uniformly,
# registers the ctest, and sets the optional TIMEOUT / WILL_FAIL / RUN_SERIAL /
# LABELS properties. Aggregated suites and the few multi-registration probes
# below stay explicit.
function(ruvia_add_test name source)
    cmake_parse_arguments(T "WILL_FAIL;SERIAL" "LIB;TIMEOUT;LABELS" "" ${ARGN})
    if(NOT DEFINED T_LIB)
        message(FATAL_ERROR "test executable ${name} requires explicit LIB")
    endif()
    ruvia_validate_test_sources(${name} ${T_LIB} ${source})
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${T_LIB})
    ruvia_apply_test_support_includes(${name} ${T_LIB})
    ruvia_apply_test_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set(_props "")
    if(DEFINED T_TIMEOUT)
        list(APPEND _props TIMEOUT ${T_TIMEOUT})
    endif()
    if(T_WILL_FAIL)
        list(APPEND _props WILL_FAIL TRUE)
    endif()
    if(T_SERIAL)
        list(APPEND _props RUN_SERIAL TRUE)
    endif()
    if(DEFINED T_LABELS)
        list(APPEND _props LABELS "${T_LABELS}")
    endif()
    if(_props)
        set_tests_properties(${name} PROPERTIES ${_props})
    endif()
endfunction()

function(ruvia_add_unit_suite name lib)
    ruvia_validate_test_sources(${name} ${lib} ${ARGN})
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ${lib})
    ruvia_apply_test_support_includes(${name} ${lib})
    ruvia_apply_test_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 180)
endfunction()

function(ruvia_add_unit_sources name lib)
    cmake_parse_arguments(S "" "" "PRIVATE" ${ARGN})
    if(S_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "ruvia_add_unit_sources(${name}) only supports PRIVATE sources")
    endif()
    ruvia_validate_test_sources(${name} ${lib} ${S_PRIVATE})
    target_sources(${name} PRIVATE ${S_PRIVATE})
endfunction()

# Cross-target: it covers whatever this configuration installs, so it lives with
# the shared support rather than under one target's guards.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/support/verify_install_closure.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/verify_install_closure.cmake
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/support/verify_header_definitions.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/verify_header_definitions.cmake
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/support/verify_package_components.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/verify_package_components.cmake
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/support/verify_package_install_consumer.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/verify_package_install_consumer.cmake
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/support/verify_package_version_compatibility.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/verify_package_version_compatibility.cmake
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/support/verify_test_layering.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/verify_test_layering.cmake
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/support/verify_product_layering.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/verify_product_layering.cmake
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/support/verify_repo_layout.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/verify_repo_layout.cmake
    @ONLY)
add_test(
    NAME ruvia_guard_header_definitions
    COMMAND ${CMAKE_COMMAND}
        -P ${CMAKE_CURRENT_BINARY_DIR}/verify_header_definitions.cmake)

add_test(
    NAME ruvia_guard_install_closure
    COMMAND ${CMAKE_COMMAND}
        -P ${CMAKE_CURRENT_BINARY_DIR}/verify_install_closure.cmake)

add_test(
    NAME ruvia_guard_package_components
    COMMAND ${CMAKE_COMMAND}
        -P ${CMAKE_CURRENT_BINARY_DIR}/verify_package_components.cmake)

add_test(
    NAME ruvia_guard_package_install_consumer
    COMMAND ${CMAKE_COMMAND}
        -DRUVIA_TEST_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_BINARY_DIR}/verify_package_install_consumer.cmake)
set_tests_properties(ruvia_guard_package_install_consumer PROPERTIES TIMEOUT 180)

add_test(
    NAME ruvia_guard_package_version_compatibility
    COMMAND ${CMAKE_COMMAND}
        -P ${CMAKE_CURRENT_BINARY_DIR}/verify_package_version_compatibility.cmake)

add_test(
    NAME ruvia_guard_test_layering
    COMMAND ${CMAKE_COMMAND}
        -P ${CMAKE_CURRENT_BINARY_DIR}/verify_test_layering.cmake)

add_test(
    NAME ruvia_guard_product_layering
    COMMAND ${CMAKE_COMMAND}
        -P ${CMAKE_CURRENT_BINARY_DIR}/verify_product_layering.cmake)

add_test(
    NAME ruvia_guard_repo_layout
    COMMAND ${CMAKE_COMMAND}
        -P ${CMAKE_CURRENT_BINARY_DIR}/verify_repo_layout.cmake)

if(RUVIA_BUILD_CORE)
ruvia_add_unit_suite(ruvia_core_unit_tests ruvia::core
    # core
    core/base64.cpp
    core/pool_waiter_queue.cpp
    core/stop_token.cpp

    # support
    support/test_main.cpp
)
endif()

if(RUVIA_BUILD_HTTP)
ruvia_add_unit_suite(ruvia_http_unit_tests ruvia::http
    # http/unit/client
    http/unit/client/origin.cpp
    http/unit/client/redirect.cpp
    http/unit/client/request.cpp
    http/unit/client/response_coding.cpp
    http/unit/client/response_exchange.cpp
    http/unit/client/response_framing.cpp
    http/unit/client/response_parser.cpp
    http/unit/client/set_cookie.cpp

    # http/unit/coding
    http/unit/coding/content_codec.cpp
    http/unit/coding/content_negotiation.cpp
    http/unit/coding/response_content_semantics.cpp

    # http/unit/cookie
    http/unit/cookie/validation.cpp

    # http/unit/field
    http/unit/field/accept_media_type.cpp
    http/unit/field/byte_range.cpp
    http/unit/field/cache.cpp
    http/unit/field/conditional_request.cpp
    http/unit/field/connection_fields.cpp
    http/unit/field/date_cache.cpp
    http/unit/field/date_parsing.cpp
    http/unit/field/expectations.cpp
    http/unit/field/header_tokens.cpp
    http/unit/field/method.cpp
    http/unit/field/quoted_field_values.cpp
    http/unit/field/semicolon_parameters.cpp
    http/unit/field/syntax.cpp
    http/unit/field/value_primitives.cpp

    # http/http1
    http/http1/chunk_decoder.cpp
    http/http1/chunk_framing.cpp
    http/http1/chunk_size.cpp
    http/http1/chunk_trailers.cpp
    http/http1/chunked_framing.cpp
    http/http1/interim_response.cpp
    http/http1/multipart.cpp
    http/http1/multipart_quoting.cpp
    http/http1/parser.cpp
    http/http1/request_body_decoding.cpp
    http/http1/request_target.cpp
    http/http1/transfer_coding_decoding.cpp

    # http/http2/connection
    http/http2/connection/data.cpp
    http/http2/connection/flow.cpp
    http/http2/connection/headers.cpp
    http/http2/connection/public.cpp
    http/http2/connection/session.cpp
    http/http2/connection/streams.cpp
    http/http2/connection/submit.cpp

    # http/http2/flow
    http/http2/flow/control.cpp
    http/http2/flow/ready_queue.cpp
    http/http2/flow/stream_flow.cpp

    # http/http2/frame
    http/http2/frame/codec.cpp
    http/http2/frame/offset_vector.cpp
    http/http2/frame/output_buffer.cpp
    http/http2/frame/payload.cpp
    http/http2/frame/payload_slice.cpp

    # http/http2/hpack
    http/http2/hpack/header_block.cpp
    http/http2/hpack/header_list.cpp
    http/http2/hpack/hpack.cpp
    http/http2/hpack/public.cpp
    http/http2/hpack/static_table.cpp

    # http/http2/message
    http/http2/message/connect.cpp
    http/http2/message/header_rules.cpp
    http/http2/message/local_content_state.cpp
    http/http2/message/remote_content_state.cpp
    http/http2/message/request_builder.cpp
    http/http2/message/request_headers.cpp
    http/http2/message/response_headers.cpp
    http/http2/message/websocket_handshake.cpp

    # http/http2/settings
    http/http2/settings/local_settings.cpp
    http/http2/settings/peer_settings.cpp
    http/http2/settings/settings.cpp

    # http/http2/stream
    http/http2/stream/classify.cpp
    http/http2/stream/closed_streams.cpp
    http/http2/stream/lifecycle.cpp
    http/http2/stream/request_data.cpp
    http/http2/stream/table.cpp

    # http/unit/parser
    http/unit/parser/char_syntax.cpp
    http/unit/parser/error.cpp
    http/unit/parser/header_block_parser.cpp
    http/unit/parser/header_classification.cpp

    # http/unit/request
    http/unit/request/access.cpp

    # http/unit/response
    http/unit/response/body.cpp
    http/unit/response/computed_response_headers.cpp
    http/unit/response/final_response_control.cpp
    http/unit/response/head_buffer.cpp
    http/unit/response/head_emit.cpp
    http/unit/response/head_policy.cpp
    http/unit/response/header_access.cpp
    http/unit/response/header_classification.cpp
    http/unit/response/header_index_cache.cpp
    http/unit/response/response.cpp
    http/unit/response/static_response_headers.cpp
    http/unit/response/trailers.cpp
    http/unit/response/vary_headers.cpp

    # http/unit/util
    http/unit/util/base64.cpp
    http/unit/util/number_format.cpp
    http/unit/util/pmr_string.cpp
    http/unit/util/url_encoding.cpp
    http/unit/util/url_encoding_detect.cpp

    # http/websocket
    http/websocket/accept.cpp
    http/websocket/assembler.cpp
    http/websocket/close.cpp
    http/websocket/deflate.cpp
    http/websocket/frame.cpp
    http/websocket/utils.cpp
    http/websocket/validation.cpp
    http/websocket/ws_connection.cpp

    # support
    support/test_main.cpp
)
endif()

if(RUVIA_BUILD_WEB)
ruvia_add_unit_suite(ruvia_web_unit_tests ruvia::web
    # support
    support/test_main.cpp

    # web/unit/app
    web/unit/app/config_validation.cpp
    web/unit/app/dotenv_parser.cpp
    web/unit/app/server_options_validation.cpp
    web/unit/app/testing_facade.cpp
    web/unit/app/route_middleware_config.cpp

    # web/unit/auth
    web/unit/auth/cookie_signature.cpp

    # web/unit/http
    web/unit/http/conn_info.cpp
    web/unit/http/trusted_proxy.cpp
    web/unit/http/request_deadline.cpp
    web/unit/http/context_capabilities.cpp
    web/unit/http/context_cookie.cpp
    web/unit/http/context_parse_body.cpp
    web/unit/http/context_redirect.cpp
    web/unit/http/context_request_lookups.cpp
    web/unit/http/cors.cpp
    web/unit/http/csrf.cpp
    web/unit/http/error.cpp
    web/unit/http/error_response.cpp
    web/unit/http/health.cpp
    web/unit/http/multipart_reader.cpp
    web/unit/http/request_body_limits.cpp
    web/unit/http/request_body_loader.cpp
    web/unit/http/response_compression.cpp
    web/unit/http/response_stream.cpp
    web/unit/http/scoped_operation_lifetime.cpp
    web/unit/http/security_headers.cpp
    web/unit/http/session.cpp
    web/unit/http/sse_writer.cpp
    web/unit/http/static_file_metadata.cpp
    web/unit/http/static_file_serving.cpp
    web/unit/http/static_files.cpp
    web/unit/http/static_path_normalization.cpp
    web/unit/http/stream_body_reader.cpp

    # web/unit/http2
    web/unit/http2/buffered_response_write.cpp
    web/unit/http2/cleartext_upgrade.cpp
    web/unit/http2/sansio_driver_body.cpp
    web/unit/http2/sansio_driver_session.cpp
    web/unit/http2/sansio_driver_websocket.cpp
    web/unit/http2/sansio_stream_runtime.cpp
    web/unit/http2/sansio_tls.cpp
    web/unit/http2/server_flow_control.cpp

    # web/unit/json
    web/unit/json/escape.cpp
    web/unit/json/json.cpp
    web/unit/json/lex.cpp
    web/unit/json/object_fields.cpp
    web/unit/json/size_hint.cpp

    # web/unit/model
    web/unit/model/content_type_match.cpp
    web/unit/model/email_like.cpp
    web/unit/model/form_parser.cpp
    web/unit/model/json_parser.cpp
    web/unit/model/plain_scalar_fields.cpp
    web/unit/model/json_writer.cpp
    web/unit/model/last_match.cpp
    web/unit/model/boxed_array.cpp
    web/unit/model/pattern_compiler.cpp
    web/unit/model/pattern_matcher.cpp
    web/unit/model/pattern_types.cpp
    web/unit/model/regex_matcher.cpp
    web/unit/model/string.cpp
    web/unit/model/validator.cpp

    # web/unit/ratelimit
    web/unit/ratelimit/rate_limit_decision.cpp
    web/unit/ratelimit/rate_limiter.cpp

    # web/unit/router
    web/unit/router/response_stream_dispatch.cpp
    web/unit/router/route_resolution.cpp
    web/unit/router/routing_dispatch.cpp
    web/unit/router/routing_matching.cpp
    web/unit/router/routing_middleware.cpp

    # web/server
    web/server/access_log.cpp
    web/server/buffered_response_write.cpp
    web/server/connection_read_buffer.cpp
    web/server/http_server_request_state.cpp
    web/server/http_server_worker_completion.cpp
    web/server/response_state.cpp

    # web/unit/websocket
    web/unit/websocket/connection.cpp
    web/unit/websocket/handshake.cpp
    web/unit/websocket/heartbeat.cpp
)

# The JWT suite links jwtSign/jwtVerify, which only exist when the optional JWT
# module is compiled in, so gate it on the same flag.
if(RUVIA_ENABLE_JWT)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/unit/auth/jwt.cpp)
endif()

# The Redis RESP-protocol suite links appendRespCommand, gated the same way.
if(RUVIA_ENABLE_REDIS)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/unit/redis/api_surface.cpp
        web/unit/redis/protocol.cpp
    )
endif()

# The backend-neutral API suite is present for either database driver.
if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/unit/db/api_surface.cpp
        web/unit/db/migration.cpp)
    ruvia_add_test(ruvia_guard_db_client_api
        web/guards/db_client_api_guard.cpp LIB ruvia::web LABELS "database;guards")
endif()

# Driver-specific parameter encoding is tested against the same library that
# performs it; neither suite pulls the other backend into a single-driver build.
if(RUVIA_ENABLE_MARIADB)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/unit/db/sql.cpp)
endif()
if(RUVIA_ENABLE_POSTGRESQL)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/unit/db/postgresql.cpp)
endif()

if(RUVIA_ENABLE_POSTGRESQL)
    ruvia_add_test(ruvia_postgresql_integration web/server/postgresql_integration.cpp
        LIB ruvia::web TIMEOUT 60 SERIAL LABELS "database;postgresql;integration")
    set_tests_properties(ruvia_postgresql_integration PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(RUVIA_ENABLE_MARIADB)
    ruvia_add_test(ruvia_mariadb_cancellation web/server/mariadb_cancellation.cpp
        LIB ruvia::web TIMEOUT 15 LABELS "database;mariadb")
    ruvia_add_test(ruvia_mariadb_connect_failure web/server/mariadb_connect_failure.cpp
        LIB ruvia::web TIMEOUT 15 LABELS "database;mariadb")
    ruvia_add_test(ruvia_mariadb_integration web/server/mariadb_integration.cpp
        LIB ruvia::web TIMEOUT 60 SERIAL LABELS "database;mariadb;integration")
    set_tests_properties(ruvia_mariadb_integration PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Needs no database: it drives the migration runner against a socket that
# accepts and stays silent, which is what an unscanned deadline used to hang on.
if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    ruvia_add_test(ruvia_standalone_db_client web/server/db_client.cpp
        LIB ruvia::web TIMEOUT 15 LABELS "database;worker")
    ruvia_add_test(ruvia_db_migration_timeout web/server/db_migration_timeout.cpp
        LIB ruvia::web TIMEOUT 60 LABELS "database")
endif()

ruvia_validate_test_sources(ruvia_http2_conformance_server ruvia::web
    web/server/http2_conformance_server.cpp)
add_executable(ruvia_http2_conformance_server web/server/http2_conformance_server.cpp)
target_link_libraries(ruvia_http2_conformance_server PRIVATE ruvia::web)
ruvia_apply_test_warnings(ruvia_http2_conformance_server)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME ruvia_http2_conformance
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/http/conformance/http2_wire_conformance.py
        --server $<TARGET_FILE:ruvia_http2_conformance_server>
)
set_tests_properties(ruvia_http2_conformance PROPERTIES
    LABELS "protocol;http2;conformance"
    RUN_SERIAL TRUE
    TIMEOUT 120
)
endif()

if(RUVIA_BUILD_CORE)
    ruvia_add_test(ruvia_asio_task core/asio_task.cpp LIB ruvia::core)

    ruvia_validate_test_sources(ruvia_smoke_core_target ruvia::core
        core/smoke_core_target.cpp)
    add_executable(ruvia_smoke_core_target core/smoke_core_target.cpp)
    target_link_libraries(ruvia_smoke_core_target PRIVATE ruvia::core)
    target_compile_definitions(ruvia_smoke_core_target PRIVATE
        RUVIA_EXPECTED_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        RUVIA_EXPECTED_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        RUVIA_EXPECTED_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        RUVIA_EXPECTED_VERSION_STRING="${PROJECT_VERSION}")
    add_test(NAME ruvia_smoke_core_target COMMAND ruvia_smoke_core_target)

    ruvia_add_test(ruvia_connection_scanner_lifetime core/connection_scanner_lifetime.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_event_loop_pool core/runtime_worker.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_operation_deadline core/operation_deadline.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_pool_lease_scheduler core/pool_lease_scheduler.cpp
        LIB ruvia::core TIMEOUT 10)
    ruvia_add_test(ruvia_task_scope core/task_scope.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_task_completion core/task_completion.cpp LIB ruvia::core)

    ruvia_validate_test_sources(ruvia_task_lifetime_violation ruvia::core
        core/task_lifetime_violation.cpp)
    add_executable(ruvia_task_lifetime_violation core/task_lifetime_violation.cpp)
    target_link_libraries(ruvia_task_lifetime_violation PRIVATE ruvia::core)
    add_test(NAME ruvia_task_lifetime_violation
        COMMAND ruvia_task_lifetime_violation void)
    set_tests_properties(ruvia_task_lifetime_violation PROPERTIES WILL_FAIL TRUE)
    add_test(NAME ruvia_task_lifetime_violation_value
        COMMAND ruvia_task_lifetime_violation value)
    set_tests_properties(
        ruvia_task_lifetime_violation_value PROPERTIES WILL_FAIL TRUE)

    ruvia_add_test(ruvia_blocking_pool core/blocking_pool.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_channel core/channel.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_one_shot core/one_shot.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_worker_timer core/worker_timer.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_worker_dispatch_failure core/worker_dispatch_failure.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_worker_async_await core/asio_await.cpp LIB ruvia::core)

    ruvia_validate_test_sources(ruvia_worker_wait_wake_violation ruvia::core
        core/worker_wait_wake_violation.cpp)
    add_executable(ruvia_worker_wait_wake_violation
        core/worker_wait_wake_violation.cpp)
    target_link_libraries(ruvia_worker_wait_wake_violation PRIVATE ruvia::core)
    foreach(_probe IN ITEMS channel one_shot)
        add_test(NAME ruvia_worker_wait_wake_${_probe}_violation
            COMMAND ruvia_worker_wait_wake_violation ${_probe})
        set_tests_properties(ruvia_worker_wait_wake_${_probe}_violation
            PROPERTIES WILL_FAIL TRUE TIMEOUT 10)
    endforeach()

    ruvia_validate_test_sources(ruvia_worker_signal_violation ruvia::core
        core/worker_signal_violation.cpp)
    add_executable(ruvia_worker_signal_violation core/worker_signal_violation.cpp)
    target_link_libraries(ruvia_worker_signal_violation PRIVATE ruvia::core)
    foreach(_probe IN ITEMS waiter notify dispatch)
        add_test(NAME ruvia_worker_signal_${_probe}_violation
            COMMAND ruvia_worker_signal_violation ${_probe})
        set_tests_properties(ruvia_worker_signal_${_probe}_violation
            PROPERTIES WILL_FAIL TRUE)
    endforeach()

endif()

if(RUVIA_BUILD_HTTP)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/http/guards/verify_http_status_literals.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/verify_http_status_literals.cmake
        @ONLY)
    add_test(
        NAME ruvia_guard_http_status_literals
        COMMAND ${CMAKE_COMMAND}
            -P ${CMAKE_CURRENT_BINARY_DIR}/verify_http_status_literals.cmake)

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/http/guards/verify_http_sansio.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/verify_http_sansio.cmake
        @ONLY)
    add_test(
        NAME ruvia_guard_http_sansio
        COMMAND ${CMAKE_COMMAND}
            -P ${CMAKE_CURRENT_BINARY_DIR}/verify_http_sansio.cmake)

    ruvia_add_test(ruvia_smoke_http_target http/unit/smoke.cpp LIB ruvia::http)

    # Compile the exhaustive public/detail HTTP contract once in the normal build.
    if(MSVC)
        set(RUVIA_HTTP_API_CONTRACT_SOURCE
            http/guards/msvc_api_contract.cpp)
    else()
        set(RUVIA_HTTP_API_CONTRACT_SOURCE
            http/guards/api_contract_guard.cpp)
    endif()
    ruvia_add_test(ruvia_guard_http_api_contract
        ${RUVIA_HTTP_API_CONTRACT_SOURCE} LIB ruvia::http)
    ruvia_add_test(ruvia_guard_http_api_runtime
        http/guards/api_contract_runtime.cpp LIB ruvia::http)
    ruvia_add_test(ruvia_guard_http2_stream_table
        http/guards/http2_stream_table_guard.cpp LIB ruvia::http)
endif()

if(RUVIA_BUILD_WEB)
    if(RUVIA_ENABLE_JWT)
        ruvia_add_test(ruvia_guard_jwt_feature_on
            web/guards/jwt_feature_on.cpp LIB ruvia::web LABELS "jwt;guards")
    else()
        # This object target deliberately uses the disabled JWT API. It is kept
        # out of the normal build and compiled by CTest: failure is the expected
        # result, while success means feature-off declarations leaked again.
        ruvia_validate_test_sources(ruvia_jwt_feature_off_probe ruvia::web
            web/guards/jwt_feature_off.cpp)
        add_library(ruvia_jwt_feature_off_probe OBJECT EXCLUDE_FROM_ALL
            web/guards/jwt_feature_off.cpp)
        target_link_libraries(ruvia_jwt_feature_off_probe PRIVATE ruvia::web)
        ruvia_apply_test_warnings(ruvia_jwt_feature_off_probe)

        configure_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/web/guards/verify_jwt_feature_off.cmake.in
            ${CMAKE_CURRENT_BINARY_DIR}/verify_jwt_feature_off.cmake
            @ONLY)
        add_test(NAME ruvia_guard_jwt_feature_off
            COMMAND ${CMAKE_COMMAND}
                -DCONFIG=$<CONFIG>
                -P ${CMAKE_CURRENT_BINARY_DIR}/verify_jwt_feature_off.cmake)
        set_tests_properties(ruvia_guard_jwt_feature_off PROPERTIES
            LABELS "jwt;guards"
            RUN_SERIAL TRUE
            TIMEOUT 60)
    endif()

    ruvia_add_test(ruvia_guard_web_api_surface
        web/guards/api_surface_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_web_config_aggregates
        web/guards/configuration_aggregates.cpp LIB ruvia::web)
    if(NOT MSVC)
        target_compile_options(ruvia_guard_web_config_aggregates PRIVATE
            -Wmissing-field-initializers
            -Werror=missing-field-initializers)
    endif()
    ruvia_add_test(ruvia_guard_web_blocking_api
        web/guards/blocking_api_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_web_cancellation_mailbox
        web/guards/cancellation_mailbox_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_web_controller_api
        web/guards/controller_api_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_web_model_api
        web/guards/model_api_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_middleware_next
        web/guards/middleware_next_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_context_response_state
        web/guards/context_response_state_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_context_request_header
        web/guards/context_request_header_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_cookie_api
        web/guards/cookie_api_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_web_error_header
        web/guards/error_header.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_app_lifecycle
        web/guards/app_lifecycle_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_app_singleton
        web/guards/app_singleton_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_http2_session_lifecycle
        web/guards/http2_session_lifecycle_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_scoped_operation_lifetime_violation
        web/unit/http/scoped_operation_lifetime_violation.cpp LIB ruvia::web WILL_FAIL)

    ruvia_add_test(ruvia_server_blocking_pool
        web/server/blocking_pool.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_immediate_shutdown
        web/server/immediate_shutdown.cpp LIB ruvia::web TIMEOUT 30)
    ruvia_add_test(ruvia_server_max_connections
        web/server/max_connections.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_notfound_keepalive
        web/server/notfound_keepalive.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_deadline_body_limit_wire
        web/server/deadline_body_limit_wire.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_deadline_error_handler_wire
        web/server/deadline_error_handler_wire.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_http_client
        web/server/http_client.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_http_client_limits
        web/server/http_client_limits.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_http2_document_root
        web/server/http2_document_root.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_head_document_root
        web/server/head_document_root.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_static_root_refresh
        web/server/static_root_refresh.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_buffered_body_limit
        web/server/buffered_body_limit.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_stream_body_limit
        web/server/stream_body_limit.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_head_stream_route
        web/server/head_stream_route.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_connection_failure
        web/server/connection_failure.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_error_static_file
        web/server/error_static_file.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_worker_state
        web/server/worker_state.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_http2_head_stream_route
        web/server/http2_head_stream_route.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_http2_connection_failure
        web/server/http2_connection_failure.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_pipeline_request_views
        web/server/pipeline_request_views.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_tls_keepalive_deadline
        web/server/tls_keepalive_deadline.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_websocket
        web/server/websocket.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_keepalive_idle_timeout
        web/server/keepalive_idle_timeout.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_write_timeout
        web/server/write_timeout.cpp LIB ruvia::web TIMEOUT 60)

    ruvia_add_test(ruvia_web_worker
        web/server/worker.cpp LIB ruvia::web TIMEOUT 30)
    ruvia_add_test(ruvia_standalone_http_client
        web/server/standalone_http_client.cpp LIB ruvia::web TIMEOUT 30)
    ruvia_add_test(ruvia_app_web_worker
        web/server/app_web_worker.cpp LIB ruvia::web TIMEOUT 30)
    ruvia_add_test(ruvia_app_startup_retry
        web/server/app_startup_retry.cpp LIB ruvia::web TIMEOUT 30)
endif()
