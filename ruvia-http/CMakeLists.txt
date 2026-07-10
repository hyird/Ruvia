add_library(ruvia-http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Error.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpBasics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpContentCoding.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpRequest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpResponseHeaderOps.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpResponseHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpResponseHeadersStorage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpResponseKnownHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MultipartReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Streaming.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/body/HttpTransferCodingDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpChunkParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpHeaderBlockParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpParserErrors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpParserSyntax.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpRequestTarget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpDateCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpResponseHead.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpResponseHeadBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2Connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2Hpack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2HpackDynamic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2HpackEncoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2HpackHuffman.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/HttpWebSocketAccept.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/HttpWebSocketUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/HttpWebSocketValidation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/WsConnection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClientResponseParser.cpp
)
add_library(ruvia::http ALIAS ruvia-http)

set_target_properties(ruvia-http PROPERTIES
    EXPORT_NAME http
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

ruvia_configure_library(ruvia-http)

target_link_libraries(ruvia-http
    PUBLIC
        # Content/transfer coding only (gzip/deflate/brotli/zstd). NO OpenSSL: the pure
        # protocol library does no TLS -- that moved to the web-layer client/server
        # drivers -- and the WebSocket Sec-WebSocket-Accept SHA-1 is hand-rolled. Every
        # http consumer would otherwise pull OpenSSL in for nothing.
        ZLIB::ZLIB
        unofficial::brotli::brotlienc
        unofficial::brotli::brotlidec
        unofficial::brotli::brotlicommon
        ${RUVIA_ZSTD_TARGET}
)

ruvia_install_target(ruvia-http http)

ruvia_install_public_headers(http
    include/ruvia/http/Cookies.h
    include/ruvia/http/Error.h
    include/ruvia/http/HttpBodyStream.h
    include/ruvia/http/HttpCache.h
    include/ruvia/http/HttpClient.h
    include/ruvia/http/HttpClientTypes.h
    include/ruvia/http/HttpCommon.h
    include/ruvia/http/HttpLimits.h
    include/ruvia/http/HttpParseTypes.h
    include/ruvia/http/HttpParser.h
    include/ruvia/http/HttpRequest.h
    include/ruvia/http/HttpResponse.h
    include/ruvia/http/HttpStatus.h
    include/ruvia/http/HttpTypes.h
    include/ruvia/http/JsonUtils.h
    include/ruvia/http/MultipartParser.h
    include/ruvia/http/Sse.h
    include/ruvia/http/UrlEncoding.h
    include/ruvia/http/WebSocketProtocol.h
)

ruvia_install_public_header_tree(http include/ruvia/http/detail)
