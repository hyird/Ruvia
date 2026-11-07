ruvia_collect_cpp_sources(ruvia_http_sources src)
if(NOT RUVIA_ENABLE_HTTP2)
    list(FILTER ruvia_http_sources EXCLUDE REGEX "/src/http2/")
endif()
if(NOT RUVIA_ENABLE_HTTP3)
    list(FILTER ruvia_http_sources EXCLUDE REGEX "/src/http3/")
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

ruvia_install_target(ruvia-http http)

set(_ruvia_http_excluded_headers)
if(NOT RUVIA_ENABLE_HTTP2)
    list(APPEND _ruvia_http_excluded_headers
        include/ruvia/http/hpack.h
        include/ruvia/http/http2_connection.h
        include/ruvia/http/http2_event.h
        include/ruvia/http/http2_framing.h
        include/ruvia/http/http2_request_content.h
        include/ruvia/http/http2_types.h)
endif()
if(NOT RUVIA_ENABLE_HTTP3)
    list(APPEND _ruvia_http_excluded_headers
        include/ruvia/http/http3_connection.h
        include/ruvia/http/http3_quic_connection.h
        include/ruvia/http/http3_request_head.h)
endif()

ruvia_install_public_header_closure(http include/ruvia/http
    EXCLUDE_FILES ${_ruvia_http_excluded_headers})

ruvia_install_export(http)
