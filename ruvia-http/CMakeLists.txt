find_package(ZLIB REQUIRED)
find_package(unofficial-brotli CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

if(TARGET zstd::libzstd)
    set(RUVIA_ZSTD_TARGET zstd::libzstd)
elseif(TARGET zstd::libzstd_static)
    set(RUVIA_ZSTD_TARGET zstd::libzstd_static)
else()
    set(RUVIA_ZSTD_TARGET zstd::libzstd_shared)
endif()

ruvia_collect_cpp_sources(RUVIA_HTTP_SOURCES src)
add_library(ruvia-http ${RUVIA_HTTP_SOURCES}
    include/ruvia/http/Http3ClientRequestHead.h
    include/ruvia/http/Http3ClientResponse.h
    include/ruvia/http/Http3Connection.h
    include/ruvia/http/Http3ConnectionError.h
    include/ruvia/http/Http3ControlStream.h
    include/ruvia/http/Http3DataWritePlan.h
    include/ruvia/http/Http3FieldSection.h
    include/ruvia/http/Http3Frames.h
    include/ruvia/http/Http3LocalCriticalStreams.h
    include/ruvia/http/Http3MessageBody.h
    include/ruvia/http/Http3MessageHead.h
    include/ruvia/http/Http3PeerStreams.h
    include/ruvia/http/Http3QpackConnection.h
    include/ruvia/http/Http3RequestWriter.h
    include/ruvia/http/Http3Qpack.h
    include/ruvia/http/Http3QpackStreams.h
    include/ruvia/http/Http3ResponseWriter.h
    include/ruvia/http/Http3ServerRequest.h
    include/ruvia/http/Http3ServerRequestAdmission.h
    include/ruvia/http/Http3Settings.h
    include/ruvia/http/Http3StreamFrames.h
    include/ruvia/http/Http3WebSocketHandshake.h
    include/ruvia/http/Http3VarInt.h
    include/ruvia/http/HttpRepresentationResponsePlan.h)
add_library(ruvia::http ALIAS ruvia-http)

set_target_properties(ruvia-http PROPERTIES
    EXPORT_NAME http
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

ruvia_configure_library(ruvia-http)

target_link_libraries(ruvia-http
    PUBLIC
        # Content/transfer coding only (gzip/deflate/brotli/zstd). NO OpenSSL: the pure
        # protocol library does no TLS -- client transports are supplied externally and
        # the server driver lives in web -- while WebSocket Sec-WebSocket-Accept SHA-1 is hand-rolled. Every
        # http consumer would otherwise pull OpenSSL in for nothing.
        ZLIB::ZLIB
        unofficial::brotli::brotlienc
        unofficial::brotli::brotlidec
        unofficial::brotli::brotlicommon
        ${RUVIA_ZSTD_TARGET}
)

ruvia_install_target(ruvia-http http)

ruvia_install_public_header_closure(http include/ruvia/http)

ruvia_install_export(http)
