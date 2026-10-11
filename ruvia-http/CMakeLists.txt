ruvia_collect_cpp_sources(ruvia_http_sources src)
add_library(ruvia-http STATIC ${ruvia_http_sources})

if(WIN32)
    # ngtcp2.h pulls in the Windows socket headers.
    target_compile_definitions(ruvia-http PRIVATE NOMINMAX)
endif()

ruvia_configure_library(ruvia-http http)

target_link_libraries(ruvia-http
    PRIVATE
        # Content/transfer codings and the QUIC transport state machine. No
        # OpenSSL: the sans-I/O protocol library does no TLS (Web supplies it)
        # and the WebSocket accept key's SHA-1 is implemented in-tree.
        ${RUVIA_HTTP_DEPENDENCIES}
)

ruvia_install_target(ruvia-http http)

ruvia_install_public_header_closure(http include/ruvia/http)

ruvia_install_export(http)
