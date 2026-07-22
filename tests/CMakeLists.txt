# One registration helper for every standalone test executable. It links the
# requested library (ruvia::web by default), puts this directory on the include
# path so a test can pull in support/test_harness.h from any subdirectory,
# applies the MSVC large-object flags uniformly, registers the ctest, and sets
# the optional TIMEOUT / WILL_FAIL / RUN_SERIAL / LABELS properties. Aggregated
# suites and the few multi-registration probes below stay explicit.
function(ruvia_add_test name source)
    cmake_parse_arguments(T "WILL_FAIL;SERIAL" "LIB;TIMEOUT;LABELS" "" ${ARGN})
    if(NOT DEFINED T_LIB)
        set(T_LIB ruvia::web)
    endif()
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${T_LIB})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
        ${CMAKE_CURRENT_SOURCE_DIR}/http/support
        ${CMAKE_CURRENT_SOURCE_DIR}/web/support
        ${CMAKE_CURRENT_SOURCE_DIR}/edge/support)
    if(MSVC)
        target_compile_options(${name} PRIVATE /bigobj /FS)
    endif()
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

if(RUVIA_BUILD_WEB)
add_executable(ruvia_unit_tests
    support/test_main.cpp

    # http/http1
    http/http1/chunk_decoder.cpp
    http/http1/chunk_size.cpp
    http/http1/form_parser.cpp
    http/http1/http1_buffered_response_write.cpp
    http/http1/http1_chunked_framing.cpp
    http/http1/http1_interim_response.cpp
    http/http1/http1_parser.cpp
    http/http1/http_parsing.cpp
    http/http1/http_stream_body_reader.cpp
    http/http1/multipart.cpp
    http/http1/multipart_reader.cpp
    http/http1/request_body_decoding.cpp
    http/http1/transfer_coding_decoding.cpp
    http/http1/request_body_loader.cpp
    http/http1/request_target.cpp

    # http/http2
    http/http2/hpack.cpp
    http/http2/hpack_static_table.cpp
    http/http2/http2_buffered_response_write.cpp
    http/http2/http2_classify.cpp
    http/http2/http2_closed_streams.cpp
    http/http2/http2_connect.cpp
    http/http2/http2_connection_data.cpp
    http/http2/http2_connection_flow.cpp
    http/http2/http2_connection_headers.cpp
    http/http2/http2_connection_session.cpp
    http/http2/http2_connection_streams.cpp
    http/http2/http2_connection_submit.cpp
    http/http2/http2_flow_control.cpp
    http/http2/http2_frame.cpp
    http/http2/http2_frame_codec.cpp
    http/http2/http2_frame_payload.cpp
    http/http2/http2_header_block.cpp
    http/http2/http2_header_list.cpp
    http/http2/http2_header_rules.cpp
    http/http2/http2_local_content_state.cpp
    http/http2/http2_local_settings.cpp
    http/http2/http2_offset_vector.cpp
    http/http2/http2_output_buffer.cpp
    http/http2/http2_payload.cpp
    http/http2/http2_payload_slice.cpp
    http/http2/http2_peer_settings.cpp
    http/http2/http2_ready_queue.cpp
    http/http2/http2_remote_content_state.cpp
    http/http2/http2_request_builder.cpp
    http/http2/http2_request_headers.cpp
    http/http2/http2_response_headers.cpp
    http/http2/http2_sansio_stream_runtime.cpp
    http/http2/http2_server_flow_control.cpp
    http/http2/http2_settings.cpp
    http/http2/http2_stream_flow.cpp
    http/http2/http2_stream_lifecycle.cpp
    http/http2/http2_stream_request_data.cpp
    http/http2/http2_stream_table.cpp
    http/http2/http2_websocket_handshake.cpp
    http/http2/sansio_driver_session.cpp
    http/http2/sansio_driver_body.cpp
    http/http2/sansio_driver_websocket.cpp
    http/http2/sansio_tls.cpp

    # http/unit
    http/unit/accept_media_type.cpp
    http/unit/conditional_request.cpp
    http/unit/connection_read_buffer.cpp
    http/unit/content_negotiation.cpp
    http/unit/computed_response_headers.cpp
    http/unit/content_codec.cpp
    http/unit/content_type_match.cpp
    http/unit/date_cache.cpp
    http/unit/error.cpp
    http/unit/error_response.cpp
    http/unit/final_response_control.cpp
    http/unit/header_block_parser.cpp
    http/unit/header_classification.cpp
    http/unit/header_params.cpp
    http/unit/header_tokens.cpp
    http/unit/http_basics.cpp
    http/unit/http_byte_range.cpp
    http/unit/http_cache.cpp
    http/unit/http_char_syntax.cpp
    http/unit/http_client_redirect.cpp
    http/unit/http_client_request.cpp
    http/unit/http_client_response_parser.cpp
    http/unit/http_client_response_framing.cpp
    http/unit/http_client_response_exchange.cpp
    http/unit/http_client_response_coding.cpp
    http/unit/http_origin.cpp
    http/unit/http_response.cpp
    http/unit/http_response_body.cpp
    http/unit/http_response_content_semantics.cpp
    http/unit/parser_validation.cpp
    http/unit/request_access.cpp
    http/unit/response_compression.cpp
    http/unit/response_head_buffer.cpp
    http/unit/response_head_emit.cpp
    http/unit/response_head_policy.cpp
    http/unit/response_header_access.cpp
    http/unit/response_header_classification.cpp
    http/unit/response_header_index_cache.cpp
    http/unit/response_trailers.cpp
    http/unit/static_response_headers.cpp
    http/unit/vary_headers.cpp

    # http/websocket
    http/websocket/websocket_accept.cpp
    http/websocket/websocket_assembler.cpp
    http/websocket/websocket_close.cpp
    http/websocket/websocket_connection.cpp
    http/websocket/websocket_deflate.cpp
    http/websocket/websocket_frame.cpp
    http/websocket/websocket_handshake.cpp
    http/websocket/websocket_heartbeat.cpp
    http/websocket/websocket_utils.cpp
    http/websocket/websocket_validation.cpp
    http/websocket/ws_connection.cpp

    # web/unit
    web/unit/access_log.cpp
    web/unit/config_validation.cpp
    web/unit/conn_info.cpp
    web/unit/context_capabilities.cpp
    web/unit/context_cookie.cpp
    web/unit/context_redirect.cpp
    web/unit/cookie_signature.cpp
    web/unit/cookie_validation.cpp
    web/unit/cors.cpp
    web/unit/csrf.cpp
    web/unit/dotenv_parser.cpp
    web/unit/email_like.cpp
    web/unit/encoding.cpp
    web/unit/file_response_helpers.cpp
    web/unit/health.cpp
    web/unit/http_server_request_state.cpp
    web/unit/http_server_worker_completion.cpp
    web/unit/json.cpp
    web/unit/json_escape.cpp
    web/unit/json_lex.cpp
    web/unit/json_object_fields.cpp
    web/unit/json_size_hint.cpp
    web/unit/json_writer.cpp
    web/unit/model_list.cpp
    web/unit/model_string.cpp
    web/unit/pattern_compiler.cpp
    web/unit/pattern_matcher.cpp
    web/unit/pattern_types.cpp
    web/unit/pmr_string.cpp
    web/unit/pool_waiter_queue.cpp
    web/unit/rate_limit_decision.cpp
    web/unit/request_body_limits.cpp
    web/unit/rate_limiter.cpp
    web/unit/regex_matcher.cpp
    web/unit/response_stream_dispatch.cpp
    web/unit/route_resolution.cpp
    web/unit/routing_matching.cpp
    web/unit/routing_middleware.cpp
    web/unit/routing_dispatch.cpp
    web/unit/security_headers.cpp
    web/unit/server_options_validation.cpp
    web/unit/session.cpp
    web/unit/static_file_serving.cpp
    web/unit/static_files.cpp
    web/unit/static_path_normalization.cpp
    web/unit/response_stream.cpp
    web/unit/scoped_operation_lifetime.cpp
    web/unit/sse_writer.cpp
    web/unit/testing_facade.cpp
    web/unit/url_encoding_detect.cpp
    web/unit/validator.cpp
)

# The JWT suite links jwtSign/jwtVerify, which only exist when the optional JWT
# module is compiled in, so gate it on the same flag.
if(RUVIA_ENABLE_JWT)
    target_sources(ruvia_unit_tests PRIVATE web/unit/jwt.cpp)
endif()

# The Redis RESP-protocol suite links appendRespCommand, gated the same way.
if(RUVIA_ENABLE_REDIS)
    target_sources(ruvia_unit_tests PRIVATE
        web/unit/redis_api_surface.cpp
        web/unit/redis_protocol.cpp
    )
endif()

# The backend-neutral API suite is present for either database driver.
if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    target_sources(ruvia_unit_tests PRIVATE web/unit/db_api_surface.cpp)
endif()

# Driver-specific parameter encoding is tested against the same library that
# performs it; neither suite pulls the other backend into a single-driver build.
if(RUVIA_ENABLE_MARIADB)
    target_sources(ruvia_unit_tests PRIVATE web/unit/db_sql.cpp)
endif()
if(RUVIA_ENABLE_POSTGRESQL)
    target_sources(ruvia_unit_tests PRIVATE web/unit/db_postgresql.cpp)
endif()

if(RUVIA_ENABLE_POSTGRESQL_INTEGRATION_TESTS)
    ruvia_add_test(ruvia_postgresql_integration web/server/postgresql_integration.cpp
        TIMEOUT 60 SERIAL LABELS "database;postgresql;integration")
endif()

target_link_libraries(ruvia_unit_tests PRIVATE ruvia::web)
target_include_directories(ruvia_unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/support
    ${CMAKE_CURRENT_SOURCE_DIR}/http/support
    ${CMAKE_CURRENT_SOURCE_DIR}/web/support)
if(MSVC)
    target_compile_options(ruvia_unit_tests PRIVATE /bigobj /FS)
endif()

add_test(NAME ruvia_unit_tests COMMAND ruvia_unit_tests)
set_tests_properties(ruvia_unit_tests PROPERTIES TIMEOUT 180)

if(RUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS)
    add_executable(ruvia_http2_conformance_server web/server/http2_conformance_server.cpp)
    target_link_libraries(ruvia_http2_conformance_server PRIVATE ruvia::web)
    if(MSVC)
        target_compile_options(ruvia_http2_conformance_server PRIVATE /bigobj /FS)
    endif()
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

    add_executable(ruvia_task_lifetime_violation core/task_lifetime_violation.cpp)
    target_link_libraries(ruvia_task_lifetime_violation PRIVATE ruvia::core)
    add_test(NAME ruvia_task_lifetime_violation
        COMMAND ruvia_task_lifetime_violation void)
    set_tests_properties(ruvia_task_lifetime_violation PROPERTIES WILL_FAIL TRUE)
    add_test(NAME ruvia_task_lifetime_violation_value
        COMMAND ruvia_task_lifetime_violation value)
    set_tests_properties(
        ruvia_task_lifetime_violation_value PROPERTIES WILL_FAIL TRUE)

    if(RUVIA_BUILD_WEB)
        ruvia_add_test(ruvia_scoped_operation_lifetime_violation
            core/scoped_operation_lifetime_violation.cpp WILL_FAIL)
    endif()

    ruvia_add_test(ruvia_channel core/channel.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_one_shot core/one_shot.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_worker_timer core/worker_timer.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_worker_dispatch_failure core/worker_dispatch_failure.cpp LIB ruvia::core)

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

    ruvia_add_test(ruvia_smoke_http_target http/unit/smoke_http_target.cpp LIB ruvia::http)

    # Compile the exhaustive public/detail HTTP contract once in the normal build.
    if(MSVC)
        set(RUVIA_HTTP_API_CONTRACT_SOURCE
            http/guards/http_msvc_api_contract.cpp)
    else()
        set(RUVIA_HTTP_API_CONTRACT_SOURCE
            http/guards/http_api_contract_guard.cpp)
    endif()
    ruvia_add_test(ruvia_guard_http_api_contract
        ${RUVIA_HTTP_API_CONTRACT_SOURCE} LIB ruvia::http)
    ruvia_add_test(ruvia_guard_http2_stream_table
        http/guards/http2_stream_table_guard.cpp LIB ruvia::http)
endif()

if(RUVIA_BUILD_WEB)
    ruvia_add_test(ruvia_guard_middleware_next web/guards/middleware_next_guard.cpp)
    ruvia_add_test(ruvia_guard_context_response_state web/guards/context_response_state_guard.cpp)
    ruvia_add_test(ruvia_guard_context_request_header web/guards/context_request_header_guard.cpp)
    ruvia_add_test(ruvia_guard_cookie_api web/guards/cookie_api_guard.cpp)
    ruvia_add_test(ruvia_guard_app_lifecycle web/guards/app_lifecycle_guard.cpp)
    ruvia_add_test(ruvia_guard_http2_session_lifecycle web/guards/http2_session_lifecycle_guard.cpp)

    ruvia_add_test(ruvia_server_immediate_shutdown web/server/immediate_shutdown.cpp TIMEOUT 30)
    ruvia_add_test(ruvia_server_max_connections web/server/max_connections.cpp TIMEOUT 60)
    ruvia_add_test(ruvia_server_notfound_keepalive web/server/notfound_keepalive.cpp TIMEOUT 60)
    ruvia_add_test(ruvia_server_http2_document_root web/server/http2_document_root.cpp TIMEOUT 60)
    ruvia_add_test(ruvia_server_head_document_root web/server/head_document_root.cpp TIMEOUT 60)
    ruvia_add_test(ruvia_server_head_stream_route web/server/head_stream_route.cpp TIMEOUT 60)
    ruvia_add_test(ruvia_server_worker_state web/server/worker_state.cpp TIMEOUT 60)
    ruvia_add_test(ruvia_server_http2_head_stream_route web/server/http2_head_stream_route.cpp TIMEOUT 60)
    ruvia_add_test(ruvia_server_pipeline_request_views web/server/pipeline_request_views.cpp TIMEOUT 60)
    ruvia_add_test(ruvia_server_tls_keepalive_deadline web/server/tls_keepalive_deadline.cpp TIMEOUT 60)
    ruvia_add_test(ruvia_server_keepalive_idle_timeout web/server/keepalive_idle_timeout.cpp TIMEOUT 60)
    ruvia_add_test(ruvia_server_write_timeout web/server/write_timeout.cpp TIMEOUT 60)

    ruvia_add_test(ruvia_web_worker web/server/web_worker.cpp TIMEOUT 30)
    ruvia_add_test(ruvia_data_access web/server/data_access.cpp TIMEOUT 30)
    ruvia_add_test(ruvia_app_web_worker web/server/app_web_worker.cpp TIMEOUT 30)
    ruvia_add_test(ruvia_app_startup_retry web/server/app_startup_retry.cpp TIMEOUT 30)
endif()

# Edge tests only need ruvia::edge; keep them outside the web gate so a
# web-off edge build still runs them.
if(RUVIA_BUILD_EDGE)
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
