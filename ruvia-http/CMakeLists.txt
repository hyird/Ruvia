add_library(ruvia-http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Cookies.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpBasics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpContentCoding.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpInterimResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpRequest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpResponseHeaderOps.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpResponseHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpResponseHeadersStorage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HttpResponseKnownHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MultipartReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Sse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/body/HttpTransferCodingDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpChunkParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpHeaderBlockParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/Http1RequestParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpParseError.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpParserSyntax.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpRequestTarget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/Http1InterimResponseWriter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpDateCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpResponseHead.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpResponseHeadBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2Connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2ConnectionSubmit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2Hpack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2HpackDynamic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2HpackEncoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2HpackHuffman.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2OutputBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/HttpWebSocketAccept.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/HttpWebSocketUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/HttpWebSocketValidation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/WsConnection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpOrigin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClientRedirect.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/Http1ClientRequestWriter.cpp
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

ruvia_install_public_headers(http
    include/ruvia/http/Cookies.h
    include/ruvia/http/ProtocolByteLimit.h
    include/ruvia/http/HttpProtocolError.h
    include/ruvia/http/HttpCache.h
    include/ruvia/http/HttpClient.h
    include/ruvia/http/HttpClientRedirect.h
    include/ruvia/http/HttpHeader.h
    include/ruvia/http/Http1InterimResponseWriter.h
    include/ruvia/http/HttpInterimResponse.h
    include/ruvia/http/HttpLimits.h
    include/ruvia/http/HttpKnownMethod.h
    include/ruvia/http/HttpParseError.h
    include/ruvia/http/HttpProtocolVersion.h
    include/ruvia/http/Http1ClientRequestWriter.h
    include/ruvia/http/Http1ClientResponseParser.h
    include/ruvia/http/Http1RequestParser.h
    include/ruvia/http/HttpRequest.h
    include/ruvia/http/HttpResponse.h
    include/ruvia/http/HttpStatus.h
    include/ruvia/http/MultipartParser.h
    include/ruvia/http/Sse.h
    include/ruvia/http/UrlEncoding.h
    include/ruvia/http/WebSocketProtocol.h
)

ruvia_install_public_header_tree(http include/ruvia/http/detail)
