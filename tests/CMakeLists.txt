# One registration helper for every standalone test executable. It links the
# requested library (ruvia::web by default), puts this directory on the include
# path so a test can pull in support/test_harness.h from any subdirectory, applies the
# MSVC large-object flags uniformly, registers the ctest, and sets the optional
# TIMEOUT / WILL_FAIL / RUN_SERIAL / LABELS properties. Aggregated suites and the
# few multi-registration probes below stay explicit.
function(ruvia_add_test name source)
    cmake_parse_arguments(T "WILL_FAIL;SERIAL" "LIB;TIMEOUT;LABELS" "" ${ARGN})
    if(NOT DEFINED T_LIB)
        set(T_LIB ruvia::web)
    endif()
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${T_LIB})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
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
    web/unit_encoding.cpp
    web/unit_json.cpp
    web/unit_model_list.cpp
    web/unit_model_string.cpp
    web/unit_cookie_signature.cpp
    web/unit_cookie_validation.cpp
    http1/unit_http_parsing.cpp
    http1/unit_multipart.cpp
    http1/unit_multipart_reader.cpp
    http1/unit_request_body_loader.cpp
    http/unit_content_negotiation.cpp
    http/unit_accept_media_type.cpp
    http1/unit_form_parser.cpp
    web/unit_rate_limiter.cpp
    http1/unit_chunk_size.cpp
    http/unit_conditional_request.cpp
    websocket/unit_websocket_deflate.cpp
    http1/unit_request_body_decoding.cpp
    http1/unit_http_stream_body_reader.cpp
    http1/unit_request_target.cpp
    websocket/unit_websocket_accept.cpp
    websocket/unit_websocket_validation.cpp
    websocket/unit_websocket_frame.cpp
    http1/unit_chunk_decoder.cpp
    web/unit_csrf.cpp
    web/unit_session.cpp
    http/unit_header_classification.cpp
    http/unit_header_block_parser.cpp
    http/unit_response_header_classification.cpp
    http/unit_http_basics.cpp
    web/unit_json_object_fields.cpp
    http/unit_response_header_index_cache.cpp
    web/unit_file_response_helpers.cpp
    web/unit_json_writer.cpp
    http2/unit_hpack.cpp
    http2/unit_http2_frame.cpp
    http2/unit_http2_settings.cpp
    http2/unit_http2_local_settings.cpp
    http2/unit_http2_stream_flow.cpp
    http2/unit_http2_payload.cpp
    http2/unit_http2_closed_streams.cpp
    http2/unit_http2_stream_table.cpp
    http2/unit_http2_remote_content_state.cpp
    http2/unit_http2_local_content_state.cpp
    http2/unit_http2_connect.cpp
    http2/unit_http2_ready_queue.cpp
    http2/unit_http2_sansio_stream_runtime.cpp
    websocket/unit_websocket_assembler.cpp
    websocket/unit_websocket_heartbeat.cpp
    websocket/unit_websocket_close.cpp
    web/unit_config_validation.cpp
    web/unit_server_options_validation.cpp
    web/unit_http_server_worker_completion.cpp
    http/unit_response_head_buffer.cpp
    web/unit_pmr_string.cpp
    http2/unit_http2_offset_vector.cpp
    http2/unit_http2_stream_lifecycle.cpp
    http2/unit_http2_header_rules.cpp
    http/unit_response_trailers.cpp
    http2/unit_http2_frame_payload.cpp
    http2/unit_sansio_tls.cpp
    http2/unit_http2_connection.cpp
    websocket/unit_ws_connection.cpp
    websocket/unit_websocket_connection.cpp
    http2/unit_sansio_driver.cpp
    http/unit_static_response_headers.cpp
    web/unit_streaming.cpp
    web/unit_response_stream_dispatch.cpp
    http2/unit_http2_buffered_response_write.cpp
    http/unit_http_cache.cpp
    http/unit_vary_headers.cpp
    http/unit_http_byte_range.cpp
    http/unit_content_range.cpp
    web/unit_static_path_normalization.cpp
    websocket/unit_websocket_utils.cpp
    http2/unit_hpack_static_table.cpp
    web/unit_pool_waiter_queue.cpp
    http2/unit_http2_header_list.cpp
    http/unit_response_head_policy.cpp
    http/unit_final_response_control.cpp
    http/unit_http_response_content_semantics.cpp
    http2/unit_http2_request_builder.cpp
    http/unit_request_access.cpp
    web/unit_context_cookie.cpp
    web/unit_conn_info.cpp
    web/unit_access_log.cpp
    web/unit_context_redirect.cpp
    web/unit_context_capabilities.cpp
    http2/unit_http2_frame_codec.cpp
    http2/unit_http2_output_buffer.cpp
    http2/unit_http2_peer_settings.cpp
    web/unit_route_resolution.cpp
    http/unit_connection_read_buffer.cpp
    http2/unit_http2_stream_request_data.cpp
    http2/unit_http2_request_headers.cpp
    http1/unit_http1_parser.cpp
    http1/unit_http1_chunked_framing.cpp
    http1/unit_http1_buffered_response_write.cpp
    http/unit_response_header_access.cpp
    http/unit_date_cache.cpp
    http/unit_error.cpp
    http/unit_error_response.cpp
    web/unit_health.cpp
    web/unit_validator.cpp
    http/unit_http_response.cpp
    http/unit_http_response_body.cpp
    http1/unit_http1_interim_response.cpp
    http/unit_parser_validation.cpp
    http2/unit_http2_flow_control.cpp
    http2/unit_http2_server_flow_control.cpp
    web/unit_http_server_request_state.cpp
    http2/unit_http2_classify.cpp
    http/unit_http_char_syntax.cpp
    web/unit_pattern_compiler.cpp
    web/unit_pattern_matcher.cpp
    web/unit_json_lex.cpp
    http/unit_content_type_match.cpp
    web/unit_email_like.cpp
    web/unit_pattern_types.cpp
    http/unit_header_tokens.cpp
    http/unit_header_params.cpp
    web/unit_json_escape.cpp
    web/unit_json_size_hint.cpp
    web/unit_url_encoding_detect.cpp
    web/unit_rate_limit_decision.cpp
    http/unit_response_compression.cpp
    http/unit_response_head_emit.cpp
    web/unit_security_headers.cpp
    websocket/unit_websocket_handshake.cpp
    web/unit_dotenv_parser.cpp
    http2/unit_http2_header_block.cpp
    http2/unit_http2_payload_slice.cpp
    http2/unit_http2_response_headers.cpp
    http2/unit_http2_websocket_handshake.cpp
    http/unit_http_client_request.cpp
    http/unit_http_client_response.cpp
    http/unit_http_origin.cpp
    http/unit_http_client_redirect.cpp
    web/unit_routing.cpp
    web/unit_testing_facade.cpp
    web/unit_cors.cpp
)

# The JWT suite links jwtSign/jwtVerify, which only exist when the optional JWT
# module is compiled in, so gate it on the same flag.
if(RUVIA_ENABLE_JWT)
    target_sources(ruvia_unit_tests PRIVATE web/unit_jwt.cpp)
endif()

# The Redis RESP-protocol suite links appendRespCommand, gated the same way.
if(RUVIA_ENABLE_REDIS)
    target_sources(ruvia_unit_tests PRIVATE
        web/unit_redis_api_surface.cpp
        web/unit_redis_protocol.cpp
    )
endif()

# The backend-neutral API suite is present for either database driver.
if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    target_sources(ruvia_unit_tests PRIVATE web/unit_db_api_surface.cpp)
endif()

# Driver-specific parameter encoding is tested against the same library that
# performs it; neither suite pulls the other backend into a single-driver build.
if(RUVIA_ENABLE_MARIADB)
    target_sources(ruvia_unit_tests PRIVATE web/unit_db_sql.cpp)
endif()
if(RUVIA_ENABLE_POSTGRESQL)
    target_sources(ruvia_unit_tests PRIVATE web/unit_db_postgresql.cpp)
endif()

if(RUVIA_ENABLE_POSTGRESQL_INTEGRATION_TESTS)
    ruvia_add_test(ruvia_postgresql_integration server/postgresql_integration.cpp
        TIMEOUT 60 SERIAL LABELS "database;postgresql;integration")
endif()

target_link_libraries(ruvia_unit_tests PRIVATE ruvia::web)
target_include_directories(ruvia_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)

if(MSVC)
    target_compile_options(ruvia_unit_tests PRIVATE /bigobj /FS)
endif()

add_test(NAME ruvia_unit_tests COMMAND ruvia_unit_tests)
set_tests_properties(ruvia_unit_tests PROPERTIES TIMEOUT 180)

if(RUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS)
    add_executable(ruvia_http2_conformance_server server/http2_conformance_server.cpp)
    target_link_libraries(ruvia_http2_conformance_server PRIVATE ruvia::web)
    if(MSVC)
        target_compile_options(ruvia_http2_conformance_server PRIVATE /bigobj /FS)
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME ruvia_http2_conformance
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/conformance/http2_wire_conformance.py
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
        ${CMAKE_CURRENT_SOURCE_DIR}/verify_http_status_literals.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/verify_http_status_literals.cmake
        @ONLY)
    add_test(
        NAME ruvia_guard_http_status_literals
        COMMAND ${CMAKE_COMMAND}
            -P ${CMAKE_CURRENT_BINARY_DIR}/verify_http_status_literals.cmake)

    ruvia_add_test(ruvia_smoke_http_target server/smoke_http_target.cpp LIB ruvia::http)

    # Compile the exhaustive public/detail HTTP contract once in the normal build.
    # MSVC gets a focused regression guard for private parser construction: the
    # complete concept matrix is disproportionately expensive in cl.exe and is
    # already compiler-checked by the regular unit sources plus non-MSVC CI.
    # The isolated package matrix below uses a small link smoke source on every host.
    if(MSVC)
        set(RUVIA_HTTP_API_CONTRACT_SOURCE guards/http_msvc_api_contract.cpp)
    else()
        set(RUVIA_HTTP_API_CONTRACT_SOURCE package-consumer/http.cpp)
    endif()
    ruvia_add_test(ruvia_guard_http_api_contract ${RUVIA_HTTP_API_CONTRACT_SOURCE} LIB ruvia::http)
endif()

if(RUVIA_BUILD_WEB)
ruvia_add_test(ruvia_guard_middleware_next guards/middleware_next_guard.cpp)
ruvia_add_test(ruvia_guard_http2_stream_table guards/http2_stream_table_guard.cpp)
ruvia_add_test(ruvia_guard_context_response_state guards/context_response_state_guard.cpp)
ruvia_add_test(ruvia_guard_context_request_header guards/context_request_header_guard.cpp)
ruvia_add_test(ruvia_guard_cookie_api guards/cookie_api_guard.cpp)
ruvia_add_test(ruvia_guard_app_lifecycle guards/app_lifecycle_guard.cpp)
ruvia_add_test(ruvia_guard_http2_session_lifecycle guards/http2_session_lifecycle_guard.cpp)

ruvia_add_test(ruvia_server_shutdown_drain server/server_shutdown_drain.cpp TIMEOUT 90)
ruvia_add_test(ruvia_server_max_connections server/server_max_connections.cpp TIMEOUT 60)
ruvia_add_test(ruvia_server_notfound_keepalive server/server_notfound_keepalive.cpp TIMEOUT 60)
ruvia_add_test(ruvia_server_http2_document_root server/server_http2_document_root.cpp TIMEOUT 60)
ruvia_add_test(ruvia_server_head_document_root server/server_head_document_root.cpp TIMEOUT 60)
ruvia_add_test(ruvia_server_head_stream_route server/server_head_stream_route.cpp TIMEOUT 60)
ruvia_add_test(ruvia_server_worker_state server/server_worker_state.cpp TIMEOUT 60)
ruvia_add_test(ruvia_server_http2_head_stream_route server/server_http2_head_stream_route.cpp TIMEOUT 60)
ruvia_add_test(ruvia_server_pipeline_request_views server/server_pipeline_request_views.cpp TIMEOUT 60)
ruvia_add_test(ruvia_server_tls_keepalive_deadline server/server_tls_keepalive_deadline.cpp TIMEOUT 60)
ruvia_add_test(ruvia_server_keepalive_idle_timeout server/server_keepalive_idle_timeout.cpp TIMEOUT 60)
ruvia_add_test(ruvia_web_worker server/web_worker.cpp TIMEOUT 30)
ruvia_add_test(ruvia_app_web_worker server/app_web_worker.cpp TIMEOUT 30)
ruvia_add_test(ruvia_app_startup_retry server/app_startup_retry.cpp TIMEOUT 30)
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/verify_package_consumers.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/verify_package_consumers.cmake
    @ONLY)
# Opt-in for the same reason as the layer-boundary check: the packaging
# contract pins installed file names and literal signatures, which makes it
# brittle across refactors. Run on demand with:
#   cmake --build <build-dir> --target ruvia_package_consumers
add_custom_target(ruvia_package_consumers
    COMMAND ${CMAKE_COMMAND}
        -DRUVIA_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_BINARY_DIR}/verify_package_consumers.cmake
    COMMENT "Verifying installed Ruvia package consumers"
    VERBATIM)
