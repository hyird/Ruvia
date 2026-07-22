# TLS on both faces (client termination and https origins) uses asio::ssl.
find_package(OpenSSL REQUIRED)

set(RUVIA_EDGE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OriginFetcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/DiskCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/DiskCacheFiles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/DiskCacheRecord.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/DiskTier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/EdgeCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/Freshness.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http1/Http1Wire.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2ResponseWriter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/ByteRange.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/ForwardHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/HeaderRules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/RangeResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/RequestDirectives.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/EdgeConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/Serve.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/Server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/Session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/TlsContext.cpp
)

add_library(ruvia-edge ${RUVIA_EDGE_SOURCES})
add_library(ruvia::edge ALIAS ruvia-edge)

set_target_properties(ruvia-edge PROPERTIES
    EXPORT_NAME edge
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

ruvia_configure_library(ruvia-edge)
ruvia_configure_runtime_library(ruvia-edge)

target_compile_definitions(ruvia-edge PUBLIC RUVIA_ENABLE_EDGE)

# The edge node is a caching reverse proxy running its own event loop: ruvia-core
# supplies the async/socket primitives (Task, asyncAsio) that drive both the
# client-facing accept loop and the origin connections on one io_context, and
# ruvia-http supplies the sans-I/O HTTP/1 request parser (client side) and
# request writer / response parser (origin side). It deliberately does not depend
# on ruvia-web: the app framework hides the worker executor an edge needs to open
# outbound origin sockets, so the edge owns its own loop instead.
target_link_libraries(ruvia-edge
    PUBLIC
        ruvia::core
        ruvia::http
        OpenSSL::SSL
        OpenSSL::Crypto
)

ruvia_install_target(ruvia-edge edge)
ruvia_install_public_header_tree(edge include/ruvia/edge)
ruvia_install_export(edge)
ruvia_assert_target_file_ownership(ruvia-edge)
