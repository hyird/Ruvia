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

add_library(ruvia-http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Sse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/Http1ClientRequestHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/Http1ClientRequestWriter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClientRedirect.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClientResponseHead.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClientResponseParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClientResponsePlan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpOriginView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/coding/HttpBrotliCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/coding/HttpContentCoding.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/coding/HttpContentEncoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/coding/HttpGzipCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/coding/HttpTransferCodingDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/coding/HttpZstdCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/coding/ZlibPmrAllocation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cookie/Cookies.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cookie/HttpSetCookie.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/field/HttpCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/field/HttpFieldSyntax.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/field/HttpMethod.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2Connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2ConnectionData.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2ConnectionFlow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2ConnectionHeaderFrames.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2ConnectionHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2ConnectionResponseHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2ConnectionStreams.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2ConnectionSubmit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2ConnectionSubmitRequest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2OutputBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2Hpack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2HpackDynamic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2HpackEncoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2HpackHuffman.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/Http1ServerRequestParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpChunkParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpHeaderBlockParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpParseError.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpParserSyntax.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpRequestTarget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpSerializedOrigin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/HttpUriGrammar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/MultipartInput.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/MultipartParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/request/HttpRequest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/response/HttpInterimResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/response/HttpResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/response/HttpResponseComputedHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/response/HttpResponseHeaderOps.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/response/HttpResponseHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/response/HttpResponseHeadersStorage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/response/HttpResponseKnownHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/response/HttpResponseSetCookieOps.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/Http1InterimResponseWriter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpDateCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpResponseHead.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpResponseHeadBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/HttpWebSocketAccept.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/HttpWebSocketHandshakeFields.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/HttpWebSocketValidation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/WsConnection.cpp
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
    include/ruvia/http/BorrowedText.h
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
    include/ruvia/http/Http1ClosePolicy.h
    include/ruvia/http/Http1RequestParser.h
    include/ruvia/http/HttpRequest.h
    include/ruvia/http/HttpResponse.h
    include/ruvia/http/HttpStatus.h
    include/ruvia/http/HttpSetCookie.h
    include/ruvia/http/MultipartParser.h
    include/ruvia/http/Sse.h
    include/ruvia/http/UrlEncoding.h
    include/ruvia/http/WebSocketProtocol.h
)

ruvia_install_public_header_tree(http include/ruvia/http/detail)

ruvia_install_export(http)
ruvia_assert_target_file_ownership(ruvia-http)
