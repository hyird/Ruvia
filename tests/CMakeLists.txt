if(RUVIA_BUILD_WEB)
add_executable(ruvia_unit_tests
    test_main.cpp
    unit_encoding.cpp
    unit_json.cpp
    unit_model_list.cpp
    unit_model_string.cpp
    unit_cookie_signature.cpp
    unit_cookie_validation.cpp
    unit_http_parsing.cpp
    unit_multipart.cpp
    unit_multipart_reader.cpp
    unit_content_negotiation.cpp
    unit_accept_media_type.cpp
    unit_form_parser.cpp
    unit_rate_limiter.cpp
    unit_chunk_size.cpp
    unit_conditional_request.cpp
    unit_websocket_deflate.cpp
    unit_request_body_decoding.cpp
    unit_http_stream_body_reader.cpp
    unit_request_target.cpp
    unit_websocket_accept.cpp
    unit_websocket_validation.cpp
    unit_websocket_frame.cpp
    unit_chunk_decoder.cpp
    unit_csrf.cpp
    unit_session.cpp
    unit_header_classification.cpp
    unit_header_block_parser.cpp
    unit_response_header_classification.cpp
    unit_http_basics.cpp
    unit_json_object_fields.cpp
    unit_response_header_index_cache.cpp
    unit_file_response_helpers.cpp
    unit_json_writer.cpp
    unit_hpack.cpp
    unit_http2_frame.cpp
    unit_http2_settings.cpp
    unit_http2_local_settings.cpp
    unit_http2_stream_flow.cpp
    unit_http2_payload.cpp
    unit_http2_closed_streams.cpp
    unit_http2_stream_table.cpp
    unit_http2_remote_content_state.cpp
    unit_http2_local_content_state.cpp
    unit_http2_connect.cpp
    unit_http2_ready_queue.cpp
    unit_http2_sansio_stream_runtime.cpp
    unit_websocket_assembler.cpp
    unit_websocket_heartbeat.cpp
    unit_websocket_close.cpp
    unit_config_validation.cpp
    unit_server_options_validation.cpp
    unit_response_head_buffer.cpp
    unit_pmr_string.cpp
    unit_http2_offset_vector.cpp
    unit_http2_stream_lifecycle.cpp
    unit_http2_header_rules.cpp
    unit_response_trailers.cpp
    unit_http2_frame_payload.cpp
    unit_sansio_tls.cpp
    unit_http2_connection.cpp
    unit_ws_connection.cpp
    unit_websocket_connection.cpp
    unit_sansio_driver.cpp
    unit_static_response_headers.cpp
    unit_streaming.cpp
    unit_response_stream_dispatch.cpp
    unit_http2_buffered_response_write.cpp
    unit_http_cache.cpp
    unit_vary_headers.cpp
    unit_http_byte_range.cpp
    unit_content_range.cpp
    unit_static_path_normalization.cpp
    unit_websocket_utils.cpp
    unit_hpack_static_table.cpp
    unit_pool_waiter_queue.cpp
    unit_http2_header_list.cpp
    unit_response_head_policy.cpp
    unit_final_response_control.cpp
    unit_http_response_content_semantics.cpp
    unit_http2_request_builder.cpp
    unit_request_access.cpp
    unit_context_cookie.cpp
    unit_conn_info.cpp
    unit_access_log.cpp
    unit_context_redirect.cpp
    unit_context_capabilities.cpp
    unit_http2_frame_codec.cpp
    unit_http2_output_buffer.cpp
    unit_http2_peer_settings.cpp
    unit_route_resolution.cpp
    unit_connection_read_buffer.cpp
    unit_http2_stream_request_data.cpp
    unit_http2_request_headers.cpp
    unit_http1_parser.cpp
    unit_http1_chunked_framing.cpp
    unit_http1_buffered_response_write.cpp
    unit_response_header_access.cpp
    unit_date_cache.cpp
    unit_error.cpp
    unit_error_response.cpp
    unit_health.cpp
    unit_validator.cpp
    unit_http_response.cpp
    unit_http_response_body.cpp
    unit_http1_interim_response.cpp
    unit_parser_validation.cpp
    unit_http2_flow_control.cpp
    unit_http2_server_flow_control.cpp
    unit_http_server_request_state.cpp
    unit_http2_classify.cpp
    unit_http_char_syntax.cpp
    unit_pattern_compiler.cpp
    unit_pattern_matcher.cpp
    unit_json_lex.cpp
    unit_content_type_match.cpp
    unit_email_like.cpp
    unit_pattern_types.cpp
    unit_header_tokens.cpp
    unit_header_params.cpp
    unit_json_escape.cpp
    unit_json_size_hint.cpp
    unit_url_encoding_detect.cpp
    unit_rate_limit_decision.cpp
    unit_response_compression.cpp
    unit_response_head_emit.cpp
    unit_security_headers.cpp
    unit_websocket_handshake.cpp
    unit_dotenv_parser.cpp
    unit_http2_header_block.cpp
    unit_http2_payload_slice.cpp
    unit_http2_response_headers.cpp
    unit_http2_websocket_handshake.cpp
    unit_http_client_request.cpp
    unit_http_client_response.cpp
    unit_http_origin.cpp
    unit_http_client_redirect.cpp
    unit_routing.cpp
    unit_cors.cpp
)

# The JWT suite links jwtSign/jwtVerify, which only exist when the optional JWT
# module is compiled in, so gate it on the same flag.
if(RUVIA_ENABLE_JWT)
    target_sources(ruvia_unit_tests PRIVATE unit_jwt.cpp)
endif()

# The Redis RESP-protocol suite links appendRespCommand, gated the same way.
if(RUVIA_ENABLE_REDIS)
    target_sources(ruvia_unit_tests PRIVATE
        unit_redis_api_surface.cpp
        unit_redis_protocol.cpp
    )
endif()

# The backend-neutral API suite is present for either database driver.
if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    target_sources(ruvia_unit_tests PRIVATE unit_db_api_surface.cpp)
endif()

# Driver-specific parameter encoding is tested against the same library that
# performs it; neither suite pulls the other backend into a single-driver build.
if(RUVIA_ENABLE_MARIADB)
    target_sources(ruvia_unit_tests PRIVATE unit_db_sql.cpp)
endif()
if(RUVIA_ENABLE_POSTGRESQL)
    target_sources(ruvia_unit_tests PRIVATE unit_db_postgresql.cpp)
endif()

if(RUVIA_ENABLE_POSTGRESQL_INTEGRATION_TESTS)
    add_executable(ruvia_postgresql_integration postgresql_integration.cpp)
    target_link_libraries(ruvia_postgresql_integration PRIVATE ruvia::web)
    if(MSVC)
        target_compile_options(ruvia_postgresql_integration PRIVATE /bigobj /FS)
    endif()
    add_test(NAME ruvia_postgresql_integration COMMAND ruvia_postgresql_integration)
    set_tests_properties(ruvia_postgresql_integration PROPERTIES
        LABELS "database;postgresql;integration"
        RUN_SERIAL TRUE
        TIMEOUT 60)
endif()

target_link_libraries(ruvia_unit_tests PRIVATE ruvia::web)

if(MSVC)
    target_compile_options(ruvia_unit_tests PRIVATE /bigobj /FS)
endif()

add_test(NAME ruvia_unit_tests COMMAND ruvia_unit_tests)
set_tests_properties(ruvia_unit_tests PROPERTIES TIMEOUT 180)

if(RUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS)
    add_executable(ruvia_http2_conformance_server http2_conformance_server.cpp)
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
    add_executable(ruvia_smoke_core_target smoke_core_target.cpp)
    target_link_libraries(ruvia_smoke_core_target PRIVATE ruvia::core)
    add_test(NAME ruvia_smoke_core_target COMMAND ruvia_smoke_core_target)

    add_executable(ruvia_connection_scanner_lifetime connection_scanner_lifetime.cpp)
    target_link_libraries(ruvia_connection_scanner_lifetime PRIVATE ruvia::core)
    add_test(NAME ruvia_connection_scanner_lifetime COMMAND ruvia_connection_scanner_lifetime)

    add_executable(ruvia_runtime_worker runtime_worker.cpp)
    target_link_libraries(ruvia_runtime_worker PRIVATE ruvia::core)
    add_test(NAME ruvia_runtime_worker COMMAND ruvia_runtime_worker)

    add_executable(ruvia_task_scope task_scope.cpp)
    target_link_libraries(ruvia_task_scope PRIVATE ruvia::core)
    add_test(NAME ruvia_task_scope COMMAND ruvia_task_scope)

    add_executable(ruvia_channel channel.cpp)
    target_link_libraries(ruvia_channel PRIVATE ruvia::core)
    add_test(NAME ruvia_channel COMMAND ruvia_channel)

    add_executable(ruvia_one_shot one_shot.cpp)
    target_link_libraries(ruvia_one_shot PRIVATE ruvia::core)
    add_test(NAME ruvia_one_shot COMMAND ruvia_one_shot)

    add_executable(ruvia_worker_timer worker_timer.cpp)
    target_link_libraries(ruvia_worker_timer PRIVATE ruvia::core)
    add_test(NAME ruvia_worker_timer COMMAND ruvia_worker_timer)

endif()

if(RUVIA_BUILD_HTTP)
    add_executable(ruvia_smoke_http_target smoke_http_target.cpp)
    target_link_libraries(ruvia_smoke_http_target PRIVATE ruvia::http)
    add_test(NAME ruvia_smoke_http_target COMMAND ruvia_smoke_http_target)

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
    add_executable(ruvia_guard_http_api_contract ${RUVIA_HTTP_API_CONTRACT_SOURCE})
    target_link_libraries(ruvia_guard_http_api_contract PRIVATE ruvia::http)
    if(MSVC)
        target_compile_options(ruvia_guard_http_api_contract PRIVATE /bigobj /FS)
    endif()
    add_test(NAME ruvia_guard_http_api_contract COMMAND ruvia_guard_http_api_contract)
endif()

if(RUVIA_BUILD_WEB)
function(ruvia_add_internal_guard name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ruvia::web)
    if(MSVC)
        target_compile_options(${name} PRIVATE /bigobj /FS)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ruvia_add_internal_guard(ruvia_guard_middleware_next guards/middleware_next_guard.cpp)
ruvia_add_internal_guard(ruvia_guard_http2_stream_table guards/http2_stream_table_guard.cpp)
ruvia_add_internal_guard(ruvia_guard_context_response_state guards/context_response_state_guard.cpp)
ruvia_add_internal_guard(ruvia_guard_context_request_header guards/context_request_header_guard.cpp)
ruvia_add_internal_guard(ruvia_guard_cookie_api guards/cookie_api_guard.cpp)

add_executable(ruvia_server_shutdown_drain server_shutdown_drain.cpp)
target_link_libraries(ruvia_server_shutdown_drain PRIVATE ruvia::web)
add_test(NAME ruvia_server_shutdown_drain COMMAND ruvia_server_shutdown_drain)
set_tests_properties(ruvia_server_shutdown_drain PROPERTIES TIMEOUT 90)

add_executable(ruvia_web_worker web_worker.cpp)
target_link_libraries(ruvia_web_worker PRIVATE ruvia::web)
add_test(NAME ruvia_web_worker COMMAND ruvia_web_worker)
set_tests_properties(ruvia_web_worker PROPERTIES TIMEOUT 30)

add_executable(ruvia_app_web_worker app_web_worker.cpp)
target_link_libraries(ruvia_app_web_worker PRIVATE ruvia::web)
add_test(NAME ruvia_app_web_worker COMMAND ruvia_app_web_worker)
set_tests_properties(ruvia_app_web_worker PROPERTIES TIMEOUT 30)
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

# Mechanised layer-boundary checks (asio-free http, no framework reach-down,
# docs ownership). The production check gates CTest; its fixture self-test
# remains available as an explicit maintenance target.
set(RUVIA_BOUNDARY_CHECK_SCRIPT
    ${CMAKE_SOURCE_DIR}/scripts/check_layer_boundaries.cmake)
add_test(
    NAME ruvia_layer_boundaries
    COMMAND ${CMAKE_COMMAND} -P ${RUVIA_BOUNDARY_CHECK_SCRIPT})
set_tests_properties(
    ruvia_layer_boundaries
    PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_custom_target(ruvia_check_boundaries
    COMMAND ${CMAKE_COMMAND} -P ${RUVIA_BOUNDARY_CHECK_SCRIPT}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking Ruvia layer boundaries"
    VERBATIM)
add_custom_target(ruvia_check_boundaries_self_test
    COMMAND ${CMAKE_COMMAND}
        -DRUVIA_BOUNDARY_SELF_TEST=ON
        -P ${RUVIA_BOUNDARY_CHECK_SCRIPT}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Self-testing the Ruvia layer-boundary rules"
    VERBATIM)
