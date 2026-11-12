ruvia_collect_cpp_sources(ruvia_http_sources src
    "${_ruvia_disabled_protocol_regex}")
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

ruvia_install_public_header_closure(http include/ruvia/http
    "${_ruvia_disabled_protocol_regex}")

