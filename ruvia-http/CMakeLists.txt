set(ruvia_http_sources
    src/http1_server_request_parser.cpp
    src/http_content_coding.cpp
    src/http_field_kind.cpp
    src/http_request.cpp
    src/multipart_parser.cpp
    src/proxy_protocol.cpp
)
if(RUVIA_ENABLE_HTTP2)
    list(APPEND ruvia_http_sources src/http2_connection.cpp)
endif()
if(RUVIA_ENABLE_HTTP3)
    list(APPEND ruvia_http_sources src/http3_connection.cpp)
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
    include/ruvia/http/borrowed_text.h
    include/ruvia/http/http_cache.h
    include/ruvia/http/http_client.h
    include/ruvia/http/http_content_coding.h
    include/ruvia/http/http_header.h
    include/ruvia/http/http_request.h
    include/ruvia/http/http_response.h
    include/ruvia/http/http_response_server.h
    include/ruvia/http/websocket_handshake.h
)
if(RUVIA_ENABLE_HTTP2)
    list(APPEND ruvia_http_headers include/ruvia/http/http2_connection.h)
endif()
if(RUVIA_ENABLE_HTTP3)
    list(APPEND ruvia_http_headers include/ruvia/http/http3_connection.h)
endif()
ruvia_install_public_headers(http ${ruvia_http_headers})

