ruvia_fetch_http_dependencies()

ruvia_collect_cpp_sources(RUVIA_HTTP_SOURCES src)
add_library(ruvia-http ${RUVIA_HTTP_SOURCES})

if(WIN32)
    target_compile_definitions(ruvia-http PRIVATE NOMINMAX)
endif()

ruvia_configure_library(ruvia-http http)

target_link_libraries(ruvia-http
    PRIVATE
        # Content/transfer coding only (gzip/deflate/brotli/zstd). NO OpenSSL: the pure
        # protocol library does no TLS -- client transports are supplied externally and
        # the server driver lives in web -- while WebSocket Sec-WebSocket-Accept SHA-1 is hand-rolled. Every
        # http consumer would otherwise pull OpenSSL in for nothing.
        ruvia_dependencies_http
)

ruvia_install_target(ruvia-http http)

ruvia_install_public_header_closure(http include/ruvia/http)

ruvia_install_export(http)
