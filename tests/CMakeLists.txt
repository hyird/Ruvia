add_executable(ruvia_unit_tests
    test_main.cpp
    unit_encoding.cpp
    unit_json.cpp
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
    unit_http2_stream_flow.cpp
    unit_http2_payload.cpp
    unit_http2_closed_streams.cpp
    unit_http2_stream_table.cpp
    unit_http2_upgrade.cpp
    unit_http2_body_accounting.cpp
    unit_http2_ready_queue.cpp
    unit_http2_body_queue.cpp
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
    unit_http1_connection.cpp
    unit_sansio_tls.cpp
    unit_http2_connection.cpp
    unit_ws_connection.cpp
    unit_sansio_driver.cpp
    unit_static_response_headers.cpp
    unit_streaming.cpp
    unit_response_stream_body.cpp
    unit_http_cache.cpp
    unit_vary_headers.cpp
    unit_content_range.cpp
    unit_static_path_normalization.cpp
    unit_websocket_utils.cpp
    unit_hpack_static_table.cpp
    unit_pool_waiter_queue.cpp
    unit_http2_header_list.cpp
    unit_response_head_policy.cpp
    unit_http2_request_builder.cpp
    unit_request_access.cpp
    unit_context_cookie.cpp
    unit_context_redirect.cpp
    unit_http2_body_state.cpp
    unit_http2_frame_codec.cpp
    unit_http2_peer_settings.cpp
    unit_route_resolution.cpp
    unit_connection_read_buffer.cpp
    unit_http2_stream_body_policy.cpp
    unit_http2_stream_request_data.cpp
    unit_http2_request_headers.cpp
    unit_http1_parser.cpp
    unit_response_header_access.cpp
    unit_date_cache.cpp
    unit_error.cpp
    unit_error_response.cpp
    unit_validator.cpp
    unit_http_response.cpp
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
    unit_http_client_response.cpp
    unit_http_client_fetch.cpp
    unit_http_client_config.cpp
    unit_http_client_redirect.cpp
    unit_http2_client.cpp
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

# The DB SQL-interpolation suite links interpolateSql (and libmariadb's escaper).
if(RUVIA_ENABLE_MARIADB)
    target_sources(ruvia_unit_tests PRIVATE
        unit_db_api_surface.cpp
        unit_db_sql.cpp
    )
endif()

target_link_libraries(ruvia_unit_tests PRIVATE ruvia::web)

add_executable(ruvia_smoke_core_target smoke_core_target.cpp)
target_link_libraries(ruvia_smoke_core_target PRIVATE ruvia::core)
add_test(NAME ruvia_smoke_core_target COMMAND ruvia_smoke_core_target)

add_executable(ruvia_smoke_http_target smoke_http_target.cpp)
target_link_libraries(ruvia_smoke_http_target PRIVATE ruvia::http)
add_test(NAME ruvia_smoke_http_target COMMAND ruvia_smoke_http_target)

# Unit tests reach into target-owned internal detail headers in addition to
# public include surfaces.
target_include_directories(ruvia_unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/ruvia-core/src
    ${CMAKE_SOURCE_DIR}/ruvia-core/include
    ${CMAKE_SOURCE_DIR}/ruvia-web/include
    ${CMAKE_SOURCE_DIR}/ruvia-http/src
    ${CMAKE_SOURCE_DIR}/ruvia-http/src/net
    ${CMAKE_SOURCE_DIR}/ruvia-http/include
    ${CMAKE_SOURCE_DIR}/ruvia-web/src
    ${CMAKE_SOURCE_DIR}/ruvia-web/src/http
)

if(MSVC)
    target_compile_options(ruvia_unit_tests PRIVATE /bigobj /FS)
endif()

add_test(NAME ruvia_unit_tests COMMAND ruvia_unit_tests)

# Mechanised layer-boundary checks (asio-free http, no framework reach-down,
# docs ownership). Runs on every test build so a violation fails fast;
# rules: README.md and AGENTS.md
if(UNIX)
    add_custom_target(ruvia_check_boundaries ALL
        COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/check_layer_boundaries.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking Ruvia layer boundaries"
        VERBATIM)
    add_test(NAME ruvia_check_boundaries
        COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/check_layer_boundaries.sh)
    add_test(NAME ruvia_check_boundaries_self_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/check_layer_boundaries.sh --self-test)
endif()
