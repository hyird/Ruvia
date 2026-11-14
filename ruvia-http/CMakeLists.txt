set(ruvia_http_sources
    src/http1_chunked_body.cpp
    src/http1_client_request.cpp
    src/http1_client_response.cpp
    src/http1_server_request.cpp
    src/http1_server_response.cpp
    src/http_cache.cpp
    src/http_c_library_memory.cpp
    src/http_client_ip.cpp
    src/http_client_redirect.cpp
    src/http_connect_udp.cpp
    src/http_connection_advertisement.cpp
    src/http_content_coding.cpp
    src/http_content_negotiation.cpp
    src/http_cookie.cpp
    src/http_cors.cpp
    src/http_date.cpp
    src/http_header.cpp
    src/http_io_buffer.cpp
    src/http_message.cpp
    src/http_priority.cpp
    src/http_protocol.cpp
    src/http_range.cpp
    src/http_request.cpp
    src/http_request_target.cpp
    src/http_representation.cpp
    src/http_response.cpp
    src/http_response_headers.cpp
    src/http_transfer_coding.cpp
    src/http_uri.cpp
    src/ip_address.cpp
    src/multipart_parser.cpp
    src/proxy_protocol.cpp
    src/server_sent_events.cpp
    src/tls_client_hello.cpp
    src/websocket_connection.cpp
    src/websocket_handshake.cpp
)
if(RUVIA_ENABLE_HTTP2)
    list(APPEND ruvia_http_sources
        src/http2_connection.cpp
        src/http2_hpack.cpp
        src/http2_stream.cpp
    )
endif()
if(RUVIA_ENABLE_HTTP3)
    list(APPEND ruvia_http_sources
        src/http3_connection.cpp
        src/http3_quic_connection.cpp
    )
endif()
add_library(ruvia-http STATIC ${ruvia_http_sources})

ruvia_configure_library(ruvia-http http)

target_link_libraries(ruvia-http
    PRIVATE
        # Content/transfer codings. No
        # OpenSSL: the sans-I/O protocol library does no TLS (Web supplies it)
        # and the WebSocket accept key's SHA-1 is implemented in-tree.
        ${RUVIA_HTTP_DEPENDENCIES}
)
if(RUVIA_ENABLE_HTTP2)
    target_compile_definitions(ruvia-http PUBLIC RUVIA_ENABLE_HTTP2)
endif()
if(RUVIA_ENABLE_HTTP3)
    target_compile_definitions(ruvia-http PUBLIC RUVIA_ENABLE_HTTP3)
    # The low-level QUIC facade deliberately exposes ngtcp2 core types while
    # keeping the OpenSSL crypto adapter in ruvia-web.
    target_link_libraries(ruvia-http PUBLIC ngtcp2::core)
endif()

ruvia_install_component(ruvia-http http)

set(ruvia_http_headers
    include/borrowed_text.h
    include/http1_chunked_body.h
    include/http1_client_request.h
    include/http1_client_response.h
    include/http1_server_request.h
    include/http1_server_response.h
    include/http2_push.h
    include/http_cache.h
    include/http_client_ip.h
    include/http_client_redirect.h
    include/http_connect_udp.h
    include/http_connection_advertisement.h
    include/http_content_coding.h
    include/http_content_negotiation.h
    include/http_cookie.h
    include/http_cors.h
    include/http_date.h
    include/http_header.h
    include/http_message.h
    include/http_multipart.h
    include/http_priority.h
    include/http_protocol.h
    include/http_range.h
    include/http_representation.h
    include/http_request.h
    include/http_response.h
    include/http_response_body.h
    include/http_response_headers.h
    include/http_server_sent_events.h
    include/http_status.h
    include/http_transfer_coding.h
    include/proxy_protocol.h
    include/tls_client_hello.h
    include/websocket_connection.h
    include/websocket_handshake.h
)
if(RUVIA_ENABLE_HTTP2)
    list(APPEND ruvia_http_headers include/http2_connection.h)
endif()
if(RUVIA_ENABLE_HTTP3)
    list(APPEND ruvia_http_headers
        include/http3_connection.h
        include/http3_quic_connection.h
    )
endif()
ruvia_install_public_headers(http ${ruvia_http_headers})

