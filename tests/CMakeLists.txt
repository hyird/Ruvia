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
            ${CMAKE_CURRENT_SOURCE_DIR}/web/support)
    elseif("${lib}" STREQUAL "ruvia::edge")
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/http/support)
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
    elseif("${lib}" STREQUAL "ruvia::edge")
        set(_prefix "edge/")
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

    # support
    support/test_main.cpp
)
endif()

if(RUVIA_BUILD_HTTP)
ruvia_add_unit_suite(ruvia_http_unit_tests ruvia::http
    # http/client
    http/client/origin.cpp
    http/client/redirect.cpp
    http/client/request.cpp
    http/client/response_coding.cpp
    http/client/response_exchange.cpp
    http/client/response_framing.cpp
    http/client/response_parser.cpp

    # http/coding
    http/coding/content_codec.cpp
    http/coding/content_negotiation.cpp
    http/coding/response_content_semantics.cpp

    # http/cookie
    http/cookie/validation.cpp

    # http/field
    http/field/accept_media_type.cpp
    http/field/byte_range.cpp
    http/field/cache.cpp
    http/field/conditional_request.cpp
    http/field/connection_fields.cpp
    http/field/date_cache.cpp
    http/field/date_parsing.cpp
    http/field/expectations.cpp
    http/field/header_tokens.cpp
    http/field/method.cpp
    http/field/quoted_field_values.cpp
    http/field/semicolon_parameters.cpp
    http/field/syntax.cpp
    http/field/value_primitives.cpp

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

    # http/parser
    http/parser/char_syntax.cpp
    http/parser/error.cpp
    http/parser/header_block_parser.cpp
    http/parser/header_classification.cpp

    # http/request
    http/request/access.cpp

    # http/response
    http/response/body.cpp
    http/response/computed_response_headers.cpp
    http/response/final_response_control.cpp
    http/response/head_buffer.cpp
    http/response/head_emit.cpp
    http/response/head_policy.cpp
    http/response/header_access.cpp
    http/response/header_classification.cpp
    http/response/header_index_cache.cpp
    http/response/response.cpp
    http/response/static_response_headers.cpp
    http/response/trailers.cpp
    http/response/vary_headers.cpp

    # http/util
    http/util/base64.cpp
    http/util/number_format.cpp
    http/util/pmr_string.cpp
    http/util/url_encoding.cpp
    http/util/url_encoding_detect.cpp

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

    # web/app
    web/app/config_validation.cpp
    web/app/dotenv_parser.cpp
    web/app/server_options_validation.cpp
    web/app/testing_facade.cpp

    # web/auth
    web/auth/cookie_signature.cpp

    # web/http
    web/http/conn_info.cpp
    web/http/context_capabilities.cpp
    web/http/context_cookie.cpp
    web/http/context_parse_body.cpp
    web/http/context_redirect.cpp
    web/http/context_request_lookups.cpp
    web/http/cors.cpp
    web/http/csrf.cpp
    web/http/error.cpp
    web/http/error_response.cpp
    web/http/health.cpp
    web/http/multipart_reader.cpp
    web/http/request_body_limits.cpp
    web/http/request_body_loader.cpp
    web/http/response_compression.cpp
    web/http/response_stream.cpp
    web/http/scoped_operation_lifetime.cpp
    web/http/security_headers.cpp
    web/http/session.cpp
    web/http/sse_writer.cpp
    web/http/static_file_metadata.cpp
    web/http/static_file_serving.cpp
    web/http/static_files.cpp
    web/http/static_path_normalization.cpp
    web/http/stream_body_reader.cpp

    # web/http2
    web/http2/buffered_response_write.cpp
    web/http2/cleartext_upgrade.cpp
    web/http2/sansio_driver_body.cpp
    web/http2/sansio_driver_session.cpp
    web/http2/sansio_driver_websocket.cpp
    web/http2/sansio_stream_runtime.cpp
    web/http2/sansio_tls.cpp
    web/http2/server_flow_control.cpp

    # web/json
    web/json/escape.cpp
    web/json/json.cpp
    web/json/lex.cpp
    web/json/object_fields.cpp
    web/json/size_hint.cpp

    # web/model
    web/model/content_type_match.cpp
    web/model/email_like.cpp
    web/model/form_parser.cpp
    web/model/json_writer.cpp
    web/model/last_match.cpp
    web/model/list.cpp
    web/model/pattern_compiler.cpp
    web/model/pattern_matcher.cpp
    web/model/pattern_types.cpp
    web/model/regex_matcher.cpp
    web/model/string.cpp
    web/model/validator.cpp

    # web/ratelimit
    web/ratelimit/rate_limit_decision.cpp
    web/ratelimit/rate_limiter.cpp

    # web/router
    web/router/response_stream_dispatch.cpp
    web/router/route_resolution.cpp
    web/router/routing_dispatch.cpp
    web/router/routing_matching.cpp
    web/router/routing_middleware.cpp

    # web/server
    web/server/access_log.cpp
    web/server/buffered_response_write.cpp
    web/server/connection_read_buffer.cpp
    web/server/http_server_request_state.cpp
    web/server/http_server_worker_completion.cpp
    web/server/response_state.cpp

    # web/websocket
    web/websocket/connection.cpp
    web/websocket/handshake.cpp
    web/websocket/heartbeat.cpp
)

# The JWT suite links jwtSign/jwtVerify, which only exist when the optional JWT
# module is compiled in, so gate it on the same flag.
if(RUVIA_ENABLE_JWT)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/auth/jwt.cpp)
endif()

# The Redis RESP-protocol suite links appendRespCommand, gated the same way.
if(RUVIA_ENABLE_REDIS)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/redis/api_surface.cpp
        web/redis/protocol.cpp
    )
endif()

# The backend-neutral API suite is present for either database driver.
if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/db/api_surface.cpp)
endif()

# Driver-specific parameter encoding is tested against the same library that
# performs it; neither suite pulls the other backend into a single-driver build.
if(RUVIA_ENABLE_MARIADB)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/db/sql.cpp)
endif()
if(RUVIA_ENABLE_POSTGRESQL)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/db/postgresql.cpp)
endif()

if(RUVIA_ENABLE_POSTGRESQL_INTEGRATION_TESTS)
    ruvia_add_test(ruvia_postgresql_integration web/server/postgresql_integration.cpp
        LIB ruvia::web TIMEOUT 60 SERIAL LABELS "database;postgresql;integration")
endif()

if(RUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS)
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
endif()

if(RUVIA_BUILD_CORE)
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
    ruvia_add_test(ruvia_pool_lease_scheduler core/pool_lease_scheduler.cpp LIB ruvia::core)
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

    ruvia_add_test(ruvia_smoke_http_target http/smoke_http_target.cpp LIB ruvia::http)

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
    ruvia_add_test(ruvia_guard_http2_stream_table
        http/guards/http2_stream_table_guard.cpp LIB ruvia::http)
endif()

if(RUVIA_BUILD_WEB)
    ruvia_add_test(ruvia_guard_web_api_surface
        web/guards/api_surface_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_middleware_next
        web/guards/middleware_next_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_context_response_state
        web/guards/context_response_state_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_context_request_header
        web/guards/context_request_header_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_cookie_api
        web/guards/cookie_api_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_app_lifecycle
        web/guards/app_lifecycle_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_guard_http2_session_lifecycle
        web/guards/http2_session_lifecycle_guard.cpp LIB ruvia::web)
    ruvia_add_test(ruvia_scoped_operation_lifetime_violation
        web/http/scoped_operation_lifetime_violation.cpp LIB ruvia::web WILL_FAIL)

    ruvia_add_test(ruvia_server_blocking_pool
        web/server/blocking_pool.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_immediate_shutdown
        web/server/immediate_shutdown.cpp LIB ruvia::web TIMEOUT 30)
    ruvia_add_test(ruvia_server_max_connections
        web/server/max_connections.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_notfound_keepalive
        web/server/notfound_keepalive.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_http2_document_root
        web/server/http2_document_root.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_head_document_root
        web/server/head_document_root.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_head_stream_route
        web/server/head_stream_route.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_connection_failure
        web/server/connection_failure.cpp LIB ruvia::web TIMEOUT 60)
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
    ruvia_add_test(ruvia_server_keepalive_idle_timeout
        web/server/keepalive_idle_timeout.cpp LIB ruvia::web TIMEOUT 60)
    ruvia_add_test(ruvia_server_write_timeout
        web/server/write_timeout.cpp LIB ruvia::web TIMEOUT 60)

    ruvia_add_test(ruvia_web_worker
        web/server/worker.cpp LIB ruvia::web TIMEOUT 30)
    ruvia_add_test(ruvia_data_access
        web/server/data_access.cpp LIB ruvia::web TIMEOUT 30)
    ruvia_add_test(ruvia_app_web_worker
        web/server/app_web_worker.cpp LIB ruvia::web TIMEOUT 30)
    ruvia_add_test(ruvia_app_startup_retry
        web/server/app_startup_retry.cpp LIB ruvia::web TIMEOUT 30)
endif()

# Edge tests only need ruvia::edge; keep them outside the web gate so a
# web-off edge build still runs them.
if(RUVIA_BUILD_EDGE)
    ruvia_add_test(ruvia_smoke_edge_target edge/smoke_edge_target.cpp LIB ruvia::edge)
    ruvia_add_test(ruvia_edge_config edge/config.cpp LIB ruvia::edge TIMEOUT 30)
    ruvia_add_test(ruvia_edge_freshness edge/freshness.cpp LIB ruvia::edge TIMEOUT 30)
    ruvia_add_test(ruvia_edge_cache edge/cache.cpp LIB ruvia::edge TIMEOUT 30)
    ruvia_add_test(ruvia_disk_cache edge/disk_cache.cpp LIB ruvia::edge TIMEOUT 30)
    ruvia_add_test(ruvia_origin_fetcher edge/origin_fetcher.cpp LIB ruvia::edge TIMEOUT 30)
    ruvia_add_test(ruvia_edge_server edge/server.cpp LIB ruvia::edge TIMEOUT 30)
    # nghttp and h2load drive the edge HTTP/2 checks. They are an external CLI
    # dependency, not a build dependency, so a machine without them configures
    # and runs everything else instead of failing outright.
    if(NOT WIN32)
        find_program(RUVIA_NGHTTP_EXECUTABLE NAMES nghttp)
        find_program(RUVIA_H2LOAD_EXECUTABLE NAMES h2load)
        if(RUVIA_NGHTTP_EXECUTABLE AND RUVIA_H2LOAD_EXECUTABLE)
            target_compile_definitions(ruvia_edge_server PRIVATE
                RUVIA_NGHTTP_EXECUTABLE="${RUVIA_NGHTTP_EXECUTABLE}"
                RUVIA_H2LOAD_EXECUTABLE="${RUVIA_H2LOAD_EXECUTABLE}")
        else()
            message(STATUS
                "nghttp2 CLI tools not found: ruvia_edge_server will skip its "
                "HTTP/2 client checks. Install nghttp2 to run them.")
        endif()
    endif()
endif()
