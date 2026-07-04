add_executable(ruvia_unit_tests
    test_main.cpp
    unit_encoding.cpp
    unit_json.cpp
    unit_cookie_signature.cpp
    unit_cookie_validation.cpp
    unit_http_parsing.cpp
    unit_multipart.cpp
    unit_content_negotiation.cpp
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
    unit_response_head_buffer.cpp
    unit_pmr_string.cpp
    unit_http2_offset_vector.cpp
    unit_http2_stream_lifecycle.cpp
    unit_http2_header_rules.cpp
    unit_http2_continuation_queue.cpp
    unit_response_trailers.cpp
    unit_http2_frame_payload.cpp
    unit_static_response_headers.cpp
    unit_websocket_utils.cpp
    unit_hpack_static_table.cpp
    unit_pool_waiter_queue.cpp
    unit_http2_header_list.cpp
    unit_response_head_policy.cpp
    unit_http2_request_builder.cpp
    unit_request_access.cpp
    unit_http2_body_state.cpp
    unit_http2_frame_codec.cpp
    unit_http2_peer_settings.cpp
    unit_route_resolution.cpp
    unit_http2_session_results.cpp
    unit_connection_read_buffer.cpp
    unit_http2_stream_routing.cpp
    unit_http2_stream_request_data.cpp
    unit_http2_request_headers.cpp
    unit_http1_parser.cpp
    unit_response_header_access.cpp
    unit_date_cache.cpp
    unit_error.cpp
    unit_http_response.cpp
    unit_parser_validation.cpp
    unit_http2_flow_control.cpp
    unit_http_server_request_state.cpp
    unit_http_client_response.cpp
    unit_http_client_fetch.cpp
    unit_http2_client.cpp
    unit_routing.cpp
    unit_cors.cpp
)

target_link_libraries(ruvia_unit_tests PRIVATE ruvia::ruvia)

# Unit tests reach into internal detail headers under src/ (e.g. core/Base64.h,
# http/CookieSignature.h) in addition to the public include/ surface.
target_include_directories(ruvia_unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(ruvia_unit_tests PRIVATE /bigobj /FS)
endif()

add_test(NAME ruvia_unit_tests COMMAND ruvia_unit_tests)
