cmake_minimum_required(VERSION 3.25)

get_filename_component(RUVIA_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(RULE_EDGE "ruvia-edge|ruvia::edge|RUVIA_BUILD_EDGE")
set(RULE_ASIO "#[ \t]*include[ \t]*[<\"]asio|asio::")
# The pure sans-I/O protocol library must do NO OS file I/O. The ResponseFileBody
# descriptor (path + size) is fine; opening the file (ifstream / ::open / CreateFile)
# is a web-layer runtime driver concern and must not ship in ruvia-http.
set(RULE_FILE_IO
    "#[ \t]*include[ \t]*[<\"](fstream|fcntl\\.h|unistd\\.h|io\\.h)|ifstream|ofstream|::open[ \t]*\\(|::CreateFile")
set(RULE_HTTP_FRAMEWORK_INCLUDE
    "#[ \t]*include[ \t]*\"ruvia/(app/|core/|memory/|detail/|router/|web/)")
set(RULE_HTTP_CORE_LINK "ruvia::core|ruvia-core")
set(RULE_HTTP_REQUEST_TRANSPORT
    "remoteAddress|clientCertificate|isSecure|secure_")
set(RULE_HTTP_WEB_ERROR
    "HttpErrorInfo|detailsJson|defaultErrorCode|makeDefaultErrorResponse|error_handler_failed")
set(RULE_HTTP_WEB_JSON
    "JsonUtils|jsonStringSizeHint|appendJsonString|jsonNeedsEscape|jsonHexDigit")
set(RULE_HTTP_IMPLICIT_SERVER_PRODUCT
    [=[([Ss]erver:[ \t]*|"server"[ \t]*,[ \t]*"|kServer[^\r\n]*)[Rr][Uu][Vv][Ii][Aa]]=])
set(RULE_DYNAMIC_RESPONSE_BODY_STREAM
    "HttpBodyStream|responseHasStreamBody|setResponseStreamBody|writeStreamingResponse")
set(RULE_HTTP_STATIC_FILE_PRODUCT
    "FileResponseHelpers|FileResponseResource|HttpFileChunkBuffer|httpGuessContentType|httpLowerFileExtension|httpMakeFileEtag|httpFileTimeToTimeT|fileResponseResource")
set(RULE_CONTEXT_NEW_RESPONSE_ALIAS "newResponse")
set(RULE_WEB_HTTP1_STREAM_FRAMING_BYTES
    "std::to_chars|lastChunk|trailers_\\.append|\"0\\\\r\\\\n\"")
set(RULE_WEB_CHUNKED_PROTOCOL_PARSER
    "validateHttpChunkTrailers|ChunkDelimiterStatus|parseSizeLine|readingTrailers_|trailerSearchOffset_|find\\(\"\\\\r\\\\n")
set(RULE_WEB_MULTIPART_PROTOCOL_PARSER
    "parseMultipartPartsFromBody|httpFindMultipartBoundary(Line|Prefix)|httpMultipartBoundary(Line|Prefix)Size|requestBody\\.find\\(\"\\\\r\\\\n\\\\r\\\\n")
set(RULE_STALE_MULTIPART_API
    "partBegin[ \\t]*\\(|partEnd[ \\t]*\\(|appendChunk[ \\t]*\\(|enum class[ \\t]+PollStatus|MultipartParser::PollResult|HttpMultipartDelimiter(Status|Match)|HttpMultipartBoundary(Parse)?Status|HttpMultipartPartHeaderStatus|httpParseMultipartPartHeaders[ \\t\\r\\n]*\\([^)]*HttpMultipartPartHeaders[ \\t]*&|MultipartParser[ \\t\\r\\n]*\\([ \\t\\r\\n]*std::string_view")
set(RULE_WEB_HANDSHAKE_PROTOCOL_BYTES
    "kHttpWebSocket(SwitchingProtocolsPrefix|SubprotocolHeaderPrefix|ExtensionsHeaderPrefix)|kHttpCrlf|cachedDateHeader|asio::buffer\\(\"\\\\r\\\\n")
set(RULE_OBSOLETE_HTTP2_UPGRADE
    "h2c|H2C|HTTP2-Settings|Http2Upgrade|beginUpgraded|seedUpgradedStream|runUpgradedHttp2|Http2SansIoUpgradeSeed|isHttp2UpgradeAttempt|parseHttp2UpgradeRequest")
set(RULE_HTTP_OWNED_BASE64URL
    "ruvia/http/detail/HttpBase64Url[.]h|kHttpBase64UrlAlphabet|httpDecodeBase64UrlChar")
set(RULE_WEB_HTTP2_TRAILER_PROTOCOL
    "HpackEncoder|httpAsciiToLower|lowerName_|HPACK-encoded")
set(RULE_WEB_TRAILER_PROTOCOL_VALIDATION "responseTrailerFieldValid")
set(RULE_WEB_WS_SPLIT_FRAME_TRANSPORT
    "writeFrame[ \t]*\\(|std::string_view[ \t]+header")
set(RULE_STALE_WS_CLOSE_CHAIN
    "WsFeedStatus|[.]feed[ \t]*\\(|nextEvent[ \t]*\\(|pendingOutput[ \t]*\\(|wantsWrite[ \t]*\\(|closeSent_|bool[ \t]+endStream|[.]closing[ \t]*\\(")
set(RULE_STALE_WS_EVENT_TUPLE
    "struct[ \t]+WsEvent[ \t]+final|WsEvent::Kind|Kind::kNone|poll[ \t]*[(][)][ \t]*[.][ \t]*kind|event[.](kind|opcode|payload|closeCode)[ \t]*(==|!=|=|;|,|[)])")
set(RULE_STALE_WS_INBOUND_RESULT
    "WebSocketProtocolError|WebSocketInboundAction|WebSocketFrameReadStatus|struct[ \t]+WebSocketFrameReadResult|validateWebSocketClosePayload|std::size_t[ \t]+requiredBytes|bool[ \t]+cleanEofAllowed|accept[ \t\r\n]*\\([^)]*WebSocketMessage[ \t]*&")
set(RULE_HTTP_WS_RUNTIME_POLICY
    "WebSocket(Heartbeat|Lifecycle)Options|pingInterval|pongTimeout|closeHandshakeTimeout|webSocket(Heartbeat|Liveness)Decision")
set(RULE_WS_SCANNER_OWNER_ABORT "return[ \t]+true[ \t]*;")
set(RULE_WEB_HTTP1_STREAM_PLAN
    "ResponseStreamFraming::|httpVersion[ \t]*\\(|parsed\\.contentLength[ \t]*==|![ \t]*parsed\\.chunked|isHttp11")
set(RULE_WEB_HTTP1_RESPONSE_FINALIZATION
    "http1ResponseWantsClose|http1MarkConnection(Close|KeepAlive)IfNeeded")
set(RULE_STALE_HTTP1_CONNECTION_LIFETIME
    "http1ShouldKeepAlive|requestCanPersist|baseDisposition|closeAfterWrite|markConnectionCloseAfterWrite|bool[ \t]+keepAlive|bool[ \t]*&[ \t]*keepAlive")
set(RULE_STALE_HTTP_CONNECTION_FIELD_STATE
    "HttpRequestFlags|httpUpdateConnectionFlags|Http1ResponseConnectionOptions")
set(RULE_STALE_HTTP1_REQUEST_BODY_SPLIT
    "(parsed|result)\\.(contentLength|chunked|transferCodings)|http1WantsContinue|sendContinue_|bool[ \t]+sendContinue|sawChunked[ \t]*&&[ \t]*block\\.transferCodings\\.count[ \t]*>[ \t]*0")
set(RULE_STALE_HTTP1_REQUEST_BODY_MODE_TUPLE
    "Http1RequestBodyMode|Http1RequestBodyPlan::(none|knownLength|chunked)[ \t]*[(]|bodyPlan_?[.](mode|hasContentLength|contentLength|isChunked|transferCodings)[ \t]*[(]|bodyPlan[(][)][.](mode|hasContentLength|contentLength|isChunked|transferCodings)[ \t]*[(]")
set(RULE_STALE_HTTP1_REQUEST_BODY_FACTORIES
    "makeWithoutBody|makeKnownLength|makeChunked")
set(RULE_STALE_SERVER_EXPECTATION_STATE
    "httpUpdateExpectContinueFlag|kExpectationFailed|http1PlanRequestBody|bool[ \t]+expectContinue|shouldSendContinue[ \t]*\\(|[.]expectsContinue[ \t]*\\(")
set(RULE_STALE_HTTP_PROTOCOL_VERSION_STATE
    "httpVersion[ \t]*\\(|httpVersion_|setHttpVersion|HttpResponseProtocolVersion|bool[ \t]+isHttp11")
set(RULE_STALE_RESPONSE_REASON_PHRASE
    "HttpStatusEntry|http(CachedStatusLine|DefaultStatusText|StatusText)|statusText_|[.]statusText[ \t]*[(][ \t]*[)]|status[ \t\r\n]*[(][ \t\r\n]*std::uint16_t[ \t]+statusCode[ \t\r\n]*,[ \t\r\n]*std::string_view")
set(RULE_STALE_CONTEXT_REASON_PHRASE
    "ResponseInit[ \t\r\n]*[{][^}]*statusText|(body|text|html|json|redirect|applyResponseState|textStaticView|jsonSerialized)[ \t\r\n]*[(][^;{]*statusText")
set(RULE_HTTP2_REASON_PHRASE
    "httpReasonPhrase|reasonPhrase|statusText")
set(RULE_STALE_HTTP1_CLIENT_RESPONSE_SPLIT
    "responseMayHaveBody|closeAfterResponse|hasTransferEncoding|hasContentEncoding|contentCoding|head\\.(hasContentLength|isChunked|contentLength|bodyOffset)[ \t]*([^A-Za-z0-9_(]|$)")
set(RULE_STALE_HTTP1_CLIENT_RESPONSE_MODE_TUPLE
    "Http1ClientResponseBodyMode|Http1ClientConnectionDisposition|ResponsePlanData|plan[(][)][.](mode|hasContentLength|contentLength|requiresBodyConsumption|selfDelimited|transferCodings|connectionDisposition|isCloseDelimited|isChunked|isOpaque|isConnectTunnel|isUpgrade)[ \t]*[(]")
set(RULE_STALE_HTTP1_CLIENT_RESPONSE_PARSER_API
    "detail/(client/HttpClientResponseParser|http1/Http1ClientResponsePlan)[.]h|parseHttpClientResponseHead|class[ \t]+HttpClientResponseHead|[.]bodyOffset[ \t]*[(]|responseContext[ \t]*[(]|Http1ClientResponseParser[ \t]*[(][ \t]*[)]")
set(RULE_STALE_HTTP1_CLIENT_REQUEST_SPLIT
    "request[.]body|std::string_view[ \t]+body[ \t]*[{]|serializeHttpClientRequest|bool[ \t]+hasRequestBody")
set(RULE_STALE_OUTBOUND_REQUEST_CONTENT_MODE_TUPLE
    "HttpClientRequestContentMode|Http1ClientRequestContentDisposition|Http2RequestContentMode")
set(RULE_STALE_HTTP_CONTENT_LENGTH_SPLIT
    "sawContentLength|parsedContentLength")
set(RULE_STALE_HTTP_TRANSFER_ENCODING_SPLIT
    "enum class[ \t]+TransferEncodingParse|parseTransferEncoding(Field)?[ \t]*\\(")
set(RULE_STALE_HTTP_CLIENT_METHOD_CASE_FOLD
    "httpAsciiEqualsIgnoreCase\\([^,\r\n]*(method|[.]method)")
set(RULE_STALE_HTTP_METHOD_DOMAIN
    "(^|[^A-Za-z0-9_])HttpMethod([^A-Za-z0-9_]|$)|parseMethod[ \t]*\\(|methodName[ \t]*\\(|kUnsupportedMethod")
set(RULE_METHOD_CLASSIFICATION_REJECTION
    "classifyHttpMethod[(][^)]*[)][^\r\n]*(==|!=)[^\r\n]*HttpKnownMethod::kUnknown")
set(RULE_WEB_RESPONSE_BODY_PROTOCOL_BOOL
    "skipBody|bodyForbidden")
set(RULE_WEB_H2_RESPONSE_PLAN_DUPLICATION
    "responseWritePolicy|responseBodySize\\(response\\)|responseFileBody\\(response\\)\\.length")
set(RULE_WEB_HEAD_BODY_DECISION
    "request[.](method|knownMethod)[(][)][ \t]*==[ \t]*(HttpKnownMethod::kHead|\"HEAD\")")
set(RULE_STALE_HTTP1_RESPONSE_HEAD_SCALAR
    "suppressAutoContentLength|streamHead[.]policy[(][)]|responseBodyFramingHeaderForbidden|responseHasForbiddenBodyFramingHeader|http1BufferedResponseHeadPlan")
set(RULE_STALE_HTTP1_RESPONSE_VERSION_SIGNAL
    "Http1ResponseConnectionSignal|responseSignal[(][)]|http1PlanRequestConnection")
set(RULE_STALE_HTTP2_RESPONSE_HEAD_SCALAR
    "Http2ExplicitContentLengthStatus|http2ExplicitResponseContentLength|bool[ 	]+emitAutoContentLength|std::uint64_t[ 	]+autoContentLength")
set(RULE_STALE_FINAL_RESPONSE_CONTROL_TUPLE
    "HttpFinalResponseControlStatus|HttpUpgradeProtocols[ 	]+upgradeProtocols[ 	]*=[ 	]*[{][}]")
set(RULE_ROUTER_CONNECTION_POLICY
    "closeConnection(OnError)?|\"Connection\"[ \t]*,[ \t]*\"close\"")
set(RULE_STALE_ERROR_API
    "ruvia/http/Error\.h|defaultStatusText|makeErrorResponse")
set(RULE_CORE_PROTOCOL "ruvia/http/|Http[A-Z]|WebSocket|websocket")
set(RULE_PUBLIC_SRC_INCLUDE "BUILD_INTERFACE:[^>\r\n]*[/\\\\]src")
set(RULE_CROSS_TARGET_SRC "ruvia-(core|http|web)[/\\\\]src")
set(RULE_CROSS_TARGET_PHYSICAL_INCLUDE
    "#[ \t]*include[ \t]*[<\"][^>\"]*ruvia-(core|http|web)[/\\\\](src|include)[/\\\\]")
set(RULE_WEB_CODEC
    "#[ \t]*include[ \t]*[<\"](zlib|zstd|brotli)|deflateInit|inflateInit|Brotli[A-Z]|ZSTD_")
set(RULE_WEB_HTTP_CLIENT
    "HttpClient(Runtime|Pool|Registry|Backend|Config|Definition)|Http2ClientSession|ContextClient|FetchResponse(Stream)?|FetchOptions|ProxyOptions|RequestBodyStream|useHttpClient|addHttpClient|removeHttpClient")
set(RULE_HTTP_CLIENT_RUNTIME_CONFIG
    "HttpClientConfig|tlsOptions|caFile|insecureSkipVerify|certificateChainFile|privateKeyFile|privateKeyPassword|sniHost|poolSizePerWorker|proxyConnectTimeout|proxyReadTimeout|proxySendTimeout|acquireTimeout|maxResponseBodyBytes|maxRedirects|expectContinue|decodeStream|bodyStream|milliseconds[ \t]+timeout")
set(RULE_STALE_HTTP_CLIENT_FETCH_MODEL
    "HttpFetchOptions|FetchResponse(Header|Access)?|HttpClientTypes[.]h")
set(RULE_STALE_HTTP_ORIGIN_SHAPE
    "struct[ \t]+HttpOrigin|bool[ \t]+tls")
set(RULE_STALE_HTTP_ORIGIN_VALIDATION
    "validateHttpOrigin[ \t]*\\(|isValidHttpOriginHost|kInvalidOrigin|static[ \t]+constexpr[ \t]+HttpOrigin[ \t]+(http|https)")
set(RULE_DUPLICATE_HTTP_AUTHORITY_PARSER
    "portText|std::from_chars")
set(RULE_COLLAPSED_HTTP_ORIGIN_AUTHORITY
    "httpClientAuthorityMatchesOrigin")
set(RULE_STALE_HTTP_CLIENT_REDIRECT_RESULT
    "ruvia/http/detail/client/HttpClientRedirect[.]h|HttpClientResponseHeaderLookupStatus|HttpClientRedirectTargetStatus|class[ \\t]+HttpClientResponseHeaderLookup[ \\t]+final|resolveHttpClientSameOriginRedirectTarget[ \\t\\r\\n]*\\([^)]*std::pmr::string[ \\t]*&")
set(RULE_STALE_HTTP_HOST_PERCENT_NORMALIZATION
    "nextNormalizedHostByte")
set(RULE_STALE_PUBLIC_HTTP1_PARSE_RESULT
    "HttpParser[.]h|class[ \t]+HttpParser([^A-Za-z0-9_]|$)|class[ \t]+HttpParseResult([^A-Za-z0-9_]|$)|HttpParseResultAccess")
set(RULE_STALE_HTTP1_PARSE_BYTE_OVERLOAD
    "std::size_t[ \t]+consumedBytes[ \t]*[{]")
set(RULE_STALE_HTTP1_SERVER_PARSE_PHASE
    "HttpParseStatus|HttpParseTypes[.]h|HttpServerParser|HttpServerParseResult|HttpParserInternal[.]h")
set(RULE_STALE_HTTP1_CHUNK_RESULT
    "ruvia/http/detail/HttpBodyFramer[.]h|HttpChunkDecodeEvent(Kind)?|struct[ \t]+HttpChunkScanResult|HttpChunkScanStatus|(^|[^A-Za-z0-9_])HttpChunkedBodyDecoder([^A-Za-z0-9_]|$)|(^|[^A-Za-z0-9_])HttpChunkDecoder([^A-Za-z0-9_]|$)|(^|[^A-Za-z0-9_])ChunkDelimiterStatus([^A-Za-z0-9_]|$)|chunked[.](status|consumedBytes)")
set(RULE_STALE_HTTP_BYTE_RANGE_RESULT
    "HttpRangeOutcome|HttpByteRangeResult|httpParseByteRange(Unsigned)?|httpByteRangeSetHasMultiple|parsedRange[.](outcome|range)")
set(RULE_SCANNER_SEMANTICS
    "http|websocket|client_header|client_body|send_timeout|keepalive")
set(RULE_HTTP1_CONNECTION
    "(^|[^A-Za-z0-9_])Http1Connection([^A-Za-z0-9_]|$)")
set(RULE_DELETED_H2_SESSION "Http2ServerSession")
set(RULE_STALE_H2_SEND_API
    "Http2SubmitResult|kBlocked|hasBlockedSend|takeUnblockedStreams|submitResponseTrailers|pumpWritable")
set(RULE_STALE_RESPONSE_TRAILER_SIDE_CHANNEL
    "[.]addTrailer[ 	]*\\(|addResponseTrailer[ 	]*\\(|ensureTrailerOpen")
set(RULE_STALE_RESPONSE_STREAM_COMMIT_BOOL
    "markCommitted[ 	]*\\([ 	]*(true|false)|bodySuppressed_")
set(RULE_STALE_H2_FIELD_BLOCK_STATE
    "refusedHeaderStream_|finishWasTrailers|multi-frame HEADERS on closed stream|dependency[ 	]*==[ 	]*header\.streamId|RFC 9113 §5\.3\.1")
set(RULE_STALE_H2_CONNECT_STATE
    "markWebSocketTunnel|kWebSocketTunnel|standardConnect_|extendedConnectWebSocket_|webSocketTunnel_|Http2ConnectKind|Http2TunnelPhase|(standardConnect|extendedConnect|extendedConnectWebSocket|webSocketTunnel|connectRequest|connectRejected|markStandardConnectPending|markExtendedConnectPending|markConnectTunnelOpen|markConnectRejected)[ \t]*[(]")
set(RULE_STALE_H2_PREFACE_API
    "Http2CoreConfig|queueLocalSettings|queueClientPreface|expectClientPreface|initialSendWindow|initialReceiveWindow|receivedFirstSettings_|connectionStarted_|awaitingClientPreface_")
set(RULE_STALE_H2_CLIENT_STREAM_API
    "openLocalStream|peerMaxConcurrentStreams")
set(RULE_STALE_H2_GOAWAY_API
    "beginGoaway|bool[ \t]+peerGoaway_?|closing_|[.]closing[ \t]*\\(")
set(RULE_STALE_H2_EVENT_TUPLE
    "struct[ \t]+Http2Event[ \t]+final|Http2Event::Kind|Kind::kNone|nextEvent[ \t]*[(][)][ \t]*[.][ \t]*kind|event[.](kind|streamId|bytes|error)[ \t]*(==|!=|=|;|,|[)])")
set(RULE_STALE_H2_FEED_TUPLE
    "Http2FeedStatus|struct[ \t]+Http2FeedResult|Http2FeedResult[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*[ \t]*[{]")
set(RULE_STALE_H2_REQUEST_HEAD_SUBMIT_TUPLE
    "Http2RequestHeadSubmitStatus|status_[ \t]*;[ \t\r\n]*std::uint32_t[ \t]+streamId_|failure returns stream ID zero|失败必须返回 stream ID 0")
set(RULE_STALE_H2_RESPONSE_HEAD_SUBMIT_TUPLE
    "Http2HeadSubmitResult|Http2BufferedHeadSubmitResult|Http2StreamingHeadSubmitResult|headResult[.](status|accepted|plan)[ \t]*[(]|Http2SubmitStatus[ \t]+status_[ \t]*[{][^}]*[}][ \t]*;[ \t\r\n]*Plan[ \t]+plan_")
set(RULE_STALE_H2_PEER_SETTING_APPLY_TUPLE
    "Http2PeerSettingsStatus|Http2PeerSettingsResult|http2PeerSettingsError(Code|Message)|result[.](status|initialWindowChanged|initialWindowDelta)([^A-Za-z0-9_]|$)")
set(RULE_STALE_H2_DATA_FLOW_ACCOUNTING
    "Http2ReceiveWindowResult|http2ConsumeReceiveWindows|http2RestoreReceiveWindows|dropDataFrame|windowConsumed")
set(RULE_STALE_H2_LOCAL_CONTENT_MODE_TUPLE
    "Http2LocalContentMode|localContent(Mode|HasKnownLength|DeclaredLength|AcceptedBytes|CommittedBytes|LengthComplete)[ \t]*[(]")
set(RULE_STALE_H2_LOCAL_SEND_PRODUCT
    "Http2LocalSendPhase|Http2LocalMessageKind|Http2LocalReset|localSendPhase_|localMessageKind_|localEndStream_|localEndStreamCommitted_|reset_|closeSource_|localSendPhase[ \t]*[(]|localMessageKind[ \t]*[(]|localEndStream[ \t]*[(]|localEndStreamCommitted[ \t]*[(]|canSubmitLocalHead[ \t]*[(]|localBodyOpen[ \t]*[(]|localTrailersOnly[ \t]*[(]|closeSource[ \t]*[(]|isReset[ \t]*[(]|markReset[ \t]*[(]|markClosed[ \t]*[(]|removeReset[ \t]*[(]|markLocalHeadSubmitted|markLocalTrailersOnlyHeadSubmitted|markLocalConnectRequestSubmitted|markLocalEndStreamQueued|markLocalEndStreamCommitted|markDispatchStarted")
set(RULE_STALE_H2_REMOTE_RECEIVE_PRODUCT
    "Http2RemoteReceivePhase|remoteReceivePhase_|headersDecoded_|peerEndStream_|bodyEnded_|headersDecoded[ \t]*[(]|peerEndStream[ \t]*[(]|bodyEnded[ \t]*[(]|markHeadersDecoded|markPeerEndStream|markBodyEnded|http2MarkBodyEnded")
set(RULE_STALE_H2_REMOTE_CONTENT_TUPLE
    "Http2StreamBodyAccounting|bodyAccounting_|http2BodyLengthComplete|Http2RemoteContentWithoutLength|Http2RemoteContentKnownLength|Http2RemoteContentCheck|checkRemoteContentAccept|acceptRemoteContent|http2RemoteContentTerminalValid|remoteContent[(][)][.]receivedBytes[(][)]|(setContentLength|hasContentLength|setReceivedBodyBytes|addReceivedBodyBytes|receivedBodyBytes|receivedBodyExceedsContentLength|bufferedBodyExceedsContentLength|bodyLengthComplete)[ \t]*[(]")
set(RULE_STALE_205_RESPONSE_BODY
    "205 [(]Reset Content[)] deliberately falls through|response_policy_reset_content_carries_framing")
set(RULE_STALE_DEPENDENCY
    "ruvia-web[ \t]*->[ \t]*ruvia-http[ \t]*->[ \t]*ruvia-core|http[ \t]*->[ \t]*core|http[^\r\n]*(asio/TLS|socket/TLS)[ \t]*runtime driver")
set(RULE_HARDCODED_VCPKG_TOOLCHAIN
    "[A-Za-z]:[/\\\\]vcpkg[/\\\\]scripts[/\\\\]buildsystems[/\\\\]vcpkg[.]cmake")
set(RULE_MONOLITHIC_PACKAGE_EXPORT
    "EXPORT[ \t]+ruviaTargets|FILE[ \t]+ruviaTargets[.]cmake")

function(expect_match label regex sample)
    string(REGEX MATCH "${regex}" detected "${sample}")
    if(detected STREQUAL "")
        message(FATAL_ERROR
            "boundary self-test FAIL: ${label} was not detected\n"
            "sample: ${sample}\nregex: ${regex}")
    endif()
endfunction()

function(example_has_private_include content output)
    string(REGEX MATCHALL "#[ \t]*include[ \t]*\"[^\"]+\"" includes "${content}")
    set(found FALSE)
    foreach(include_line IN LISTS includes)
        if(NOT include_line MATCHES "#[ \t]*include[ \t]*\"ruvia/")
            set(found TRUE)
            break()
        endif()
    endforeach()
    set(${output} ${found} PARENT_SCOPE)
endfunction()

if(RUVIA_BOUNDARY_SELF_TEST)
    expect_match("removed edge target" "${RULE_EDGE}" "add_subdirectory(ruvia-edge)")
    expect_match("asio in HTTP" "${RULE_ASIO}" "#include <asio.hpp>")
    expect_match("file I/O in HTTP" "${RULE_FILE_IO}" "#include <fstream>")
    expect_match("native open in HTTP" "${RULE_FILE_IO}" "::open(path, O_RDONLY)")
    expect_match("framework include in HTTP" "${RULE_HTTP_FRAMEWORK_INCLUDE}"
        "#include \"ruvia/core/detail/AsioAwait.h\"")
    expect_match("core link in HTTP" "${RULE_HTTP_CORE_LINK}"
        "target_link_libraries(ruvia-http PRIVATE ruvia::core)")
    expect_match("transport metadata in HTTP request" "${RULE_HTTP_REQUEST_TRANSPORT}"
        "bool isSecure() const noexcept;")
    expect_match("Web application error model in HTTP" "${RULE_HTTP_WEB_ERROR}"
        "class HttpErrorInfo {};")
    expect_match("implicit HTTP/1 Server product banner"
        "${RULE_HTTP_IMPLICIT_SERVER_PRODUCT}" "Server: ruvia")
    expect_match("implicit literal-name HTTP/2 Server product banner"
        "${RULE_HTTP_IMPLICIT_SERVER_PRODUCT}" "encodeHeader(block, \"server\", \"Ruvia\")")
    expect_match("implicit static-name HTTP/2 Server product banner"
        "${RULE_HTTP_IMPLICIT_SERVER_PRODUCT}" "HpackStaticIndex::kServer, \"ruvia\"")
    expect_match("Web JSON serialization in HTTP" "${RULE_HTTP_WEB_JSON}"
        "appendJsonString(output, value);")
    expect_match("dynamic response-body streaming bypass"
        "${RULE_DYNAMIC_RESPONSE_BODY_STREAM}"
        "setResponseStreamBody(response, stream);")
    expect_match("static-file product/runtime helper in HTTP"
        "${RULE_HTTP_STATIC_FILE_PRODUCT}"
        "httpGuessContentType(path);")
    expect_match("duplicate Context response constructor alias"
        "${RULE_CONTEXT_NEW_RESPONSE_ALIAS}"
        "context.newResponse(body);")
    expect_match("HTTP/1 stream framing bytes in Web runtime"
        "${RULE_WEB_HTTP1_STREAM_FRAMING_BYTES}"
        "constexpr std::string_view lastChunk = \"0\\r\\n\";")
    expect_match("chunked protocol parser in Web runtime"
        "${RULE_WEB_CHUNKED_PROTOCOL_PARSER}"
        "validateHttpChunkTrailers(source);")
    expect_match("multipart protocol parser in Web facade"
        "${RULE_WEB_MULTIPART_PROTOCOL_PARSER}"
        "httpFindMultipartBoundaryLine(requestBody, boundary);")
    expect_match("stale multipart boolean chunk API"
        "${RULE_STALE_MULTIPART_API}"
        "if (part.partEnd()) consume(part);")
    expect_match("raw multipart parser boundary"
        "${RULE_STALE_MULTIPART_API}"
        "MultipartParser(std::string_view boundary, memory_resource* resource);")
    expect_match("stale multipart poll status/payload tuple"
        "${RULE_STALE_MULTIPART_API}"
        "enum class PollStatus { kNeedInput, kPart, kDone };")
    expect_match("stale multipart delimiter field tuple"
        "${RULE_STALE_MULTIPART_API}"
        "struct HttpMultipartDelimiterMatch { HttpMultipartDelimiterStatus status; };")
    expect_match("stale multipart part-header out parameter"
        "${RULE_STALE_MULTIPART_API}"
        "httpParseMultipartPartHeaders(std::string_view headers, HttpMultipartPartHeaders& output);")
    expect_match("WebSocket handshake serialization in Web runtime"
        "${RULE_WEB_HANDSHAKE_PROTOCOL_BYTES}"
        "asio::buffer(kHttpWebSocketSwitchingProtocolsPrefix)")
    expect_match("obsolete HTTP/2 HTTP/1.1 Upgrade path"
        "${RULE_OBSOLETE_HTTP2_UPGRADE}"
        "connection.beginUpgraded(seed, settingsPayload, body);")
    expect_match("generic base64url helper in the HTTP target"
        "${RULE_HTTP_OWNED_BASE64URL}"
        "#include \"ruvia/http/detail/HttpBase64Url.h\"")
    expect_match("HTTP/2 trailer encoding in Web runtime"
        "${RULE_WEB_HTTP2_TRAILER_PROTOCOL}"
        "HpackEncoder::encodeHeader(trailers, name, value)")
    expect_match("response trailer protocol validation in Web state"
        "${RULE_WEB_TRAILER_PROTOCOL_VALIDATION}"
        "responseTrailerFieldValid(name, value)")
    expect_match("split WebSocket frame transport contract"
        "${RULE_WEB_WS_SPLIT_FRAME_TRANSPORT}"
        "writeFrame(std::string_view header, std::string_view payload)")
    expect_match("stale split WebSocket close chain"
        "${RULE_STALE_WS_CLOSE_CHAIN}"
        "writeBytes(bytes, bool endStream)")
    expect_match("stale WebSocket sentinel event tuple"
        "${RULE_STALE_WS_EVENT_TUPLE}"
        "struct WsEvent final { Kind kind{Kind::kNone}; };")
    expect_match("stale WebSocket raw event field access"
        "${RULE_STALE_WS_EVENT_TUPLE}"
        "if (event.closeCode == 1000) finish();")
    expect_match("stale WebSocket frame status tuple"
        "${RULE_STALE_WS_INBOUND_RESULT}"
        "struct WebSocketFrameReadResult { WebSocketFrameReadStatus status; std::size_t requiredBytes; };")
    expect_match("stale WebSocket assembler action/output contract"
        "${RULE_STALE_WS_INBOUND_RESULT}"
        "WebSocketInboundAction accept(Frame frame, WebSocketMessage& out);")
    expect_match("peer WebSocket wire failure thrown as an exception"
        "${RULE_STALE_WS_INBOUND_RESULT}"
        "throw WebSocketProtocolError(1002, \"bad frame\");")
    expect_match("WebSocket runtime policy in HTTP target"
        "${RULE_HTTP_WS_RUNTIME_POLICY}"
        "struct WebSocketLifecycleOptions { milliseconds closeHandshakeTimeout; };")
    expect_match("WebSocket liveness closes scanner owner"
        "${RULE_WS_SCANNER_OWNER_ABORT}"
        "case kTimeout: return true;")
    expect_match("HTTP/1 stream planning in Web runtime"
        "${RULE_WEB_HTTP1_STREAM_PLAN}"
        "const bool isHttp11 = request.httpVersion() == \"HTTP/1.1\";")
    expect_match("HTTP/1 response finalization in Web runtime"
        "${RULE_WEB_HTTP1_RESPONSE_FINALIZATION}"
        "if (http1ResponseWantsClose(response)) keepAlive = false;")
    expect_match("stale split HTTP/1 connection lifetime"
        "${RULE_STALE_HTTP1_CONNECTION_LIFETIME}"
        "bool closeAfterWrite = false;")
    expect_match("stale boolean HTTP/1 reuse verdict"
        "${RULE_STALE_HTTP1_CONNECTION_LIFETIME}"
        "bool keepAlive = true;")
    expect_match("stale split HTTP connection-field state"
        "${RULE_STALE_HTTP_CONNECTION_FIELD_STATE}"
        "HttpRequestFlags flags;")
    expect_match("split HTTP/1 request-body framing facts"
        "${RULE_STALE_HTTP1_REQUEST_BODY_SPLIT}"
        "reader(parsed.contentLength, parsed.chunked, parsed.transferCodings, sendContinue);")
    expect_match("valid leading HTTP/1 transfer coding rejected"
        "${RULE_STALE_HTTP1_REQUEST_BODY_SPLIT}"
        "if (block.sawChunked && block.transferCodings.count > 0) return unsupported;")
    expect_match("mode/payload HTTP/1 request-body tuple"
        "${RULE_STALE_HTTP1_REQUEST_BODY_MODE_TUPLE}"
        "if (bodyPlan.isChunked()) decode(bodyPlan.transferCodings());")
    expect_match("publicly discoverable HTTP/1 request-body factory"
        "${RULE_STALE_HTTP1_REQUEST_BODY_FACTORIES}"
        "return Http1RequestBodyPlan::makeKnownLength(length);")
    expect_match("parser-owned Expect policy"
        "${RULE_STALE_SERVER_EXPECTATION_STATE}"
        "return HttpParseError::kExpectationFailed;")
    expect_match("scalar Expect parser state"
        "${RULE_STALE_SERVER_EXPECTATION_STATE}"
        "bool expectContinue = false;")
    expect_match("split HTTP/1 request-body factory"
        "${RULE_STALE_SERVER_EXPECTATION_STATE}"
        "http1PlanRequestBody(hasLength, length, chunked, codings, sendContinue)")
    expect_match("borrowed HTTP protocol-version string"
        "${RULE_STALE_HTTP_PROTOCOL_VERSION_STATE}"
        "std::string_view httpVersion() const noexcept;")
    expect_match("parallel final-response protocol enum"
        "${RULE_STALE_HTTP_PROTOCOL_VERSION_STATE}"
        "enum class HttpResponseProtocolVersion { kHttp1, kHttp2 };")
    expect_match("persistent HTTP/1.1 classification boolean"
        "${RULE_STALE_HTTP_PROTOCOL_VERSION_STATE}"
        "bool isHttp11 = version == \"HTTP/1.1\";")
    expect_match("custom reason phrase in the generic response model"
        "${RULE_STALE_RESPONSE_REASON_PHRASE}"
        "void status(std::uint16_t statusCode, std::string_view statusText);")
    expect_match("pre-baked HTTP/1 status-line table"
        "${RULE_STALE_RESPONSE_REASON_PHRASE}"
        "auto line = httpCachedStatusLine(status, phrase);")
    expect_match("custom reason phrase in Context ResponseInit"
        "${RULE_STALE_CONTEXT_REASON_PHRASE}"
        "struct ResponseInit { std::string_view statusText; };")
    expect_match("HTTP/2 consumes an HTTP/1 reason phrase"
        "${RULE_HTTP2_REASON_PHRASE}"
        "auto reasonPhrase = httpReasonPhrase(status);")
    expect_match("split HTTP/1 client response framing verdict"
        "${RULE_STALE_HTTP1_CLIENT_RESPONSE_SPLIT}"
        "if (head.isChunked && !head.closeAfterResponse) reuse();")
    expect_match("mode/payload HTTP/1 client response tuple"
        "${RULE_STALE_HTTP1_CLIENT_RESPONSE_MODE_TUPLE}"
        "if (plan().mode() == Http1ClientResponseBodyMode::kContentLength) plan().contentLength();")
    expect_match("stale exception/out-parameter HTTP/1 client response parser"
        "${RULE_STALE_HTTP1_CLIENT_RESPONSE_PARSER_API}"
        "auto head = parseHttpClientResponseHead(context, bytes, response, resource);")
    expect_match("reconstructed HTTP/1 client response context"
        "${RULE_STALE_HTTP1_CLIENT_RESPONSE_PARSER_API}"
        "parser.parse(prepared.responseContext(), bytes);")
    expect_match("split/manual HTTP/1 client request contract"
        "${RULE_STALE_HTTP1_CLIENT_REQUEST_SPLIT}"
        "request.body = payload;")
    expect_match("mode/payload outbound request-content tuple"
        "${RULE_STALE_OUTBOUND_REQUEST_CONTENT_MODE_TUPLE}"
        "enum class Http2RequestContentMode { kNone, kKnownLength, kStreaming };")
    expect_match("split Content-Length parser state"
        "${RULE_STALE_HTTP_CONTENT_LENGTH_SPLIT}"
        "bool sawContentLength = false;")
    expect_match("split Transfer-Encoding parser state"
        "${RULE_STALE_HTTP_TRANSFER_ENCODING_SPLIT}"
        "auto parseTransferEncodingField(std::string_view value);")
    expect_match("case-folded HTTP client method semantics"
        "${RULE_STALE_HTTP_CLIENT_METHOD_CASE_FOLD}"
        "httpAsciiEqualsIgnoreCase(options.method, \"HEAD\")")
    expect_match("closed HTTP method enum used as the wire domain"
        "${RULE_STALE_HTTP_METHOD_DOMAIN}"
        "HttpMethod parseMethod(std::string_view method);")
    expect_match("unknown method classification rejected as invalid syntax"
        "${RULE_METHOD_CLASSIFICATION_REJECTION}"
        "if (classifyHttpMethod(value) == HttpKnownMethod::kUnknown) reject();")
    expect_match("loose response body protocol bool in Web runtime"
        "${RULE_WEB_RESPONSE_BODY_PROTOCOL_BOOL}"
        "const bool skipBody = request.method() == HttpMethod::kHead;")
    expect_match("duplicated HTTP/2 response plan in Web runtime"
        "${RULE_WEB_H2_RESPONSE_PLAN_DUPLICATION}"
        "const auto policy = responseWritePolicy(response.status());")
    expect_match("HEAD response body decision in Web runtime"
        "${RULE_WEB_HEAD_BODY_DECISION}"
        "request.knownMethod() == HttpKnownMethod::kHead")
    expect_match("scalar HTTP/1 response-head framing"
        "${RULE_STALE_HTTP1_RESPONSE_HEAD_SCALAR}"
        "bool suppressAutoContentLength = true;")
    expect_match("lossy HTTP/1 response-version signal"
        "${RULE_STALE_HTTP1_RESPONSE_VERSION_SIGNAL}"
        "Http1ResponseConnectionSignal responseSignal();")
    expect_match("scalar HTTP/2 response-head Content-Length ownership"
        "${RULE_STALE_HTTP2_RESPONSE_HEAD_SCALAR}"
        "std::uint64_t autoContentLength, bool emitAutoContentLength")
    expect_match("status plus default final-response control payload"
        "${RULE_STALE_FINAL_RESPONSE_CONTROL_TUPLE}"
        "HttpFinalResponseControlStatus status; HttpUpgradeProtocols upgradeProtocols = {};")
    expect_match("connection policy in Router" "${RULE_ROUTER_CONNECTION_POLICY}"
        "bool closeConnectionOnError")
    expect_match("removed mixed-layer error API" "${RULE_STALE_ERROR_API}"
        "#include \"ruvia/http/Error.h\"")
    expect_match("protocol semantics in core" "${RULE_CORE_PROTOCOL}"
        "#include \"ruvia/http/HttpParser.h\"")
    expect_match("public src include" "${RULE_PUBLIC_SRC_INCLUDE}"
        "$<BUILD_INTERFACE:C:/repo/ruvia-http/src>")
    expect_match("cross-target private source include" "${RULE_CROSS_TARGET_SRC}"
        "target_include_directories(ruvia-web PRIVATE C:/repo/ruvia-http/src)")
    expect_match("cross-target physical header include" "${RULE_CROSS_TARGET_PHYSICAL_INCLUDE}"
        "#include \"../../../ruvia-http/src/HttpParserInternal.h\"")
    expect_match("codec in web" "${RULE_WEB_CODEC}" "#include <zlib.h>")
    expect_match("outbound HTTP client runtime in web" "${RULE_WEB_HTTP_CLIENT}"
        "class HttpClientPool {};")
    expect_match("runtime-only HTTP client config" "${RULE_HTTP_CLIENT_RUNTIME_CONFIG}"
        "std::size_t poolSizePerWorker{4};")
    expect_match("stale Fetch-shaped HTTP client model"
        "${RULE_STALE_HTTP_CLIENT_FETCH_MODEL}" "struct HttpFetchOptions {};")
    expect_match("mutable boolean HTTP origin" "${RULE_STALE_HTTP_ORIGIN_SHAPE}"
        "struct HttpOrigin { bool tls; };")
    expect_match("helper-side HTTP origin validation"
        "${RULE_STALE_HTTP_ORIGIN_VALIDATION}"
        "validateHttpOrigin(origin);")
    expect_match("constexpr origin factory bypasses validation"
        "${RULE_STALE_HTTP_ORIGIN_VALIDATION}"
        "static constexpr HttpOrigin http(std::string_view host);")
    expect_match("duplicate redirect authority parser"
        "${RULE_DUPLICATE_HTTP_AUTHORITY_PARSER}"
        "std::string_view portText;")
    expect_match("collapsed redirect authority classification"
        "${RULE_COLLAPSED_HTTP_ORIGIN_AUTHORITY}"
        "if (!httpClientAuthorityMatchesOrigin(origin, authority)) return false;")
    expect_match("status/value redirect header lookup"
        "${RULE_STALE_HTTP_CLIENT_REDIRECT_RESULT}"
        "enum class HttpClientResponseHeaderLookupStatus { kAbsent, kFound };")
    expect_match("status/out-parameter redirect target"
        "${RULE_STALE_HTTP_CLIENT_REDIRECT_RESULT}"
        "resolveHttpClientSameOriginRedirectTarget(origin, current, location, std::pmr::string& outTarget)")
    expect_match("host comparison decodes reserved percent-encodings"
        "${RULE_STALE_HTTP_HOST_PERCENT_NORMALIZATION}"
        "nextNormalizedHostByte(host, cursor, byte);")
    expect_match("public HTTP/1 parser exposes a loose status tuple"
        "${RULE_STALE_PUBLIC_HTTP1_PARSE_RESULT}"
        "class HttpParseResult final {};")
    expect_match("HTTP/1 parse state overloads consumed bytes as required size"
        "${RULE_STALE_HTTP1_PARSE_BYTE_OVERLOAD}"
        "std::size_t consumedBytes{0};")
    expect_match("HTTP/1 request-head and whole-message completion share one status"
        "${RULE_STALE_HTTP1_SERVER_PARSE_PHASE}"
        "if (parsed.status == HttpParseStatus::kComplete) dispatch();")
    expect_match("stale HTTP/1 chunk decoder event tuple"
        "${RULE_STALE_HTTP1_CHUNK_RESULT}"
        "HttpChunkDecodeEvent event;")
    expect_match("stale HTTP/1 whole-message chunk status tuple"
        "${RULE_STALE_HTTP1_CHUNK_RESULT}"
        "struct HttpChunkScanResult { HttpChunkScanStatus status; };")
    expect_match("stale HTTP byte-range outcome/payload tuple"
        "${RULE_STALE_HTTP_BYTE_RANGE_RESULT}"
        "struct HttpByteRangeResult { HttpRangeOutcome outcome; HttpByteRange range; };")
    expect_match("stale split HTTP byte-range pre-scan"
        "${RULE_STALE_HTTP_BYTE_RANGE_RESULT}"
        "if (httpByteRangeSetHasMultiple(value)) return fullResponse();")
    expect_match("generic HTTP chunk decoder without protocol ownership"
        "${RULE_STALE_HTTP1_CHUNK_RESULT}"
        "HttpChunkedBodyDecoder decoder;")
    expect_match("protocol semantics in core scanner" "${RULE_SCANNER_SEMANTICS}"
        "client_header_timeout")
    expect_match("parallel HTTP/1 state machine" "${RULE_HTTP1_CONNECTION}"
        "class Http1Connection {};")
    expect_match("deleted HTTP/2 session in docs" "${RULE_DELETED_H2_SESSION}"
        "Http2ServerSession owns the socket")
    expect_match("stale HTTP/2 send API" "${RULE_STALE_H2_SEND_API}"
        "if (result == Http2SubmitResult::kBlocked) pumpWritable();")
    expect_match("mode/payload HTTP/2 local-content tuple"
        "${RULE_STALE_H2_LOCAL_CONTENT_MODE_TUPLE}"
        "if (stream.localContentMode() == Http2LocalContentMode::kKnownLength) stream.localContentDeclaredLength();")
    expect_match("phase/kind/boolean HTTP/2 local-send product"
        "${RULE_STALE_H2_LOCAL_SEND_PRODUCT}"
        "if (stream.localSendPhase() == Http2LocalSendPhase::kBodyOpen) stream.markLocalEndStreamQueued();")
    expect_match("reset vocabulary applied to GOAWAY-aborted HTTP/2 streams"
        "${RULE_STALE_H2_LOCAL_SEND_PRODUCT}"
        "if (stream.isReset()) table.removeReset(callback);")
    expect_match("boolean-product HTTP/2 remote receive lifecycle"
        "${RULE_STALE_H2_REMOTE_RECEIVE_PRODUCT}"
        "if (stream.headersDecoded() && stream.bodyEnded()) stream.markPeerEndStream();")
    expect_match("presence/value HTTP/2 remote-content tuple"
        "${RULE_STALE_H2_REMOTE_CONTENT_TUPLE}"
        "if (stream.hasContentLength()) stream.contentLength();")
    expect_match("stale incremental response trailer side channel"
        "${RULE_STALE_RESPONSE_TRAILER_SIDE_CHANNEL}"
        "stream.addTrailer(name, value);")
    expect_match("stale response-stream commit boolean"
        "${RULE_STALE_RESPONSE_STREAM_COMMIT_BOOL}"
        "state.markCommitted(true);")
    expect_match("stale HTTP/2 field-block state" "${RULE_STALE_H2_FIELD_BLOCK_STATE}"
        "if (dependency == header.streamId) appendRstStream(streamId, error);")
    expect_match("stale HTTP/2 CONNECT marker state" "${RULE_STALE_H2_CONNECT_STATE}"
        "stream.markWebSocketTunnel();")
    expect_match("kind/phase HTTP/2 CONNECT product state"
        "${RULE_STALE_H2_CONNECT_STATE}"
        "enum class Http2TunnelPhase { kNone, kAwaitingResponse, kOpen };" )
    expect_match("stale HTTP/2 preface/config API" "${RULE_STALE_H2_PREFACE_API}"
        "config.initialSendWindow = 1048576;")
    expect_match("stale HTTP/2 two-stage client stream API"
        "${RULE_STALE_H2_CLIENT_STREAM_API}" "auto id = client.openLocalStream();")
    expect_match("stale HTTP/2 owner-driven GOAWAY lifecycle"
        "${RULE_STALE_H2_GOAWAY_API}" "connection.beginGoaway(1);")
    expect_match("stale HTTP/2 collapsed terminal boolean"
        "${RULE_STALE_H2_GOAWAY_API}" "if (connection.closing()) return;")
    expect_match("stale HTTP/2 sentinel event tuple"
        "${RULE_STALE_H2_EVENT_TUPLE}" "struct Http2Event final { Kind kind{Kind::kNone}; };")
    expect_match("stale HTTP/2 raw event field access"
        "${RULE_STALE_H2_EVENT_TUPLE}" "if (event.streamId == 1) dispatch();")
    expect_match("stale HTTP/2 feed status enum"
        "${RULE_STALE_H2_FEED_TUPLE}"
        "enum class Http2FeedStatus { kOk, kNeedMore, kError };")
    expect_match("stale HTTP/2 feed result tuple"
        "${RULE_STALE_H2_FEED_TUPLE}"
        "struct Http2FeedResult final { std::size_t consumed; Http2FeedStatus status; };")
    expect_match("stale HTTP/2 request-head accepted status"
        "${RULE_STALE_H2_REQUEST_HEAD_SUBMIT_TUPLE}"
        "enum class Http2RequestHeadSubmitStatus { kAccepted, kInvalidMessage };")
    expect_match("stale HTTP/2 request-head status/stream tuple"
        "${RULE_STALE_H2_REQUEST_HEAD_SUBMIT_TUPLE}"
        "Http2RequestHeadSubmitStatus status_; std::uint32_t streamId_;")
    expect_match("stale HTTP/2 response-head status/plan tuple"
        "${RULE_STALE_H2_RESPONSE_HEAD_SUBMIT_TUPLE}"
        "class Http2HeadSubmitResult { Http2SubmitStatus status_; Plan plan_; };")
    expect_match("stale HTTP/2 response-head status-first consumer"
        "${RULE_STALE_H2_RESPONSE_HEAD_SUBMIT_TUPLE}"
        "if (headResult.accepted()) drive(headResult.plan());")
    expect_match("stale HTTP/2 peer-setting status/delta tuple"
        "${RULE_STALE_H2_PEER_SETTING_APPLY_TUPLE}"
        "struct Http2PeerSettingsResult { Http2PeerSettingsStatus status; bool initialWindowChanged; };")
    expect_match("stale HTTP/2 peer-setting status-first consumer"
        "${RULE_STALE_H2_PEER_SETTING_APPLY_TUPLE}"
        "if (result.initialWindowChanged) apply(result.initialWindowDelta);")
    expect_match("stale HTTP/2 DATA flow-control accounting"
        "${RULE_STALE_H2_DATA_FLOW_ACCOUNTING}"
        "dropDataFrame(payload.size(), windowConsumed);")
    expect_match("stale 205 response-body semantics" "${RULE_STALE_205_RESPONSE_BODY}"
        "205 (Reset Content) deliberately falls through to the normal policy")
    expect_match("stale dependency description" "${RULE_STALE_DEPENDENCY}"
        "ruvia-web -> ruvia-http -> ruvia-core")
    expect_match("hard-coded vcpkg toolchain root" "${RULE_HARDCODED_VCPKG_TOOLCHAIN}"
        "F:/vcpkg/scripts/buildsystems/vcpkg.cmake")
    expect_match("monolithic package target export" "${RULE_MONOLITHIC_PACKAGE_EXPORT}"
        "install(EXPORT ruviaTargets FILE ruviaTargets.cmake)")
    example_has_private_include("#include \"HttpServerInternal.h\"" private_example)
    if(NOT private_example)
        message(FATAL_ERROR
            "boundary self-test FAIL: private example include was not detected")
    endif()
    example_has_private_include("#include \"ruvia/web/Context.h\"" public_example)
    if(public_example)
        message(FATAL_ERROR
            "boundary self-test FAIL: public example include was rejected")
    endif()
    message(STATUS "boundary self-test OK (all negative rules detect planted violations)")
    return()
endif()

set_property(GLOBAL PROPERTY RUVIA_BOUNDARY_FAILED FALSE)

function(boundary_error label details)
    message(STATUS "boundary-check FAIL: ${label}\n    ${details}")
    set_property(GLOBAL PROPERTY RUVIA_BOUNDARY_FAILED TRUE)
endfunction()

function(check_files_no_match label regex)
    set(hit_files)
    foreach(path IN LISTS ARGN)
        if(EXISTS "${path}" AND NOT IS_DIRECTORY "${path}")
            file(READ "${path}" content)
            string(REGEX MATCH "${regex}" detected "${content}")
            if(NOT detected STREQUAL "")
                file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${path}")
                list(APPEND hit_files "${relative}: ${detected}")
            endif()
        endif()
    endforeach()
    if(hit_files)
        list(JOIN hit_files "\n    " details)
        boundary_error("${label}" "${details}")
    endif()
endfunction()

function(check_files_no_lower_match label regex)
    set(hit_files)
    foreach(path IN LISTS ARGN)
        if(EXISTS "${path}" AND NOT IS_DIRECTORY "${path}")
            file(READ "${path}" content)
            string(TOLOWER "${content}" content)
            string(REGEX MATCH "${regex}" detected "${content}")
            if(NOT detected STREQUAL "")
                file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${path}")
                list(APPEND hit_files "${relative}: ${detected}")
            endif()
        endif()
    endforeach()
    if(hit_files)
        list(JOIN hit_files "\n    " details)
        boundary_error("${label}" "${details}")
    endif()
endfunction()

foreach(required_dir IN ITEMS ruvia-core ruvia-http ruvia-web)
    if(NOT IS_DIRECTORY "${RUVIA_ROOT}/${required_dir}")
        boundary_error("missing target directory"
            "${required_dir}/ is required; otherwise checks would be vacuous")
    endif()
endforeach()
foreach(required_doc IN ITEMS README.md AGENTS.md)
    if(NOT EXISTS "${RUVIA_ROOT}/${required_doc}")
        boundary_error("missing project boundary document"
            "${required_doc} is required; otherwise checks would be vacuous")
    endif()
endforeach()

file(READ "${RUVIA_ROOT}/CMakeLists.txt" root_cmake)
if(root_cmake MATCHES "${RULE_MONOLITHIC_PACKAGE_EXPORT}")
    boundary_error("package targets were collapsed into one export set"
        "core/http/web must remain independently importable from partial installs")
endif()
if(NOT root_cmake MATCHES "EXPORT[ \t]+ruvia_[$][{]component[}]_targets" OR
   NOT root_cmake MATCHES "FILE[ \t]+ruvia-[$][{]_ruvia_component[}]-targets[.]cmake" OR
   NOT root_cmake MATCHES "include[(].*ruvia-core-targets[.]cmake" OR
   NOT root_cmake MATCHES "include[(].*ruvia-http-targets[.]cmake" OR
   NOT root_cmake MATCHES "include[(].*ruvia-web-targets[.]cmake")
    boundary_error("component-scoped package export contract is incomplete"
        "each target needs its own export and package config must load only the requested closure")
endif()

function(check_target_header_ownership target expected_namespace)
    set(include_root "${RUVIA_ROOT}/${target}/include/ruvia")
    if(NOT IS_DIRECTORY "${include_root}")
        boundary_error("target include root is missing" "${target}/include/ruvia")
        return()
    endif()

    file(GLOB namespace_entries RELATIVE "${include_root}" "${include_root}/*")
    foreach(entry IN LISTS namespace_entries)
        if(NOT entry STREQUAL expected_namespace)
            boundary_error("target public headers escape their namespace"
                "${target}/include/ruvia/${entry} is outside ruvia/${expected_namespace}")
        endif()
    endforeach()

    file(READ "${RUVIA_ROOT}/${target}/CMakeLists.txt" target_cmake)
    string(REGEX MATCHALL "include/ruvia/[A-Za-z0-9_-]+" installed_header_roots "${target_cmake}")
    foreach(header_root IN LISTS installed_header_roots)
        if(NOT header_root STREQUAL "include/ruvia/${expected_namespace}")
            boundary_error("target install list uses another namespace"
                "${target}/CMakeLists.txt: ${header_root}")
        endif()
    endforeach()
endfunction()

check_target_header_ownership(ruvia-core core)
check_target_header_ownership(ruvia-http http)
check_target_header_ownership(ruvia-web web)

if(IS_DIRECTORY "${RUVIA_ROOT}/ruvia-edge")
    boundary_error("ruvia-edge must remain fully removed" "ruvia-edge/ exists")
endif()
foreach(forbidden_web_client_path IN ITEMS
    "ruvia-web/src/client"
    "ruvia-web/include/ruvia/web/detail/client")
    if(EXISTS "${RUVIA_ROOT}/${forbidden_web_client_path}")
        boundary_error("ruvia-web outbound client runtime must remain removed"
            "${forbidden_web_client_path} exists")
    endif()
endforeach()
if(EXISTS "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClientTypes.h")
    boundary_error("stale split outbound client model header must remain removed"
        "ruvia-http/include/ruvia/http/HttpClientTypes.h exists")
endif()
if(EXISTS "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpParser.h")
    boundary_error("generic loose HTTP parser API must remain removed"
        "ruvia-http/include/ruvia/http/HttpParser.h exists")
endif()
set(OBSOLETE_HTTP_BODY_FRAMER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpBodyFramer.h")
set(HTTP1_CHUNK_DECODER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h")
set(HTTP1_CHUNK_SCANNER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpChunkParser.h")
if(EXISTS "${OBSOLETE_HTTP_BODY_FRAMER}")
    boundary_error("generic HTTP body framer must remain removed"
        "ruvia-http/include/ruvia/http/detail/HttpBodyFramer.h exists")
endif()
foreach(http1_chunk_contract IN ITEMS
        "${HTTP1_CHUNK_DECODER}"
        "${HTTP1_CHUNK_SCANNER}")
    if(NOT EXISTS "${http1_chunk_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${http1_chunk_contract}")
        boundary_error("typed HTTP/1 chunked contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP1_CHUNK_DECODER}" AND EXISTS "${HTTP1_CHUNK_SCANNER}")
    file(READ "${HTTP1_CHUNK_DECODER}" http1_chunk_decoder)
    file(READ "${HTTP1_CHUNK_SCANNER}" http1_chunk_scanner)
    if(NOT http1_chunk_decoder MATCHES "class Http1ChunkedBodyDecoder final" OR
       NOT http1_chunk_decoder MATCHES "class Http1ChunkDecodeNeedMore final" OR
       NOT http1_chunk_decoder MATCHES "class Http1ChunkDecodeBodyChunk final" OR
       NOT http1_chunk_decoder MATCHES "class Http1ChunkDecodeComplete final" OR
       NOT http1_chunk_decoder MATCHES "using Value = std::variant" OR
       NOT http1_chunk_decoder MATCHES "std::size_t consumedBytes[(][)] const" OR
       NOT http1_chunk_decoder MATCHES "std::string_view bytes[(][)] const" OR
       NOT http1_chunk_decoder MATCHES "std::get_if<Http1ChunkDecodeBodyChunk>")
        boundary_error("HTTP/1 streaming chunk decoder lost field ownership"
            "need-more/body/complete must be discriminated; only body owns borrowed bytes and every incremental outcome owns its consumed prefix")
    endif()
    if(NOT http1_chunk_scanner MATCHES "class HttpChunkScanNeedMore final" OR
       NOT http1_chunk_scanner MATCHES "class HttpChunkScanComplete final" OR
       NOT http1_chunk_scanner MATCHES "class HttpChunkScanFailure final" OR
       NOT http1_chunk_scanner MATCHES "class HttpChunkScanResult final" OR
       NOT http1_chunk_scanner MATCHES "using Value = std::variant" OR
       NOT http1_chunk_scanner MATCHES "std::optional<HttpChunkScanError> validateHttpChunkTrailers" OR
       NOT http1_chunk_scanner MATCHES "HttpChunkScanError error[(][)] const")
        boundary_error("HTTP/1 whole-message chunk scanner lost field ownership"
            "only complete may expose the final consumed boundary and only failure may expose HttpChunkScanError")
    endif()
endif()
foreach(forbidden_dynamic_response_stream_path IN ITEMS
    "ruvia-http/include/ruvia/http/HttpBodyStream.h"
    "ruvia-web/include/ruvia/web/detail/http/HttpBodyStreamAccess.h")
    if(EXISTS "${RUVIA_ROOT}/${forbidden_dynamic_response_stream_path}")
        boundary_error("dynamic response-body streaming bypass must remain removed"
            "${forbidden_dynamic_response_stream_path} exists")
    endif()
endforeach()
foreach(forbidden_http_static_file_path IN ITEMS
    "ruvia-http/include/ruvia/http/detail/FileResponseHelpers.h"
    "ruvia-http/include/ruvia/http/detail/FileResponseResource.h"
    "ruvia-http/include/ruvia/http/detail/server/HttpFileChunkBuffer.h")
    if(EXISTS "${RUVIA_ROOT}/${forbidden_http_static_file_path}")
        boundary_error("static-file product/runtime helper must remain outside ruvia-http"
            "${forbidden_http_static_file_path} exists")
    endif()
endforeach()
if(NOT EXISTS "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ChunkedFraming.h")
    boundary_error("HTTP/1 streaming framer is missing from ruvia-http"
        "ruvia-http/include/ruvia/http/detail/http1/Http1ChunkedFraming.h")
endif()
file(GLOB_RECURSE HTTP_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-http/*.h"
    "${RUVIA_ROOT}/ruvia-http/*.cpp"
    "${RUVIA_ROOT}/ruvia-http/*.inl")
file(GLOB_RECURSE WEB_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-web/*.h"
    "${RUVIA_ROOT}/ruvia-web/*.cpp"
    "${RUVIA_ROOT}/ruvia-web/*.inl")
file(GLOB_RECURSE CORE_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-core/*.h"
    "${RUVIA_ROOT}/ruvia-core/*.cpp"
    "${RUVIA_ROOT}/ruvia-core/*.inl")
check_files_no_match("ruvia-http must not invent a Ruvia Server product identity"
    "${RULE_HTTP_IMPLICIT_SERVER_PRODUCT}" ${HTTP_SOURCE})
file(GLOB_RECURSE EDGE_REFERENCE_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-core/*.h" "${RUVIA_ROOT}/ruvia-core/*.cpp" "${RUVIA_ROOT}/ruvia-core/*.inl"
    "${RUVIA_ROOT}/ruvia-core/*.cmake" "${RUVIA_ROOT}/ruvia-core/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-http/*.h" "${RUVIA_ROOT}/ruvia-http/*.cpp" "${RUVIA_ROOT}/ruvia-http/*.inl"
    "${RUVIA_ROOT}/ruvia-http/*.cmake" "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/*.h" "${RUVIA_ROOT}/ruvia-web/*.cpp" "${RUVIA_ROOT}/ruvia-web/*.inl"
    "${RUVIA_ROOT}/ruvia-web/*.cmake" "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt"
    "${RUVIA_ROOT}/examples/*.h" "${RUVIA_ROOT}/examples/*.cpp" "${RUVIA_ROOT}/examples/*.inl"
    "${RUVIA_ROOT}/examples/*.cmake" "${RUVIA_ROOT}/examples/CMakeLists.txt"
    "${RUVIA_ROOT}/tests/*.h" "${RUVIA_ROOT}/tests/*.cpp" "${RUVIA_ROOT}/tests/*.inl"
    "${RUVIA_ROOT}/tests/*.cmake" "${RUVIA_ROOT}/tests/CMakeLists.txt")
list(APPEND EDGE_REFERENCE_SOURCE
    "${RUVIA_ROOT}/CMakeLists.txt"
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")

check_files_no_match("removed edge target is still referenced" "${RULE_EDGE}"
    ${EDGE_REFERENCE_SOURCE})
check_files_no_match("removed mixed-layer error API is still referenced"
    "${RULE_STALE_ERROR_API}" ${EDGE_REFERENCE_SOURCE})
check_files_no_match("ruvia-http must not reference asio" "${RULE_ASIO}"
    ${HTTP_SOURCE})
check_files_no_match("ruvia-http must not perform OS file I/O (sans-I/O protocol lib)"
    "${RULE_FILE_IO}" ${HTTP_SOURCE})
check_files_no_match("ruvia-http must not include core/web headers"
    "${RULE_HTTP_FRAMEWORK_INCLUDE}" ${HTTP_SOURCE})
check_files_no_match("ruvia-http client models must not contain runtime configuration"
    "${RULE_HTTP_CLIENT_RUNTIME_CONFIG}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h")
check_files_no_match("stale Fetch-shaped outbound client model must remain removed"
    "${RULE_STALE_HTTP_CLIENT_FETCH_MODEL}" ${EDGE_REFERENCE_SOURCE})
check_files_no_match("HTTP origin must use an immutable typed scheme"
    "${RULE_STALE_HTTP_ORIGIN_SHAPE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h")
check_files_no_match("HTTP origin validity must be established only by its factories"
    "${RULE_STALE_HTTP_ORIGIN_VALIDATION}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpOrigin.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClientRedirect.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpOrigin.cpp")
check_files_no_match("redirects must use the shared typed authority parser"
    "${RULE_DUPLICATE_HTTP_AUTHORITY_PARSER}"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp")
check_files_no_match("redirect authority validity must not collapse into origin equality"
    "${RULE_COLLAPSED_HTTP_ORIGIN_AUTHORITY}"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp")
check_files_no_match("redirect results must remain public discriminated values"
    "${RULE_STALE_HTTP_CLIENT_REDIRECT_RESULT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClientRedirect.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp"
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/tests/unit_http_client_redirect.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/examples/api_surface.cpp")
check_files_no_match("host comparison must preserve encoded reserved characters"
    "${RULE_STALE_HTTP_HOST_PERCENT_NORMALIZATION}"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpRequestTarget.cpp")
check_files_no_match("public HTTP/1 parsing must keep discriminated outcomes"
    "${RULE_STALE_PUBLIC_HTTP1_PARSE_RESULT}"
    ${HTTP_SOURCE}
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/examples/api_surface.cpp"
    "${RUVIA_ROOT}/tests/smoke_http_target.cpp"
    "${RUVIA_ROOT}/tests/guards/cookie_api_guard.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/1 parse state must separate message and required bytes"
    "${RULE_STALE_HTTP1_PARSE_BYTE_OVERLOAD}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerRequestParser.h")
file(GLOB_RECURSE HTTP1_PARSE_PHASE_REFERENCE_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/examples/*.h" "${RUVIA_ROOT}/examples/*.cpp" "${RUVIA_ROOT}/examples/*.inl"
    "${RUVIA_ROOT}/tests/*.h" "${RUVIA_ROOT}/tests/*.cpp" "${RUVIA_ROOT}/tests/*.inl")
list(APPEND HTTP1_PARSE_PHASE_REFERENCE_SOURCE
    ${HTTP_SOURCE}
    ${WEB_SOURCE}
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt")
check_files_no_match("HTTP/1 request-head and whole-message readiness were conflated again"
    "${RULE_STALE_HTTP1_SERVER_PARSE_PHASE}"
    ${HTTP1_PARSE_PHASE_REFERENCE_SOURCE})
check_files_no_match("loose or protocol-ambiguous HTTP/1 chunked result was restored"
    "${RULE_STALE_HTTP1_CHUNK_RESULT}"
    ${HTTP_SOURCE}
    ${WEB_SOURCE}
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt")
check_files_no_match("HTTP byte ranges must use one discriminated resolution"
    "${RULE_STALE_HTTP_BYTE_RANGE_RESULT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpByteRange.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextFileResponse.cpp"
    "${RUVIA_ROOT}/tests/unit_http_byte_range.cpp"
    "${RUVIA_ROOT}/tests/unit_content_range.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP method wire tokens must not collapse back into a closed enum"
    "${RULE_STALE_HTTP_METHOD_DOMAIN}"
    ${EDGE_REFERENCE_SOURCE})
check_files_no_match("valid extension methods must not be rejected by semantic classification"
    "${RULE_METHOD_CLASSIFICATION_REJECTION}"
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")

set(HTTP_METHOD_COMMON
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpCommon.h")
set(HTTP_REQUEST_MODEL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpRequest.h")
set(HTTP1_REQUEST_PARSER
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp")
set(HTTP2_REQUEST_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h")
set(HTTP2_REQUEST_BUILDER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestBuilder.h")
set(WEB_ROUTER_DISPATCH
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp")
foreach(method_contract_file IN ITEMS
        "${HTTP_METHOD_COMMON}"
        "${HTTP_REQUEST_MODEL}"
        "${HTTP1_REQUEST_PARSER}"
        "${HTTP2_REQUEST_HEADERS}"
        "${HTTP2_REQUEST_BUILDER}"
        "${WEB_ROUTER_DISPATCH}")
    if(NOT EXISTS "${method_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${method_contract_file}")
        boundary_error("HTTP method token/classification contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_METHOD_COMMON}" AND EXISTS "${HTTP_REQUEST_MODEL}")
    file(READ "${HTTP_METHOD_COMMON}" http_method_common)
    file(READ "${HTTP_REQUEST_MODEL}" http_request_model)
    if(NOT http_method_common MATCHES "enum class HttpKnownMethod" OR
       NOT http_method_common MATCHES "classifyHttpMethod" OR
       NOT http_method_common MATCHES "isValidHttpMethodToken" OR
       NOT http_request_model MATCHES "std::string_view method[(][)] const noexcept" OR
       NOT http_request_model MATCHES "HttpKnownMethod knownMethod[(][)] const noexcept")
        boundary_error("HTTP request method lost raw-token/known-class separation"
            "HttpRequest::method must expose the exact token and knownMethod the fixed semantic class")
    endif()
endif()
if(EXISTS "${HTTP1_REQUEST_PARSER}" AND EXISTS "${HTTP2_REQUEST_HEADERS}" AND
   EXISTS "${HTTP2_REQUEST_BUILDER}" AND EXISTS "${WEB_ROUTER_DISPATCH}")
    file(READ "${HTTP1_REQUEST_PARSER}" http1_request_parser)
    file(READ "${HTTP2_REQUEST_HEADERS}" http2_request_headers)
    file(READ "${HTTP2_REQUEST_BUILDER}" http2_request_builder)
    file(READ "${WEB_ROUTER_DISPATCH}" web_router_dispatch)
    if(NOT http1_request_parser MATCHES "HttpRequestAccess::setMethod[(]state[.]request, method[)]" OR
       NOT http2_request_headers MATCHES "!isValidHttpMethodToken[(]value[)]" OR
       NOT http2_request_headers MATCHES "assignRequestMethod[(]value[)]" OR
       NOT http2_request_builder MATCHES "routeMethod" OR
       NOT http2_request_builder MATCHES "setMethod[(]request, method[)]" OR
       NOT web_router_dispatch MATCHES "knownMethod[(][)] == HttpKnownMethod::kUnknown" OR
       NOT web_router_dispatch MATCHES "HttpErrorInfo[(]501")
        boundary_error("HTTP method handling bypasses the shared extensible-token chain"
            "H1/H2 must preserve valid tokens, WS CONNECT may map only route lookup, and Web must render 501")
    endif()
endif()
set(HTTP_PROTOCOL_VERSION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpProtocolVersion.h")
set(HTTP_REQUEST_ACCESS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpRequestInternal.h")
set(HTTP1_CLIENT_RESPONSE_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp")
set(HTTP_FINAL_RESPONSE_CONTROL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpFinalResponseControlPlan.h")
foreach(protocol_version_file IN ITEMS
        "${HTTP_PROTOCOL_VERSION_HEADER}"
        "${HTTP_REQUEST_MODEL}"
        "${HTTP_REQUEST_ACCESS}"
        "${HTTP1_REQUEST_PARSER}"
        "${HTTP2_REQUEST_BUILDER}"
        "${HTTP1_CLIENT_RESPONSE_SOURCE}"
        "${HTTP_FINAL_RESPONSE_CONTROL}")
    if(NOT EXISTS "${protocol_version_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${protocol_version_file}")
        boundary_error("typed HTTP protocol-version contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_PROTOCOL_VERSION_HEADER}" AND
   EXISTS "${HTTP_REQUEST_MODEL}" AND
   EXISTS "${HTTP_REQUEST_ACCESS}" AND
   EXISTS "${HTTP1_REQUEST_PARSER}" AND
   EXISTS "${HTTP2_REQUEST_BUILDER}" AND
   EXISTS "${HTTP1_CLIENT_RESPONSE_SOURCE}" AND
   EXISTS "${HTTP_FINAL_RESPONSE_CONTROL}")
    file(READ "${HTTP_PROTOCOL_VERSION_HEADER}" http_protocol_version_header)
    file(READ "${HTTP_REQUEST_MODEL}" http_protocol_request_model)
    file(READ "${HTTP_REQUEST_ACCESS}" http_protocol_request_access)
    file(READ "${HTTP1_REQUEST_PARSER}" http_protocol_http1_parser)
    file(READ "${HTTP2_REQUEST_BUILDER}" http_protocol_http2_builder)
    file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
        http_protocol_client_model)
    file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientAccess.h"
        http_protocol_client_access)
    file(READ "${HTTP1_CLIENT_RESPONSE_SOURCE}" http_protocol_client_parser)
    file(READ "${HTTP_FINAL_RESPONSE_CONTROL}" http_protocol_final_control)
    file(READ "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt" http_protocol_cmake)
    if(NOT http_protocol_version_header MATCHES "enum class HttpProtocolVersion" OR
       NOT http_protocol_version_header MATCHES "kHttp10" OR
       NOT http_protocol_version_header MATCHES "kHttp11" OR
       NOT http_protocol_version_header MATCHES "kHttp2" OR
       NOT http_protocol_request_model MATCHES "HttpProtocolVersion protocolVersion[(][)] const noexcept" OR
       NOT http_protocol_request_access MATCHES "setProtocolVersion" OR
       NOT http_protocol_http1_parser MATCHES "HttpProtocolVersion::kHttp10" OR
       NOT http_protocol_http1_parser MATCHES "HttpProtocolVersion::kHttp11" OR
       NOT http_protocol_http2_builder MATCHES "HttpProtocolVersion::kHttp2" OR
       NOT http_protocol_client_model MATCHES "HttpProtocolVersion protocolVersion[(][)] const noexcept" OR
       NOT http_protocol_client_access MATCHES "HttpProtocolVersion protocolVersion" OR
       http_protocol_client_access MATCHES "setProtocolVersion" OR
       NOT http_protocol_client_parser MATCHES "make[(]" OR
       NOT http_protocol_client_parser MATCHES "parsed[.]protocolVersion" OR
       NOT http_protocol_final_control MATCHES "HttpProtocolVersion protocolVersion" OR
       NOT http_protocol_cmake MATCHES "include/ruvia/http/HttpProtocolVersion[.]h")
        boundary_error("HTTP protocol version split back into wire strings or parallel transport state"
            "H1/H2 request, client response, connection, and final-response control must share HttpProtocolVersion")
    endif()
endif()
file(READ "${RUVIA_ROOT}/tests/unit_request_access.cpp"
    http_protocol_request_tests)
file(READ "${RUVIA_ROOT}/tests/unit_http1_parser.cpp"
    http_protocol_http1_tests)
file(READ "${RUVIA_ROOT}/tests/unit_http2_request_builder.cpp"
    http_protocol_http2_tests)
file(READ "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    http_protocol_client_tests)
if(NOT http_protocol_request_tests MATCHES
       "request_access_protocol_version_is_typed_control_data" OR
   NOT http_protocol_http1_tests MATCHES
       "http1_parser_maps_wire_versions_to_typed_control_data" OR
   NOT http_protocol_http2_tests MATCHES
       "h2_request_builder_uses_connection_protocol_version" OR
   NOT http_protocol_client_tests MATCHES
       "http_client_response_preserves_typed_protocol_version")
    boundary_error("typed HTTP protocol-version coverage is incomplete"
        "request defaults, H1 start-lines, H2 connection version, and client responses all require direct tests")
endif()
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    http_client_public_model)
if(NOT http_client_public_model MATCHES "enum class HttpScheme" OR
   NOT http_client_public_model MATCHES "class HttpOrigin final" OR
   NOT http_client_public_model MATCHES "basic_string<char, Traits, Allocator>&&" OR
   NOT http_client_public_model MATCHES "class HttpClientRequestContent final" OR
   NOT http_client_public_model MATCHES
       "class HttpClientRequestWithoutContent final" OR
   NOT http_client_public_model MATCHES "class HttpClientRequestBytes final" OR
   NOT http_client_public_model MATCHES "using Content = std::variant" OR
   NOT http_client_public_model MATCHES
       "std::get_if<HttpClientRequestWithoutContent>" OR
   NOT http_client_public_model MATCHES "std::get_if<HttpClientRequestBytes>" OR
   NOT http_client_public_model MATCHES "borrowedBytes" OR
   NOT http_client_public_model MATCHES "class HttpClientResponse final" OR
   NOT http_client_public_model MATCHES "struct HttpClientRequest" OR
   NOT http_client_public_model MATCHES "std::string_view target" OR
   http_client_public_model MATCHES "std::string_view[ \t]+body[ \t]*[{]")
    boundary_error("outbound HTTP public model lost its transport-free typed contract"
        "HttpClient.h must distinguish absent/explicit content and own typed scheme/origin, request-target, and response models")
endif()
if(NOT EXISTS "${RUVIA_ROOT}/ruvia-http/src/client/HttpOrigin.cpp")
    boundary_error("outbound origin factory implementation is missing"
        "ruvia-http/src/client/HttpOrigin.cpp")
else()
    file(READ "${RUVIA_ROOT}/ruvia-http/src/client/HttpOrigin.cpp"
        http_client_origin_factory)
    if(NOT http_client_origin_factory MATCHES "isValidHttpHost[(]host[)]")
        boundary_error("outbound origin host validation drifted from request-target grammar"
            "HttpOrigin factories must reuse the shared HTTP uri-host parser")
    endif()
endif()
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpRequestTarget.h"
    http_authority_contract)
if(NOT http_authority_contract MATCHES "enum class HttpAuthorityPortKind" OR
   NOT http_authority_contract MATCHES "class HttpAuthorityView final" OR
   NOT http_authority_contract MATCHES "parseHttpAuthority" OR
   NOT http_authority_contract MATCHES "httpUriHostEquals")
    boundary_error("HTTP authority parsing lost its shared typed contract"
        "Host, absolute-form, origin, and redirects must share HttpAuthorityView")
endif()
file(READ "${RUVIA_ROOT}/ruvia-http/src/parser/HttpRequestTarget.cpp"
    http_authority_implementation)
if(NOT http_authority_implementation MATCHES "isUnreservedByte" OR
   NOT http_authority_implementation MATCHES "encodedReserved" OR
   NOT http_authority_implementation MATCHES "!isUnreservedByte[(]byte[)]")
    boundary_error("HTTP host comparison lost RFC percent-encoding normalization"
        "only encoded unreserved octets may collapse to raw spelling; encoded reserved octets remain distinct")
endif()
check_files_no_match("ruvia-http must not link/name ruvia-core in CMake"
    "${RULE_HTTP_CORE_LINK}" "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt")
check_files_no_match("ruvia-http request model must not contain socket/TLS metadata"
    "${RULE_HTTP_REQUEST_TRANSPORT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpRequest.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpRequestInternal.h")
check_files_no_match("ruvia-http must not own Web application error models or JSON envelopes"
    "${RULE_HTTP_WEB_ERROR}" ${HTTP_SOURCE})
check_files_no_match("ruvia-http must not own application JSON serialization helpers"
    "${RULE_HTTP_WEB_JSON}" ${HTTP_SOURCE})
check_files_no_match("ruvia-http must not own static-file product/runtime helpers"
    "${RULE_HTTP_STATIC_FILE_PRODUCT}" ${HTTP_SOURCE})
check_files_no_match("Context must expose only body() for raw response construction"
    "${RULE_CONTEXT_NEW_RESPONSE_ALIAS}"
    ${WEB_SOURCE}
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")
set(HTTP_BYTE_RANGE_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpByteRange.h")
set(WEB_FILE_RESPONSE_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextFileResponse.cpp")
set(HTTP_BYTE_RANGE_TEST
    "${RUVIA_ROOT}/tests/unit_http_byte_range.cpp")
set(HTTP_CONTENT_RANGE_TEST
    "${RUVIA_ROOT}/tests/unit_content_range.cpp")
set(HTTP_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
foreach(byte_range_contract_file IN ITEMS
        "${HTTP_BYTE_RANGE_HEADER}"
        "${WEB_FILE_RESPONSE_SOURCE}"
        "${HTTP_BYTE_RANGE_TEST}"
        "${HTTP_CONTENT_RANGE_TEST}"
        "${HTTP_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${byte_range_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${byte_range_contract_file}")
        boundary_error("HTTP byte-range resolution contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_BYTE_RANGE_HEADER}" AND
   EXISTS "${WEB_FILE_RESPONSE_SOURCE}" AND
   EXISTS "${HTTP_BYTE_RANGE_TEST}" AND
   EXISTS "${HTTP_CONTENT_RANGE_TEST}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP_BYTE_RANGE_HEADER}" http_byte_range_header)
    file(READ "${WEB_FILE_RESPONSE_SOURCE}" web_file_response_source)
    file(READ "${HTTP_BYTE_RANGE_TEST}" http_byte_range_test)
    file(READ "${HTTP_CONTENT_RANGE_TEST}" http_content_range_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http_range_package_consumer)
    if(NOT http_byte_range_header MATCHES "class HttpByteRangeIgnored final" OR
       NOT http_byte_range_header MATCHES
           "class HttpByteRangeUnsatisfiable final" OR
       NOT http_byte_range_header MATCHES "class HttpResolvedByteRange final" OR
       NOT http_byte_range_header MATCHES "class HttpByteRangeResolution final" OR
       NOT http_byte_range_header MATCHES "using Value = std::variant" OR
       NOT http_byte_range_header MATCHES "std::get_if<HttpByteRangeIgnored>" OR
       NOT http_byte_range_header MATCHES
           "std::get_if<HttpByteRangeUnsatisfiable>" OR
       NOT http_byte_range_header MATCHES "std::get_if<HttpResolvedByteRange>" OR
       NOT http_byte_range_header MATCHES "resolveHttpByteRange" OR
       NOT http_byte_range_header MATCHES "httpAsciiEqualsIgnoreCase" OR
       NOT http_byte_range_header MATCHES "std::errc::result_out_of_range" OR
       NOT http_byte_range_header MATCHES "representationLength == 0" OR
       NOT http_byte_range_header MATCHES "length_ == 0")
        boundary_error("HTTP byte-range resolver lost its discriminated RFC contract"
            "ignored/unsatisfiable outcomes must be payload-free; only one bounded nonempty range owns slicing coordinates")
    endif()
    if(NOT web_file_response_source MATCHES "resolveHttpByteRange" OR
       NOT web_file_response_source MATCHES "rangeResolution[.]ignored[(][)]" OR
       NOT web_file_response_source MATCHES
           "rangeResolution[.]unsatisfiable[(][)]" OR
       NOT web_file_response_source MATCHES "rangeResolution[.]resolved[(][)]" OR
       NOT web_file_response_source MATCHES "resolved[.]offset[(][)]" OR
       NOT web_file_response_source MATCHES "resolved[.]length[(][)]")
        boundary_error("ruvia-web bypasses the HTTP byte-range resolution"
            "file responses must map the three typed outcomes without reparsing Range")
    endif()
    if(NOT http_byte_range_test MATCHES
           "byte_range_resolution_is_discriminated" OR
       NOT http_byte_range_test MATCHES
           "!std::default_initializable<HttpByteRangeResolution>" OR
       NOT http_byte_range_test MATCHES
           "!HasByteRangeOutcomeField<HttpByteRangeResolution>" OR
       NOT http_byte_range_test MATCHES
           "HasByteRangeOffsetAccessor<HttpResolvedByteRange>" OR
       NOT http_byte_range_test MATCHES
           "byte_range_unit_is_case_insensitive" OR
       NOT http_byte_range_test MATCHES
           "byte_range_huge_decimal_numerals_preserve_semantics" OR
       NOT http_byte_range_test MATCHES
           "byte_range_empty_representation_uses_ignore_policy" OR
       NOT http_content_range_test MATCHES "Bytes=5-9" OR
       NOT http_content_range_test MATCHES "empty[.]txt" OR
       NOT http_range_package_consumer MATCHES "HttpByteRangeResolution" OR
       NOT http_range_package_consumer MATCHES "!HasByteRangeOutcomeField" OR
       NOT http_range_package_consumer MATCHES
           "installedResolvedRange[.]resolved[(][)]->offset[(][)]" OR
       NOT http_range_package_consumer MATCHES
           "installedUnsatisfiableRange[.]unsatisfiable[(][)]")
        boundary_error("HTTP byte-range resolution contract is under-tested"
            "unit, Web integration, and installed-package consumers must pin exclusive outcomes and RFC numeric/unit edges")
    endif()
endif()
check_files_no_match("ruvia-web response stream sink must not serialize HTTP/1 chunk/trailer bytes"
    "${RULE_WEB_HTTP1_STREAM_FRAMING_BYTES}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h")
check_files_no_match("ruvia-web stream body reader must drive the HTTP chunked decoder"
    "${RULE_WEB_CHUNKED_PROTOCOL_PARSER}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderChunked.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderPipeline.inl")
check_files_no_match("ruvia-web buffered multipart facade must use parseMultipartBody"
    "${RULE_WEB_MULTIPART_PROTOCOL_PARSER}"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
check_files_no_match("multipart parsing must use one typed boundary and chunk lifecycle"
    "${RULE_STALE_MULTIPART_API}" ${EDGE_REFERENCE_SOURCE})
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/MultipartParser.h"
    multipart_public_api)
if(NOT multipart_public_api MATCHES "class MultipartBoundary final" OR
   NOT multipart_public_api MATCHES "std::array<char, kMaxSize>" OR
   NOT multipart_public_api MATCHES "enum class MultipartChunkPhase" OR
   NOT multipart_public_api MATCHES "class MultipartPollNeedInput final" OR
   NOT multipart_public_api MATCHES "class MultipartPollDone final" OR
   NOT multipart_public_api MATCHES "enum class MultipartParseError" OR
   NOT multipart_public_api MATCHES "class MultipartPollFailure final" OR
   NOT multipart_public_api MATCHES "class MultipartPollResult final" OR
   NOT multipart_public_api MATCHES "using Value = std::variant" OR
   NOT multipart_public_api MATCHES "std::get_if<MultipartStreamPart>" OR
   NOT multipart_public_api MATCHES "std::get_if<MultipartPollFailure>" OR
   NOT multipart_public_api MATCHES "multipartParseErrorMessage" OR
   NOT multipart_public_api MATCHES "MultipartParser[(]MultipartBoundary boundary" OR
   NOT multipart_public_api MATCHES "void feed[(]std::string_view chunk[)]" OR
   NOT multipart_public_api MATCHES "void finishInput[(][)] noexcept" OR
   NOT multipart_public_api MATCHES "parseMultipartBody[(][^)]*MultipartBoundary boundary")
    boundary_error("multipart public API lost its typed sans-I/O contract"
        "MultipartParser.h must validate boundary ownership once and expose discriminated phase/need-input/part/done results")
endif()
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/MultipartParsing.h"
    multipart_protocol_helpers)
if(NOT multipart_protocol_helpers MATCHES "class HttpMultipartDelimiterNoMatch final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartDelimiterNeedInput final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartPartDelimiter final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartCloseDelimiter final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartDelimiterResult final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartBoundaryParseFailure final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartBoundaryParseResult final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartPartHeaderParseFailure final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartPartHeaderParseResult final" OR
   NOT multipart_protocol_helpers MATCHES "using Value = std::variant" OR
   NOT multipart_protocol_helpers MATCHES "httpMatchMultipartDelimiterLine" OR
   NOT multipart_protocol_helpers MATCHES "std::get_if<HttpMultipartDelimiterNeedInput>")
    boundary_error("multipart delimiter and Content-Type decisions escaped the HTTP core"
        "ruvia-http must own discriminated boundary/header extraction and an input-aware shared delimiter scanner")
endif()
file(READ "${RUVIA_ROOT}/ruvia-http/src/MultipartReader.cpp"
    multipart_parser_implementation)
string(FIND "${multipart_parser_implementation}"
    "MultipartPollResult MultipartParser::poll" multipart_poll_offset)
if(multipart_poll_offset EQUAL -1)
    boundary_error("multipart poll implementation is missing"
        "MultipartReader.cpp must implement the typed MultipartPollResult driver")
else()
    string(SUBSTRING "${multipart_parser_implementation}"
        ${multipart_poll_offset} -1 multipart_incremental_implementation)
    if(multipart_incremental_implementation MATCHES
           "throw[ \\t]+std::invalid_argument" OR
       NOT multipart_incremental_implementation MATCHES
           "MultipartPollResult::makeFailure" OR
       NOT multipart_parser_implementation MATCHES
           "multipartParseErrorMessage")
        boundary_error("incremental multipart wire failures bypass typed results"
            "poll/processBoundary/processHeaders/readBodyChunk must return MultipartPollFailure; only the complete-body convenience may throw invalid_argument")
    endif()
endif()
file(READ "${RUVIA_ROOT}/ruvia-web/src/http/MultipartReader.cpp"
    multipart_web_driver)
if(NOT multipart_web_driver MATCHES "parser_[.]finishInput[(][)]" OR
   NOT multipart_web_driver MATCHES "result[.]part[(][)]" OR
   NOT multipart_web_driver MATCHES "result[.]done[(][)]" OR
   NOT multipart_web_driver MATCHES "result[.]needInput[(][)]" OR
   NOT multipart_web_driver MATCHES "result[.]failure[(][)]" OR
   NOT multipart_web_driver MATCHES "failure->error[(][)]" OR
   NOT multipart_web_driver MATCHES "HttpProtocolError" OR
   NOT multipart_web_driver MATCHES "while [(]!bodyEnded_ && co_await bodyReader_[.]read[(][)][)]")
    boundary_error("multipart Web facade stopped driving the complete HTTP body lifecycle"
        "the runtime must drive typed results, signal EOF to the protocol parser, and drain RFC 2046 epilogue bytes")
endif()
file(READ "${RUVIA_ROOT}/tests/unit_multipart.cpp" multipart_unit_test)
file(READ "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    multipart_package_consumer)
file(READ "${RUVIA_ROOT}/examples/api_surface.cpp" multipart_api_surface)
if(NOT multipart_unit_test MATCHES "multipart_parser_commits_an_eof_close_only_after_finish_input" OR
   NOT multipart_unit_test MATCHES "multipart_parser_reports_typed_incomplete_body" OR
   NOT multipart_unit_test MATCHES "multipart_part_header_result_is_discriminated" OR
   NOT multipart_unit_test MATCHES "default_initializable<ruvia::MultipartPollResult>" OR
   NOT multipart_unit_test MATCHES "HasMultipartLineBytes" OR
   NOT multipart_package_consumer MATCHES "ruvia::MultipartPollResult" OR
   NOT multipart_package_consumer MATCHES "HttpMultipartDelimiterResult" OR
   NOT multipart_api_surface MATCHES "HasMultipartPollResultAccessors<ruvia::MultipartPollResult>")
    boundary_error("typed multipart result ownership is insufficiently tested"
        "unit, example, and installed-consumer contracts must pin poll, delimiter, boundary, and part-header alternatives")
endif()
check_files_no_match("ruvia-web handshake writers must only submit HTTP-owned parts"
    "${RULE_WEB_HANDSHAKE_PROTOCOL_BYTES}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketHandshake.h")
check_files_no_match("ruvia-web HTTP/2 stream sink must not encode trailer protocol bytes"
    "${RULE_WEB_HTTP2_TRAILER_PROTOCOL}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
check_files_no_match("ruvia-web stream state must not validate response trailer protocol fields"
    "${RULE_WEB_TRAILER_PROTOCOL_VALIDATION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamState.h")
check_files_no_match("ruvia-web WebSocket transports must flush opaque HTTP-owned bytes"
    "${RULE_WEB_WS_SPLIT_FRAME_TRANSPORT}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnection.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionWrite.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketSocketTransport.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
check_files_no_match("WebSocket close and transport end must use one typed protocol plan"
    "${RULE_STALE_WS_CLOSE_CHAIN}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsConnection.h"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnection.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionRead.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionWrite.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketSocketTransport.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h"
    "${RUVIA_ROOT}/tests/unit_ws_connection.cpp")
check_files_no_match("WebSocket events must remain optional and discriminated"
    "${RULE_STALE_WS_EVENT_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsEvent.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsConnection.h"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionRead.inl")
check_files_no_match("WebSocket inbound parsing must remain nonthrowing and discriminated"
    "${RULE_STALE_WS_INBOUND_RESULT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketUtils.h"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/HttpWebSocketValidation.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp"
    "${RUVIA_ROOT}/tests/unit_websocket_frame.cpp"
    "${RUVIA_ROOT}/tests/unit_websocket_assembler.cpp"
    "${RUVIA_ROOT}/tests/unit_websocket_close.cpp"
    "${RUVIA_ROOT}/tests/unit_ws_connection.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")
check_files_no_match("ruvia-http must not own WebSocket timer/runtime policy"
    "${RULE_HTTP_WS_RUNTIME_POLICY}" ${HTTP_SOURCE})
check_files_no_match("WebSocket liveness must abort its transport, not the scanner owner"
    "${RULE_WS_SCANNER_OWNER_ABORT}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionHeartbeat.inl")
check_files_no_match("ruvia-web must drive the HTTP-owned HTTP/1 stream plan"
    "${RULE_WEB_HTTP1_STREAM_PLAN}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h")
check_files_no_match("ruvia-web response state must use HTTP-owned persistence finalization"
    "${RULE_WEB_HTTP1_RESPONSE_FINALIZATION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseState.h")
check_files_no_match("HTTP/1 connection lifetime must use one typed plan"
    "${RULE_STALE_HTTP1_CONNECTION_LIFETIME}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerConnectionPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseState.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBufferedRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h"
    "${RUVIA_ROOT}/tests/unit_http_server_request_state.cpp"
    "${RUVIA_ROOT}/tests/unit_response_head_emit.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP connection fields must use the shared typed state"
    "${RULE_STALE_HTTP_CONNECTION_FIELD_STATE}"
    ${HTTP_SOURCE} ${WEB_SOURCE})
check_files_no_match("HTTP/1 request-body framing must use one typed plan"
    "${RULE_STALE_HTTP1_REQUEST_BODY_SPLIT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1RequestBodyPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerRequestParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpLazyBufferedBody.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderContentLength.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderPipeline.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBufferedRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerRequestState.h"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/1 request-body plans must use exclusive alternatives"
    "${RULE_STALE_HTTP1_REQUEST_BODY_MODE_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1RequestBodyPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerRequestParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderContentLength.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderPipeline.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerRequestState.h"
    "${RUVIA_ROOT}/tests/unit_http1_parser.cpp"
    "${RUVIA_ROOT}/tests/unit_request_body_decoding.cpp"
    "${RUVIA_ROOT}/tests/unit_http_server_request_state.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/examples/api_surface.cpp")
check_files_no_match("HTTP/1 request-body plans must use parser-only constructors"
    "${RULE_STALE_HTTP1_REQUEST_BODY_FACTORIES}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1RequestBodyPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerRequestParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp")
check_files_no_match("server Expect semantics must use one cross-version typed state"
    "${RULE_STALE_SERVER_EXPECTATION_STATE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpParseError.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpParseError.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HeaderTokenUtils.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpHeaderBlockParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1RequestBodyPlan.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("HTTP message protocol version must use one typed control datum"
    "${RULE_STALE_HTTP_PROTOCOL_VERSION_STATE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpRequest.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpRequestInternal.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientAccess.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerConnectionPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestBuilder.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpFinalResponseControlPlan.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/HttpWebSocketValidation.cpp"
    "${RUVIA_ROOT}/examples/api_surface.cpp"
    "${RUVIA_ROOT}/tests/unit_request_access.cpp")
check_files_no_match("HTTP/1 client requests must use one typed writer contract"
    "${RULE_STALE_HTTP1_CLIENT_REQUEST_SPLIT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientRequestWriter.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/Http1ClientRequestWriter.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_request.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_redirect.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/tests/smoke_http_target.cpp")
check_files_no_match("outbound request content must use exclusive alternatives"
    "${RULE_STALE_OUTBOUND_REQUEST_CONTENT_MODE_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientRequestWriter.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/Http1ClientRequestWriter.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestContent.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_request.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/examples/api_surface.cpp")
check_files_no_match("HTTP/1 client response framing must use one typed plan"
    "${RULE_STALE_HTTP1_CLIENT_RESPONSE_SPLIT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/1 client response plans must use exclusive alternatives"
    "${RULE_STALE_HTTP1_CLIENT_RESPONSE_MODE_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_request.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/tests/smoke_http_target.cpp")
check_files_no_match("HTTP/1 client response parsing must use the public discriminated API"
    "${RULE_STALE_HTTP1_CLIENT_RESPONSE_PARSER_API}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/1 request and response must share Content-Length parsing"
    "${RULE_STALE_HTTP_CONTENT_LENGTH_SPLIT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpHeaderBlockParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp")
check_files_no_match("HTTP/1 request and response must share Transfer-Encoding parsing"
    "${RULE_STALE_HTTP_TRANSFER_ENCODING_SPLIT}"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp")
check_files_no_match("HTTP client method semantics must remain case-sensitive"
    "${RULE_STALE_HTTP_CLIENT_METHOD_CASE_FOLD}"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp")
check_files_no_match("ruvia-web must not pass loose response-body protocol booleans"
    "${RULE_WEB_RESPONSE_BODY_PROTOCOL_BOOL}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpBufferedResponse.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamState.h")
check_files_no_match("ruvia-web HTTP/2 runtime must not recompute the response write plan"
    "${RULE_WEB_H2_RESPONSE_PLAN_DUPLICATION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("ruvia-web must not decide HEAD response body semantics"
    "${RULE_WEB_HEAD_BODY_DECISION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpBufferedResponse.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
check_files_no_match("HTTP/1 response-head framing must not collapse to a boolean"
    "${RULE_STALE_HTTP1_RESPONSE_HEAD_SCALAR}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ResponseHeadPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHead.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHeadPolicy.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseStreamHead.h"
    "${RUVIA_ROOT}/ruvia-http/src/server/HttpResponseHead.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h")
check_files_no_match("HTTP/2 send path must not restore ambiguous retry ownership"
    "${RULE_STALE_H2_SEND_API}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
check_files_no_match("HTTP/2 local content accounting must use exclusive alternatives"
    "${RULE_STALE_H2_LOCAL_CONTENT_MODE_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2LocalContentState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
check_files_no_match("HTTP/2 local send permission must use one exclusive state"
    "${RULE_STALE_H2_LOCAL_SEND_PRODUCT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2LocalSendState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamLifecycle.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamTable.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2BodyQueue.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
check_files_no_match("HTTP/2 remote receive permission must use one exclusive state"
    "${RULE_STALE_H2_REMOTE_RECEIVE_PRODUCT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RemoteReceiveState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamLifecycle.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamRequestState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2BodyState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2BodyQueue.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
check_files_no_match("HTTP/2 remote content accounting must use exclusive alternatives"
    "${RULE_STALE_H2_REMOTE_CONTENT_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RemoteContentState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2BodyState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2WebSocketHandshake.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
check_files_no_match("response trailers must remain one terminal section"
    "${RULE_STALE_RESPONSE_TRAILER_SIDE_CHANNEL}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Streaming.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/StreamingInternal.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/tests/unit_streaming.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
check_files_no_match("response-stream runtime must consume the typed commit plan"
    "${RULE_STALE_RESPONSE_STREAM_COMMIT_BOOL}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamState.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
check_files_no_match("HTTP/2 must not restore split discard state or deprecated priority semantics"
    "${RULE_STALE_H2_FIELD_BLOCK_STATE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2HeaderContinuation.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
check_files_no_match("HTTP/2 CONNECT must use exclusive tunnel alternatives"
    "${RULE_STALE_H2_CONNECT_STATE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2TunnelState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamRequestState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2BodyState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestBuilder.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2WebSocketHandshake.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("HTTP/2 must not restore split preface APIs or wire/accounting knobs"
    "${RULE_STALE_H2_PREFACE_API}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/2 client request heads must allocate and submit atomically"
    "${RULE_STALE_H2_CLIENT_STREAM_API}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connect.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/2 GOAWAY lifecycle must remain typed and core-owned"
    "${RULE_STALE_H2_GOAWAY_API}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/SansIoDriver.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/2 events must remain optional and discriminated"
    "${RULE_STALE_H2_EVENT_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Event.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("HTTP/2 feed must remain a direct all-or-nothing ownership enum"
    "${RULE_STALE_H2_FEED_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connect.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/2 request-head submission must remain discriminated"
    "${RULE_STALE_H2_REQUEST_HEAD_SUBMIT_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connect.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")
check_files_no_match("HTTP/2 response-head submission must remain discriminated"
    "${RULE_STALE_H2_RESPONSE_HEAD_SUBMIT_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/2 peer-setting application must remain discriminated"
    "${RULE_STALE_H2_PEER_SETTING_APPLY_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2PeerSettings.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")
check_files_no_match("HTTP/2 DATA must keep connection-first receive-window accounting"
    "${RULE_STALE_H2_DATA_FLOW_ACCOUNTING}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2FlowControl.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_flow_control.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
check_files_no_match("205 Reset Content must not regain a sendable body"
    "${RULE_STALE_205_RESPONSE_BODY}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHeadPolicy.h"
    "${RUVIA_ROOT}/tests/unit_response_head_policy.cpp")

set(HTTP_RESPONSE_HEAD_POLICY
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHeadPolicy.h")
if(EXISTS "${HTTP_RESPONSE_HEAD_POLICY}")
    file(READ "${HTTP_RESPONSE_HEAD_POLICY}" http_response_head_policy)
    if(NOT http_response_head_policy MATCHES "statusCode == 205" OR
       NOT http_response_head_policy MATCHES "ResponseWritePolicy::zeroLengthContent")
        boundary_error("205 Reset Content bypasses the shared response policy"
            "205 must suppress content while retaining writer-owned zero-length framing")
    endif()
endif()

set(HTTP1_RESPONSE_HEAD_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ResponseHeadPlan.h")
set(HTTP_RESPONSE_HEAD_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHead.h")
set(HTTP_RESPONSE_STREAM_HEAD
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseStreamHead.h")
set(HTTP1_SERVER_SEMANTICS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h")
set(WEB_RESPONSE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h")
set(WEB_RESPONSE_STREAM_SINK
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h")
set(WEB_RESPONSE_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
foreach(http1_response_head_contract_file IN ITEMS
        "${HTTP1_RESPONSE_HEAD_PLAN}"
        "${HTTP_RESPONSE_HEAD_HEADER}"
        "${HTTP_RESPONSE_STREAM_HEAD}"
        "${HTTP1_SERVER_SEMANTICS}"
        "${WEB_RESPONSE_WRITER}"
        "${WEB_RESPONSE_STREAM_SINK}"
        "${WEB_RESPONSE_SESSION}")
    if(NOT EXISTS "${http1_response_head_contract_file}")
        file(RELATIVE_PATH relative
            "${RUVIA_ROOT}" "${http1_response_head_contract_file}")
        boundary_error("typed HTTP/1 response-head call chain is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP1_RESPONSE_HEAD_PLAN}" AND
   EXISTS "${HTTP_RESPONSE_HEAD_HEADER}" AND
   EXISTS "${HTTP_RESPONSE_STREAM_HEAD}" AND
   EXISTS "${HTTP1_SERVER_SEMANTICS}" AND
   EXISTS "${WEB_RESPONSE_WRITER}" AND
   EXISTS "${WEB_RESPONSE_STREAM_SINK}" AND
   EXISTS "${WEB_RESPONSE_SESSION}")
    file(READ "${HTTP1_RESPONSE_HEAD_PLAN}" http1_response_head_plan)
    file(READ "${HTTP_RESPONSE_HEAD_HEADER}" http_response_head_header)
    file(READ "${HTTP_RESPONSE_STREAM_HEAD}" http_response_stream_head)
    file(READ "${HTTP1_SERVER_SEMANTICS}" http1_response_head_semantics)
    file(READ "${WEB_RESPONSE_WRITER}" web_response_writer)
    file(READ "${WEB_RESPONSE_STREAM_SINK}" web_response_stream_sink)
    file(READ "${WEB_RESPONSE_SESSION}" web_response_session)
    set(http1_response_head_missing)
    foreach(http1_head_probe IN ITEMS
            "class Http1BufferedResponseHead final"
            "class Http1ChunkedResponseStreamHead final"
            "class Http1CloseDelimitedResponseStreamHead final"
            "using Framing = std::variant"
            "std::get_if<Http1BufferedResponseHead>"
            "std::get_if<Http1ChunkedResponseStreamHead>"
            "std::get_if<Http1CloseDelimitedResponseStreamHead>"
            "HttpResponseBodyPlan bodyPlan_"
            "std::uint64_t contentLength_"
            "HttpProtocolVersion protocolVersion_"
            "class Http1BufferedResponsePlan final"
            "http1BufferedResponsePlan")
        if(NOT http1_response_head_plan MATCHES "${http1_head_probe}")
            list(APPEND http1_response_head_missing
                "plan:${http1_head_probe}")
        endif()
    endforeach()
    if(NOT http_response_head_header MATCHES
           "const Http1ResponseHeadPlan& plan")
        list(APPEND http1_response_head_missing "head-signature")
    endif()
    foreach(http1_stream_probe IN ITEMS
            "writerOwnsHttp1Chunked"
            "response[.]header[(]\"Transfer-Encoding\", std::nullopt[)]"
            "response[.]header[(]\"Content-Length\", std::nullopt[)]")
        if(NOT http_response_stream_head MATCHES "${http1_stream_probe}")
            list(APPEND http1_response_head_missing
                "stream:${http1_stream_probe}")
        endif()
    endforeach()
    foreach(http1_semantics_probe IN ITEMS
            "Http1ResponseHeadPlan responseHeadPlan_"
            "http1ChunkedResponseStreamHeadPlan"
            "http1CloseDelimitedResponseStreamHeadPlan")
        if(NOT http1_response_head_semantics MATCHES "${http1_semantics_probe}")
            list(APPEND http1_response_head_missing
                "semantics:${http1_semantics_probe}")
        endif()
    endforeach()
    if(NOT web_response_writer MATCHES
           "const Http1BufferedResponsePlan& responsePlan" OR
       NOT web_response_writer MATCHES "responsePlan[.]headPlan[(][)]" OR
       web_response_writer MATCHES "http1BufferedResponseHeadPlan")
        list(APPEND http1_response_head_missing "web-buffered-driver")
    endif()
    if(NOT web_response_session MATCHES "http1BufferedResponsePlan" OR
       NOT web_response_session MATCHES
           "responsePreparation[.]writePlan[(][)]" OR
       NOT web_response_session MATCHES "connectionPlan")
        list(APPEND http1_response_head_missing
            "web-buffered-composition")
    endif()
    if(NOT web_response_stream_sink MATCHES
           "streamHead[.]responseHeadPlan[(][)]")
        list(APPEND http1_response_head_missing "web-stream-driver")
    endif()
    if(http1_response_head_missing)
        string(JOIN ", " http1_response_head_missing_text
            ${http1_response_head_missing})
        boundary_error("HTTP/1 response-head framing escaped its exclusive plan"
            "buffered, chunked-stream, and close-delimited-stream heads must remain exclusive; the prepared HTTP/1 plan owns canonical framing and Web may only drive it; missing ${http1_response_head_missing_text}")
    endif()
endif()

set(HTTP_RESPONSE_HEAD_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/server/HttpResponseHead.cpp")
if(EXISTS "${HTTP_RESPONSE_HEAD_SOURCE}")
    file(READ "${HTTP_RESPONSE_HEAD_SOURCE}" http1_typed_response_head_source)
    if(NOT http1_typed_response_head_source MATCHES
           "plan[.]chunkedStream[(][)]" OR
       NOT http1_typed_response_head_source MATCHES
           "plan[.]closeDelimitedStream[(][)]" OR
       NOT http1_typed_response_head_source MATCHES
           "plan[.]protocolVersion[(][)]" OR
       NOT http1_typed_response_head_source MATCHES
           "buffered->contentLength[(][)]" OR
       NOT http1_typed_response_head_source MATCHES "HTTP/1[.]0" OR
       NOT http1_typed_response_head_source MATCHES "HTTP/1[.]1" OR
       NOT http1_typed_response_head_source MATCHES
           "kChunkedTransferEncodingHeader" OR
       NOT http1_typed_response_head_source MATCHES
           "knownBit == kResponseHeaderTransferEncoding" OR
       NOT http1_typed_response_head_source MATCHES
           "knownBit == kResponseHeaderContentLength")
        boundary_error("HTTP/1 response-head emitter bypasses its typed framing plan"
            "status-line version, canonical buffered length, chunked ownership, and close-delimited TE/CL filtering must derive from Http1ResponseHeadPlan")
    endif()
endif()
set(HTTP2_RESPONSE_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeaders.h")
set(HTTP2_RESPONSE_HEAD_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeadPlan.h")
set(HTTP2_RESPONSE_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
check_files_no_match("HTTP/2 response-head encoding must not restore scalar Content-Length ownership"
    "${RULE_STALE_HTTP2_RESPONSE_HEAD_SCALAR}"
    "${HTTP2_RESPONSE_HEADERS}"
    "${HTTP2_RESPONSE_CONNECTION_SOURCE}")
if(NOT EXISTS "${HTTP2_RESPONSE_HEAD_PLAN}" OR
   NOT EXISTS "${HTTP2_RESPONSE_HEADERS}" OR
   NOT EXISTS "${HTTP2_RESPONSE_CONNECTION_SOURCE}")
    boundary_error("typed HTTP/2 response-head plan is incomplete"
        "the plan, HPACK encoder, and connection driver are all required")
else()
    file(READ "${HTTP2_RESPONSE_HEAD_PLAN}" http2_response_head_plan)
    file(READ "${HTTP2_RESPONSE_HEADERS}" http2_response_head_encoder)
    file(READ "${HTTP2_RESPONSE_CONNECTION_SOURCE}"
        http2_response_head_connection)
    set(http2_response_head_missing)
    foreach(http2_head_probe IN ITEMS
            "class Http2CanonicalResponseContentLength final"
            "class Http2ExplicitResponseContentLength final"
            "class Http2AbsentResponseContentLength final"
            "class Http2ForbiddenResponseContentLength final"
            "class Http2ResponseHeadPlan final"
            "using ContentLength = std::variant"
            "std::get_if<Http2CanonicalResponseContentLength>"
            "std::get_if<Http2ExplicitResponseContentLength>"
            "std::get_if<Http2AbsentResponseContentLength>"
            "std::get_if<Http2ForbiddenResponseContentLength>"
            "class Http2ResponseHeadPlanResult final"
            "std::get_if<Http2ResponseHeadPlan>"
            "HttpResponseBodyPlan bodyPlan_"
            "http2BufferedResponseHeadPlan"
            "http2StreamingResponseHeadPlan"
            "http2ConnectResponseHeadPlan")
        if(NOT http2_response_head_plan MATCHES "${http2_head_probe}")
            list(APPEND http2_response_head_missing
                "plan:${http2_head_probe}")
        endif()
    endforeach()
    foreach(http2_encoder_probe IN ITEMS
            "const Http2ResponseHeadPlan& plan"
            "knownBit == kResponseHeaderContentLength"
            "plan[.]canonicalContentLength[(][)]"
            "plan[.]explicitContentLength[(][)]")
        if(NOT http2_response_head_encoder MATCHES
               "${http2_encoder_probe}")
            list(APPEND http2_response_head_missing
                "encoder:${http2_encoder_probe}")
        endif()
    endforeach()
    foreach(http2_connection_probe IN ITEMS
            "http2BufferedResponseHeadPlan"
            "http2StreamingResponseHeadPlan"
            "http2ConnectResponseHeadPlan"
            "headPlan->explicitContentLength[(][)]")
        if(NOT http2_response_head_connection MATCHES
               "${http2_connection_probe}")
            list(APPEND http2_response_head_missing
                "connection:${http2_connection_probe}")
        endif()
    endforeach()
    if(http2_response_head_encoder MATCHES
           "Http2ExplicitContentLengthStatus|http2ExplicitResponseContentLength|emitAutoContentLength|std::uint64_t[ 	]+autoContentLength" OR
       http2_response_head_connection MATCHES
           "Http2ExplicitContentLengthStatus|http2ExplicitResponseContentLength|emitAutoContentLength")
        list(APPEND http2_response_head_missing "stale-scalar-api")
    endif()
    if(http2_response_head_missing)
        string(JOIN ", " http2_response_head_missing_text
            ${http2_response_head_missing})
        boundary_error("HTTP/2 response-head Content-Length escaped its exclusive plan"
            "canonical, explicit, absent, and forbidden ownership must be exclusive; HPACK and DATA accounting consume the same plan; missing ${http2_response_head_missing_text}")
    endif()
endif()
set(HTTP_STATUS_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpStatus.h")
set(HTTP_RESPONSE_MODEL_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpResponse.h")
set(HTTP_RESPONSE_MODEL_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/HttpResponse.cpp")
set(HTTP1_INTERIM_RESPONSE_WRITER
    "${RUVIA_ROOT}/ruvia-http/src/server/Http1InterimResponseWriter.cpp")
set(WEB_CONTEXT_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h")
set(WEB_CONTEXT_INLINE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.inl")
set(WEB_CONTEXT_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextModel.h")
set(WEB_CONTEXT_RESPONSE_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextResponse.cpp")
set(WEB_ERROR_NORMALIZE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/HttpErrorNormalize.h")

foreach(response_status_contract_file IN ITEMS
        "${HTTP_STATUS_HEADER}"
        "${HTTP_RESPONSE_MODEL_HEADER}"
        "${HTTP_RESPONSE_MODEL_SOURCE}"
        "${HTTP_RESPONSE_HEAD_SOURCE}"
        "${HTTP1_INTERIM_RESPONSE_WRITER}"
        "${HTTP2_RESPONSE_HEADERS}"
        "${WEB_CONTEXT_HEADER}"
        "${WEB_CONTEXT_INLINE}"
        "${WEB_CONTEXT_MODEL}"
        "${WEB_CONTEXT_RESPONSE_SOURCE}"
        "${WEB_ERROR_NORMALIZE}")
    if(NOT EXISTS "${response_status_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${response_status_contract_file}")
        boundary_error("version-neutral response-status contract is incomplete"
            "${relative} is required")
    endif()
endforeach()

check_files_no_match("generic response model must not own an HTTP/1 reason phrase"
    "${RULE_STALE_RESPONSE_REASON_PHRASE}"
    "${HTTP_STATUS_HEADER}"
    "${HTTP_RESPONSE_MODEL_HEADER}"
    "${HTTP_RESPONSE_MODEL_SOURCE}"
    "${HTTP_RESPONSE_HEAD_SOURCE}"
    "${HTTP1_INTERIM_RESPONSE_WRITER}")
check_files_no_match("Context response helpers must remain status-code only"
    "${RULE_STALE_CONTEXT_REASON_PHRASE}"
    "${WEB_CONTEXT_HEADER}"
    "${WEB_CONTEXT_INLINE}"
    "${WEB_CONTEXT_MODEL}"
    "${WEB_CONTEXT_RESPONSE_SOURCE}")
check_files_no_match("HTTP/2 response encoding must not consume a reason phrase"
    "${RULE_HTTP2_REASON_PHRASE}"
    "${HTTP2_RESPONSE_HEADERS}"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")

if(EXISTS "${HTTP_STATUS_HEADER}" AND
   EXISTS "${HTTP_RESPONSE_MODEL_HEADER}" AND
   EXISTS "${HTTP_RESPONSE_HEAD_SOURCE}" AND
   EXISTS "${HTTP1_INTERIM_RESPONSE_WRITER}" AND
   EXISTS "${HTTP2_RESPONSE_HEADERS}" AND
   EXISTS "${WEB_CONTEXT_HEADER}" AND
   EXISTS "${WEB_ERROR_NORMALIZE}")
    file(READ "${HTTP_STATUS_HEADER}" http_status_header)
    file(READ "${HTTP_RESPONSE_MODEL_HEADER}" http_response_model_header)
    file(READ "${HTTP_RESPONSE_HEAD_SOURCE}" http1_response_head_source)
    file(READ "${HTTP1_INTERIM_RESPONSE_WRITER}" http1_interim_response_writer)
    file(READ "${HTTP2_RESPONSE_HEADERS}" http2_status_response_headers)
    file(READ "${WEB_CONTEXT_HEADER}" web_context_status_header)
    file(READ "${WEB_ERROR_NORMALIZE}" web_error_status_normalize)
    if(NOT http_status_header MATCHES "httpReasonPhrase" OR
       NOT http_status_header MATCHES "default:[ \t]*return[ \t]*[{][}];" OR
       NOT http_response_model_header MATCHES
           "void status[(]std::uint16_t statusCode[)];" OR
       NOT http1_response_head_source MATCHES
           "httpReasonPhrase[(]response[.]status[(][)][)]" OR
       NOT http1_response_head_source MATCHES "sink[.]append[(]' '[)]" OR
       NOT http1_interim_response_writer MATCHES
           "httpReasonPhrase[(]response[.]status[(][)][)]" OR
       NOT http1_interim_response_writer MATCHES
           "[*]cursor[+][+][ \t]*=[ \t]*' '" OR
       NOT http2_status_response_headers MATCHES
           "encodeStatus[(]headerBlock, response[.]status[(][)][)]" OR
       NOT web_context_status_header MATCHES
           "struct ResponseInit final[ \t\r\n]*[{][ \t\r\n]*std::uint16_t status" OR
       NOT web_context_status_header MATCHES "ResponseHeaderInit headers" OR
       NOT web_error_status_normalize MATCHES "statusText = \"HTTP Error\"")
        boundary_error("response status and reason-phrase ownership split again"
            "messages carry only a status code; H1 derives an optional phrase, H2 emits :status, and Web labels stay presentation-only")
    endif()
endif()

set(RESPONSE_STATUS_MODEL_TEST
    "${RUVIA_ROOT}/tests/unit_http_response.cpp")
set(RESPONSE_REASON_PHRASE_TEST
    "${RUVIA_ROOT}/tests/unit_error.cpp")
set(RESPONSE_HEAD_REASON_PHRASE_TEST
    "${RUVIA_ROOT}/tests/unit_response_head_emit.cpp")
set(RESPONSE_ERROR_LABEL_TEST
    "${RUVIA_ROOT}/tests/unit_error_response.cpp")
set(RESPONSE_API_SURFACE_TEST
    "${RUVIA_ROOT}/examples/api_surface.cpp")
if(EXISTS "${RESPONSE_STATUS_MODEL_TEST}" AND
   EXISTS "${RESPONSE_REASON_PHRASE_TEST}" AND
   EXISTS "${RESPONSE_HEAD_REASON_PHRASE_TEST}" AND
   EXISTS "${RESPONSE_ERROR_LABEL_TEST}" AND
   EXISTS "${RESPONSE_API_SURFACE_TEST}")
    file(READ "${RESPONSE_STATUS_MODEL_TEST}" response_status_model_test)
    file(READ "${RESPONSE_REASON_PHRASE_TEST}" response_reason_phrase_test)
    file(READ "${RESPONSE_HEAD_REASON_PHRASE_TEST}" response_head_reason_phrase_test)
    file(READ "${RESPONSE_ERROR_LABEL_TEST}" response_error_label_test)
    file(READ "${RESPONSE_API_SURFACE_TEST}" response_status_api_surface)
    if(NOT response_status_model_test MATCHES
           "response_status_is_version_neutral_code_only" OR
       NOT response_reason_phrase_test MATCHES
           "http_reason_phrase_does_not_mislabel_extension_statuses" OR
       NOT response_head_reason_phrase_test MATCHES
           "response_head_extension_status_uses_an_empty_reason_phrase" OR
       NOT response_head_reason_phrase_test MATCHES "HTTP/1[.]1 299" OR
       NOT response_error_label_test MATCHES "HTTP Error" OR
       NOT response_status_api_surface MATCHES "HasResponseReasonPhraseSetter")
        boundary_error("response status/reason-phrase regression coverage is incomplete"
            "API shape, unknown-code phrase, H1 empty phrase, and Web-only error label all require direct coverage")
    endif()
endif()

set(HTTP1_RESPONSE_HEAD_POLICY_TEST
    "${RUVIA_ROOT}/tests/unit_response_head_policy.cpp")
set(HTTP1_RESPONSE_STREAM_PLAN_TEST
    "${RUVIA_ROOT}/tests/unit_http_server_request_state.cpp")
set(HTTP_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
if(EXISTS "${RESPONSE_HEAD_REASON_PHRASE_TEST}" AND
   EXISTS "${HTTP1_RESPONSE_HEAD_POLICY_TEST}" AND
   EXISTS "${HTTP1_RESPONSE_STREAM_PLAN_TEST}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${RESPONSE_HEAD_REASON_PHRASE_TEST}"
        http1_response_head_wire_test)
    file(READ "${HTTP1_RESPONSE_HEAD_POLICY_TEST}"
        http1_response_head_policy_test)
    file(READ "${HTTP1_RESPONSE_STREAM_PLAN_TEST}"
        http1_response_stream_plan_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}"
        http1_response_head_package_test)
    if(NOT http1_response_head_wire_test MATCHES
           "response_head_close_delimited_stream_rejects_declared_framing" OR
       NOT http1_response_head_wire_test MATCHES
           "http1_buffered_response_plan_owns_request_version_and_length" OR
       NOT http1_response_head_wire_test MATCHES "HTTP/1[.]0 200 OK" OR
       NOT http1_response_head_wire_test MATCHES
           "Transfer-Encoding: chunked" OR
       NOT http1_response_head_wire_test MATCHES
           "Content-Length: 8" OR
       NOT http1_response_head_policy_test MATCHES
           "http1_response_head_framing_is_an_exclusive_plan" OR
       NOT http1_response_head_policy_test MATCHES
           "Http1BufferedResponsePlan" OR
       NOT http1_response_stream_plan_test MATCHES
           "http1_prepared_stream_head_owns_exact_wire_framing" OR
       NOT http1_response_stream_plan_test MATCHES
           "responseHeadPlan[(][)][.]closeDelimitedStream[(][)]" OR
       NOT http1_response_stream_plan_test MATCHES
           "http10Wire[.]starts_with[(]\"HTTP/1[.]0" OR
       NOT http1_response_stream_plan_test MATCHES "failedHttp10" OR
       NOT http1_response_stream_plan_test MATCHES
           "failedHttp10[.]connectionPlan[.]protocolVersion[(][)]" OR
       NOT http1_response_head_package_test MATCHES
           "HasHttp1ResponseHeadAlternatives" OR
       NOT http1_response_head_package_test MATCHES
           "HasHttp1ProtocolVersion" OR
       NOT http1_response_head_package_test MATCHES
           "HasHttp1BufferedPlanComposition" OR
       NOT http1_response_head_package_test MATCHES
           "!HasStaleHttp1ResponseSignal" OR
       NOT http1_response_head_package_test MATCHES
           "!HasStaleHttp1ResponseHeadScalar" OR
       NOT http1_response_head_package_test MATCHES
           "!HasStalePreparedStreamPolicy")
        boundary_error("typed HTTP/1 response-head framing is under-tested"
            "wire tests, parser body-failure tests, prepared-plan tests, and installed consumers must pin exact HTTP/1.0 status-line ownership, canonical buffered length, chunked framing, TE/CL filtering, HEAD metadata, and removal of scalar/version-signal APIs")
    endif()
endif()

set(HTTP2_RESPONSE_HEAD_PLAN_TEST
    "${RUVIA_ROOT}/tests/unit_http2_response_headers.cpp")
set(HTTP2_RESPONSE_HEAD_CONNECTION_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
if(EXISTS "${HTTP2_RESPONSE_HEAD_PLAN_TEST}" AND
   EXISTS "${HTTP2_RESPONSE_HEAD_CONNECTION_TEST}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_RESPONSE_HEAD_PLAN_TEST}"
        http2_response_head_plan_test)
    file(READ "${HTTP2_RESPONSE_HEAD_CONNECTION_TEST}"
        http2_response_head_connection_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}"
        http2_response_head_package_test)
    if(NOT http2_response_head_plan_test MATCHES
           "http2_response_head_content_length_plan_is_exclusive" OR
       NOT http2_response_head_plan_test MATCHES
           "http2_response_headers_canonicalize_valid_explicit_content_length_once" OR
       NOT http2_response_head_plan_test MATCHES
           "http2_response_headers_reject_only_preserved_invalid_content_length" OR
       NOT http2_response_head_plan_test MATCHES
           "!std::is_default_constructible_v<Http2ResponseHeadPlan>" OR
       NOT http2_response_head_connection_test MATCHES
           "http2_connection_streaming_content_length_finish_and_trailers_are_exact" OR
       NOT http2_response_head_package_test MATCHES
           "HasHttp2ResponseHeadContentLengthAlternatives" OR
       NOT http2_response_head_package_test MATCHES
           "HasHttp2ResponseContentLengthValue" OR
       NOT http2_response_head_package_test MATCHES
           "!std::default_initializable<[ \t\r\n]*ruvia::detail::Http2ResponseHeadPlan>" OR
       NOT http2_response_head_package_test MATCHES
           "http2BufferedResponseHeadPlan" OR
       NOT http2_response_head_package_test MATCHES
           "http2StreamingResponseHeadPlan" OR
       NOT http2_response_head_package_test MATCHES
           "Http2ResponseHeadPlanError::kInvalidContentLength")
        boundary_error("typed HTTP/2 response-head plan is under-tested"
            "unit, connection, and installed-package tests must pin exclusive length ownership, single parsing, canonical wire bytes, invalid failure, and DATA accounting")
    endif()
endif()

if(EXISTS "${HTTP_RESPONSE_HEAD_SOURCE}" AND EXISTS "${HTTP2_RESPONSE_HEADERS}")
    file(READ "${HTTP_RESPONSE_HEAD_SOURCE}" http_response_head_source)
    file(READ "${HTTP2_RESPONSE_HEADERS}" http2_response_headers)
    if(NOT http_response_head_source MATCHES "canonicalContentLength" OR
       NOT http2_response_headers MATCHES "canonicalContentLength")
        boundary_error("205 zero-length canonicalization is incomplete across protocols"
            "HTTP/1 and HTTP/2 writers must replace application framing with length zero")
    endif()
endif()

set(HTTP_FINAL_RESPONSE_CONTROL_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpFinalResponseControlPlan.h")
set(HTTP1_SERVER_SEMANTICS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h")
set(HTTP2_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(HTTP2_HEADER_RULES
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2HeaderRules.h")
set(HTTP_RESPONSE_CONTENT_SEMANTICS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseContentSemantics.h")
set(HTTP_RESPONSE_WRITE_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseWritePlan.h")
if(NOT EXISTS "${HTTP_FINAL_RESPONSE_CONTROL_PLAN}" OR
   NOT EXISTS "${HTTP1_SERVER_SEMANTICS}" OR
   NOT EXISTS "${HTTP2_CONNECTION_SOURCE}" OR
   NOT EXISTS "${HTTP2_HEADER_RULES}" OR
   NOT EXISTS "${HTTP2_RESPONSE_HEADERS}")
    boundary_error("final response control plan is missing"
        "the shared result, H1 finalizer, H2 field rules, encoder, and all H2 submit paths are required")
else()
    file(READ "${HTTP_FINAL_RESPONSE_CONTROL_PLAN}" http_final_response_control_plan)
    file(READ "${HTTP1_SERVER_SEMANTICS}" http1_server_semantics)
    file(READ "${HTTP2_CONNECTION_SOURCE}" http2_connection_source)
    file(READ "${HTTP2_HEADER_RULES}" http2_header_rules)
    file(READ "${HTTP2_RESPONSE_HEADERS}" http2_response_headers)
    if(NOT http_final_response_control_plan MATCHES
           "class Http1FinalResponseControl final" OR
       NOT http_final_response_control_plan MATCHES
           "class Http2FinalResponseControl final" OR
       NOT http_final_response_control_plan MATCHES
           "class HttpFinalResponseControlPlan final" OR
       NOT http_final_response_control_plan MATCHES
           "using Protocol = std::variant" OR
       NOT http_final_response_control_plan MATCHES
           "class HttpFinalResponseControlPlanFailure final" OR
       NOT http_final_response_control_plan MATCHES
           "class HttpFinalResponseControlPlanResult final" OR
       NOT http_final_response_control_plan MATCHES
           "using Value = std::variant" OR
       NOT http_final_response_control_plan MATCHES
           "std::get_if<HttpFinalResponseControlPlan>" OR
       NOT http_final_response_control_plan MATCHES "statusCode == 426" OR
       NOT http_final_response_control_plan MATCHES "kUpgradeUnavailable" OR
       NOT http_final_response_control_plan MATCHES
           "kConnectionSpecificFieldForbidden" OR
       NOT http_final_response_control_plan MATCHES "HttpProtocolVersion protocolVersion" OR
       NOT http2_header_rules MATCHES
           "http2IsForbiddenResponseConnectionField" OR
       NOT http2_response_headers MATCHES
           "const Http2FinalResponseControl& control" OR
       http2_response_headers MATCHES
           "http2ResponseConnectionHeaderForbidden" OR
       http_final_response_control_plan MATCHES
           "${RULE_STALE_FINAL_RESPONSE_CONTROL_TUPLE}")
        boundary_error("final response status/Upgrade paths have diverged"
            "one discriminated result must own exact H1/H2 alternatives, typed failure, parsed H1 fields, and RFC 9113 connection-field rejection before encoding")
    endif()
    if(NOT http1_server_semantics MATCHES "controlResult[.]failure[(][)]" OR
       NOT http1_server_semantics MATCHES "controlResult[.]plan[(][)]" OR
       NOT http1_server_semantics MATCHES "controlPlan->http1[(][)]" OR
       NOT http1_server_semantics MATCHES "connectionOptions[(][)]" OR
       NOT http1_server_semantics MATCHES "upgradeProtocols[(][)]" OR
       http1_server_semantics MATCHES "http1ResponseConnectionOptions")
        boundary_error("HTTP/1 final response control was reparsed or flattened"
            "the finalizer must unwrap the shared result and consume the H1 alternative's already parsed Connection and Upgrade states")
    endif()
    string(REGEX MATCHALL "httpFinalResponseControlPlan[(]"
        http2_final_control_calls "${http2_connection_source}")
    list(LENGTH http2_final_control_calls http2_final_control_call_count)
    if(http2_final_control_call_count LESS 3 OR
       NOT http2_connection_source MATCHES "controlPlan->http2[(][)]" OR
       NOT http2_connection_source MATCHES
           "submitConnectResponseHead[^(]*[(]")
        boundary_error("HTTP/2 final response paths bypassed shared control planning"
            "buffered, streaming, and CONNECT final heads must each obtain the HTTP/2 control alternative before HPACK or stream mutation")
    endif()
endif()
set(FINAL_RESPONSE_CONTROL_TEST
    "${RUVIA_ROOT}/tests/unit_final_response_control.cpp")
set(HTTP2_FINAL_RESPONSE_HEADER_TEST
    "${RUVIA_ROOT}/tests/unit_http2_response_headers.cpp")
set(HTTP2_FINAL_RESPONSE_CONNECTION_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
set(HTTP2_CONNECT_RESPONSE_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connect.cpp")
set(HTTP_FINAL_CONTROL_PACKAGE_TEST
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
foreach(final_control_test_file IN ITEMS
        "${FINAL_RESPONSE_CONTROL_TEST}"
        "${HTTP2_FINAL_RESPONSE_HEADER_TEST}"
        "${HTTP2_FINAL_RESPONSE_CONNECTION_TEST}"
        "${HTTP2_CONNECT_RESPONSE_TEST}"
        "${HTTP_FINAL_CONTROL_PACKAGE_TEST}")
    if(NOT EXISTS "${final_control_test_file}")
        file(RELATIVE_PATH relative
            "${RUVIA_ROOT}" "${final_control_test_file}")
        boundary_error("final-response control coverage is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${FINAL_RESPONSE_CONTROL_TEST}" AND
   EXISTS "${HTTP2_FINAL_RESPONSE_HEADER_TEST}" AND
   EXISTS "${HTTP2_FINAL_RESPONSE_CONNECTION_TEST}" AND
   EXISTS "${HTTP2_CONNECT_RESPONSE_TEST}" AND
   EXISTS "${HTTP_FINAL_CONTROL_PACKAGE_TEST}")
    file(READ "${FINAL_RESPONSE_CONTROL_TEST}" final_response_control_test)
    file(READ "${HTTP2_FINAL_RESPONSE_HEADER_TEST}"
        http2_final_response_header_test)
    file(READ "${HTTP2_FINAL_RESPONSE_CONNECTION_TEST}"
        http2_final_response_connection_test)
    file(READ "${HTTP2_CONNECT_RESPONSE_TEST}"
        http2_connect_response_test)
    file(READ "${HTTP_FINAL_CONTROL_PACKAGE_TEST}"
        http_final_control_package_test)
    if(NOT final_response_control_test MATCHES
           "final_response_control_plan_owns_exact_protocol_alternative" OR
       NOT final_response_control_test MATCHES
           "final_response_control_failure_never_exposes_a_default_plan" OR
       NOT final_response_control_test MATCHES
           "final_response_control_rejects_every_http2_connection_specific_field" OR
       NOT http2_final_response_header_test MATCHES
           "http2_response_headers_reject_connection_specific_fields_before_hpack" OR
       NOT http2_final_response_connection_test MATCHES
           "http2_connection_rejects_connection_specific_final_heads_transactionally" OR
       NOT http2_connect_response_test MATCHES "invalidConnection" OR
       NOT http_final_control_package_test MATCHES
           "HasFinalResponseControlResultAlternatives" OR
       NOT http_final_control_package_test MATCHES
           "HasFinalResponseControlProtocolAlternatives" OR
       NOT http_final_control_package_test MATCHES
           "!HasStaleFinalResponseControlStatus")
        boundary_error("final-response control coverage is incomplete"
            "unit, encoder, buffered/streaming/CONNECT, and installed-consumer tests must pin exclusive result alternatives and pre-HPACK rejection")
    endif()
endif()
check_files_no_match("outbound responses restored the invalid 600..999 status range"
    "statusCode[ ]*>[ ]*999"
    "${RUVIA_ROOT}/ruvia-http/src/HttpResponse.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextResponse.cpp")

set(HTTP_INTERIM_RESPONSE_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpInterimResponse.h")
set(HTTP_INTERIM_RESPONSE_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/HttpInterimResponse.cpp")
set(HTTP1_INTERIM_RESPONSE_WRITER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1InterimResponseWriter.h")
set(HTTP1_INTERIM_RESPONSE_WRITER_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/server/Http1InterimResponseWriter.cpp")
set(HTTP_INTERIM_RESPONSE_VALIDATION
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpInterimResponseValidation.h")
set(WEB_CONTINUE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpContinueWriter.h")
set(HTTP2_CONNECTION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
if(NOT EXISTS "${HTTP_INTERIM_RESPONSE_HEADER}" OR
   NOT EXISTS "${HTTP_INTERIM_RESPONSE_SOURCE}" OR
   NOT EXISTS "${HTTP1_INTERIM_RESPONSE_WRITER}" OR
   NOT EXISTS "${HTTP1_INTERIM_RESPONSE_WRITER_SOURCE}" OR
   NOT EXISTS "${HTTP_INTERIM_RESPONSE_VALIDATION}" OR
   NOT EXISTS "${WEB_CONTINUE_WRITER}")
    boundary_error("typed interim response head is missing"
        "non-switching 1xx needs typed HTTP/1 and HTTP/2 protocol writers")
else()
    file(READ "${HTTP_INTERIM_RESPONSE_HEADER}" http_interim_response_header)
    file(READ "${HTTP_INTERIM_RESPONSE_SOURCE}" http_interim_response_source)
    file(READ "${HTTP1_INTERIM_RESPONSE_WRITER}" http1_interim_response_writer)
    file(READ "${HTTP1_INTERIM_RESPONSE_WRITER_SOURCE}" http1_interim_response_writer_source)
    file(READ "${HTTP_INTERIM_RESPONSE_VALIDATION}" http_interim_response_validation)
    file(READ "${WEB_CONTINUE_WRITER}" web_continue_writer)
    file(READ "${HTTP2_CONNECTION_HEADER}" http2_connection_header)
    file(READ "${HTTP2_RESPONSE_HEADERS}" http2_response_headers)
    file(READ "${RUVIA_ROOT}/ruvia-http/src/HttpResponse.cpp" http_response_source)
    if(NOT http_interim_response_header MATCHES "HttpInterimResponseHead" OR
       NOT http_interim_response_header MATCHES "HeaderInit" OR
       NOT http_interim_response_source MATCHES "httpInterimStatusCodeValid" OR
       NOT http_response_source MATCHES "httpFinalStatusCodeValid" OR
       NOT http1_interim_response_writer MATCHES "Http1InterimResponseWriter" OR
       NOT http1_interim_response_writer MATCHES "requiresFinalConnectionClose" OR
       NOT http1_interim_response_writer_source MATCHES "kUpgradeConnectionOptionRequired" OR
       NOT http1_interim_response_writer_source MATCHES "headBuffer[.]size" OR
       NOT http_interim_response_validation MATCHES "kContentLengthForbidden" OR
       NOT http_interim_response_validation MATCHES "kTransferEncodingForbidden" OR
       NOT http_interim_response_validation MATCHES "kRepeatedSingleton" OR
       NOT web_continue_writer MATCHES "Http1InterimResponseWriter" OR
       NOT web_continue_writer MATCHES "system_error" OR
       NOT http2_connection_header MATCHES "submitInterimResponseHead" OR
       NOT http2_response_headers MATCHES "appendHttp2InterimResponseHeaders" OR
       NOT http2_response_headers MATCHES "validateHttpInterimResponseHeaders" OR
       NOT http2_response_headers MATCHES "kInvalidHeader")
        boundary_error("interim/final response types have drifted"
            "typed 1xx must use exact, transactionally validated HTTP/1 and HTTP/2 writers")
    endif()
endif()
check_files_no_match("obsolete untyped informational response submit API was restored"
    "submitInformationalResponseHead"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
check_files_no_match("raw HTTP/1 100 Continue bytes escaped the protocol writer"
    "HTTP/1[.]1 100 Continue"
    ${WEB_SOURCE})
check_files_no_match("obsolete raw HTTP/1 continue response constant was restored"
    "kHttp1ContinueResponse|writeContinue[(]"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpContinueWriter.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl")

set(WEB_BUFFERED_RESPONSE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpBufferedResponse.h")
if(EXISTS "${WEB_BUFFERED_RESPONSE}")
    file(READ "${WEB_BUFFERED_RESPONSE}" web_buffered_response)
    if(NOT web_buffered_response MATCHES "HttpBufferedResponseWritePlan")
        boundary_error("ruvia-web buffered response bypasses the HTTP-owned write plan"
            "HttpBufferedResponse.h must carry HttpBufferedResponseWritePlan")
    endif()
endif()

set(WEB_H2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
if(EXISTS "${WEB_H2_SESSION}")
    file(READ "${WEB_H2_SESSION}" web_h2_session)
    if(NOT web_h2_session MATCHES "writePlan\\.sendBody" OR
       NOT web_h2_session MATCHES "headResult[.]submitted[(][)]" OR
       NOT web_h2_session MATCHES "headResult[.]failure[(][)][-][>]error[(][)]" OR
       NOT web_h2_session MATCHES "Http2ResponseHeadSubmitError::kClosed" OR
       NOT web_h2_session MATCHES "submittedHead[-][>]plan[(][)]" OR
       NOT web_h2_session MATCHES "Http2ErrorCode::kInternalError")
        boundary_error("ruvia-web HTTP/2 runtime bypasses the HTTP-owned send-body verdict"
            "Http2SansIoSession.h must consume only a submitted plan and terminate typed final-head failures")
    endif()
endif()

set(WEB_H2_STREAM_SINK
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
if(EXISTS "${WEB_H2_STREAM_SINK}")
    file(READ "${WEB_H2_STREAM_SINK}" web_h2_stream_sink)
    if(NOT web_h2_stream_sink MATCHES "headResult[.]submitted[(][)]" OR
       NOT web_h2_stream_sink MATCHES "headResult[.]failure[(][)][-][>]error[(][)]" OR
       NOT web_h2_stream_sink MATCHES "Http2ResponseHeadSubmitError::kClosed" OR
       NOT web_h2_stream_sink MATCHES
           "markCommitted[(]submittedHead[-][>]plan[(][)][)]")
        boundary_error("ruvia-web HTTP/2 streaming sink bypasses the submitted-head plan"
            "the sink must distinguish typed failure before committing the successful streaming plan")
    endif()
endif()

set(HTTP2_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(HTTP2_LOCAL_SEND_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2LocalSendState.h")
set(HTTP2_STREAM_CLOSE_SOURCE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamCloseSource.h")
set(HTTP2_STREAM_LIFECYCLE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamLifecycle.h")
set(HTTP2_STREAM_TABLE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamTable.h")
set(HTTP2_LOCAL_CONTENT_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2LocalContentState.h")
set(HTTP2_REMOTE_CONTENT_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RemoteContentState.h")
set(HTTP2_REMOTE_RECEIVE_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RemoteReceiveState.h")
set(HTTP2_STALE_BODY_ACCOUNTING
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamBodyAccounting.h")
set(HTTP2_BODY_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2BodyState.h")
set(HTTP2_REQUEST_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h")
set(HTTP2_STREAM_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h")
set(HTTP2_REQUEST_CONTENT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestContent.h")
set(HTTP2_TUNNEL_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2TunnelState.h")
set(HTTP2_LOCAL_SETTINGS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2LocalSettings.h")
set(HTTP2_PEER_SETTINGS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2PeerSettings.h")
if(NOT EXISTS "${HTTP_RESPONSE_CONTENT_SEMANTICS}" OR
   NOT EXISTS "${HTTP_RESPONSE_WRITE_PLAN}" OR
   NOT EXISTS "${HTTP1_CLIENT_RESPONSE_SOURCE}" OR
   NOT EXISTS "${HTTP2_CONNECTION_SOURCE}")
    boundary_error("shared response-content semantics contract is missing"
        "HTTP/1 client, HTTP/2 client, and response writers must consume one method/status classification")
else()
    file(READ "${HTTP_RESPONSE_CONTENT_SEMANTICS}"
        http_response_content_semantics)
    file(READ "${HTTP_RESPONSE_WRITE_PLAN}" http_response_write_plan)
    file(READ "${HTTP1_CLIENT_RESPONSE_SOURCE}"
        http1_shared_response_semantics)
    file(READ "${HTTP2_CONNECTION_SOURCE}"
        http2_shared_response_semantics)
    if(NOT http_response_content_semantics MATCHES
           "class HttpInformationalResponseContent final" OR
       NOT http_response_content_semantics MATCHES
           "class HttpProtocolSwitchResponseContent final" OR
       NOT http_response_content_semantics MATCHES
           "class HttpConnectTunnelResponseContent final" OR
       NOT http_response_content_semantics MATCHES
           "class HttpResponseWithoutContent final" OR
       NOT http_response_content_semantics MATCHES
           "class HttpResponseWithContent final" OR
       NOT http_response_content_semantics MATCHES "using State = std::variant" OR
       NOT http_response_content_semantics MATCHES
           "std::get_if<HttpResponseWithoutContent>" OR
       NOT http_response_content_semantics MATCHES
           "httpResponseContentSemantics" OR
       NOT http1_shared_response_semantics MATCHES
           "detail::httpResponseContentSemantics" OR
       NOT http2_shared_response_semantics MATCHES
           "httpResponseContentSemantics" OR
       NOT http_response_write_plan MATCHES
           "HttpResponseContentSemantics semantics_" OR
       NOT http_response_write_plan MATCHES
           "semantics_[.]withContent[(][)] == nullptr" OR
       http_response_write_plan MATCHES "bodySuppressed_")
        boundary_error("response content semantics split by protocol direction"
            "informational, switch, CONNECT, without-content, and with-content alternatives must drive H1 client, H2 client, and server body plans")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_SEND_STATE}" OR
   NOT EXISTS "${HTTP2_STREAM_CLOSE_SOURCE}" OR
   NOT EXISTS "${HTTP2_STREAM_LIFECYCLE}" OR
   NOT EXISTS "${HTTP2_STREAM_TABLE}" OR
   NOT EXISTS "${HTTP2_STREAM_STATE}" OR
   NOT EXISTS "${HTTP2_CONNECTION_SOURCE}")
    boundary_error("HTTP/2 local send state is missing"
        "local frame permission must be owned by one installed discriminated state")
else()
    file(READ "${HTTP2_LOCAL_SEND_STATE}" http2_local_send_state)
    file(READ "${HTTP2_STREAM_CLOSE_SOURCE}" http2_stream_close_source)
    file(READ "${HTTP2_STREAM_LIFECYCLE}" http2_stream_lifecycle)
    file(READ "${HTTP2_STREAM_TABLE}" http2_stream_table)
    file(READ "${HTTP2_STREAM_STATE}" http2_local_send_stream_state)
    file(READ "${HTTP2_CONNECTION_SOURCE}" http2_local_send_connection)
    if(NOT http2_stream_close_source MATCHES
           "enum class Http2StreamCloseSource" OR
       NOT http2_stream_close_source MATCHES "kNone" OR
       NOT http2_stream_close_source MATCHES "kPeerGoaway" OR
       NOT http2_local_send_state MATCHES
           "Http2StreamCloseSource[.]h" OR
       NOT http2_local_send_state MATCHES
           "private:[\r\n \t]+friend class Http2StreamLifecycle" OR
       http2_local_send_state MATCHES
           "enum class Http2StreamCloseSource" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalHeadPending final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalRequestContentOpen final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalResponseContentOpen final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalResponseTrailersOnly final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalConnectPending final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalTunnelOpen final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalEndStreamQueued final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalEndStreamCommitted final" OR
       NOT http2_local_send_state MATCHES
           "class Http2StreamAborted final" OR
       NOT http2_local_send_state MATCHES "using State = std::variant" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalHeadPending>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalRequestContentOpen>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalResponseContentOpen>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalResponseTrailersOnly>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalConnectPending>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalTunnelOpen>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalEndStreamQueued>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalEndStreamCommitted>" OR
       NOT http2_local_send_state MATCHES "std::get_if<Http2StreamAborted>" OR
       NOT http2_local_send_state MATCHES
           "Http2StreamCloseSource source[(][)] const noexcept" OR
       http2_local_send_state MATCHES
           "source == Http2StreamCloseSource::kNone" OR
       NOT http2_stream_lifecycle MATCHES
           "const Http2LocalSendState& localSend[(][)] const noexcept" OR
       NOT http2_stream_lifecycle MATCHES
           "private:[\r\n \t]+friend class Http2StreamState" OR
       NOT http2_stream_lifecycle MATCHES
           "bool aborted[(][)] const noexcept" OR
       NOT http2_stream_lifecycle MATCHES
           "bool abort[(]Http2StreamCloseSource source[)] noexcept" OR
       NOT http2_stream_lifecycle MATCHES "queued_ = false" OR
       NOT http2_local_send_stream_state MATCHES
           "const Http2LocalSendState& localSend[(][)] const noexcept" OR
       NOT http2_local_send_stream_state MATCHES
           "bool isAborted[(][)] const noexcept" OR
       NOT http2_local_send_stream_state MATCHES
           "bool abort[(]Http2StreamCloseSource source[)] noexcept" OR
       NOT http2_stream_table MATCHES "void removeAborted" OR
       NOT http2_local_send_connection MATCHES "beginLocalRequestContent" OR
       NOT http2_local_send_connection MATCHES "beginLocalResponseContent" OR
       NOT http2_local_send_connection MATCHES
           "beginLocalResponseTrailersOnly" OR
       NOT http2_local_send_connection MATCHES "beginLocalConnectRequest" OR
       NOT http2_local_send_connection MATCHES "openLocalConnectTunnel" OR
       NOT http2_local_send_connection MATCHES "queueLocalEndStream" OR
       NOT http2_local_send_connection MATCHES "commitLocalEndStream")
        boundary_error("HTTP/2 local send lifecycle lost its discriminated state"
            "head, request/response content, trailers, CONNECT, queued/committed END_STREAM, and whole-stream abort must remain exclusive; only abort owns a non-none close source and it must atomically clear queue ownership")
    endif()
endif()
if(NOT EXISTS "${HTTP2_REMOTE_RECEIVE_STATE}" OR
   NOT EXISTS "${HTTP2_STREAM_LIFECYCLE}" OR
   NOT EXISTS "${HTTP2_STREAM_STATE}" OR
   NOT EXISTS "${HTTP2_CONNECTION_SOURCE}")
    boundary_error("HTTP/2 remote receive state is missing"
        "remote HEADERS, content, CONNECT, tunnel, END_STREAM, and abort permission must be one installed discriminated state")
else()
    file(READ "${HTTP2_REMOTE_RECEIVE_STATE}" http2_remote_receive_state)
    file(READ "${HTTP2_STREAM_LIFECYCLE}" http2_remote_receive_lifecycle)
    file(READ "${HTTP2_STREAM_STATE}" http2_remote_receive_stream)
    file(READ "${HTTP2_CONNECTION_SOURCE}" http2_remote_receive_connection)
    if(NOT http2_remote_receive_state MATCHES
           "private:[\r\n \t]+friend class Http2StreamLifecycle" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteHeadPending final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteHeadEndStreamPending final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteContentOpen final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteConnectPending final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteConnectPendingEndStream final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteConnectRejectedAwaitingEndStream final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteTunnelOpen final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteEndStream final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteAborted final" OR
       NOT http2_remote_receive_state MATCHES "using State = std::variant" OR
       NOT http2_remote_receive_state MATCHES
           "std::get_if<Http2RemoteHeadPending>" OR
       NOT http2_remote_receive_state MATCHES
           "std::get_if<Http2RemoteConnectRejectedAwaitingEndStream>" OR
       NOT http2_remote_receive_state MATCHES
           "std::get_if<Http2RemoteEndStream>" OR
       NOT http2_remote_receive_lifecycle MATCHES
           "const Http2RemoteReceiveState& remoteReceive[(][)] const noexcept" OR
       NOT http2_remote_receive_lifecycle MATCHES "remoteReceive_[.]abort[(][)]" OR
       NOT http2_remote_receive_stream MATCHES
           "const Http2RemoteReceiveState& remoteReceive[(][)] const noexcept" OR
       NOT http2_remote_receive_stream MATCHES "finalizeRemoteConnectHead" OR
       NOT http2_remote_receive_stream MATCHES "finishRemotePendingConnect" OR
       NOT http2_remote_receive_stream MATCHES "finishRemoteRejectedConnect" OR
       NOT http2_remote_receive_connection MATCHES
           "http2RemoteFinalHeadDecoded" OR
       NOT http2_remote_receive_connection MATCHES
           "http2RemotePeerHalfClosed" OR
       NOT http2_remote_receive_connection MATCHES
           "connectRejectedAwaitingEndStream" OR
       NOT http2_remote_receive_connection MATCHES
           "finishRemoteRejectedConnect" OR
       NOT http2_remote_receive_connection MATCHES
           "remote[.]tunnelOpen[(][)]")
        boundary_error("HTTP/2 remote receive lifecycle lost its discriminated state"
            "final-head decoding, content/trailer DATA, CONNECT decisions, tunnel flow control, normal peer half-close, and whole-stream abort must remain exclusive and stream-owned")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_CONTENT_STATE}")
    boundary_error("HTTP/2 local content state is missing"
        "Http2LocalContentState.h must own outbound response length accounting")
elseif(NOT EXISTS "${HTTP2_STREAM_STATE}")
    boundary_error("HTTP/2 stream state is missing"
        "Http2StreamState.h must expose the const local-content contract")
else()
    file(READ "${HTTP2_LOCAL_CONTENT_STATE}" http2_local_content_state)
    file(READ "${HTTP2_STREAM_STATE}" http2_stream_state)
    if(NOT http2_local_content_state MATCHES
           "class Http2LocalContentUnset final" OR
       NOT http2_local_content_state MATCHES
           "class Http2LocalContentForbidden final" OR
       NOT http2_local_content_state MATCHES
           "class Http2LocalContentUnbounded final" OR
       NOT http2_local_content_state MATCHES
           "class Http2LocalContentKnownLength final" OR
       NOT http2_local_content_state MATCHES "using Content = std::variant" OR
       NOT http2_local_content_state MATCHES
           "std::get_if<Http2LocalContentUnset>" OR
       NOT http2_local_content_state MATCHES
           "std::get_if<Http2LocalContentForbidden>" OR
       NOT http2_local_content_state MATCHES
           "std::get_if<Http2LocalContentUnbounded>" OR
       NOT http2_local_content_state MATCHES
           "std::get_if<Http2LocalContentKnownLength>" OR
       NOT http2_local_content_state MATCHES "kNotStarted" OR
       NOT http2_local_content_state MATCHES "if [(]unset[(][)] != nullptr[)]" OR
       NOT http2_stream_state MATCHES
           "const Http2LocalContentState& localContent[(][)] const noexcept")
        boundary_error("HTTP/2 local content accounting lost its discriminated state"
            "unset, forbidden, unbounded, and known-length must be exclusive and only known-length may own a declared length")
    endif()
endif()
if(EXISTS "${HTTP2_STALE_BODY_ACCOUNTING}")
    boundary_error("stale HTTP/2 body accounting header was restored"
        "peer content must be represented only by Http2RemoteContentState.h")
elseif(NOT EXISTS "${HTTP2_REMOTE_CONTENT_STATE}")
    boundary_error("HTTP/2 remote content state is missing"
        "Http2RemoteContentState.h must own peer Content-Length and DATA accounting")
elseif(NOT EXISTS "${HTTP2_BODY_STATE}" OR
       NOT EXISTS "${HTTP2_REQUEST_HEADERS}" OR
       NOT EXISTS "${HTTP2_STREAM_STATE}")
    boundary_error("HTTP/2 remote content call chain is incomplete"
        "header decode, DATA preflight, and stream state must consume one remote-content contract")
else()
    file(READ "${HTTP2_REMOTE_CONTENT_STATE}" http2_remote_content_state)
    file(READ "${HTTP2_BODY_STATE}" http2_body_state)
    file(READ "${HTTP2_REQUEST_HEADERS}" http2_request_headers)
    file(READ "${HTTP2_STREAM_STATE}" http2_remote_stream_state)
    if(NOT http2_remote_content_state MATCHES
           "class Http2RemoteContentAllowedWithoutLength final" OR
       NOT http2_remote_content_state MATCHES
           "class Http2RemoteContentAllowedKnownLength final" OR
       NOT http2_remote_content_state MATCHES
           "class Http2RemoteContentMetadataOnlyWithoutLength final" OR
       NOT http2_remote_content_state MATCHES
           "class Http2RemoteContentMetadataOnlyKnownLength final" OR
       NOT http2_remote_content_state MATCHES "using State = std::variant" OR
       NOT http2_remote_content_state MATCHES
           "std::get_if<Http2RemoteContentAllowedWithoutLength>" OR
       NOT http2_remote_content_state MATCHES
           "std::get_if<Http2RemoteContentAllowedKnownLength>" OR
       NOT http2_remote_content_state MATCHES
           "std::get_if<[\r\n \t]*Http2RemoteContentMetadataOnlyWithoutLength>" OR
       NOT http2_remote_content_state MATCHES
           "std::get_if<[\r\n \t]*Http2RemoteContentMetadataOnlyKnownLength>" OR
       NOT http2_remote_content_state MATCHES "kCounterOverflow" OR
       NOT http2_remote_content_state MATCHES "kDeclaredLengthExceeded" OR
       NOT http2_remote_content_state MATCHES "kContentForbidden" OR
       NOT http2_remote_content_state MATCHES "selectMetadataOnly" OR
       NOT http2_remote_content_state MATCHES "account[(]" OR
       NOT http2_remote_content_state MATCHES "terminalLengthValid" OR
       NOT http2_remote_stream_state MATCHES
           "const Http2RemoteContentState&[ \t\r\n]+remoteContent[(][)] const noexcept" OR
       NOT http2_remote_stream_state MATCHES "accountRemoteContent" OR
       NOT http2_remote_stream_state MATCHES
           "selectRemoteContentMetadataOnly" OR
       NOT http2_body_state MATCHES "accountRemoteContent" OR
       NOT http2_body_state MATCHES
           "Http2RemoteContentAccountingResult::kDeclaredLengthExceeded" OR
       NOT http2_body_state MATCHES
           "Http2RemoteContentAccountingResult::kContentForbidden" OR
       NOT http2_request_headers MATCHES "declareRemoteContentLength")
        boundary_error("HTTP/2 remote content accounting lost its discriminated transaction"
            "content allowance and length must be exclusive, DATA accounting must be atomic, and metadata-only responses must reject payload")
    endif()
endif()
if(NOT EXISTS "${HTTP2_REQUEST_CONTENT}")
    boundary_error("HTTP/2 request content contract is missing"
        "Http2RequestContent.h must own regular request Content-Length/END_STREAM selection")
else()
    file(READ "${HTTP2_REQUEST_CONTENT}" http2_request_content)
    if(NOT http2_request_content MATCHES
           "class Http2RequestWithoutContent final" OR
       NOT http2_request_content MATCHES
           "class Http2KnownLengthRequestContent final" OR
       NOT http2_request_content MATCHES
           "class Http2StreamingRequestContent final" OR
       NOT http2_request_content MATCHES "using Content = std::variant" OR
       NOT http2_request_content MATCHES
           "std::get_if<Http2RequestWithoutContent>" OR
       NOT http2_request_content MATCHES
           "std::get_if<Http2KnownLengthRequestContent>" OR
       NOT http2_request_content MATCHES
           "std::get_if<Http2StreamingRequestContent>" OR
       NOT http2_request_content MATCHES "knownLengthContent" OR
       NOT http2_request_content MATCHES "streamingContent")
        boundary_error("HTTP/2 request content lost its exclusive alternatives"
            "absent, known-length, and streaming contracts must own only their relevant payload")
    endif()
endif()
if(NOT EXISTS "${HTTP2_TUNNEL_STATE}")
    boundary_error("HTTP/2 CONNECT tunnel state is missing"
        "Http2TunnelState.h must own pending, open, and rejected phases")
else()
    file(READ "${HTTP2_TUNNEL_STATE}" http2_tunnel_state)
    file(READ "${HTTP2_STREAM_STATE}" http2_tunnel_stream_state)
    file(READ "${HTTP2_BODY_STATE}" http2_tunnel_body_state)
    file(READ
        "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestBuilder.h"
        http2_tunnel_request_builder)
    file(READ
        "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2WebSocketHandshake.h"
        http2_tunnel_websocket_handshake)
    if(NOT http2_tunnel_state MATCHES "enum class Http2ConnectForm" OR
       NOT http2_tunnel_state MATCHES "class Http2NotConnect final" OR
       NOT http2_tunnel_state MATCHES "class Http2ConnectPending final" OR
       NOT http2_tunnel_state MATCHES "class Http2TunnelOpen final" OR
       NOT http2_tunnel_state MATCHES "class Http2ConnectRejected final" OR
       NOT http2_tunnel_state MATCHES "using State = std::variant" OR
       NOT http2_tunnel_state MATCHES "std::get_if<Http2NotConnect>" OR
       NOT http2_tunnel_state MATCHES "std::get_if<Http2ConnectPending>" OR
       NOT http2_tunnel_state MATCHES "std::get_if<Http2TunnelOpen>" OR
       NOT http2_tunnel_state MATCHES "std::get_if<Http2ConnectRejected>" OR
       NOT http2_tunnel_state MATCHES
           "form != Http2ConnectForm::kStandard" OR
       NOT http2_tunnel_state MATCHES
           "form != Http2ConnectForm::kExtended" OR
       NOT http2_tunnel_stream_state MATCHES
           "const Http2TunnelState& tunnel[(][)] const noexcept" OR
       http2_tunnel_body_state MATCHES "tunnel[(]" OR
       NOT http2_tunnel_request_builder MATCHES
           "tunnel[(][)][.]pending[(][)]" OR
       NOT http2_tunnel_websocket_handshake MATCHES
           "http2IsPendingWebSocketConnect")
        boundary_error("HTTP/2 CONNECT tunnel state lost exclusive alternatives"
            "only pending may own standard/extended form; message-content accounting must not reinterpret tunnel bytes")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_SETTINGS}")
    boundary_error("HTTP/2 local SETTINGS contract is missing"
        "Http2LocalSettings.h must own wire values and matching receive accounting")
else()
    file(READ "${HTTP2_LOCAL_SETTINGS}" http2_local_settings)
    if(NOT http2_local_settings MATCHES "struct Http2LocalSettings" OR
       NOT http2_local_settings MATCHES "kMaxConcurrentStreams" OR
       NOT http2_local_settings MATCHES "kInitialWindowSize" OR
       NOT http2_local_settings MATCHES "kMaxFrameSize" OR
       NOT http2_local_settings MATCHES "http2WriteLocalSettingsFrame")
        boundary_error("HTTP/2 local SETTINGS has more than one source of truth"
            "one typed contract must drive emitted bytes, capacities, and receive windows")
    endif()
endif()
if(NOT EXISTS "${HTTP2_PEER_SETTINGS}")
    boundary_error("HTTP/2 peer SETTINGS contract is missing"
        "Http2PeerSettings.h must own role-aware peer setting validation")
else()
    file(READ "${HTTP2_PEER_SETTINGS}" http2_peer_settings)
    if(NOT http2_peer_settings MATCHES
           "explicit Http2PeerSettings.*Http2Role localRole" OR
       NOT http2_peer_settings MATCHES
           "localRole_.*Http2Role::kClient.*value == 1")
        boundary_error("HTTP/2 peer SETTINGS validation lost endpoint direction"
            "peer settings must bind Http2Role and reject server ENABLE_PUSH=1 at clients")
    endif()
    if(NOT http2_peer_settings MATCHES "enum class Http2PeerSettingError" OR
       NOT http2_peer_settings MATCHES "class Http2PeerSettingApplied final" OR
       NOT http2_peer_settings MATCHES "class Http2PeerInitialWindowChange final" OR
       NOT http2_peer_settings MATCHES "class Http2PeerSettingFailure final" OR
       NOT http2_peer_settings MATCHES "class Http2PeerSettingApplyResult final" OR
       NOT http2_peer_settings MATCHES "using Value = std::variant" OR
       NOT http2_peer_settings MATCHES "std::get_if<Http2PeerSettingApplied>" OR
       NOT http2_peer_settings MATCHES "std::get_if<Http2PeerInitialWindowChange>" OR
       NOT http2_peer_settings MATCHES "std::get_if<Http2PeerSettingFailure>" OR
       NOT http2_peer_settings MATCHES "makeInitialWindowChange" OR
       NOT http2_peer_settings MATCHES "makeFailure")
        boundary_error("HTTP/2 peer SETTINGS application lost its discriminated result"
            "ordinary settings, initial-window delta, and failure must remain exclusive alternatives")
    endif()
endif()
if(EXISTS "${HTTP2_CONNECTION_SOURCE}")
    file(READ "${HTTP2_CONNECTION_SOURCE}" http2_connection_source)
    if(NOT http2_connection_source MATCHES
           "localSend[(][)][.]headPending[(][)]" OR
       NOT http2_connection_source MATCHES "requestContentOpen[(][)]" OR
       NOT http2_connection_source MATCHES "responseContentOpen[(][)]" OR
       NOT http2_connection_source MATCHES "tunnelOpen[(][)]" OR
       NOT http2_connection_source MATCHES "Http2DataSubmitStatus::kBackpressured")
        boundary_error("HTTP/2 core does not enforce its typed local send state"
            "head ownership, request/response/tunnel DATA permission, and zero-ownership backpressure must be core-owned")
    endif()
    if(NOT http2_connection_source MATCHES "discardDeferredStreamState\\(streamId\\)" OR
       NOT http2_connection_source MATCHES "first && http2EndsStream\\(endStream\\)")
        boundary_error("HTTP/2 terminal transitions bypass the shared protocol path"
            "reset cleanup and HEADERS-only END_STREAM placement must remain centralized")
    endif()
    if(NOT http2_connection_source MATCHES "Http2HeaderBlockKind::kDiscarded" OR
       NOT http2_connection_source MATCHES "decodeDiscardedHeaderBlock" OR
       NOT http2_connection_source MATCHES "detachActiveHeaderBlock" OR
       NOT http2_connection_source MATCHES
           "kCompressionError, \"field block not decompressed\"")
        boundary_error("HTTP/2 discarded field blocks bypass connection-scoped HPACK"
            "detached continuation state and COMPRESSION_ERROR fallback must remain core-owned")
    endif()
    if(NOT http2_connection_source MATCHES "checkLocalContentAccept" OR
       NOT http2_connection_source MATCHES "acceptLocalContent" OR
       NOT http2_connection_source MATCHES "commitLocalContent" OR
       NOT http2_connection_source MATCHES
           "localContent[(][)][.]lengthComplete[(][)]" OR
       NOT http2_connection_source MATCHES
           "Http2LocalContentCheck::kNotStarted")
        boundary_error("HTTP/2 response Content-Length bypasses stream-owned accounting"
            "head, DATA emission, deferred drain, and finish must share local content state")
    endif()
    if(NOT http2_connection_source MATCHES "declareRemoteContentLength" OR
       NOT http2_connection_source MATCHES
           "remoteContent[(][)][.]allowedKnownLength[(][)]" OR
       NOT http2_connection_source MATCHES
           "remoteContent[(][)][.]terminalLengthValid[(][)]" OR
       NOT http2_connection_source MATCHES
           "selectRemoteContentMetadataOnly")
        boundary_error("HTTP/2 inbound Content-Length bypasses remote content state"
            "response semantics, CONNECT validation, DATA, and terminal HEADERS must share the peer-content contract")
    endif()
    string(REGEX MATCHALL "remoteContent[(][)][.]terminalLengthValid[(][)]"
        http2_remote_terminal_call_sites "${http2_connection_source}")
    list(LENGTH http2_remote_terminal_call_sites
        http2_remote_terminal_call_site_count)
    if(http2_remote_terminal_call_site_count LESS 3)
        boundary_error("HTTP/2 END_STREAM paths split remote length validation"
            "initial HEADERS, DATA, and trailing HEADERS must all consult the active remote-content alternative")
    endif()
    if(NOT http2_connection_source MATCHES "submitRegularRequestHead" OR
       NOT http2_connection_source MATCHES "content[.]withoutContent" OR
       NOT http2_connection_source MATCHES "content[.]knownLengthContent" OR
       NOT http2_connection_source MATCHES "content[.]streamingContent" OR
       NOT http2_connection_source MATCHES "beginLocalContentKnownLength" OR
       NOT http2_connection_source MATCHES "knownLengthContent->length" OR
       NOT http2_connection_source MATCHES "http2IsValidOutboundRegularRequestHead" OR
       NOT http2_connection_source MATCHES
           "method != \"CONNECT\" && isValidHttpMethodToken[(]method[)]")
        boundary_error("HTTP/2 regular request framing bypasses its typed content contract"
            "regular request validation, generated length, END_STREAM, and CONNECT isolation must remain core-owned")
    endif()
    if(NOT http2_connection_source MATCHES "localRequestAdmissionError" OR
       NOT http2_connection_source MATCHES
           "activeLocalRequestStreams_.*peerSettings_\.maxConcurrentStreams" OR
       NOT http2_connection_source MATCHES
           "Http2RequestHeadSubmitResult::makeSubmitted" OR
       NOT http2_connection_source MATCHES
           "Http2RequestHeadSubmitResult::makeFailure" OR
       NOT http2_connection_source MATCHES "activateLocalRequestStream" OR
       NOT http2_connection_source MATCHES "releaseLocalRequestStreamIfClosed")
        boundary_error("HTTP/2 client stream concurrency escaped the protocol core"
            "request submission must atomically discriminate success/failure and retain/release peer concurrency slots")
    endif()
    if(NOT http2_connection_source MATCHES "submitConnectRequestHead" OR
       NOT http2_connection_source MATCHES "submitExtendedConnectRequestHead" OR
       NOT http2_connection_source MATCHES "submitConnectResponseHead" OR
       NOT http2_connection_source MATCHES "beginStandardConnect" OR
       NOT http2_connection_source MATCHES "beginExtendedConnect" OR
       NOT http2_connection_source MATCHES "acceptConnect" OR
       NOT http2_connection_source MATCHES "rejectConnect" OR
       NOT http2_connection_source MATCHES
           "tunnel[(][)][.]pending[(][)]" OR
       NOT http2_connection_source MATCHES "tunnel[(][)][.]open[(][)]" OR
       NOT http2_connection_source MATCHES "Http2Event::tunnelData" OR
       NOT http2_connection_source MATCHES "Http2Event::tunnelEnd" OR
       NOT http2_connection_source MATCHES
           "prefacePhase_ != PrefacePhase::kReady" OR
       NOT http2_connection_source MATCHES
           "remote[.]tunnelOpen[(][)]" OR
       NOT http2_connection_source MATCHES
           "http2RemotePeerHalfClosed")
        boundary_error("HTTP/2 CONNECT bypasses the shared tunnel lifecycle"
            "typed pending/open/rejected transitions, dedicated heads, tunnel events, and peer half-close enforcement must remain core-owned")
    endif()
    if(NOT http2_connection_source MATCHES "Http2Connection::beginConnection" OR
       NOT http2_connection_source MATCHES "Http2LocalSettings::kFrameBytes" OR
       NOT http2_connection_source MATCHES "Http2LocalSettings::kInitialWindowSize" OR
       NOT http2_connection_source MATCHES
           "Http2FeedResult::kConnectionNotStarted" OR
       NOT http2_connection_source MATCHES
           "PrefacePhase::kAwaitingClientMagic" OR
       NOT http2_connection_source MATCHES
           "PrefacePhase::kAwaitingPeerSettings" OR
       NOT http2_connection_source MATCHES "PrefacePhase::kReady" OR
       NOT http2_connection_source MATCHES "SETTINGS ACK before SETTINGS" OR
       NOT http2_connection_source MATCHES
           "connectionSendWindow_.*kHttp2DefaultInitialWindowSize")
        boundary_error("HTTP/2 preface bytes diverge from flow-control accounting"
            "typed role-aware startup, initial non-ACK SETTINGS, and local/peer window ownership must remain centralized")
    endif()
    if(NOT http2_connection_source MATCHES
           "const auto[*]? failure = result[.]failure[(][)]" OR
       NOT http2_connection_source MATCHES
           "http2PeerSettingErrorCode[(]failure->error[(][)][)]" OR
       NOT http2_connection_source MATCHES
           "const auto[*]? initialWindowChange = result[.]initialWindowChange[(][)]" OR
       NOT http2_connection_source MATCHES
           "initialWindowChange->delta[(][)]")
        boundary_error("HTTP/2 peer SETTINGS result escaped its connection owner"
            "the connection must branch on failure or the sole delta-owning alternative before mutating stream windows")
    endif()
    if(NOT http2_connection_source MATCHES
           "eventOffset_ < events_\.size\(\)" OR
       NOT http2_connection_source MATCHES
           "Http2FeedResult::kEventsPending" OR
       NOT http2_connection_source MATCHES
           "inputOffset_ < input_\.size\(\)" OR
       NOT http2_connection_source MATCHES
           "Http2FeedResult::kAccepted" OR
       NOT http2_connection_source MATCHES
           "Http2FeedResult::kNeedInput" OR
       NOT http2_connection_source MATCHES
           "Http2FeedResult::kProtocolFailure")
        boundary_error("HTTP/2 feed restored lossy event/input ownership"
            "retained spans, wholly accepted spans, partial protocol units, and terminal failure must remain distinguishable without a byte count")
    endif()
    if(NOT http2_connection_source MATCHES
           "Http2Connection::processRstStream" OR
       NOT http2_connection_source MATCHES
           "static_cast<Http2ErrorCode>[(]http2Read32" OR
       NOT http2_connection_source MATCHES
           "closeStream[(]header[.]streamId, Http2StreamCloseSource::kPeer, error[)]" OR
       NOT http2_connection_source MATCHES
           "Http2Event::streamClosed[(]streamId, source, error[)]")
        boundary_error("HTTP/2 stream-close events lost their RFC error reason"
            "RST_STREAM must be decoded once and the exact peer/local error must reach the typed close event")
    endif()
    if(NOT http2_connection_source MATCHES "Http2Connection::processGoaway" OR
       NOT http2_connection_source MATCHES
           "goaway[.]lastStreamId[(][)] > peerGoaway_->lastStreamId[(][)]" OR
       NOT http2_connection_source MATCHES
           "Http2StreamCloseSource::kPeerGoaway" OR
       NOT http2_connection_source MATCHES
           "Http2Event::goaway[(]goaway[)]" OR
       NOT http2_connection_source MATCHES
           "Http2Event::requestUnprocessed[(]streamId[)]" OR
       NOT http2_connection_source MATCHES "closeStreamImpl" OR
       NOT http2_connection_source MATCHES "beginDrain\\(\\)" OR
       NOT http2_connection_source MATCHES "connectionError_ = error")
        boundary_error("HTTP/2 peer GOAWAY escaped the protocol core"
            "monotonic last-stream-id, bilateral drain, typed fatal error, cleanup, and safe-retry events must remain centralized")
    endif()
    if(NOT http2_connection_source MATCHES
           "http2DebitConnectionReceiveWindow" OR
       NOT http2_connection_source MATCHES
           "http2DebitStreamReceiveWindow" OR
       NOT http2_connection_source MATCHES
           "releaseDroppedDataConnectionWindow" OR
       NOT http2_connection_source MATCHES
           "http2CreditConnectionReceiveWindow")
        boundary_error("HTTP/2 DATA bypasses connection-first receive-window accounting"
            "every structurally valid DATA payload must debit connection credit before stream lookup and release discarded credit exactly once")
    endif()
    if(NOT http2_connection_source MATCHES
           "Http2BufferedResponseHeadSubmitResult::makeFailure" OR
       NOT http2_connection_source MATCHES
           "Http2BufferedResponseHeadSubmitResult::makeSubmitted" OR
       NOT http2_connection_source MATCHES
           "Http2StreamingResponseHeadSubmitResult::makeFailure" OR
       NOT http2_connection_source MATCHES
           "Http2StreamingResponseHeadSubmitResult::makeSubmitted" OR
       NOT http2_connection_source MATCHES
           "Http2ResponseHeadSubmitError::kClosed" OR
       NOT http2_connection_source MATCHES
           "Http2ResponseHeadSubmitError::kInvalidState" OR
       NOT http2_connection_source MATCHES
           "Http2ResponseHeadSubmitError::kInvalidMessage")
        boundary_error("HTTP/2 response-head transaction bypasses its typed result"
            "buffered and streaming heads must return error-only failures and plan-only committed submissions")
    endif()
endif()

set(HTTP2_FLOW_CONTROL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2FlowControl.h")
if(EXISTS "${HTTP2_FLOW_CONTROL}")
    file(READ "${HTTP2_FLOW_CONTROL}" http2_flow_control)
    if(NOT http2_flow_control MATCHES "Http2ReceiveWindowDebitStatus" OR
       NOT http2_flow_control MATCHES "http2DebitConnectionReceiveWindow" OR
       NOT http2_flow_control MATCHES "http2DebitStreamReceiveWindow" OR
       NOT http2_flow_control MATCHES "http2CreditConnectionReceiveWindow" OR
       NOT http2_flow_control MATCHES "http2CreditStreamReceiveWindow")
        boundary_error("HTTP/2 receive-window primitives lost scope ownership"
            "connection and stream debit/credit operations must remain separate and typed")
    endif()
endif()

set(HTTP2_EVENT_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Event.h")
if(NOT EXISTS "${HTTP2_EVENT_HEADER}")
    boundary_error("HTTP/2 typed event contract is missing"
        "ruvia-http must own Http2Event.h")
else()
    file(READ "${HTTP2_EVENT_HEADER}" http2_event_header)
    if(NOT http2_event_header MATCHES "enum class Http2EventKind" OR
       NOT http2_event_header MATCHES "using Value = std::variant" OR
       NOT http2_event_header MATCHES "class Http2StreamClosedEvent final" OR
       NOT http2_event_header MATCHES "Http2StreamCloseSource source" OR
       NOT http2_event_header MATCHES "Http2ErrorCode error" OR
       NOT http2_event_header MATCHES "class Http2RequestUnprocessedEvent final" OR
       NOT http2_event_header MATCHES "class Http2GoawayEvent final" OR
       NOT http2_event_header MATCHES "lastStreamId[(][)] const" OR
       NOT http2_event_header MATCHES "std::get_if<Http2StreamClosedEvent>")
        boundary_error("HTTP/2 event payloads lost their discriminated contract"
            "every materialized event must have one typed payload, with close and GOAWAY metadata on their actual owners")
    endif()
endif()

set(HTTP2_EVENT_TEST "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
set(HTTP_RESPONSE_CONTENT_SEMANTICS_TEST
    "${RUVIA_ROOT}/tests/unit_http_response_content_semantics.cpp")
set(HTTP2_PEER_SETTINGS_TEST "${RUVIA_ROOT}/tests/unit_http2_peer_settings.cpp")
set(HTTP2_LOCAL_CONTENT_TEST
    "${RUVIA_ROOT}/tests/unit_http2_local_content_state.cpp")
set(HTTP2_LOCAL_SEND_TEST
    "${RUVIA_ROOT}/tests/unit_http2_stream_lifecycle.cpp")
set(HTTP2_REMOTE_CONTENT_TEST
    "${RUVIA_ROOT}/tests/unit_http2_remote_content_state.cpp")
set(HTTP2_BODY_STATE_TEST
    "${RUVIA_ROOT}/tests/unit_http2_body_state.cpp")
set(HTTP2_CONNECT_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connect.cpp")
set(HTTP_PACKAGE_CONSUMER "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
if(NOT EXISTS "${HTTP_RESPONSE_CONTENT_SEMANTICS_TEST}" OR
   NOT EXISTS "${HTTP_PACKAGE_CONSUMER}")
    boundary_error("shared response-content semantics are untested"
        "unit and installed consumers must pin every exclusive response classification")
else()
    file(READ "${HTTP_RESPONSE_CONTENT_SEMANTICS_TEST}"
        http_response_content_semantics_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}"
        http_response_content_semantics_package_test)
    if(NOT http_response_content_semantics_test MATCHES
           "response_content_semantics_owns_method_status_precedence" OR
       NOT http_response_content_semantics_test MATCHES
           "response_content_semantics_preserves_case_sensitive_method_tokens" OR
       NOT http_response_content_semantics_test MATCHES
           "HttpKnownMethod::kConnect, 204" OR
       NOT http_response_content_semantics_test MATCHES
           "HttpKnownMethod::kGet, 205" OR
       NOT http_response_content_semantics_package_test MATCHES
           "HasHttpResponseContentAlternatives" OR
       NOT http_response_content_semantics_package_test MATCHES
           "!std::default_initializable<[\r\n \t]*ruvia::detail::HttpResponseContentSemantics>" OR
       NOT http_response_content_semantics_package_test MATCHES
           "httpResponseContentSemantics")
        boundary_error("shared response-content semantics ownership is under-tested"
            "method/status precedence, case sensitivity, CONNECT, no-content, and installed alternatives must remain explicit")
    endif()
endif()
if(NOT EXISTS "${HTTP2_PEER_SETTINGS_TEST}")
    boundary_error("HTTP/2 peer SETTINGS result contract is untested"
        "unit_http2_peer_settings.cpp must pin all exclusive alternatives")
elseif(EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_PEER_SETTINGS_TEST}" http2_peer_settings_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http_package_consumer)
    if(NOT http2_peer_settings_test MATCHES
           "peer_setting_apply_result_is_discriminated" OR
       NOT http2_peer_settings_test MATCHES
           "!std::default_initializable<Http2PeerSettingApplyResult>" OR
       NOT http2_peer_settings_test MATCHES
           "!HasPeerSettingStatusField<Http2PeerSettingApplyResult>" OR
       NOT http2_peer_settings_test MATCHES
           "!HasPeerSettingChangedField<Http2PeerSettingApplyResult>" OR
       NOT http2_peer_settings_test MATCHES
           "!HasPeerSettingDeltaField<Http2PeerSettingApplyResult>" OR
       NOT http2_peer_settings_test MATCHES
           "HasPeerSettingDeltaAccessor<Http2PeerInitialWindowChange>" OR
       NOT http2_peer_settings_test MATCHES
           "HasPeerSettingErrorAccessor<Http2PeerSettingFailure>" OR
       NOT http2_peer_settings_test MATCHES
           "Http2PeerSettingError::kInvalidInitialWindow" OR
       NOT http2_peer_settings_test MATCHES
           "Http2ErrorCode::kFlowControlError" OR
       NOT http_package_consumer MATCHES
           "Http2PeerSettingApplyResult" OR
       NOT http_package_consumer MATCHES
           "Http2PeerSettingApplied" OR
       NOT http_package_consumer MATCHES
           "Http2PeerInitialWindowChange" OR
       NOT http_package_consumer MATCHES
           "Http2PeerSettingFailure" OR
       NOT http_package_consumer MATCHES
           "!HasPeerSettingStatusField" OR
       NOT http_package_consumer MATCHES
           "!HasPeerSettingChangedField" OR
       NOT http_package_consumer MATCHES
           "!HasPeerSettingDeltaField" OR
       NOT http_package_consumer MATCHES
           "windowSetting[.]initialWindowChange[(][)]->delta[(][)]" OR
       NOT http_package_consumer MATCHES
           "invalidSetting[.]failure[(][)]->error[(][)]")
        boundary_error("HTTP/2 peer SETTINGS result contract is under-tested"
            "unit and installed-package consumers must pin payload-free application, delta-only change, and error-only failure")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_SEND_TEST}")
    boundary_error("HTTP/2 local send alternatives are untested"
        "unit_http2_stream_lifecycle.cpp must pin every transition and exclusive payload owner")
elseif(EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_LOCAL_SEND_TEST}" http2_local_send_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_local_send_package_test)
    if(NOT http2_local_send_test MATCHES
           "http2_local_send_state_request_content_has_exclusive_transitions" OR
       NOT http2_local_send_test MATCHES
           "http2_local_send_state_response_content_and_trailers_are_distinct" OR
       NOT http2_local_send_test MATCHES
           "http2_local_send_state_connect_waits_for_acceptance" OR
       NOT http2_local_send_test MATCHES
           "http2_local_send_state_abort_owns_immutable_close_source" OR
       NOT http2_local_send_test MATCHES
           "!std::default_initializable<Http2LocalSendState>" OR
       NOT http2_local_send_test MATCHES
           "!std::default_initializable<Http2StreamLifecycle>" OR
       NOT http2_local_send_test MATCHES
           "!std::default_initializable<Http2LocalHeadPending>" OR
       NOT http2_local_send_test MATCHES
           "HasCloseSource<Http2StreamAborted>" OR
       NOT http2_local_send_test MATCHES
           "std::constructible_from<[\r\n \t]*Http2StreamAborted" OR
       NOT http2_local_send_test MATCHES
           "!HasStaleLocalSendProduct<Http2StreamLifecycle>" OR
       NOT http2_local_send_test MATCHES
           "!HasStaleResetAccessor<Http2LocalSendState>" OR
       NOT http2_local_send_test MATCHES
           "!HasStaleResetAccessor<Http2StreamLifecycle>" OR
       NOT http2_local_send_test MATCHES
           "!HasStaleMarkReset<Http2StreamLifecycle>" OR
       NOT http2_local_send_test MATCHES
           "!HasStaleMarkClosed<Http2StreamLifecycle>" OR
       NOT http2_local_send_test MATCHES
           "stream[.]abort[(]Http2StreamCloseSource::kNone[)]" OR
       NOT http2_local_send_test MATCHES "!queuedThenAborted[.]queued[(][)]" OR
       NOT http2_local_send_test MATCHES
           "static_cast<Http2StreamCloseSource>[(]0xFF[)]" OR
       NOT http2_local_send_package_test MATCHES
           "HasHttp2LocalSendAlternatives" OR
       NOT http2_local_send_package_test MATCHES
           "!std::default_initializable<[\r\n \t]*ruvia::detail::Http2LocalSendState" OR
       NOT http2_local_send_package_test MATCHES
           "!HasStaleHttp2LocalSendProduct" OR
       NOT http2_local_send_package_test MATCHES
           "!HasStaleHttp2StreamLocalSendForwarders" OR
       NOT http2_local_send_package_test MATCHES "HasHttp2AbortLifecycle" OR
       NOT http2_local_send_package_test MATCHES "!HasStaleHttp2IsReset" OR
       NOT http2_local_send_package_test MATCHES "!HasStaleHttp2MarkReset" OR
       NOT http2_local_send_package_test MATCHES "!HasStaleHttp2MarkClosed" OR
       NOT http2_local_send_package_test MATCHES "HasHttp2RemoveAborted" OR
       NOT http2_local_send_package_test MATCHES "!HasStaleHttp2RemoveReset" OR
       NOT http2_local_send_package_test MATCHES
           "HasHttp2LocalCloseSource" OR
       NOT http2_local_send_package_test MATCHES
           "std::constructible_from<[\r\n \t]*ruvia::detail::Http2StreamAborted" OR
       NOT http2_local_send_package_test MATCHES
           "const ruvia::detail::Http2LocalSendState&" OR
       NOT http2_local_send_package_test MATCHES
           "localSendStream[.]beginLocalResponseTrailersOnly" OR
       NOT http2_local_send_package_test MATCHES
           "localSend[.]responseTrailersOnly[(][)]" OR
       NOT http2_local_send_package_test MATCHES
           "localSend[.]endStreamQueued[(][)]" OR
       NOT http2_local_send_package_test MATCHES
           "localSend[.]aborted[(][)][-][>]source[(][)]")
        boundary_error("HTTP/2 local send alternative ownership is under-tested"
            "unit and installed consumers must reject phase/kind/boolean products, private alternatives, none/invalid abort sources, reset vocabulary, and stale forwarding accessors")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_SEND_TEST}" OR
   NOT EXISTS "${HTTP2_CONNECT_TEST}")
    boundary_error("HTTP/2 remote receive alternatives are untested"
        "stream lifecycle and CONNECT tests must pin every remote transition and terminal-flow regression")
elseif(EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_LOCAL_SEND_TEST}" http2_remote_receive_test)
    file(READ "${HTTP2_CONNECT_TEST}" http2_remote_receive_connect_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_remote_receive_package_test)
    if(NOT http2_remote_receive_test MATCHES
           "http2_remote_receive_state_owns_head_content_connect_and_terminal_transitions" OR
       NOT http2_remote_receive_test MATCHES
           "!std::default_initializable<Http2RemoteReceiveState>" OR
       NOT http2_remote_receive_test MATCHES
           "Http2RemoteConnectRejectedAwaitingEndStream" OR
       NOT http2_remote_receive_test MATCHES
           "!HasStaleBodyEnded<Http2StreamState>" OR
       NOT http2_remote_receive_test MATCHES
           "!HasStalePeerEndStream<Http2StreamState>" OR
       NOT http2_remote_receive_test MATCHES
           "!HasStaleHeadersDecoded<Http2StreamState>" OR
       NOT http2_remote_receive_connect_test MATCHES
           "http2_connect_server_rejection_accepts_empty_terminal_data" OR
       NOT http2_remote_receive_connect_test MATCHES
           "http2_connect_pending_accepts_empty_request_half_close" OR
       NOT http2_remote_receive_connect_test MATCHES
           "http2_connect_open_tunnel_replenishes_deferred_stream_window" OR
       NOT http2_remote_receive_package_test MATCHES
           "HasHttp2RemoteReceiveAlternatives" OR
       NOT http2_remote_receive_package_test MATCHES
           "!std::default_initializable<[\r\n \t]*ruvia::detail::Http2RemoteReceiveState" OR
       NOT http2_remote_receive_package_test MATCHES
           "!HasStaleHttp2BodyEnded" OR
       NOT http2_remote_receive_package_test MATCHES
           "!HasStaleHttp2PeerEndStream" OR
       NOT http2_remote_receive_package_test MATCHES
           "!HasStaleHttp2HeadersDecoded" OR
       NOT http2_remote_receive_package_test MATCHES
           "const ruvia::detail::Http2RemoteReceiveState&" OR
       NOT http2_remote_receive_package_test MATCHES
           "remoteReceiveStream[.]finishRemoteRejectedConnect" OR
       NOT http2_remote_receive_package_test MATCHES
           "remotePendingEndStream[.]finishRemotePendingConnect")
        boundary_error("HTTP/2 remote receive alternative ownership is under-tested"
            "unit and installed consumers must reject head/body/peer booleans, pin private alternatives, and preserve rejected-CONNECT termination plus tunnel stream-window replenishment")
    endif()
endif()
if(NOT EXISTS "${HTTP2_CONNECT_TEST}")
    boundary_error("HTTP/2 CONNECT tunnel alternatives are untested"
        "unit_http2_connect.cpp must pin phase and form ownership")
elseif(EXISTS "${HTTP2_EVENT_TEST}" AND EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_CONNECT_TEST}" http2_tunnel_test)
    file(READ "${HTTP2_EVENT_TEST}" http2_tunnel_connection_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_tunnel_package_test)
    if(NOT http2_tunnel_test MATCHES
           "http2_tunnel_state_alternatives_own_valid_transitions" OR
       NOT http2_tunnel_test MATCHES
           "HasConnectForm<Http2ConnectPending>" OR
       NOT http2_tunnel_test MATCHES
           "!HasStaleTunnelKindPhase<Http2TunnelState>" OR
       NOT http2_tunnel_test MATCHES
           "static_cast<Http2ConnectForm>[(]0xFF[)]" OR
       NOT http2_tunnel_test MATCHES "state[.]notConnect[(][)]" OR
       NOT http2_tunnel_test MATCHES "state[.]pending[(][)]" OR
       NOT http2_tunnel_test MATCHES "state[.]open[(][)]" OR
       NOT http2_tunnel_test MATCHES "rejected[.]rejected[(][)]" OR
       NOT http2_tunnel_connection_test MATCHES
           "!HasStaleTunnelForwarders<Http2StreamState>" OR
       NOT http2_tunnel_package_test MATCHES
           "HasHttp2TunnelAlternatives" OR
       NOT http2_tunnel_package_test MATCHES
           "!HasStaleHttp2TunnelKindPhase" OR
       NOT http2_tunnel_package_test MATCHES
           "!HasStaleHttp2StreamTunnelForwarders" OR
       NOT http2_tunnel_package_test MATCHES
           "HasHttp2ConnectForm" OR
       NOT http2_tunnel_package_test MATCHES
           "tunnel[.]pending[(][)]->form[(][)]")
        boundary_error("HTTP/2 CONNECT tunnel alternative ownership is under-tested"
            "unit and installed consumers must reject kind/phase products and inspect form only on pending")
    endif()
endif()
if(NOT EXISTS "${HTTP2_REMOTE_CONTENT_TEST}" OR
   NOT EXISTS "${HTTP2_BODY_STATE_TEST}")
    boundary_error("HTTP/2 remote content alternatives are untested"
        "unit tests must pin typed length ownership and transactional DATA acceptance")
elseif(EXISTS "${HTTP2_EVENT_TEST}" AND EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_REMOTE_CONTENT_TEST}" http2_remote_content_test)
    file(READ "${HTTP2_BODY_STATE_TEST}" http2_remote_body_state_test)
    file(READ "${HTTP2_EVENT_TEST}" http2_remote_connection_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_remote_package_test)
    if(NOT http2_remote_content_test MATCHES
           "http2_remote_content_allowance_and_length_alternatives_are_explicit" OR
       NOT http2_remote_content_test MATCHES
           "http2_remote_content_metadata_only_preserves_representation_length" OR
       NOT http2_remote_content_test MATCHES
           "http2_remote_content_accounting_is_atomic" OR
       NOT http2_remote_content_test MATCHES
           "http2_remote_content_counter_overflow_is_atomic" OR
       NOT http2_remote_content_test MATCHES
           "http2_remote_content_rejects_late_semantic_transitions" OR
       NOT http2_remote_content_test MATCHES
           "HasDeclaredLength<Http2RemoteContentAllowedKnownLength>" OR
       NOT http2_remote_content_test MATCHES
           "HasDeclaredLength<[\r\n \t]*Http2RemoteContentMetadataOnlyKnownLength>" OR
       NOT http2_remote_content_test MATCHES
           "!HasStaleLengthTuple<Http2RemoteContentState>" OR
       NOT http2_remote_content_test MATCHES
           "!HasReceivedBytes<Http2RemoteContentState>" OR
       NOT http2_remote_content_test MATCHES
           "!HasStaleCheckAcceptSplit<Http2RemoteContentState>" OR
       NOT http2_remote_content_test MATCHES
           "Http2RemoteContentAccountingResult::kCounterOverflow" OR
       NOT http2_remote_content_test MATCHES
           "Http2RemoteContentAccountingResult::kDeclaredLengthExceeded" OR
       NOT http2_remote_content_test MATCHES
           "Http2RemoteContentAccountingResult::kContentForbidden" OR
       NOT http2_remote_body_state_test MATCHES
           "Http2BodyAccountingResult::kContentLengthExceeded" OR
       NOT http2_remote_body_state_test MATCHES
           "Http2BodyAccountingResult::kContentForbidden" OR
       NOT http2_remote_body_state_test MATCHES
           "remoteContent[(][)][.]allowedWithoutLength[(][)][-][>]receivedBytes[(][)]" OR
       NOT http2_remote_body_state_test MATCHES
           "h2_remote_content_terminal_validation_is_owned_by_active_alternative" OR
       NOT http2_remote_body_state_test MATCHES
           "selectRemoteContentMetadataOnly" OR
       NOT http2_remote_connection_test MATCHES "remoteKnownLength" OR
       NOT http2_remote_connection_test MATCHES
           "http2_connection_client_head_representation_length_survives_trailer_terminal" OR
       NOT http2_remote_connection_test MATCHES
           "http2_connection_client_rejects_data_for_responses_without_content" OR
       NOT http2_remote_connection_test MATCHES
           "http2_connection_client_allows_empty_terminal_data_without_content_event" OR
       NOT http2_remote_package_test MATCHES
           "HasHttp2RemoteContentAlternatives" OR
       NOT http2_remote_package_test MATCHES
           "!HasStaleHttp2RemoteContentTuple" OR
       NOT http2_remote_package_test MATCHES
           "!HasHttp2RemoteReceivedBytes<[\r\n \t]*ruvia::detail::Http2RemoteContentState>" OR
       NOT http2_remote_package_test MATCHES
           "!HasStaleHttp2RemoteCheckAcceptSplit" OR
       NOT http2_remote_package_test MATCHES
           "!HasStaleHttp2StreamRemoteContentForwarders")
        boundary_error("HTTP/2 remote content ownership is under-tested"
            "unit and installed consumers must pin metadata-only alternatives, atomic accounting, and malformed no-content DATA rejection")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_CONTENT_TEST}")
    boundary_error("HTTP/2 local content alternatives are untested"
        "unit_http2_local_content_state.cpp must pin state and payload ownership")
elseif(EXISTS "${HTTP2_EVENT_TEST}" AND EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_LOCAL_CONTENT_TEST}" http2_local_content_test)
    file(READ "${HTTP2_EVENT_TEST}" http2_local_content_connection_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_local_content_package_test)
    if(NOT http2_local_content_test MATCHES
           "http2_local_content_known_length_preflight_is_transactional" OR
       NOT http2_local_content_test MATCHES
           "http2_local_content_alternatives_are_explicit" OR
       NOT http2_local_content_test MATCHES
           "!HasLocalContentMode<Http2LocalContentState>" OR
       NOT http2_local_content_test MATCHES
           "HasDeclaredLength<Http2LocalContentKnownLength>" OR
       NOT http2_local_content_test MATCHES
           "Http2LocalContentCheck::kNotStarted" OR
       NOT http2_local_content_test MATCHES "!state[.]lengthComplete[(][)]" OR
       NOT http2_local_content_connection_test MATCHES
           "!HasStaleLocalContentForwarders<Http2StreamState>" OR
       NOT http2_local_content_connection_test MATCHES
           "requireLocalKnownLength" OR
       NOT http2_local_content_package_test MATCHES
           "HasHttp2LocalContentAlternatives" OR
       NOT http2_local_content_package_test MATCHES
           "!HasStaleHttp2LocalModeAccessor" OR
       NOT http2_local_content_package_test MATCHES
           "!HasStaleHttp2StreamLocalContentForwarders")
        boundary_error("HTTP/2 local content alternative ownership is under-tested"
            "unit and installed consumers must reject mode/fake-length access, pin unset rejection, and inspect counters through one const state")
    endif()
endif()
if(EXISTS "${HTTP2_EVENT_TEST}" AND EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_EVENT_TEST}" http2_event_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http_package_consumer)
    if(NOT http2_event_test MATCHES
           "http2_connection_request_content_alternatives_own_wire_framing" OR
       NOT http2_event_test MATCHES
           "!HasRequestContentMode<Http2RequestContent>" OR
       NOT http2_event_test MATCHES
           "HasRequestContentLength<[\r\n \t]*ruvia::detail::Http2KnownLengthRequestContent>" OR
       NOT http2_event_test MATCHES "withoutContent[.]withoutContent" OR
       NOT http2_event_test MATCHES "zeroLength[.]knownLengthContent" OR
       NOT http2_event_test MATCHES "streaming[.]streamingContent" OR
       NOT http_package_consumer MATCHES
           "HasHttp2RequestContentAlternatives" OR
       NOT http_package_consumer MATCHES
           "!HasStaleHttp2ContentMode" OR
       NOT http_package_consumer MATCHES
           "HasHttp2RequestContentLength<[\r\n \t]*ruvia::detail::Http2KnownLengthRequestContent>")
        boundary_error("HTTP/2 request-content alternatives are under-tested"
            "unit and installed consumers must pin absent, explicit zero-length, and streaming payload ownership")
    endif()
    if(NOT http2_event_test MATCHES
           "http2_connection_event_queue_is_optional_and_discriminated" OR
       NOT http2_event_test MATCHES
           "closed->error[(][)] == Http2ErrorCode::kCancel" OR
       NOT http2_event_test MATCHES
           "closed->error[(][)] == Http2ErrorCode::kProtocolError" OR
       NOT http2_event_test MATCHES
           "event[.]goaway[(][)]->lastStreamId[(][)]" OR
       NOT http2_event_test MATCHES
           "event[.]requestUnprocessed[(][)]->streamId[(][)]" OR
       NOT http_package_consumer MATCHES
           "std::optional<ruvia::detail::Http2Event>" OR
       NOT http_package_consumer MATCHES
           "default_initializable<ruvia::detail::Http2Event>" OR
       NOT http_package_consumer MATCHES
           "Http2RequestUnprocessedEvent")
        boundary_error("HTTP/2 typed event contract is under-tested"
            "unit and installed-package consumers must pin optional draining, exclusive payloads, RST reasons, and GOAWAY ownership")
    endif()
    if(NOT http2_event_test MATCHES
           "http2_connection_feed_before_begin_retains_input_and_is_retryable" OR
       NOT http2_event_test MATCHES "std::is_enum_v<Http2FeedResult>" OR
       NOT http2_event_test MATCHES "!HasFeedStatusField<Http2FeedResult>" OR
       NOT http2_event_test MATCHES "!HasFeedConsumedField<Http2FeedResult>" OR
       NOT http_package_consumer MATCHES
           "std::is_enum_v<ruvia::detail::Http2FeedResult>" OR
       NOT http_package_consumer MATCHES
           "!HasFeedStatusField<ruvia::detail::Http2FeedResult>" OR
       NOT http_package_consumer MATCHES
           "!HasFeedConsumedField<ruvia::detail::Http2FeedResult>" OR
       NOT http_package_consumer MATCHES
           "Http2FeedResult::kConnectionNotStarted")
        boundary_error("HTTP/2 feed ownership contract is under-tested"
            "unit and installed-package consumers must pin the direct enum shape, removed tuple fields, and retained pre-start input")
    endif()
    if(NOT http2_event_test MATCHES "Http2SubmittedRequestHead" OR
       NOT http2_event_test MATCHES "Http2RequestHeadSubmitFailure" OR
       NOT http2_event_test MATCHES
           "!HasRequestHeadStatusAccessor<Http2RequestHeadSubmitResult>" OR
       NOT http2_event_test MATCHES
           "!HasRequestHeadAcceptedAccessor<Http2RequestHeadSubmitResult>" OR
       NOT http2_event_test MATCHES
           "!HasRequestHeadStreamIdAccessor<Http2RequestHeadSubmitResult>" OR
       NOT http2_event_test MATCHES
           "!std::constructible_from<Http2SubmittedRequestHead, std::uint32_t>" OR
       NOT http2_event_test MATCHES
           "HasRequestHeadStreamIdAccessor<Http2SubmittedRequestHead>" OR
       NOT http2_event_test MATCHES
           "HasRequestHeadErrorAccessor<Http2RequestHeadSubmitFailure>" OR
       NOT http_package_consumer MATCHES
           "Http2SubmittedRequestHead" OR
       NOT http_package_consumer MATCHES
           "Http2RequestHeadSubmitFailure" OR
       NOT http_package_consumer MATCHES
           "!HasRequestHeadStatusAccessor" OR
       NOT http_package_consumer MATCHES
           "!HasRequestHeadAcceptedAccessor" OR
       NOT http_package_consumer MATCHES
           "!HasRequestHeadStreamIdAccessor" OR
       NOT http_package_consumer MATCHES
           "!std::constructible_from" OR
       NOT http_package_consumer MATCHES
           "submittedRequest->streamId[(][)]" OR
       NOT http_package_consumer MATCHES
           "unavailable[.]failure[(][)]->error[(][)]")
        boundary_error("HTTP/2 request-head result contract is under-tested"
            "unit and installed-package consumers must pin exclusive submitted/failure payloads and removed top-level accessors")
    endif()
    if(NOT http2_event_test MATCHES
           "http2_connection_response_head_submit_result_is_discriminated" OR
       NOT http2_event_test MATCHES
           "Http2SubmittedBufferedResponseHead" OR
       NOT http2_event_test MATCHES
           "Http2SubmittedStreamingResponseHead" OR
       NOT http2_event_test MATCHES
           "Http2ResponseHeadSubmitFailure" OR
       NOT http2_event_test MATCHES
           "!HasResponseHeadStatusAccessor" OR
       NOT http2_event_test MATCHES
           "!HasResponseHeadAcceptedAccessor" OR
       NOT http2_event_test MATCHES
           "!HasResponseHeadPlanAccessor" OR
       NOT http2_event_test MATCHES
           "HasResponseHeadErrorAccessor" OR
       NOT http_package_consumer MATCHES
           "Http2BufferedResponseHeadSubmitResult" OR
       NOT http_package_consumer MATCHES
           "Http2StreamingResponseHeadSubmitResult" OR
       NOT http_package_consumer MATCHES
           "Http2SubmittedBufferedResponseHead" OR
       NOT http_package_consumer MATCHES
           "Http2SubmittedStreamingResponseHead" OR
       NOT http_package_consumer MATCHES
           "Http2ResponseHeadSubmitFailure" OR
       NOT http_package_consumer MATCHES
           "!HasResponseHeadStatusAccessor" OR
       NOT http_package_consumer MATCHES
           "!HasResponseHeadAcceptedAccessor" OR
       NOT http_package_consumer MATCHES
           "!HasResponseHeadPlanAccessor")
        boundary_error("HTTP/2 response-head result contract is under-tested"
            "unit and installed-package consumers must pin plan-only success, error-only failure, and removed top-level accessors")
    endif()
endif()

set(HTTP2_CONNECTION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
if(EXISTS "${HTTP2_CONNECTION_HEADER}")
    file(READ "${HTTP2_CONNECTION_HEADER}" http2_connection_header)
    if(NOT http2_connection_header MATCHES "submitRegularRequestHead" OR
       NOT http2_connection_header MATCHES "Http2RequestContent content" OR
       NOT http2_connection_header MATCHES "Http2RequestHeadSubmitResult" OR
       NOT http2_connection_header MATCHES "Http2RequestHeadSubmitError" OR
       NOT http2_connection_header MATCHES "class Http2SubmittedRequestHead final" OR
       NOT http2_connection_header MATCHES
           "class Http2RequestHeadSubmitFailure final" OR
       NOT http2_connection_header MATCHES "streamId_ == 0" OR
       NOT http2_connection_header MATCHES "[(]streamId_ [&] 1U[)] == 0" OR
       NOT http2_connection_header MATCHES
           "std::variant<[ \t\r\n]*Http2SubmittedRequestHead,[ \t\r\n]*Http2RequestHeadSubmitFailure" OR
       NOT http2_connection_header MATCHES "submitted[(][)] const noexcept" OR
       NOT http2_connection_header MATCHES "failure[(][)] const noexcept" OR
       NOT http2_connection_header MATCHES "kPeerStreamLimitReached" OR
       http2_connection_header MATCHES "submitRequestHead")
        boundary_error("HTTP/2 client request API restored an ambiguous framing entry"
            "request submission must keep dedicated entries and exclusive submitted/failure payloads")
    endif()
    if(NOT http2_connection_header MATCHES
           "enum class Http2ResponseHeadSubmitError" OR
       NOT http2_connection_header MATCHES
           "class Http2SubmittedResponseHead final" OR
       NOT http2_connection_header MATCHES
           "class Http2ResponseHeadSubmitFailure final" OR
       NOT http2_connection_header MATCHES
           "class Http2ResponseHeadSubmitResult final" OR
       NOT http2_connection_header MATCHES
           "std::variant<Submitted, Http2ResponseHeadSubmitFailure>" OR
       NOT http2_connection_header MATCHES "std::get_if<Submitted>" OR
       NOT http2_connection_header MATCHES
           "std::get_if<Http2ResponseHeadSubmitFailure>" OR
       NOT http2_connection_header MATCHES
           "Http2BufferedResponseHeadSubmitResult" OR
       NOT http2_connection_header MATCHES
           "Http2StreamingResponseHeadSubmitResult" OR
       NOT http2_connection_header MATCHES
           "Http2SubmittedBufferedResponseHead" OR
       NOT http2_connection_header MATCHES
           "Http2SubmittedStreamingResponseHead")
        boundary_error("HTTP/2 final response-head result lost exclusive ownership"
            "only submitted heads may expose buffered/streaming plans and only failures may expose their typed error")
    endif()
    if(NOT http2_connection_header MATCHES "submitConnectRequestHead" OR
       NOT http2_connection_header MATCHES "submitExtendedConnectRequestHead" OR
       NOT http2_connection_header MATCHES "submitConnectResponseHead" OR
       NOT http2_connection_header MATCHES "kPeerCapabilityUnavailable")
        boundary_error("HTTP/2 CONNECT restored an implicit request path"
            "standard, extended, server acceptance, and SETTINGS-gate statuses need dedicated API")
    endif()
    if(NOT http2_connection_header MATCHES "Http2ConnectionLimits" OR
       NOT http2_connection_header MATCHES "Http2Role role" OR
       NOT http2_connection_header MATCHES "beginConnection" OR
       NOT http2_connection_header MATCHES "enum class Http2FeedResult" OR
       NOT http2_connection_header MATCHES "enum class PrefacePhase" OR
       NOT http2_connection_header MATCHES "kNotStarted" OR
       NOT http2_connection_header MATCHES "kAwaitingClientMagic" OR
       NOT http2_connection_header MATCHES "kAwaitingPeerSettings" OR
       NOT http2_connection_header MATCHES "kConnectionNotStarted" OR
       NOT http2_connection_header MATCHES "kEventsPending" OR
       NOT http2_connection_header MATCHES "kAccepted" OR
       NOT http2_connection_header MATCHES "kNeedInput" OR
       NOT http2_connection_header MATCHES "kProtocolFailure")
        boundary_error("HTTP/2 connection startup restored ambiguous configuration ordering"
            "role-aware startup and the direct all-or-nothing feed ownership enum must remain")
    endif()
    if(NOT http2_connection_header MATCHES
           "ruvia/http/detail/http2/Http2Event.h" OR
       NOT http2_connection_header MATCHES
           "std::optional<Http2PeerGoaway> peerGoaway" OR
       NOT http2_connection_header MATCHES
           "std::optional<Http2ErrorCode> connectionError" OR
       NOT http2_connection_header MATCHES
           "std::optional<Http2Event> nextEvent")
        boundary_error("HTTP/2 peer GOAWAY lost typed lifecycle observability"
            "optional event draining, last-stream-id, error code, and per-request safe-retry events must remain public protocol facts")
    endif()
endif()

set(WEB_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
if(EXISTS "${WEB_HTTP2_SESSION}")
    file(READ "${WEB_HTTP2_SESSION}" web_http2_session)
    if(NOT web_http2_session MATCHES "event->tunnelData[(][)]" OR
       NOT web_http2_session MATCHES "event->tunnelEnd[(][)]")
        boundary_error("ruvia-web collapses tunnel bytes back into HTTP message content"
            "the HTTP/2 driver must consume the core's dedicated tunnel DATA and FIN events")
    endif()
    if(NOT web_http2_session MATCHES "event->streamClosed[(][)]" OR
       NOT web_http2_session MATCHES "eraseRouteState[(]streamId[)]" OR
       NOT web_http2_session MATCHES "already-removed core state")
        boundary_error("ruvia-web re-derived an already-closed HTTP/2 stream"
            "stream-close cleanup must use the typed event ID without querying erased protocol state")
    endif()
    if(NOT web_http2_session MATCHES "feedAndDrain" OR
       NOT web_http2_session MATCHES "Http2FeedResult::kEventsPending" OR
       NOT web_http2_session MATCHES "Http2FeedResult::kConnectionNotStarted" OR
       NOT web_http2_session MATCHES "Http2FeedResult::kProtocolFailure" OR
       web_http2_session MATCHES "(result|feedResult)[.]consumed")
        boundary_error("ruvia-web bypasses HTTP/2 feed ownership"
            "all inbound spans must share drain/retry ownership, trust the direct enum, and stop on its typed terminal failure")
    endif()
endif()

set(CORE_SANSIO_DRIVER
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/SansIoDriver.h")
if(EXISTS "${CORE_SANSIO_DRIVER}")
    file(READ "${CORE_SANSIO_DRIVER}" core_sansio_driver)
    if(NOT core_sansio_driver MATCHES "typename ShouldStop" OR
       NOT core_sansio_driver MATCHES "if.*shouldStop.*connection")
        boundary_error("generic sans-I/O driver regained protocol lifecycle coupling"
            "the protocol adapter must explicitly supply an inlinable transport-stop predicate")
    endif()
endif()

foreach(web_h2_content_consumer IN ITEMS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
    if(EXISTS "${web_h2_content_consumer}")
        file(READ "${web_h2_content_consumer}" web_h2_content_consumer_source)
        if(NOT web_h2_content_consumer_source MATCHES "kContentLengthExceeded" OR
           NOT web_h2_content_consumer_source MATCHES "kContentLengthIncomplete")
            file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${web_h2_content_consumer}")
            boundary_error("ruvia-web can busy-retry an HTTP/2 length rejection"
                "${relative} must handle both zero-ownership Content-Length statuses")
        endif()
    endif()
endforeach()

set(WEB_HTTP1_STREAM_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h")
set(HTTP1_SERVER_CONNECTION_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerConnectionPlan.h")
set(HTTP1_SERVER_SEMANTICS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h")
set(HTTP1_PARSER_INTERNAL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerRequestParser.h")
set(HTTP1_PARSER_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp")
set(PUBLIC_HTTP1_REQUEST_PARSER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1RequestParser.h")
if(NOT EXISTS "${HTTP1_SERVER_CONNECTION_PLAN}")
    boundary_error("HTTP/1 server connection plan is missing"
        "ruvia-http must own Http1ServerConnectionPlan")
else()
    file(READ "${HTTP1_SERVER_CONNECTION_PLAN}" http1_server_connection_plan)
    if(NOT http1_server_connection_plan MATCHES "class Http1ServerConnectionPlan" OR
       NOT http1_server_connection_plan MATCHES "HttpProtocolVersion protocolVersion_" OR
       NOT http1_server_connection_plan MATCHES "protocolVersion[(][)] const noexcept" OR
       NOT http1_server_connection_plan MATCHES "http1PlanHttp10RequestConnection" OR
       NOT http1_server_connection_plan MATCHES "http1PlanHttp11RequestConnection" OR
       NOT http1_server_connection_plan MATCHES "http11Close" OR
       NOT http1_server_connection_plan MATCHES "requireClose")
        boundary_error("HTTP/1 connection plan lost part of its typed contract"
            "exact protocol version, disposition, version-specific parser construction, and close-only tightening must stay bound")
    endif()
    if(http1_server_connection_plan MATCHES
           "${RULE_STALE_HTTP1_RESPONSE_VERSION_SIGNAL}")
        boundary_error("HTTP/1 connection plan compressed its protocol version"
            "the exact HTTP/1.0 or HTTP/1.1 value must survive; responseSignal and the generic version factory are forbidden")
    endif()
endif()
if(EXISTS "${HTTP1_SERVER_SEMANTICS}")
    file(READ "${HTTP1_SERVER_SEMANTICS}" http1_server_semantics)
    if(NOT http1_server_semantics MATCHES "Http1RequestBodyConsumption" OR
       NOT http1_server_semantics MATCHES "Http1ServerConnectionPlan requestConnectionPlan" OR
       NOT http1_server_semantics MATCHES "Http1ServerClosePolicy closePolicy" OR
       NOT http1_server_semantics MATCHES "controlResult[.]failure[(][)]" OR
       NOT http1_server_semantics MATCHES "controlResult[.]plan[(][)]" OR
       NOT http1_server_semantics MATCHES "controlPlan->http1[(][)]" OR
       NOT http1_server_semantics MATCHES "std::nullopt" OR
       NOT http1_server_semantics MATCHES "bodyPlan\\.bodySuppressed\\(\\)" OR
       NOT http1_server_semantics MATCHES "http1FinalizeResponseConnection" OR
       NOT http1_server_semantics MATCHES "plan[.]protocolVersion[(][)]")
        boundary_error("HTTP/1 connection lifecycle lost its commit-time typed plan"
            "request version, shared final-control result, runtime policy, response body semantics, status-line bytes, and socket disposition must share one typed path")
    endif()
    if(http1_server_semantics MATCHES "http1RequestNeedsKeepAliveSignal" OR
       http1_server_semantics MATCHES "needsKeepAliveSignal" OR
       http1_server_semantics MATCHES "http1RequestConnectionDisposition")
        boundary_error("HTTP/1 connection plan was split back into scalar facts"
            "exact protocol version and disposition must remain bound in Http1ServerConnectionPlan")
    endif()
    if(http1_server_semantics MATCHES
           "httpFinalResponseControlPlan\\([^)]*HttpProtocolVersion::kHttp11")
        boundary_error("HTTP/1 final response control hard-coded its version"
            "HttpFinalResponseControlPlan must consume the exact version retained by Http1ServerConnectionPlan")
    endif()
endif()
if(EXISTS "${HTTP1_PARSER_INTERNAL}" AND EXISTS "${HTTP1_PARSER_SOURCE}")
    file(READ "${HTTP1_PARSER_INTERNAL}" http1_parser_internal)
    file(READ "${HTTP1_PARSER_SOURCE}" http1_parser_source)
    if(NOT http1_parser_internal MATCHES "Http1ServerConnectionPlan connectionPlan" OR
       NOT http1_parser_source MATCHES
           "http1PlanHttp10RequestConnection" OR
       NOT http1_parser_source MATCHES
           "http1PlanHttp11RequestConnection" OR
       NOT http1_parser_source MATCHES
           "state[.]connectionPlan[.]requireClose[(][)]")
        boundary_error("HTTP/1 parser stopped owning the connection plan"
            "the validated request version and flags must produce one plan stored in Http1ServerRequestParseState, and body-framing failure must preserve its version while forcing close")
    endif()
    if(NOT http1_parser_internal MATCHES "enum class Http1ServerRequestParsePhase" OR
       NOT http1_parser_internal MATCHES "kNeedRequestHead" OR
       NOT http1_parser_internal MATCHES "kRequestHeadReady" OR
       NOT http1_parser_internal MATCHES "kNeedRequestBody" OR
       NOT http1_parser_internal MATCHES "kRequestMessageReady" OR
       NOT http1_parser_internal MATCHES "class Http1ServerRequestParseState final" OR
       NOT http1_parser_internal MATCHES "class Http1ServerRequestParser final" OR
       NOT http1_parser_internal MATCHES "void parseHead" OR
       NOT http1_parser_internal MATCHES "parseMessage" OR
       NOT http1_parser_source MATCHES
           "state\\.phase_[ \\t]*=[ \\t]*Http1ServerRequestParsePhase::kRequestHeadReady" OR
       NOT http1_parser_source MATCHES
           "state\\.phase_[ \\t]*=[ \\t]*Http1ServerRequestParsePhase::kRequestMessageReady")
        boundary_error("HTTP/1 parser lost its distinct head/message phases"
            "the hot-path head parser and whole-message scanner must have different typed readiness states")
    endif()
endif()
if(NOT EXISTS "${PUBLIC_HTTP1_REQUEST_PARSER}")
    boundary_error("public typed HTTP/1 request parser is missing"
        "ruvia-http/include/ruvia/http/Http1RequestParser.h")
else()
    file(READ "${PUBLIC_HTTP1_REQUEST_PARSER}" public_http1_request_parser)
    if(NOT public_http1_request_parser MATCHES "class Http1RequestNeedMore final" OR
       NOT public_http1_request_parser MATCHES "class Http1ParsedRequest final" OR
       NOT public_http1_request_parser MATCHES "class Http1RequestParseFailure final" OR
       NOT public_http1_request_parser MATCHES "class Http1RequestParseResult final" OR
       NOT public_http1_request_parser MATCHES "std::variant" OR
       NOT public_http1_request_parser MATCHES "requiredTotalBytes" OR
       NOT public_http1_request_parser MATCHES "bodyPlan" OR
       NOT public_http1_request_parser MATCHES "wireBody" OR
       NOT public_http1_request_parser MATCHES
           "error_[ \t]*==[ \t]*HttpParseError::kNone" OR
       NOT public_http1_request_parser MATCHES
           "requiredTotalBytes_[^\r\n]*==[ \t]*0")
        boundary_error("public HTTP/1 parse result lost its discriminated contract"
            "need-more, parsed request, and failure must own disjoint facts while success retains framing bytes")
    endif()
endif()
if(EXISTS "${HTTP1_PARSER_INTERNAL}")
    file(READ "${HTTP1_PARSER_INTERNAL}" public_http1_request_parser_state)
    if(NOT public_http1_request_parser_state MATCHES "std::size_t messageBytes" OR
       NOT public_http1_request_parser_state MATCHES
           "std::optional<std::size_t> requiredTotalBytes")
        boundary_error("HTTP/1 internal parse state conflates completed and required bytes"
            "messageBytes and optional requiredTotalBytes must remain distinct")
    endif()
endif()
if(EXISTS "${HTTP1_PARSER_SOURCE}")
    file(READ "${HTTP1_PARSER_SOURCE}" public_http1_request_parser_source)
    if(NOT public_http1_request_parser_source MATCHES
           "parsed\\.requiredTotalBytes" OR
       NOT public_http1_request_parser_source MATCHES
           "buffer\\.substr[(][\r\n \t]*parsed\\.headerBytes,[\r\n \t]*parsed\\.messageBytes[ \t]*-[ \t]*parsed\\.headerBytes")
        boundary_error("public HTTP/1 parser collapsed required size or discarded wire body"
            "incomplete fixed lengths and successful body bytes need separate typed outputs")
    endif()
endif()

set(HTTP1_REQUEST_BODY_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1RequestBodyPlan.h")
if(NOT EXISTS "${HTTP1_REQUEST_BODY_PLAN}")
    boundary_error("HTTP/1 request-body plan is missing"
        "ruvia-http must own Http1RequestBodyPlan")
else()
    file(READ "${HTTP1_REQUEST_BODY_PLAN}" http1_request_body_plan)
    if(NOT http1_request_body_plan MATCHES "class Http1RequestBodyPlan" OR
       NOT http1_request_body_plan MATCHES "class Http1RequestWithoutBody final" OR
       NOT http1_request_body_plan MATCHES "class Http1KnownLengthRequestBody final" OR
       NOT http1_request_body_plan MATCHES "class Http1ChunkedRequestBody final" OR
       NOT http1_request_body_plan MATCHES "using Framing = std::variant" OR
       NOT http1_request_body_plan MATCHES "std::get_if<Http1RequestWithoutBody>" OR
       NOT http1_request_body_plan MATCHES "std::get_if<Http1KnownLengthRequestBody>" OR
       NOT http1_request_body_plan MATCHES "std::get_if<Http1ChunkedRequestBody>" OR
       NOT http1_request_body_plan MATCHES "requiresConsumption" OR
       NOT http1_request_body_plan MATCHES "Http1RequestBodyConsumption" OR
       NOT http1_request_body_plan MATCHES "transferCodings" OR
       NOT http1_request_body_plan MATCHES "HttpRequestExpectations" OR
       NOT http1_request_body_plan MATCHES "expectationAction" OR
       NOT http1_request_body_plan MATCHES "friend class Http1ServerRequestParseState" OR
       NOT http1_request_body_plan MATCHES "friend class Http1ServerRequestParser" OR
       NOT http1_request_body_plan MATCHES
           "explicit Http1RequestBodyPlan[ \t\r\n]*[(][ \t\r\n]*HttpRequestExpectations[ \t]+expectations" OR
       NOT http1_request_body_plan MATCHES
           "Http1RequestBodyPlan[ \t\r\n]*[(][ \t\r\n]*std::size_t[ \t]+contentLength" OR
       NOT http1_request_body_plan MATCHES
           "Http1RequestBodyPlan[ \t\r\n]*[(][ \t\r\n]*HttpTransferCodings[ \t]+transferCodings")
        boundary_error("HTTP/1 request-body plan lost part of its typed contract"
            "parser-only exclusive framing alternatives, transfer decode order, consumption, and 100-continue must stay bound")
    endif()
endif()

set(HTTP1_SERVER_PARSER "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp")
set(WEB_HTTP1_BODY_READER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h")
set(WEB_HTTP1_BODY_READER_CORE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl")
if(EXISTS "${HTTP1_SERVER_PARSER}")
    file(READ "${HTTP1_SERVER_PARSER}" http1_server_parser)
    if(NOT http1_server_parser MATCHES
           "Http1RequestBodyPlan[ \t\r\n]*[(][ \t\r\n]*block[.]transferEncoding[.]codings[(][)]" OR
       NOT http1_server_parser MATCHES
           "Http1RequestBodyPlan[ \t\r\n]*[(][ \t\r\n]*block[.]contentLength[.]value[(][)]" OR
       NOT http1_server_parser MATCHES
           "Http1RequestBodyPlan[(]expectations[)]" OR
       NOT http1_server_parser MATCHES "bodyPlan[.]chunked[(][)]" OR
       NOT http1_server_parser MATCHES "bodyPlan[.]knownLength[(][)]" OR
       NOT http1_server_parser MATCHES "expectations[.]ignore100Continue")
        boundary_error("HTTP/1 parser bypasses the typed request-body plan"
            "Http1RequestParser.cpp must produce Http1RequestBodyPlan once after header validation")
    endif()
endif()
if(EXISTS "${WEB_HTTP1_BODY_READER}" AND EXISTS "${WEB_HTTP1_BODY_READER_CORE}")
    file(READ "${WEB_HTTP1_BODY_READER}" web_http1_body_reader)
    file(READ "${WEB_HTTP1_BODY_READER_CORE}" web_http1_body_reader_core)
    if(NOT web_http1_body_reader MATCHES "Http1RequestBodyPlan[ \t]+bodyPlan" OR
       NOT web_http1_body_reader MATCHES "readKnownLengthAll" OR
       NOT web_http1_body_reader MATCHES "readKnownLength" OR
       NOT web_http1_body_reader_core MATCHES "bodyPlan_[.]withoutBody[(][)]" OR
       NOT web_http1_body_reader_core MATCHES "bodyPlan_[.]knownLength[(][)]" OR
       NOT web_http1_body_reader_core MATCHES "bodyPlan_[.]chunked[(][)]" OR
       NOT web_http1_body_reader_core MATCHES "chunked->transferCodings[(][)]" OR
       NOT web_http1_body_reader_core MATCHES "bodyPlan_\\.expectationAction" OR
       NOT web_http1_body_reader_core MATCHES "kSend100Continue")
        boundary_error("ruvia-web request-body reader bypasses the HTTP-owned plan"
            "StreamBodyReader must consume Http1RequestBodyPlan directly")
    endif()
endif()

set(HTTP_REQUEST_EXPECTATIONS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpExpectations.h")
set(HTTP1_HEADER_BLOCK_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpHeaderBlockParser.h")
set(HTTP1_HEADER_BLOCK_PARSER
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp")
set(HTTP2_STREAM_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h")
set(HTTP2_REQUEST_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h")
set(WEB_HTTP1_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
if(NOT EXISTS "${HTTP_REQUEST_EXPECTATIONS}")
    boundary_error("shared request-expectation state is missing"
        "HTTP/1 and HTTP/2 recipients must share HttpRequestExpectations")
elseif(EXISTS "${HTTP1_HEADER_BLOCK_STATE}" AND
       EXISTS "${HTTP1_HEADER_BLOCK_PARSER}" AND
       EXISTS "${HTTP2_STREAM_STATE}" AND
       EXISTS "${HTTP2_REQUEST_HEADERS}" AND
       EXISTS "${WEB_HTTP1_SESSION}" AND
       EXISTS "${WEB_HTTP2_SESSION}")
    file(READ "${HTTP_REQUEST_EXPECTATIONS}" http_request_expectations)
    file(READ "${HTTP1_HEADER_BLOCK_STATE}" http1_header_block_state)
    file(READ "${HTTP1_HEADER_BLOCK_PARSER}" http1_header_block_parser)
    file(READ "${HTTP2_STREAM_STATE}" http2_stream_state)
    file(READ "${HTTP2_REQUEST_HEADERS}" http2_request_headers)
    file(READ "${WEB_HTTP1_SESSION}" web_http1_session)
    file(READ "${WEB_HTTP2_SESSION}" web_http2_session)
    if(NOT http_request_expectations MATCHES "class HttpRequestExpectations" OR
       NOT http_request_expectations MATCHES "httpVisitCommaSeparatedQuoted" OR
       NOT http_request_expectations MATCHES "HttpRequestContentIndication" OR
       NOT http_request_expectations MATCHES "HttpServerExpectationAction" OR
       NOT http_request_expectations MATCHES "kUnsupported" OR
       NOT http1_header_block_state MATCHES "HttpRequestExpectations expectations" OR
       NOT http1_header_block_parser MATCHES "expectations[.]parseField" OR
       NOT http2_stream_state MATCHES "HttpRequestExpectations expectations_" OR
       NOT http2_request_headers MATCHES "parseRequestExpectationField" OR
       NOT web_http1_session MATCHES "HttpErrorInfo[(]417" OR
       NOT web_http1_session MATCHES "HttpServerExpectationAction::kUnsupported" OR
       NOT web_http2_session MATCHES "submitInterimResponseHead" OR
       NOT web_http2_session MATCHES "HttpInterimResponseHead[(]100" OR
       NOT web_http2_session MATCHES "HttpErrorInfo[(]417" OR
       NOT web_http2_session MATCHES "HttpServerExpectationAction::kUnsupported")
        boundary_error("server Expect ownership has split across protocol versions"
            "shared list state must feed HTTP/1/H2 actions; Web alone chooses 417 and drives typed 100 writers")
    endif()
endif()

set(HTTP_EXPECTATION_LIST_TEST "${RUVIA_ROOT}/tests/unit_header_params.cpp")
set(HTTP1_EXPECTATION_TEST "${RUVIA_ROOT}/tests/unit_http1_parser.cpp")
set(HTTP2_EXPECTATION_HEADER_TEST "${RUVIA_ROOT}/tests/unit_http2_request_headers.cpp")
set(HTTP2_EXPECTATION_RUNTIME_TEST "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
if(EXISTS "${HTTP_EXPECTATION_LIST_TEST}" AND
   EXISTS "${HTTP1_EXPECTATION_TEST}" AND
   EXISTS "${HTTP2_EXPECTATION_HEADER_TEST}" AND
   EXISTS "${HTTP2_EXPECTATION_RUNTIME_TEST}")
    file(READ "${HTTP_EXPECTATION_LIST_TEST}" http_expectation_list_test)
    file(READ "${HTTP1_EXPECTATION_TEST}" http1_expectation_test)
    file(READ "${HTTP2_EXPECTATION_HEADER_TEST}" http2_expectation_header_test)
    file(READ "${HTTP2_EXPECTATION_RUNTIME_TEST}" http2_expectation_runtime_test)
    if(NOT http_expectation_list_test MATCHES
           "expectations_parse_one_logical_recipient_list" OR
       NOT http_expectation_list_test MATCHES
           "expectations_preserve_unsupported_extensions_as_semantics" OR
       NOT http1_expectation_test MATCHES
           "http1_public_parser_preserves_expect_extensions_as_semantics" OR
       NOT http2_expectation_header_test MATCHES
           "h2_headers_expect_is_an_extensible_repeated_list" OR
       NOT http2_expectation_runtime_test MATCHES
           "sansio_driver_h2_expectation_decision_precedes_request_content")
        boundary_error("cross-version Expect contract is under-tested"
            "tests must pin list/repeat/empty parsing, semantic extensions, H2 100-before-DATA, and immediate Web 417")
    endif()
endif()

set(HTTP1_REQUEST_BODY_TEST
    "${RUVIA_ROOT}/tests/unit_request_body_decoding.cpp")
if(EXISTS "${HTTP1_REQUEST_BODY_TEST}")
    file(READ "${HTTP1_REQUEST_BODY_TEST}" http1_request_body_test)
    if(NOT http1_request_body_test MATCHES
           "http1_request_body_plan_has_one_framing_truth" OR
       NOT http1_request_body_test MATCHES
           "transfer_coded_chunked_request_plan_drives_decode_order" OR
       NOT http1_request_body_test MATCHES
           "!HasPublicRequestBodyPlanFactories<Http1RequestBodyPlan>" OR
       NOT http1_request_body_test MATCHES
           "!HasRequestBodyMode<Http1RequestBodyPlan>" OR
       NOT http1_request_body_test MATCHES
           "HasRequestContentLength<ruvia::detail::Http1KnownLengthRequestBody>" OR
       NOT http1_request_body_test MATCHES
           "HasRequestTransferCodings<ruvia::detail::Http1ChunkedRequestBody>" OR
       NOT http1_request_body_test MATCHES
           "!std::default_initializable<Http1RequestBodyPlan>")
        boundary_error("HTTP/1 request-body alternative ownership is under-tested"
            "tests must prove parser-only construction, exclusive alternative payloads, explicit Content-Length: 0, and dechunk-before-transfer-decoding order")
    endif()
endif()

set(HTTP1_CLIENT_REQUEST_WRITER_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientRequestWriter.h")
set(HTTP1_CLIENT_REQUEST_WRITER
    "${RUVIA_ROOT}/ruvia-http/src/client/Http1ClientRequestWriter.cpp")
set(HTTP1_CLIENT_REQUEST_TEST
    "${RUVIA_ROOT}/tests/unit_http_client_request.cpp")
if(NOT EXISTS "${HTTP1_CLIENT_REQUEST_WRITER_HEADER}" OR
   NOT EXISTS "${HTTP1_CLIENT_REQUEST_WRITER}")
    boundary_error("public HTTP/1 client request writer is missing"
        "ruvia-http must own the complete outbound HTTP/1 wire plan")
else()
    file(READ "${HTTP1_CLIENT_REQUEST_WRITER_HEADER}"
        http1_client_request_writer_header)
    file(READ "${HTTP1_CLIENT_REQUEST_WRITER}"
        http1_client_request_writer)
    if(NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestWirePolicy final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestContentPlan final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestWithoutContent final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientImmediateRequestContent final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientContinueGatedRequestContent final" OR
       NOT http1_client_request_writer_header MATCHES
           "std::get_if<Http1ClientRequestWithoutContent>" OR
       NOT http1_client_request_writer_header MATCHES
           "std::get_if<Http1ClientImmediateRequestContent>" OR
       NOT http1_client_request_writer_header MATCHES
           "std::get_if<Http1ClientContinueGatedRequestContent>" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestBufferTooSmall final" OR
       NOT http1_client_request_writer_header MATCHES
           "class PreparedHttp1ClientRequest final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestPrepareFailure final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestPrepareResult final" OR
       NOT http1_client_request_writer_header MATCHES "std::variant" OR
       NOT http1_client_request_writer_header MATCHES
           "std::span<char> headBuffer" OR
       NOT http1_client_request_writer_header MATCHES "prepareConnect" OR
       NOT http1_client_request_writer_header MATCHES
           "friend class Http1ClientResponseParser" OR
       http1_client_request_writer_header MATCHES "responseContext[ \t]*[(][ \t]*[)]" OR
       http1_client_request_writer_header MATCHES "expectsContinue")
        boundary_error("HTTP/1 client request writer lost its transactional contract"
            "buffer sizing, prepared content gate, typed failure, CONNECT, and Prepared-bound response state must remain one result")
    endif()
    if(NOT http1_client_request_writer MATCHES "isValidHttpMethodToken" OR
       NOT http1_client_request_writer MATCHES "isValidOriginFormTarget" OR
       NOT http1_client_request_writer MATCHES "kHostPrefix" OR
       NOT http1_client_request_writer MATCHES "kContentLengthPrefix" OR
       NOT http1_client_request_writer MATCHES "kConnectionClose" OR
       NOT http1_client_request_writer MATCHES "kExpectContinue" OR
       NOT http1_client_request_writer MATCHES "kMaxHttpHeaderBytes" OR
       NOT http1_client_request_writer MATCHES "kMaxHttpHeaderFields" OR
       NOT http1_client_request_writer MATCHES "kExpectHeaderManagedByWriter" OR
       NOT http1_client_request_writer MATCHES "kExpectationWithoutContent" OR
       NOT http1_client_request_writer MATCHES "content[.]borrowedBytes" OR
       NOT http1_client_request_writer MATCHES "preparedWithoutContent" OR
       NOT http1_client_request_writer MATCHES "preparedImmediateContent" OR
       NOT http1_client_request_writer MATCHES
           "preparedContinueGatedContent" OR
       NOT http1_client_request_writer MATCHES "method == \"TRACE\"" OR
       NOT http1_client_request_writer MATCHES "method == \"OPTIONS\"" OR
       NOT http1_client_request_writer MATCHES
           "headBuffer[.]size[(][)] < headBytes" OR
       http1_client_request_writer MATCHES "httpHasToken" OR
       http1_client_request_writer MATCHES "throw[ \t]" OR
       http1_client_request_writer MATCHES "std::pmr::string")
        boundary_error("HTTP/1 client request serialization split or became allocating"
            "one allocation-free writer must validate target/fields/method content, generate Host/framing/close, and size before writing")
    endif()
endif()
if(EXISTS "${HTTP1_CLIENT_REQUEST_TEST}")
    file(READ "${HTTP1_CLIENT_REQUEST_TEST}" http1_client_request_test)
    if(NOT http1_client_request_test MATCHES
           "http1_client_request_writer_emits_one_canonical_scatter_gather_plan" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_content_distinguishes_absent_from_explicit_empty" OR
       NOT http1_client_request_test MATCHES
           "http1_client_connect_entry_generates_authority_form_atomically" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_writer_is_the_only_host_and_framing_owner" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_writer_enforces_expect_content_semantics" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_writer_enforces_method_content_semantics" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_writer_returns_exact_buffer_requirement_without_partial_output" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_context_binds_the_actual_close_signal")
        boundary_error("HTTP/1 client request writer invariants are under-tested"
            "tests must pin content presence/gating, target forms, Host/framing/Expect ownership, buffer atomicity, method semantics, and Prepared-bound response state")
    endif()
    if(NOT http1_client_request_test MATCHES
           "!HasRequestContentMode<ruvia::HttpClientRequestContent>" OR
       NOT http1_client_request_test MATCHES
           "HasRequestContentValue<ruvia::HttpClientRequestBytes>" OR
       NOT http1_client_request_test MATCHES
           "!HasPreparedContentDisposition<[\r\n \t]*ruvia::Http1ClientRequestContentPlan>" OR
       NOT http1_client_request_test MATCHES
           "HasPreparedContentBytes<[\r\n \t]*ruvia::Http1ClientImmediateRequestContent>" OR
       NOT http1_client_request_test MATCHES
           "HasPreparedContentBytes<[\r\n \t]*ruvia::Http1ClientContinueGatedRequestContent>" OR
       NOT http1_client_request_test MATCHES
           "withoutContent[(][)][ 	]*==[ 	]*nullptr" OR
       NOT http1_client_request_test MATCHES
           "continueGated[(][)][ 	]*==[ 	]*nullptr")
        boundary_error("HTTP/1 outbound request-content alternatives are under-tested"
            "tests must reject plan-wide mode/payload access and prove absent, immediate-empty, and continue-gated exclusivity")
    endif()
endif()
set(HTTP1_CLIENT_API_SURFACE "${RUVIA_ROOT}/examples/api_surface.cpp")
set(HTTP1_CLIENT_PACKAGE_CONSUMER "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
if(EXISTS "${HTTP1_CLIENT_API_SURFACE}" AND
   EXISTS "${HTTP1_CLIENT_PACKAGE_CONSUMER}")
    file(READ "${HTTP1_CLIENT_API_SURFACE}" http1_client_api_surface)
    file(READ "${HTTP1_CLIENT_PACKAGE_CONSUMER}" http1_client_package_consumer)
    if(NOT http1_client_api_surface MATCHES
           "HasHttp1RequestBodyPlanAlternatives<[\r\n \t]*ruvia::detail::Http1RequestBodyPlan>" OR
       NOT http1_client_api_surface MATCHES
           "!HasPublicHttp1RequestBodyPlanFactories<[\r\n \t]*ruvia::detail::Http1RequestBodyPlan>" OR
       NOT http1_client_api_surface MATCHES
           "HasHttp1RequestBodyContentLength<[\r\n \t]*ruvia::detail::Http1KnownLengthRequestBody>" OR
       NOT http1_client_package_consumer MATCHES
           "HasHttp1RequestBodyPlanAlternatives" OR
       NOT http1_client_package_consumer MATCHES
           "!HasPublicHttp1RequestBodyPlanFactories" OR
       NOT http1_client_api_surface MATCHES
           "!HasRawHttpClientRequestBody<ruvia::HttpClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "HasDiscriminatedHttpClientRequestContent<ruvia::HttpClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "!HasStaleHttpClientRequestContentTuple<ruvia::HttpClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "HasHttp1ClientPreparedContentPlan<[\r\n \t]*ruvia::PreparedHttp1ClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "!HasStaleHttp1ClientPreparedContentTuple<[\r\n \t]*ruvia::PreparedHttp1ClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "!HasStaleHttp1ClientResponseContext<[\r\n \t]*ruvia::PreparedHttp1ClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "std::is_constructible_v<[\r\n \t]*ruvia::Http1ClientResponseParser,[\r\n \t]*const ruvia::PreparedHttp1ClientRequest&>" OR
       NOT http1_client_api_surface MATCHES
           "!std::is_default_constructible_v<[\r\n \t]*ruvia::Http1ClientResponseParser>" OR
       NOT http1_client_api_surface MATCHES
           "!std::is_move_constructible_v<[\r\n \t]*ruvia::Http1ClientResponseParser>" OR
       NOT http1_client_api_surface MATCHES
           "HasHttp1ClientResponsePlanAlternatives<[\r\n \t]*ruvia::Http1ClientResponsePlan>" OR
       NOT http1_client_api_surface MATCHES
           "!HasStaleHttp1ClientResponseMode<[\r\n \t]*ruvia::Http1ClientResponsePlan>" OR
       NOT http1_client_api_surface MATCHES
           "!HasHttp1ClientResponsePersistence<[\r\n \t]*ruvia::Http1ClientCloseDelimitedResponse>" OR
       NOT http1_client_package_consumer MATCHES "Http1ClientRequestWriter" OR
       NOT http1_client_package_consumer MATCHES
           "HasHttpClientRequestContentAlternatives" OR
       NOT http1_client_package_consumer MATCHES
           "HasHttp1PreparedContentAlternatives" OR
       NOT http1_client_package_consumer MATCHES "Http1ClientRequestWirePolicy::expectContinue" OR
       NOT http1_client_package_consumer MATCHES "Http1ClientRequestContentSignal::kContinue" OR
       NOT http1_client_package_consumer MATCHES "completeRequestContent" OR
       NOT http1_client_package_consumer MATCHES "HasHttp1ClientResponsePlanAlternatives" OR
       NOT http1_client_package_consumer MATCHES "Http1ClientProtocolUpgrade" OR
       http1_client_package_consumer MATCHES "responseContext[(][)]")
        boundary_error("installed HTTP/1 API can bypass protocol preparation"
            "API surface must remove raw server/client body, context, and framing tuples; package consumers must use parser-only request alternatives and chain Prepared/content signals through exclusive client response alternatives")
    endif()
endif()

set(HTTP1_CLIENT_RESPONSE_PARSER_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h")
set(HTTP1_CLIENT_RESPONSE_PARSER
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp")
set(HTTP_CONTENT_LENGTH_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpContentLength.h")
set(HTTP_TRANSFER_ENCODING_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpTransferEncoding.h")
foreach(obsolete_http1_client_response_header IN ITEMS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientResponseParser.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ClientResponsePlan.h")
    if(EXISTS "${obsolete_http1_client_response_header}")
        boundary_error("obsolete private HTTP/1 client response parser API was restored"
            "${obsolete_http1_client_response_header}")
    endif()
endforeach()
if(NOT EXISTS "${HTTP1_CLIENT_RESPONSE_PARSER_HEADER}")
    boundary_error("public HTTP/1 client response parser is missing"
        "ruvia-http must install Http1ClientResponseParser.h")
elseif(EXISTS "${HTTP1_CLIENT_RESPONSE_PARSER}")
    file(READ "${HTTP1_CLIENT_RESPONSE_PARSER_HEADER}" http1_client_response_parser_header)
    file(READ "${HTTP1_CLIENT_RESPONSE_PARSER}" http1_client_response_parser)
    if(NOT http1_client_response_parser_header MATCHES "Http1ClientRequestWriter[.]h" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientResponsePlan final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientInformationalResponse final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientResponseWithoutContent final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientKnownLengthResponse final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientChunkedResponse final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientCloseDelimitedResponse final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientConnectTunnel final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientProtocolUpgrade final" OR
       NOT http1_client_response_parser_header MATCHES "Http1ClientResponsePersistence" OR
       NOT http1_client_response_parser_header MATCHES "knownLength[(][)]" OR
       NOT http1_client_response_parser_header MATCHES "closeDelimited[(][)]" OR
       NOT http1_client_response_parser_header MATCHES "connectTunnel[(][)]" OR
       NOT http1_client_response_parser_header MATCHES "protocolUpgrade[(][)]" OR
       NOT http1_client_response_parser_header MATCHES "Http1ClientRequestContentSignal" OR
       NOT http1_client_response_parser_header MATCHES "requestContentSignal" OR
       NOT http1_client_response_parser_header MATCHES "Http1ClientRequestContentCompletionStatus" OR
       NOT http1_client_response_parser_header MATCHES "completeRequestContent" OR
       NOT http1_client_response_parser_header MATCHES "const PreparedHttp1ClientRequest& request" OR
       NOT http1_client_response_parser_header MATCHES
           "request[.]contentPlan_[.]continueGated" OR
       NOT http1_client_response_parser_header MATCHES "continueGated_" OR
       NOT http1_client_response_parser_header MATCHES
           "requestContentStartsComplete" OR
       NOT http1_client_response_parser_header MATCHES "enum class Phase" OR
       NOT http1_client_response_parser_header MATCHES "kExchangeComplete" OR
       NOT http1_client_response_parser_header MATCHES "kExchangeFailed" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientResponseNeedMore final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ParsedClientResponseHead final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientResponseParseFailure final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientResponseParseResult final" OR
       NOT http1_client_response_parser_header MATCHES "Http1ClientResponseParseError" OR
       NOT http1_client_response_parser_header MATCHES "std::variant" OR
       NOT http1_client_response_parser_header MATCHES "consumedBytes")
        boundary_error("public HTTP/1 client response parser lost its discriminated contract"
            "NeedMore, owning Parsed, typed Failure, exact head consumption, and one immutable plan must stay bound")
    endif()
    if(NOT http1_client_response_parser MATCHES "findHttpHeaderEnd" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::withoutContent" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::knownLength" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::chunked" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::closeDelimited" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::connectTunnel" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::protocolUpgrade" OR
       NOT http1_client_response_parser MATCHES "using ResponsePlanningResult = std::variant" OR
       NOT http1_client_response_parser MATCHES "std::get_if<Http1ClientResponseParseError>" OR
       NOT http1_client_response_parser MATCHES "requestAllowsProtocolSwitch" OR
       NOT http1_client_response_parser MATCHES
           "continueGated && !sawContinue" OR
       NOT http1_client_response_parser MATCHES "!requestContentComplete" OR
       NOT http1_client_response_parser MATCHES "requestContentComplete_ = true" OR
       NOT http1_client_response_parser MATCHES "sawContinue_ = true" OR
       NOT http1_client_response_parser MATCHES "phase_ = Phase::kComplete" OR
       NOT http1_client_response_parser MATCHES "Http1ClientRequestContentSignal::kContinue" OR
       NOT http1_client_response_parser MATCHES "Http1ClientRequestContentSignal::kExchangeComplete" OR
       NOT http1_client_response_parser MATCHES
           "request[.]closePolicy[(][)] ==[\r\n \t]*Http1ClientRequestClosePolicy::kCloseAfterResponse" OR
       NOT http1_client_response_parser MATCHES "contentLengthFieldPresent" OR
       NOT http1_client_response_parser MATCHES
           "detail::httpResponseContentSemantics" OR
       http1_client_response_parser MATCHES "request[.]expectsContinue" OR
       http1_client_response_parser MATCHES "throw[ \t]+std::runtime_error")
        boundary_error("HTTP/1 client response parser bypasses its typed plan"
            "head scanning, Prepared-bound informational/final state, content signals, RFC body precedence, persistence, CONNECT, and Upgrade must have one output without wire exceptions")
    endif()
    if(EXISTS "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientRedirect.h")
        boundary_error("private redirect protocol header returned"
            "external sans-I/O redirect users must include the public HttpClientRedirect.h API")
    endif()
    file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClientRedirect.h"
        http_client_redirect_header)
    file(READ "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp"
        http_client_redirect)
    if(NOT http_client_redirect_header MATCHES "HttpClientRedirectRequestPlan" OR
       NOT http_client_redirect MATCHES "request\\.method[ \t]*==[ \t]*\"POST\"" OR
       NOT http_client_redirect MATCHES "request\\.method[ \t]*==[ \t]*\"HEAD\"" OR
       http_client_redirect MATCHES "httpAsciiEqualsIgnoreCase\\([^,\r\n]*request\\.method")
        boundary_error("HTTP client redirect request plan lost RFC method semantics"
            "303 must select GET/HEAD, only POST may become GET for 301/302, and tokens remain case-sensitive")
    endif()
    if(NOT http_client_redirect_header MATCHES "enum class HttpClientOriginAuthorityStatus" OR
       NOT http_client_redirect_header MATCHES "kSameOrigin" OR
       NOT http_client_redirect_header MATCHES "kDifferentOrigin" OR
       NOT http_client_redirect_header MATCHES "kInvalidAuthority" OR
       NOT http_client_redirect MATCHES "case HttpClientOriginAuthorityStatus::kInvalidAuthority" OR
       NOT http_client_redirect MATCHES "HttpClientRedirectTargetError::kInvalidLocation")
        boundary_error("HTTP client redirect collapsed authority syntax into origin equality"
            "malformed/userinfo authorities must be invalid Location values; only valid unequal origins are cross-origin")
    endif()
    if(NOT http_client_redirect_header MATCHES "class HttpClientResponseHeaderAbsent final" OR
       NOT http_client_redirect_header MATCHES "class HttpClientResponseHeaderFound final" OR
       NOT http_client_redirect_header MATCHES "class HttpClientResponseHeaderRepeated final" OR
       NOT http_client_redirect_header MATCHES "class HttpClientResponseHeaderLookupResult final" OR
       NOT http_client_redirect_header MATCHES "std::get_if<HttpClientResponseHeaderFound>" OR
       NOT http_client_redirect_header MATCHES "enum class HttpClientRedirectTargetError" OR
       NOT http_client_redirect_header MATCHES "class HttpClientRedirectTarget final" OR
       NOT http_client_redirect_header MATCHES "class HttpClientRedirectTargetFailure final" OR
       NOT http_client_redirect_header MATCHES "class HttpClientRedirectTargetResult final" OR
       NOT http_client_redirect_header MATCHES "std::get_if<HttpClientRedirectTarget>" OR
       NOT http_client_redirect_header MATCHES "std::get_if<HttpClientRedirectTargetFailure>" OR
       NOT http_client_redirect MATCHES "HttpClientRedirectTargetResult::makeTarget" OR
       NOT http_client_redirect MATCHES "HttpClientRedirectTargetResult::makeFailure")
        boundary_error("HTTP client redirect results lost their discriminated ownership"
            "only found may expose a borrowed field value, only target may own PMR bytes, and only failure may expose an error")
    endif()
    file(READ "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt" http_client_redirect_cmake)
    file(READ "${RUVIA_ROOT}/tests/unit_http_client_redirect.cpp" http_client_redirect_tests)
    file(READ "${RUVIA_ROOT}/tests/package-consumer/http.cpp" http_package_consumer)
    file(READ "${RUVIA_ROOT}/examples/api_surface.cpp" api_surface)
    if(NOT http_client_redirect_cmake MATCHES "src/client/HttpClientRedirect[.]cpp" OR
       NOT http_client_redirect_cmake MATCHES "include/ruvia/http/HttpClientRedirect[.]h" OR
       NOT http_client_redirect_tests MATCHES "HasHeaderValue" OR
       NOT http_client_redirect_tests MATCHES "HasRedirectTargetError" OR
       NOT http_package_consumer MATCHES "ruvia/http/HttpClientRedirect[.]h" OR
       NOT http_package_consumer MATCHES "HttpClientRedirectTargetResult" OR
       NOT api_surface MATCHES "ruvia/http/HttpClientRedirect[.]h" OR
       NOT api_surface MATCHES "HasHttpClientRedirectTargetAccessors")
        boundary_error("public redirect API is not pinned across consumers"
            "CMake install, unit ownership checks, package consumption, and API surface must compile the same result contract")
    endif()
endif()
if(NOT EXISTS "${HTTP_CONTENT_LENGTH_STATE}")
    boundary_error("shared Content-Length state is missing"
        "HTTP/1 request and response parsers must share HttpContentLengthState")
else()
    file(READ "${HTTP_CONTENT_LENGTH_STATE}" http_content_length_state)
    if(NOT http_content_length_state MATCHES "class HttpContentLengthState" OR
       NOT http_content_length_state MATCHES "HttpContentLengthParseStatus::kConflicting" OR
       NOT http_content_length_state MATCHES "httpVisitCommaSeparatedQuotedItems")
        boundary_error("shared Content-Length parser lost full-list validation"
            "every combined/repeated decimal member must be parsed and compared")
    endif()
endif()
if(NOT EXISTS "${HTTP_TRANSFER_ENCODING_STATE}")
    boundary_error("shared Transfer-Encoding state is missing"
        "HTTP/1 request and response parsers must share HttpTransferEncodingState")
else()
    file(READ "${HTTP_TRANSFER_ENCODING_STATE}" http_transfer_encoding_state)
    if(NOT http_transfer_encoding_state MATCHES "class HttpTransferEncodingState" OR
       NOT http_transfer_encoding_state MATCHES "finalChunked" OR
       NOT http_transfer_encoding_state MATCHES "item\\.find\\(';'")
        boundary_error("shared Transfer-Encoding parser lost ordered-list validation"
            "supported codings, final chunked, and parameter rejection must remain one state machine")
    endif()
endif()
if(EXISTS "${HTTP1_SERVER_PARSER}" AND EXISTS "${HTTP1_CLIENT_RESPONSE_PARSER}")
    file(READ "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp"
        http1_request_header_parser)
    if(NOT http1_request_header_parser MATCHES "contentLength\\.parseField" OR
       NOT http1_client_response_parser MATCHES "contentLength\\.parseField")
        boundary_error("HTTP/1 Content-Length parsing has split again"
            "request and client response heads must both drive HttpContentLengthState")
    endif()
    if(NOT http1_request_header_parser MATCHES "transferEncoding\\.parseField" OR
       NOT http1_client_response_parser MATCHES "transferEncoding\\.parseField")
        boundary_error("HTTP/1 Transfer-Encoding parsing has split again"
            "request and client response heads must both drive HttpTransferEncodingState")
    endif()
endif()
set(HTTP1_CLIENT_RESPONSE_TEST
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp")
if(EXISTS "${HTTP1_CLIENT_RESPONSE_TEST}")
    file(READ "${HTTP1_CLIENT_RESPONSE_TEST}" http1_client_response_test)
    if(NOT http1_client_response_test MATCHES
           "http_client_response_plan_alternatives_are_exclusive" OR
       NOT http1_client_response_test MATCHES
           "http_client_unframed_body_response_is_close_delimited" OR
       NOT http1_client_response_test MATCHES
           "http_client_successful_connect_transitions_to_tunnel" OR
       NOT http1_client_response_test MATCHES
           "http_client_transfer_coding_before_final_chunked_is_typed" OR
       NOT http1_client_response_test MATCHES
           "http_client_content_length_combined_and_repeated_equal_values" OR
       NOT http1_client_response_test MATCHES
           "http_client_no_body_precedence_ignores_framing_fields" OR
       NOT http1_client_response_test MATCHES
           "http_client_205_uses_normal_http1_message_framing" OR
       NOT http1_client_response_test MATCHES
           "http_client_switching_protocols_is_an_exclusive_upgrade_transition" OR
       NOT http1_client_response_test MATCHES
           "http_client_switching_protocols_requires_wire_agreement" OR
       NOT http1_client_response_test MATCHES
           "http_client_expect_continue_is_one_stateful_exchange_contract" OR
       NOT http1_client_response_test MATCHES
           "http_client_upgrade_after_expect_requires_prior_continue" OR
       NOT http1_client_response_test MATCHES
           "http_client_upgrade_requires_complete_request_content" OR
       NOT http1_client_response_test MATCHES
           "http_client_response_parser_need_more_is_distinct" OR
       NOT http1_client_response_test MATCHES
           "http_client_response_parser_owns_exact_head_boundary" OR
       NOT http1_client_response_test MATCHES
           "http_client_response_parser_failure_is_typed_and_allocation_free" OR
       NOT http1_client_response_test MATCHES
           "!HasResponsePlanMode<ruvia::Http1ClientResponsePlan>" OR
       NOT http1_client_response_test MATCHES
           "!HasResponseConnectionDisposition" OR
       NOT http1_client_response_test MATCHES
           "HasResponseContentLength<ruvia::Http1ClientKnownLengthResponse>" OR
       NOT http1_client_response_test MATCHES
           "HasResponseTransferCodings<ruvia::Http1ClientChunkedResponse>" OR
       NOT http1_client_response_test MATCHES
           "!HasResponsePersistence<[\r\n \t]*ruvia::Http1ClientCloseDelimitedResponse>" OR
       NOT http1_client_response_test MATCHES
           "!std::is_default_constructible_v<ruvia::Http1ClientResponsePlan>")
        boundary_error("HTTP/1 client response plan invariants are under-tested"
            "tests must pin exclusive framing/lifecycle payload ownership, tri-state/stateful parsing, Expect progress, transactional ownership, close delimiting, CONNECT, Upgrade agreement/order, transfer order, and full Content-Length lists")
    endif()
endif()

if(EXISTS "${WEB_HTTP1_STREAM_ROUTE}")
    file(READ "${WEB_HTTP1_STREAM_ROUTE}" web_http1_stream_route)
    if(NOT web_http1_stream_route MATCHES "http1PlanResponseStream")
        boundary_error("ruvia-web HTTP/1 stream route bypasses the protocol plan"
            "HttpServerResponseStreamRoute.h must call http1PlanResponseStream")
    endif()
    if(NOT web_http1_stream_route MATCHES "nextHttp1ResponseClosePolicy")
        boundary_error("ruvia-web HTTP/1 stream limit is recomputed after commit"
            "the request limit must enter the pre-commit close policy before response bytes are emitted")
    endif()
    if(web_http1_stream_route MATCHES
       "streamPlan\.(baseDisposition|connectionWillClose)\(\)")
        boundary_error("ruvia-web treats a pre-commit stream constraint as the final disposition"
            "HttpServerResponseStreamRoute.h must consume the committed sink disposition")
    endif()
    if(NOT web_http1_stream_route MATCHES
       "responseSink\.connectionPlan\(\)")
        boundary_error("ruvia-web ignores the committed HTTP/1 stream connection plan"
            "HttpServerResponseStreamRoute.h must drive responseSink.connectionPlan()")
    endif()
endif()

set(RESPONSE_TRAILER_H2_TEST "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
set(RESPONSE_TRAILER_H1_TEST "${RUVIA_ROOT}/tests/unit_http_server_request_state.cpp")
if(EXISTS "${RESPONSE_TRAILER_H2_TEST}" AND EXISTS "${RESPONSE_TRAILER_H1_TEST}")
    file(READ "${RESPONSE_TRAILER_H2_TEST}" response_trailer_h2_test)
    file(READ "${RESPONSE_TRAILER_H1_TEST}" response_trailer_h1_test)
    if(NOT response_trailer_h2_test MATCHES
           "http2_connection_head_response_can_end_with_trailers_only" OR
       NOT response_trailer_h2_test MATCHES
           "http2_response_trailer_section_is_phase_typed_and_atomic" OR
       NOT response_trailer_h1_test MATCHES
           "http1_stream_commit_plan_exposes_exact_trailer_capability")
        boundary_error("response trailer terminal contract is under-tested"
            "tests must pin H1 framing capability, H2 trailers-only, phase refusal, and atomic validation")
    endif()
endif()

foreach(boundary_doc IN ITEMS
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")
    file(READ "${boundary_doc}" boundary_doc_content)
    if(NOT boundary_doc_content MATCHES "Http1ResponseStreamPlan")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/1 stream-plan boundary is undocumented"
            "${relative} must describe Http1ResponseStreamPlan ownership")
    endif()
    if(NOT boundary_doc_content MATCHES "PreparedHttp1ResponseStream")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/1 commit-time disposition boundary is undocumented"
            "${relative} must describe PreparedHttp1ResponseStream ownership")
    endif()
    if(NOT boundary_doc_content MATCHES "Http1ResponseHeadPlan" OR
       NOT boundary_doc_content MATCHES "Http1BufferedResponseHead" OR
       NOT boundary_doc_content MATCHES
           "Http1ChunkedResponseStreamHead" OR
       NOT boundary_doc_content MATCHES
           "Http1CloseDelimitedResponseStreamHead" OR
       NOT boundary_doc_content MATCHES "Http1BufferedResponsePlan" OR
       NOT boundary_doc_content MATCHES "contentLength" OR
       NOT boundary_doc_content MATCHES "section-2[.]5" OR
       NOT boundary_doc_content MATCHES "suppressAutoContentLength" OR
       NOT boundary_doc_content MATCHES "section-6[.]1" OR
       NOT boundary_doc_content MATCHES "section-6[.]3")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/1 response-head plan is undocumented"
            "${relative} must document exact status-line version ownership, buffered write/head composition and canonical length, exclusive framing alternatives, removal of the scalar API, and RFC 9110/9112 behavior")
    endif()
    if(NOT boundary_doc_content MATCHES "Http1ServerConnectionPlan" OR
       NOT boundary_doc_content MATCHES "HttpProtocolVersion" OR
       NOT boundary_doc_content MATCHES "http1PlanHttp10RequestConnection" OR
       NOT boundary_doc_content MATCHES "http1PlanHttp11RequestConnection" OR
       NOT boundary_doc_content MATCHES "http11Close" OR
       NOT boundary_doc_content MATCHES "responseSignal")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/1 connection-plan boundary is undocumented"
            "${relative} must document exact version/disposition ownership, version-specific factories, the unparsed HTTP/1.1 close fallback, and removal of responseSignal")
    endif()
    if(NOT boundary_doc_content MATCHES "Http1RequestBodyPlan" OR
       NOT boundary_doc_content MATCHES "Http1RequestWithoutBody" OR
       NOT boundary_doc_content MATCHES "Http1KnownLengthRequestBody" OR
       NOT boundary_doc_content MATCHES "Http1ChunkedRequestBody" OR
       NOT boundary_doc_content MATCHES "Content-Length: 0" OR
       NOT boundary_doc_content MATCHES "Http1ServerRequestParser" OR
       NOT boundary_doc_content MATCHES "section-6[.]3" OR
       NOT boundary_doc_content MATCHES "gzip, chunked")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/1 request-body plan boundary is undocumented"
            "${relative} must document parser-only alternatives, payload ownership, explicit zero length, RFC framing, and final-chunked transfer-coding order")
    endif()
    if(NOT boundary_doc_content MATCHES "Http1ChunkedBodyDecoder" OR
       NOT boundary_doc_content MATCHES "Http1ChunkDecodeNeedMore" OR
       NOT boundary_doc_content MATCHES "Http1ChunkDecodeBodyChunk" OR
       NOT boundary_doc_content MATCHES "Http1ChunkDecodeComplete" OR
       NOT boundary_doc_content MATCHES "HttpChunkScanNeedMore" OR
       NOT boundary_doc_content MATCHES "HttpChunkScanComplete" OR
       NOT boundary_doc_content MATCHES "HttpChunkScanFailure" OR
       NOT boundary_doc_content MATCHES "section-7[.]1" OR
       NOT boundary_doc_content MATCHES "section-8")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/1 chunked result ownership is undocumented"
            "${relative} must pin RFC completion, incremental consumption, and whole-message alternative field ownership")
    endif()
    if(NOT boundary_doc_content MATCHES "HttpClientRequestContent" OR
       NOT boundary_doc_content MATCHES "HttpClientRequestWithoutContent" OR
       NOT boundary_doc_content MATCHES "HttpClientRequestBytes" OR
       NOT boundary_doc_content MATCHES "Http1ClientRequestWriter" OR
       NOT boundary_doc_content MATCHES "PreparedHttp1ClientRequest" OR
       NOT boundary_doc_content MATCHES "Http1ClientRequestWirePolicy" OR
       NOT boundary_doc_content MATCHES "Http1ClientRequestContentPlan" OR
       NOT boundary_doc_content MATCHES "Http1ClientRequestWithoutContent" OR
       NOT boundary_doc_content MATCHES
           "Http1ClientImmediateRequestContent" OR
       NOT boundary_doc_content MATCHES
           "Http1ClientContinueGatedRequestContent" OR
       NOT boundary_doc_content MATCHES "continue-gated" OR
       NOT boundary_doc_content MATCHES "caller-provided" OR
       NOT boundary_doc_content MATCHES "prepareConnect" OR
       NOT boundary_doc_content MATCHES "Content-Length: 0" OR
       NOT boundary_doc_content MATCHES "section-6[.]3" OR
       NOT boundary_doc_content MATCHES "Host")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/1 client request writer boundary is undocumented"
            "${relative} must document typed content/gating, caller-buffer preparation, managed Host/framing/Expect, dedicated CONNECT, and Prepared-bound parsing")
    endif()
    if(NOT boundary_doc_content MATCHES "Http1ClientResponseParser" OR
       NOT boundary_doc_content MATCHES "Http1ClientResponseParseResult" OR
       NOT boundary_doc_content MATCHES "Http1ClientResponseNeedMore" OR
       NOT boundary_doc_content MATCHES "Http1ClientResponseParseFailure" OR
       NOT boundary_doc_content MATCHES "consumedBytes" OR
       NOT boundary_doc_content MATCHES "101 Switching Protocols" OR
       NOT boundary_doc_content MATCHES "Http1ClientResponsePlan" OR
       NOT boundary_doc_content MATCHES "Http1ClientInformationalResponse" OR
       NOT boundary_doc_content MATCHES "Http1ClientResponseWithoutContent" OR
       NOT boundary_doc_content MATCHES "Http1ClientKnownLengthResponse" OR
       NOT boundary_doc_content MATCHES "Http1ClientChunkedResponse" OR
       NOT boundary_doc_content MATCHES "Http1ClientCloseDelimitedResponse" OR
       NOT boundary_doc_content MATCHES "Http1ClientConnectTunnel" OR
       NOT boundary_doc_content MATCHES "Http1ClientProtocolUpgrade" OR
       NOT boundary_doc_content MATCHES "Http1ClientResponsePersistence" OR
       NOT boundary_doc_content MATCHES "section-6[.]3" OR
       NOT boundary_doc_content MATCHES "section-9[.]3" OR
       NOT boundary_doc_content MATCHES "close-delimited" OR
       NOT boundary_doc_content MATCHES "Http1ClientRequestContentSignal" OR
       NOT boundary_doc_content MATCHES "Http1ClientRequestContentCompletionStatus" OR
       NOT boundary_doc_content MATCHES "completeRequestContent" OR
       NOT boundary_doc_content MATCHES "stateful" OR
       NOT boundary_doc_content MATCHES "100 Continue" OR
       NOT boundary_doc_content MATCHES "HttpContentLengthState" OR
       NOT boundary_doc_content MATCHES "HttpTransferEncodingState")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/1 client response plan boundary is undocumented"
            "${relative} must document public tri-state head parsing, exact consumption, typed framing/persistence/Upgrade, stateful Expect progress, and shared field parsing")
    endif()
    if(NOT boundary_doc_content MATCHES "HttpKnownMethod" OR
       NOT boundary_doc_content MATCHES "knownMethod[(][)]" OR
       NOT boundary_doc_content MATCHES "isValidHttpMethodToken" OR
       NOT boundary_doc_content MATCHES "501" OR
       NOT boundary_doc_content MATCHES "Http2RequestBuilder::routeMethod")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("extensible HTTP method contract is undocumented"
            "${relative} must distinguish exact wire tokens, known semantics, Web 501, and WS route-only mapping")
    endif()
    if(NOT boundary_doc_content MATCHES "resolveHttpByteRange" OR
       NOT boundary_doc_content MATCHES "HttpByteRangeResolution" OR
       NOT boundary_doc_content MATCHES "HttpByteRangeIgnored" OR
       NOT boundary_doc_content MATCHES "HttpByteRangeUnsatisfiable" OR
       NOT boundary_doc_content MATCHES "HttpResolvedByteRange" OR
       NOT boundary_doc_content MATCHES "section-14[.]1" OR
       NOT boundary_doc_content MATCHES "section-14[.]1[.]2" OR
       NOT boundary_doc_content MATCHES "section-14[.]2" OR
       NOT boundary_doc_content MATCHES "416" OR
       NOT boundary_doc_content MATCHES "206")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP byte-range resolution contract is undocumented"
            "${relative} must document exclusive outcomes, overflow-safe resolution, and the RFC 200/416/206 mapping")
    endif()
    if(NOT boundary_doc_content MATCHES "HttpResponseContentSemantics" OR
       NOT boundary_doc_content MATCHES "HttpInformationalResponseContent" OR
       NOT boundary_doc_content MATCHES "HttpProtocolSwitchResponseContent" OR
       NOT boundary_doc_content MATCHES "HttpConnectTunnelResponseContent" OR
       NOT boundary_doc_content MATCHES "HttpResponseWithoutContent" OR
       NOT boundary_doc_content MATCHES "HttpResponseWithContent" OR
       NOT boundary_doc_content MATCHES "HttpResponseBodyPlan" OR
       NOT boundary_doc_content MATCHES "HttpBufferedResponseWritePlan")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("response body-plan boundary is undocumented"
            "${relative} must describe the shared response-content alternatives and HTTP-owned write plans")
    endif()
    if(NOT boundary_doc_content MATCHES "ResponseStreamCommitPlan" OR
       NOT boundary_doc_content MATCHES "ResponseStreamWriter::end" OR
       NOT boundary_doc_content MATCHES "submitResponseTrailerSection" OR
       NOT boundary_doc_content MATCHES "Http2ResponseTrailerSubmitStatus")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("response trailer terminal contract is undocumented"
            "${relative} must document commit capability, terminal API, and typed H2 ownership")
    endif()
    if(NOT boundary_doc_content MATCHES "205 Reset Content" OR
       NOT boundary_doc_content MATCHES "Content-Length: 0")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("205 response-body contract is undocumented"
            "${relative} must describe HTTP-owned zero-length 205 framing")
    endif()
    if(NOT boundary_doc_content MATCHES "Http2LocalSendState" OR
       NOT boundary_doc_content MATCHES "Http2LocalHeadPending" OR
       NOT boundary_doc_content MATCHES "Http2LocalRequestContentOpen" OR
       NOT boundary_doc_content MATCHES "Http2LocalResponseContentOpen" OR
       NOT boundary_doc_content MATCHES "Http2LocalResponseTrailersOnly" OR
       NOT boundary_doc_content MATCHES "Http2LocalConnectPending" OR
       NOT boundary_doc_content MATCHES "Http2LocalTunnelOpen" OR
       NOT boundary_doc_content MATCHES "Http2LocalEndStreamQueued" OR
       NOT boundary_doc_content MATCHES "Http2LocalEndStreamCommitted" OR
       NOT boundary_doc_content MATCHES "Http2StreamAborted" OR
       NOT boundary_doc_content MATCHES "localSend[(][)]" OR
       NOT boundary_doc_content MATCHES "isAborted[(][)]" OR
       NOT boundary_doc_content MATCHES "abort[(]source[)]" OR
       NOT boundary_doc_content MATCHES "friend" OR
       NOT boundary_doc_content MATCHES "Http2StreamState" OR
       NOT boundary_doc_content MATCHES "kNone" OR
       NOT boundary_doc_content MATCHES "section-5[.]1" OR
       NOT boundary_doc_content MATCHES "section-6[.]1" OR
       NOT boundary_doc_content MATCHES "section-6[.]2" OR
       NOT boundary_doc_content MATCHES "section-6[.]4" OR
       NOT boundary_doc_content MATCHES "section-6[.]8")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/2 local send lifecycle is undocumented"
            "${relative} must document exclusive head/content/trailer/CONNECT/END_STREAM/abort alternatives, friend-only mutation through Http2StreamState, one const view, atomic abort cleanup, and RFC frame/GOAWAY semantics")
    endif()
    if(NOT boundary_doc_content MATCHES "Http2RemoteReceiveState" OR
       NOT boundary_doc_content MATCHES "Http2RemoteHeadPending" OR
       NOT boundary_doc_content MATCHES
           "Http2RemoteHeadEndStreamPending" OR
       NOT boundary_doc_content MATCHES "Http2RemoteContentOpen" OR
       NOT boundary_doc_content MATCHES "Http2RemoteConnectPending" OR
       NOT boundary_doc_content MATCHES
           "Http2RemoteConnectPendingEndStream" OR
       NOT boundary_doc_content MATCHES
           "Http2RemoteConnectRejectedAwaitingEndStream" OR
       NOT boundary_doc_content MATCHES "Http2RemoteTunnelOpen" OR
       NOT boundary_doc_content MATCHES "Http2RemoteEndStream" OR
       NOT boundary_doc_content MATCHES "Http2RemoteAborted" OR
       NOT boundary_doc_content MATCHES "remoteReceive[(][)]" OR
       NOT boundary_doc_content MATCHES "section-5[.]1" OR
       NOT boundary_doc_content MATCHES "section-8[.]1" OR
       NOT boundary_doc_content MATCHES "section-8[.]5" OR
       NOT boundary_doc_content MATCHES "rfc8441[.]html#section-5")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/2 remote receive lifecycle is undocumented"
            "${relative} must document exclusive head/content/CONNECT/tunnel/END_STREAM/abort alternatives, one const view, rejected-CONNECT termination, tunnel flow-control ownership, and RFC half-close semantics")
    endif()
    if(NOT boundary_doc_content MATCHES "Http2LocalContentState" OR
       NOT boundary_doc_content MATCHES "Http2LocalContentUnset" OR
       NOT boundary_doc_content MATCHES "Http2LocalContentForbidden" OR
       NOT boundary_doc_content MATCHES "Http2LocalContentUnbounded" OR
       NOT boundary_doc_content MATCHES "Http2LocalContentKnownLength" OR
       NOT boundary_doc_content MATCHES "kNotStarted" OR
       NOT boundary_doc_content MATCHES "localContent[(][)]" OR
       NOT boundary_doc_content MATCHES "section-8[.]1" OR
       NOT boundary_doc_content MATCHES "section-8[.]1[.]1" OR
       NOT boundary_doc_content MATCHES "accepted" OR
       NOT boundary_doc_content MATCHES "committed" OR
       NOT boundary_doc_content MATCHES "Http2ResponseHeadPlan" OR
       NOT boundary_doc_content MATCHES
           "Http2CanonicalResponseContentLength" OR
       NOT boundary_doc_content MATCHES
           "Http2ExplicitResponseContentLength" OR
       NOT boundary_doc_content MATCHES
           "Http2AbsentResponseContentLength" OR
       NOT boundary_doc_content MATCHES
           "Http2ForbiddenResponseContentLength" OR
       NOT boundary_doc_content MATCHES
           "Http2ResponseHeadPlanResult" OR
       NOT boundary_doc_content MATCHES "http2BufferedResponseHeadPlan" OR
       NOT boundary_doc_content MATCHES "http2StreamingResponseHeadPlan" OR
       NOT boundary_doc_content MATCHES "http2ConnectResponseHeadPlan" OR
       NOT boundary_doc_content MATCHES "emitAutoContentLength")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/2 outbound Content-Length contract is undocumented"
            "${relative} must document exclusive head ownership, removal of the scalar API, single explicit-length parsing, local-content states, and accepted versus committed DATA")
    endif()
    if(NOT boundary_doc_content MATCHES "Http2RemoteContentState" OR
       NOT boundary_doc_content MATCHES
           "Http2RemoteContentAllowedWithoutLength" OR
       NOT boundary_doc_content MATCHES
           "Http2RemoteContentAllowedKnownLength" OR
       NOT boundary_doc_content MATCHES
           "Http2RemoteContentMetadataOnlyWithoutLength" OR
       NOT boundary_doc_content MATCHES
           "Http2RemoteContentMetadataOnlyKnownLength" OR
       NOT boundary_doc_content MATCHES "remoteContent[(][)]" OR
       NOT boundary_doc_content MATCHES "kCounterOverflow" OR
       NOT boundary_doc_content MATCHES "kDeclaredLengthExceeded" OR
       NOT boundary_doc_content MATCHES "kContentForbidden" OR
       NOT boundary_doc_content MATCHES "account[(][)]" OR
       NOT boundary_doc_content MATCHES "terminalLengthValid[(][)]" OR
       NOT boundary_doc_content MATCHES "section-6[.]4[.]1" OR
       NOT boundary_doc_content MATCHES "section-8[.]1[.]1")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/2 inbound Content-Length contract is undocumented"
            "${relative} must document allowance/length alternatives, atomic DATA accounting, and malformed no-content payload rejection")
    endif()
    if(NOT boundary_doc_content MATCHES "Http2RequestContent" OR
       NOT boundary_doc_content MATCHES "Http2RequestWithoutContent" OR
       NOT boundary_doc_content MATCHES
           "Http2KnownLengthRequestContent" OR
       NOT boundary_doc_content MATCHES
           "Http2StreamingRequestContent" OR
       NOT boundary_doc_content MATCHES "submitRegularRequestHead" OR
       NOT boundary_doc_content MATCHES "Http2RequestHeadSubmitResult" OR
       NOT boundary_doc_content MATCHES "Http2SubmittedRequestHead" OR
       NOT boundary_doc_content MATCHES "Http2RequestHeadSubmitFailure" OR
       NOT boundary_doc_content MATCHES "Http2RequestHeadSubmitError" OR
       NOT boundary_doc_content MATCHES "section-8[.]1" OR
       NOT boundary_doc_content MATCHES "section-8[.]1[.]1" OR
       NOT boundary_doc_content MATCHES "section-5[.]1[.]1" OR
       NOT boundary_doc_content MATCHES "section-5[.]1[.]2" OR
       NOT boundary_doc_content MATCHES "section-6[.]5[.]2" OR
       NOT boundary_doc_content MATCHES "SETTINGS_MAX_CONCURRENT_STREAMS")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/2 typed request-content contract is undocumented"
            "${relative} must document request framing, discriminated stream admission, and CONNECT isolation")
    endif()
    if(NOT boundary_doc_content MATCHES
           "Http2BufferedResponseHeadSubmitResult" OR
       NOT boundary_doc_content MATCHES
           "Http2StreamingResponseHeadSubmitResult" OR
       NOT boundary_doc_content MATCHES
           "Http2SubmittedResponseHead" OR
       NOT boundary_doc_content MATCHES
           "Http2ResponseHeadSubmitFailure" OR
       NOT boundary_doc_content MATCHES
           "Http2ResponseHeadSubmitError" OR
       NOT boundary_doc_content MATCHES "section-8[.]1" OR
       NOT boundary_doc_content MATCHES "section-8[.]1[.]1" OR
       NOT boundary_doc_content MATCHES "section-6[.]2")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/2 response-head submit ownership is undocumented"
            "${relative} must document plan-only success, error-only failure, and RFC response HEADERS framing")
    endif()
    if(NOT boundary_doc_content MATCHES "submitConnectRequestHead" OR
       NOT boundary_doc_content MATCHES "submitExtendedConnectRequestHead" OR
       NOT boundary_doc_content MATCHES "kTunnelData" OR
       NOT boundary_doc_content MATCHES "kTunnelEnd" OR
       NOT boundary_doc_content MATCHES "Http2TunnelState" OR
       NOT boundary_doc_content MATCHES "Http2NotConnect" OR
       NOT boundary_doc_content MATCHES "Http2ConnectPending" OR
       NOT boundary_doc_content MATCHES "Http2TunnelOpen" OR
       NOT boundary_doc_content MATCHES "Http2ConnectRejected" OR
       NOT boundary_doc_content MATCHES "Http2ConnectForm" OR
       NOT boundary_doc_content MATCHES "tunnel[(][)]" OR
       NOT boundary_doc_content MATCHES "section-8[.]5" OR
       NOT boundary_doc_content MATCHES "rfc8441[.]html#section-4")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/2 CONNECT lifecycle is undocumented"
            "${relative} must document exclusive tunnel states, pending-only form, dedicated heads, events, and half-close ownership")
    endif()
    if(NOT boundary_doc_content MATCHES "Http2LocalSettings" OR
       NOT boundary_doc_content MATCHES "Http2ConnectionLimits" OR
       NOT boundary_doc_content MATCHES "beginConnection" OR
       NOT boundary_doc_content MATCHES "Http2PeerSettings" OR
       NOT boundary_doc_content MATCHES "SETTINGS_ENABLE_PUSH" OR
       NOT boundary_doc_content MATCHES "Http2PeerSettingApplyResult" OR
       NOT boundary_doc_content MATCHES "Http2PeerSettingApplied" OR
       NOT boundary_doc_content MATCHES "Http2PeerInitialWindowChange" OR
       NOT boundary_doc_content MATCHES "Http2PeerSettingFailure" OR
       NOT boundary_doc_content MATCHES "section-6[.]9[.]2" OR
       NOT boundary_doc_content MATCHES "rfc8441[.]html#section-3")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/2 local SETTINGS and startup contract is undocumented"
            "${relative} must document the one settings source, role-aware startup, and discriminated peer-setting result")
    endif()
    if(NOT boundary_doc_content MATCHES "Http2FeedResult" OR
       NOT boundary_doc_content MATCHES "kConnectionNotStarted" OR
       NOT boundary_doc_content MATCHES "kEventsPending" OR
       NOT boundary_doc_content MATCHES "kAccepted" OR
       NOT boundary_doc_content MATCHES "kNeedInput" OR
       NOT boundary_doc_content MATCHES "kProtocolFailure" OR
       NOT boundary_doc_content MATCHES "section-3[.]4" OR
       NOT boundary_doc_content MATCHES "section-4[.]1")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/2 feed ownership contract is undocumented"
            "${relative} must document every direct enum outcome and its RFC preface/frame boundary")
    endif()
    if(NOT boundary_doc_content MATCHES "std::optional<Http2Event>" OR
       NOT boundary_doc_content MATCHES "std::nullopt" OR
       NOT boundary_doc_content MATCHES "std::variant" OR
       NOT boundary_doc_content MATCHES "Http2StreamClosedEvent" OR
       NOT boundary_doc_content MATCHES "Http2RequestUnprocessedEvent" OR
       NOT boundary_doc_content MATCHES "Http2GoawayEvent" OR
       NOT boundary_doc_content MATCHES "section-6[.]4" OR
       NOT boundary_doc_content MATCHES "section-6[.]8")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("HTTP/2 typed event ownership is undocumented"
            "${relative} must pin optional draining, discriminated payloads, exact RST errors, and connection-level GOAWAY metadata")
    endif()
endforeach()
check_files_no_match("normal responses must not reintroduce a dynamic streaming-body bypass"
    "${RULE_DYNAMIC_RESPONSE_BODY_STREAM}" ${EDGE_REFERENCE_SOURCE})
check_files_no_match("ruvia-http CMake contains stale mixed-responsibility names"
    "src/Streaming\.cpp|include/ruvia/http/(JsonUtils|HttpBodyStream)\.h"
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt")
check_files_no_match("Router/error mapping must not decide HTTP/1 connection persistence"
    "${RULE_ROUTER_CONNECTION_POLICY}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RequestDispatcher.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteTable.h"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterMiddlewareDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/Error.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextErrorResponse.cpp")
check_files_no_match("ruvia-core must not contain HTTP/WebSocket semantics"
    "${RULE_CORE_PROTOCOL}" ${CORE_SOURCE})
check_files_no_match("target src directories must not be PUBLIC/INTERFACE includes"
    "${RULE_PUBLIC_SRC_INCLUDE}"
    "${RUVIA_ROOT}/ruvia-core/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt")
check_files_no_match("targets must not include another target's private src tree"
    "${RULE_CROSS_TARGET_SRC}"
    "${RUVIA_ROOT}/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-core/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt"
    "${RUVIA_ROOT}/tests/CMakeLists.txt")
check_files_no_match("targets must not include another target by physical path"
    "${RULE_CROSS_TARGET_PHYSICAL_INCLUDE}"
    ${CORE_SOURCE} ${HTTP_SOURCE} ${WEB_SOURCE})
check_files_no_match("ruvia-web must not implement content/transfer coding"
    "${RULE_WEB_CODEC}" ${WEB_SOURCE})
check_files_no_match("ruvia-web must not provide an outbound HTTP client runtime"
    "${RULE_WEB_HTTP_CLIENT}" ${WEB_SOURCE})
check_files_no_lower_match("core connection scanner contains protocol/product semantics"
    "${RULE_SCANNER_SEMANTICS}"
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/ConnectionScanner.h"
    "${RUVIA_ROOT}/ruvia-core/src/ConnectionScanner.cpp")

# Cross-target contracts live in include/.../detail. Source trees contain
# implementations only (plus each target's own PCH), so another target cannot
# silently become a friend by reaching into private implementation headers.
file(GLOB_RECURSE HTTP_PRIVATE_HEADERS LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-http/src/*.h"
    "${RUVIA_ROOT}/ruvia-http/src/*.inl")
if(HTTP_PRIVATE_HEADERS)
    list(JOIN HTTP_PRIVATE_HEADERS "\n    " details)
    boundary_error("ruvia-http/src contains private headers" "${details}")
endif()
file(GLOB_RECURSE CORE_PRIVATE_HEADERS LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-core/src/*.h"
    "${RUVIA_ROOT}/ruvia-core/src/*.inl")
list(FILTER CORE_PRIVATE_HEADERS EXCLUDE REGEX "[/\\\\]pch\\.h$")
if(CORE_PRIVATE_HEADERS)
    list(JOIN CORE_PRIVATE_HEADERS "\n    " details)
    boundary_error("ruvia-core/src contains cross-target contract headers" "${details}")
endif()
file(GLOB_RECURSE WEB_PRIVATE_HEADERS LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-web/src/*.h"
    "${RUVIA_ROOT}/ruvia-web/src/*.inl")
list(FILTER WEB_PRIVATE_HEADERS EXCLUDE REGEX "[/\\\\]pch\\.h$")
if(WEB_PRIVATE_HEADERS)
    list(JOIN WEB_PRIVATE_HEADERS "\n    " details)
    boundary_error("ruvia-web/src contains contract headers" "${details}")
endif()
foreach(stale_dir IN ITEMS
    "${RUVIA_ROOT}/ruvia-core/src/memory"
    "${RUVIA_ROOT}/ruvia-core/src/net"
    "${RUVIA_ROOT}/ruvia-core/src/runtime"
    "${RUVIA_ROOT}/ruvia-http/src/net"
    "${RUVIA_ROOT}/ruvia-web/src/net"
    "${RUVIA_ROOT}/ruvia-web/src/db/core"
    "${RUVIA_ROOT}/ruvia-web/src/redis/core"
    "${RUVIA_ROOT}/ruvia-web/src/router/core")
    if(IS_DIRECTORY "${stale_dir}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${stale_dir}")
        boundary_error("redundant source directory layer was reintroduced" "${relative}")
    endif()
endforeach()

file(GLOB_RECURSE EXAMPLE_SOURCE LIST_DIRECTORIES FALSE "${RUVIA_ROOT}/examples/*.cpp")
set(private_examples)
foreach(path IN LISTS EXAMPLE_SOURCE)
    file(READ "${path}" content)
    example_has_private_include("${content}" has_private)
    if(has_private)
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${path}")
        list(APPEND private_examples "${relative}")
    endif()
endforeach()
if(private_examples)
    list(JOIN private_examples "\n    " details)
    boundary_error("examples include target-private headers" "${details}")
endif()

set(WS_PROTOCOL_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsConnection.h")
set(WS_EVENT_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsEvent.h")
set(WS_INBOUND_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketUtils.h")
set(WS_PROTOCOL_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp")
set(WS_VALIDATION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/websocket/HttpWebSocketValidation.cpp")
set(WS_RUNTIME_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnection.h")
set(WS_RUNTIME_READ
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionRead.inl")
set(WS_RUNTIME_WRITE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionWrite.inl")
set(WS_H2_TRANSPORT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
set(WS_LIVENESS_POLICY
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketLiveness.h")
set(WS_PUBLIC_CONFIG "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/WebSocket.h")
foreach(required IN ITEMS
    "${WS_PROTOCOL_HEADER}"
    "${WS_EVENT_HEADER}"
    "${WS_INBOUND_HEADER}"
    "${WS_PROTOCOL_SOURCE}"
    "${WS_VALIDATION_SOURCE}"
    "${WS_RUNTIME_HEADER}"
    "${WS_RUNTIME_READ}"
    "${WS_RUNTIME_WRITE}"
    "${WS_H2_TRANSPORT}"
    "${WS_LIVENESS_POLICY}"
    "${WS_PUBLIC_CONFIG}")
    if(NOT EXISTS "${required}")
        boundary_error("typed WebSocket close chain is missing" "${required}")
    endif()
endforeach()
if(EXISTS "${WS_PROTOCOL_HEADER}" AND EXISTS "${WS_EVENT_HEADER}" AND
   EXISTS "${WS_INBOUND_HEADER}" AND EXISTS "${WS_PROTOCOL_SOURCE}" AND
   EXISTS "${WS_VALIDATION_SOURCE}" AND
   EXISTS "${WS_RUNTIME_HEADER}" AND EXISTS "${WS_RUNTIME_READ}" AND
   EXISTS "${WS_RUNTIME_WRITE}" AND EXISTS "${WS_H2_TRANSPORT}" AND
   EXISTS "${WS_LIVENESS_POLICY}" AND EXISTS "${WS_PUBLIC_CONFIG}")
    file(READ "${WS_PROTOCOL_HEADER}" ws_protocol)
    file(READ "${WS_EVENT_HEADER}" ws_event)
    file(READ "${WS_INBOUND_HEADER}" ws_inbound)
    file(READ "${WS_PROTOCOL_SOURCE}" ws_protocol_source)
    file(READ "${WS_VALIDATION_SOURCE}" ws_validation_source)
    file(READ "${WS_RUNTIME_HEADER}" ws_runtime)
    file(READ "${WS_RUNTIME_READ}" ws_runtime_read)
    file(READ "${WS_RUNTIME_WRITE}" ws_runtime_write)
    file(READ "${WS_H2_TRANSPORT}" ws_h2_transport)
    file(READ "${WS_LIVENESS_POLICY}" ws_liveness)
    file(READ "${WS_PUBLIC_CONFIG}" ws_public_config)
    if(NOT ws_protocol MATCHES "enum class WsClosePhase" OR
       NOT ws_protocol MATCHES "class WsOutputPlan" OR
       NOT ws_protocol MATCHES "WsTransportDisposition" OR
       NOT ws_protocol MATCHES "std::optional<WsEvent> poll" OR
       NOT ws_protocol MATCHES "ruvia/http/detail/websocket/WsEvent.h")
        boundary_error("WebSocket close lifecycle lost its protocol-owned plan"
            "WsClosePhase, WsOutputPlan, typed transport disposition, and optional poll() must stay bound")
    endif()
    if(NOT ws_event MATCHES "enum class WsEventKind" OR
       NOT ws_event MATCHES "using Value = std::variant" OR
       NOT ws_event MATCHES "class WsMessageEvent final" OR
       NOT ws_event MATCHES "class WsPingEvent final" OR
       NOT ws_event MATCHES "class WsPongEvent final" OR
       NOT ws_event MATCHES "class WsCloseEvent final" OR
       NOT ws_event MATCHES "std::string_view reason" OR
       NOT ws_event MATCHES "class WsProtocolErrorEvent final" OR
       NOT ws_event MATCHES "class WsTransportEndEvent final" OR
       NOT ws_event MATCHES "std::get_if<WsCloseEvent>")
        boundary_error("WebSocket event payloads lost their discriminated contract"
            "need-input must be optional and every materialized message, control, close, failure, or terminal event must own only its valid fields")
    endif()
    if(NOT ws_inbound MATCHES "enum class WebSocketProtocolFailure" OR
       NOT ws_inbound MATCHES "class WebSocketFrameNeedInput final" OR
       NOT ws_inbound MATCHES "class WebSocketFrameReadFailure final" OR
       NOT ws_inbound MATCHES "class WebSocketFrameReadResult final" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketFrameNeedInput>" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketFrameView>" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketFrameReadFailure>" OR
       NOT ws_inbound MATCHES "class WebSocketInboundContinue final" OR
       NOT ws_inbound MATCHES "class WebSocketInboundControlFrame final" OR
       NOT ws_inbound MATCHES "enum class WebSocketInboundContentEncoding" OR
       NOT ws_inbound MATCHES "class WebSocketInboundMessage final" OR
       NOT ws_inbound MATCHES "class WebSocketInboundFailure final" OR
       NOT ws_inbound MATCHES "class WebSocketInboundResult final" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketInboundContinue>" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketInboundControlFrame>" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketInboundMessage>" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketInboundFailure>" OR
       NOT ws_inbound MATCHES "webSocketClosePayloadFailure" OR
       NOT ws_protocol_source MATCHES "read[.]failure[(][)]" OR
       NOT ws_protocol_source MATCHES "inbound[.]failure[(][)]" OR
       NOT ws_protocol_source MATCHES "webSocketProtocolFailureCloseCode" OR
       NOT ws_validation_source MATCHES
           "std::optional<WebSocketProtocolFailure>" OR
       NOT ws_validation_source MATCHES
           "webSocketClosePayloadFailure[(]std::string_view payload[)] noexcept")
        boundary_error("WebSocket inbound results lost their discriminated contract"
            "frame decode, reassembly, close validation, and WsConnection failure mapping must remain typed and nonthrowing for peer bytes")
    endif()
    if(NOT ws_runtime MATCHES "WsConnection protocol_" OR
       NOT ws_runtime_read MATCHES "event[.]has_value[(][)]" OR
       NOT ws_runtime_read MATCHES "event->message[(][)]" OR
       NOT ws_runtime_read MATCHES "event->ping[(][)]" OR
       NOT ws_runtime_read MATCHES "event->pong[(][)]" OR
       NOT ws_runtime_read MATCHES "event->close[(][)]" OR
       NOT ws_runtime_read MATCHES "event->protocolError[(][)]" OR
       NOT ws_runtime_read MATCHES "event->transportEnd[(][)]" OR
       NOT ws_runtime_write MATCHES "plan\\.disposition\\(\\)" OR
       NOT ws_runtime_write MATCHES "[(]void[)]co_await read\\(\\)")
        boundary_error("ruvia-web WebSocket runtime bypasses the protocol close plan"
            "the driver must flush WsOutputPlan and await peer Close after local Close")
    endif()
    if(NOT ws_h2_transport MATCHES
           "WsTransportDisposition::kEndTransport" OR
       NOT ws_h2_transport MATCHES
           "submitReset\\(streamId_, Http2ErrorCode::kCancel\\)")
        boundary_error("RFC 8441 transport mapping lost stream-local lifecycle"
            "typed end must map to END_STREAM and liveness abort to RST_STREAM(CANCEL)")
    endif()
    if(NOT ws_public_config MATCHES "struct WebSocketLifecycleOptions" OR
       NOT ws_public_config MATCHES "closeHandshakeTimeout" OR
       NOT ws_liveness MATCHES "WsClosePhase closePhase" OR
       NOT ws_runtime MATCHES "WebSocketLifecycleOptions lifecycleOptions_")
        boundary_error("WebSocket liveness policy is not Web-owned"
            "timer configuration and enforcement must remain in ruvia-web")
    endif()
endif()

set(WS_PROTOCOL_TEST "${RUVIA_ROOT}/tests/unit_ws_connection.cpp")
set(WS_RUNTIME_TEST "${RUVIA_ROOT}/tests/unit_websocket_connection.cpp")
set(WS_H2_DRIVER_TEST "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
set(WS_PACKAGE_CONSUMER "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
if(EXISTS "${WS_PROTOCOL_TEST}" AND EXISTS "${WS_RUNTIME_TEST}" AND
   EXISTS "${WS_H2_DRIVER_TEST}" AND EXISTS "${WS_PACKAGE_CONSUMER}")
    file(READ "${WS_PROTOCOL_TEST}" ws_protocol_test)
    file(READ "${WS_RUNTIME_TEST}" ws_runtime_test)
    file(READ "${WS_H2_DRIVER_TEST}" ws_h2_driver_test)
    file(READ "${WS_PACKAGE_CONSUMER}" ws_package_consumer)
    if(NOT ws_protocol_test MATCHES
           "ws_connection_event_is_optional_and_discriminated" OR
       NOT ws_protocol_test MATCHES "close_without_status_reports_1005" OR
       NOT ws_protocol_test MATCHES
           "protocolError[(][)]->closeCode[(][)]" OR
       NOT ws_protocol_test MATCHES "local_close_waits_for_peer_close" OR
       NOT ws_protocol_test MATCHES "transport_eof_discards_unsent_close" OR
       NOT ws_runtime_test MATCHES "liveness_aborts_transport_not_scanner_owner" OR
       NOT ws_h2_driver_test MATCHES "h2_server_close_waits_for_peer_close" OR
       NOT ws_package_consumer MATCHES
           "std::optional<ruvia::detail::WsEvent>" OR
       NOT ws_package_consumer MATCHES
           "default_initializable<ruvia::detail::WsEvent>" OR
       NOT ws_package_consumer MATCHES "WsProtocolErrorEvent")
        boundary_error("typed WebSocket close lifecycle is insufficiently tested"
            "optional typed events, close semantics, EOF, stream-local timeout, and RFC 8441 END_STREAM ordering are required")
    endif()
endif()

set(WS_FRAME_RESULT_TEST "${RUVIA_ROOT}/tests/unit_websocket_frame.cpp")
set(WS_ASSEMBLER_RESULT_TEST "${RUVIA_ROOT}/tests/unit_websocket_assembler.cpp")
set(WS_CLOSE_RESULT_TEST "${RUVIA_ROOT}/tests/unit_websocket_close.cpp")
if(EXISTS "${WS_FRAME_RESULT_TEST}" AND
   EXISTS "${WS_ASSEMBLER_RESULT_TEST}" AND
   EXISTS "${WS_CLOSE_RESULT_TEST}" AND
   EXISTS "${WS_PACKAGE_CONSUMER}")
    file(READ "${WS_FRAME_RESULT_TEST}" ws_frame_result_test)
    file(READ "${WS_ASSEMBLER_RESULT_TEST}" ws_assembler_result_test)
    file(READ "${WS_CLOSE_RESULT_TEST}" ws_close_result_test)
    file(READ "${WS_PACKAGE_CONSUMER}" ws_inbound_package_consumer)
    if(NOT ws_frame_result_test MATCHES
           "ws_frame_reader_needs_input_without_sentinel_metadata" OR
       NOT ws_frame_result_test MATCHES
           "ws_frame_reader_reports_typed_wire_failures" OR
       NOT ws_frame_result_test MATCHES
           "default_initializable<WebSocketFrameReadResult>" OR
       NOT ws_frame_result_test MATCHES
           "WebSocketProtocolFailure::kMessageTooLarge" OR
       NOT ws_assembler_result_test MATCHES
           "default_initializable<WebSocketInboundResult>" OR
       NOT ws_assembler_result_test MATCHES
           "WebSocketInboundContentEncoding::kPerMessageDeflate" OR
       NOT ws_assembler_result_test MATCHES
           "ws_assembler_protocol_errors" OR
       NOT ws_close_result_test MATCHES
           "webSocketClosePayloadFailure" OR
       NOT ws_close_result_test MATCHES
           "WebSocketProtocolFailure::kInvalidPayloadData" OR
       NOT ws_inbound_package_consumer MATCHES
           "WebSocketFrameReadResult" OR
       NOT ws_inbound_package_consumer MATCHES
           "WebSocketInboundResult" OR
       NOT ws_inbound_package_consumer MATCHES
           "HasWsRequiredBytesField" OR
       NOT ws_inbound_package_consumer MATCHES
           "HasWsInboundActionAccessor")
        boundary_error("typed WebSocket inbound results are insufficiently tested"
            "frame, reassembly, close validation, and installed-consumer tests must pin exclusive alternatives and RFC failure codes")
    endif()
endif()

set(HTTP1_SCAN ${HTTP_SOURCE} ${WEB_SOURCE} ${EXAMPLE_SOURCE})
file(GLOB_RECURSE TEST_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/tests/*.h" "${RUVIA_ROOT}/tests/*.cpp" "${RUVIA_ROOT}/tests/*.inl")
list(APPEND HTTP1_SCAN ${TEST_SOURCE}
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt"
    "${RUVIA_ROOT}/tests/CMakeLists.txt"
    "${RUVIA_ROOT}/examples/CMakeLists.txt")
check_files_no_match("parallel Http1Connection state machine was reintroduced"
    "${RULE_HTTP1_CONNECTION}" ${HTTP1_SCAN})

set(HTTP_RUNTIME_STATE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpConnectionState.h")
set(HTTP_BODY_READER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h")
if(EXISTS "${HTTP_RUNTIME_STATE}" AND EXISTS "${HTTP_BODY_READER}")
    file(READ "${HTTP_RUNTIME_STATE}" runtime_state)
    file(READ "${HTTP_BODY_READER}" body_reader)
    if(NOT runtime_state MATCHES "Http1ServerRequestParser" OR
       NOT body_reader MATCHES "Http1ChunkedBodyDecoder")
        boundary_error("ruvia-web HTTP/1 runtime is not driving http-owned primitives"
            "expected Http1ServerRequestParser and Http1ChunkedBodyDecoder in web runtime state")
    endif()
endif()

set(HTTP1_CHUNK_WEB_DRIVER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderChunked.inl")
set(HTTP1_CHUNK_DECODER_TEST "${RUVIA_ROOT}/tests/unit_chunk_decoder.cpp")
set(HTTP1_CHUNK_SCANNER_TEST "${RUVIA_ROOT}/tests/unit_http_parsing.cpp")
set(HTTP1_CHUNK_PACKAGE_CONSUMER "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
foreach(http1_chunk_driver_contract IN ITEMS
        "${HTTP1_CHUNK_WEB_DRIVER}"
        "${HTTP1_CHUNK_DECODER_TEST}"
        "${HTTP1_CHUNK_SCANNER_TEST}"
        "${HTTP1_CHUNK_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${http1_chunk_driver_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${http1_chunk_driver_contract}")
        boundary_error("HTTP/1 chunked driver contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP1_CHUNK_WEB_DRIVER}" AND
   EXISTS "${HTTP1_CHUNK_DECODER_TEST}" AND
   EXISTS "${HTTP1_CHUNK_SCANNER_TEST}" AND
   EXISTS "${HTTP1_CHUNK_PACKAGE_CONSUMER}")
    file(READ "${HTTP1_CHUNK_WEB_DRIVER}" http1_chunk_web_driver)
    file(READ "${HTTP1_CHUNK_DECODER_TEST}" http1_chunk_decoder_test)
    file(READ "${HTTP1_CHUNK_SCANNER_TEST}" http1_chunk_scanner_test)
    file(READ "${HTTP1_CHUNK_PACKAGE_CONSUMER}" http1_chunk_package_consumer)
    if(NOT http1_chunk_web_driver MATCHES "result[.]consumedBytes[(][)]" OR
       NOT http1_chunk_web_driver MATCHES "result[.]bodyChunk[(][)]" OR
       NOT http1_chunk_web_driver MATCHES "result[.]complete[(][)]" OR
       NOT http1_chunk_web_driver MATCHES "result[.]needMore[(][)]")
        boundary_error("ruvia-web bypasses the typed HTTP/1 chunk decoder result"
            "the runtime must drive consumed/body/complete/need-more accessors without reconstructing protocol state")
    endif()
    if(NOT http1_chunk_decoder_test MATCHES
           "chunked_body_decoder_emits_zero_copy_chunks_and_preserves_pipeline" OR
       NOT http1_chunk_decoder_test MATCHES
           "chunked_body_decoder_handles_single_byte_input_fragmentation" OR
       NOT http1_chunk_decoder_test MATCHES
           "HasChunkBytes<Http1ChunkDecodeBodyChunk>" OR
       NOT http1_chunk_scanner_test MATCHES
           "chunk_scan_result_is_discriminated" OR
       NOT http1_chunk_scanner_test MATCHES
           "HasChunkScanConsumedBytes<HttpChunkScanComplete>" OR
       NOT http1_chunk_package_consumer MATCHES
           "default_initializable<ruvia::detail::Http1ChunkDecodeResult>" OR
       NOT http1_chunk_package_consumer MATCHES
           "HasConsumedBytes<ruvia::detail::HttpChunkScanComplete>")
        boundary_error("typed HTTP/1 chunk result ownership is insufficiently tested"
            "unit and installed-consumer contracts must pin fragmentation, pipeline preservation, and alternative-specific fields")
    endif()
endif()

set(HTTP1_WEB_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
if(EXISTS "${HTTP1_WEB_SESSION}")
    file(READ "${HTTP1_WEB_SESSION}" http1_web_session)
    if(NOT http1_web_session MATCHES "parsed\\.headReady[(][)]" OR
       NOT http1_web_session MATCHES "parsed\\.failed[(][)]")
        boundary_error("ruvia-web stopped respecting the HTTP/1 head/message boundary"
            "route dispatch consumes head-ready while body readers own completion")
    endif()
endif()

foreach(obsolete_http2_upgrade_path IN ITEMS
    "ruvia-http/include/ruvia/http/detail/http2/Http2Upgrade.h"
    "ruvia-web/include/ruvia/web/detail/http2/Http2UpgradeHandshake.h"
    "ruvia-web/include/ruvia/web/detail/server/HttpServerHttp2UpgradeRoute.h"
    "tests/unit_http2_upgrade.cpp")
    if(EXISTS "${RUVIA_ROOT}/${obsolete_http2_upgrade_path}")
        boundary_error("obsolete HTTP/2 HTTP/1.1 Upgrade path was restored"
            "${obsolete_http2_upgrade_path} exists")
    endif()
endforeach()
check_files_no_match("obsolete HTTP/2 HTTP/1.1 Upgrade path was restored"
    "${RULE_OBSOLETE_HTTP2_UPGRADE}"
    ${HTTP1_PARSE_PHASE_REFERENCE_SOURCE})
check_files_no_match("generic base64url helper escaped ruvia-core"
    "${RULE_HTTP_OWNED_BASE64URL}"
    ${HTTP1_PARSE_PHASE_REFERENCE_SOURCE})
if(EXISTS "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpBase64Url.h")
    boundary_error("generic base64url helper escaped ruvia-core"
        "ruvia-http/include/ruvia/http/detail/HttpBase64Url.h exists")
endif()
set(CORE_BASE64URL
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/Base64Url.h")
set(WEB_JWT_ENCODING "${RUVIA_ROOT}/ruvia-web/src/auth/JwtEncoding.cpp")
if(NOT EXISTS "${CORE_BASE64URL}")
    boundary_error("core base64url primitive is missing"
        "ruvia-core/include/ruvia/core/detail/Base64Url.h")
elseif(EXISTS "${WEB_JWT_ENCODING}")
    file(READ "${WEB_JWT_ENCODING}" web_jwt_encoding)
    if(NOT web_jwt_encoding MATCHES "ruvia/core/detail/Base64Url[.]h" OR
       NOT web_jwt_encoding MATCHES "decodeBase64UrlChar")
        boundary_error("JWT stopped reusing the core base64url primitive"
            "ruvia-web must not recreate or recover the removed HTTP-owned helper")
    endif()
endif()

set(HTTP2_CLEARTEXT_DRIVER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerCleartextHttp2.h")
set(HTTP2_SANSIO_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
if(EXISTS "${HTTP2_CLEARTEXT_DRIVER}" AND EXISTS "${HTTP2_SANSIO_SESSION}")
    file(READ "${HTTP2_CLEARTEXT_DRIVER}" http2_cleartext_driver)
    file(READ "${HTTP2_SANSIO_SESSION}" http2_sansio_session)
    if(NOT http2_cleartext_driver MATCHES "probeCleartextHttp2Preface" OR
       NOT http2_cleartext_driver MATCHES "kHttp2ClientPreface" OR
       NOT http2_cleartext_driver MATCHES "kCompletePreface" OR
       NOT http2_cleartext_driver MATCHES "kNeedMorePreface" OR
       NOT http2_sansio_session MATCHES "connection\\.beginConnection[(][)]")
        boundary_error("HTTP/2 current startup path is incomplete"
            "TLS ALPN and cleartext prior knowledge must converge on beginConnection plus the client preface")
    endif()
endif()

set(BOUNDARY_DOCS "${RUVIA_ROOT}/README.md" "${RUVIA_ROOT}/AGENTS.md")
check_files_no_match("docs reference the deleted coroutine h2 server session"
    "${RULE_DELETED_H2_SESSION}" ${BOUNDARY_DOCS})
check_files_no_match("docs contain stale dependency/runtime ownership"
    "${RULE_STALE_DEPENDENCY}" ${BOUNDARY_DOCS})
check_files_no_match("docs must read the vcpkg toolchain from VCPKG_ROOT"
    "${RULE_HARDCODED_VCPKG_TOOLCHAIN}" ${BOUNDARY_DOCS})
file(READ "${RUVIA_ROOT}/README.md" readme_content)
file(READ "${RUVIA_ROOT}/AGENTS.md" agents_content)
if(NOT readme_content MATCHES "RFC 9113" OR
   NOT readme_content MATCHES "prior knowledge" OR
   NOT readme_content MATCHES "HTTP2-Settings" OR
   NOT agents_content MATCHES "RFC 9113" OR
   NOT agents_content MATCHES "prior knowledge" OR
   NOT agents_content MATCHES "HTTP2-Settings")
    boundary_error("current HTTP/2 startup contract is undocumented"
        "README and AGENTS must record ALPN/prior-knowledge startup and removal of HTTP/1.1 Upgrade")
endif()
if(NOT readme_content MATCHES "Http1ServerRequestParseState" OR
   NOT readme_content MATCHES "kRequestHeadReady" OR
   NOT readme_content MATCHES "kRequestMessageReady" OR
   NOT agents_content MATCHES "Http1ServerRequestParser" OR
   NOT agents_content MATCHES "kNeedRequestBody")
    boundary_error("HTTP/1 head/message parse phases are undocumented"
        "README and AGENTS must pin the distinct runtime and whole-message readiness contracts")
endif()
if(NOT readme_content MATCHES "WsOutputPlan" OR
   NOT readme_content MATCHES "std::optional<WsEvent>" OR
   NOT readme_content MATCHES "std::nullopt" OR
   NOT readme_content MATCHES "std::variant" OR
   NOT readme_content MATCHES "WsMessageEvent" OR
   NOT readme_content MATCHES "WsCloseEvent" OR
   NOT readme_content MATCHES "WsProtocolErrorEvent" OR
   NOT readme_content MATCHES "WsTransportEndEvent" OR
   NOT readme_content MATCHES "section-5[.]5[.]1" OR
   NOT readme_content MATCHES "section-5[.]5[.]2" OR
   NOT readme_content MATCHES "section-7[.]1[.]5" OR
   NOT readme_content MATCHES "section-7[.]4[.]1" OR
   NOT readme_content MATCHES "WebSocketFrameReadResult" OR
   NOT readme_content MATCHES "WebSocketInboundResult" OR
   NOT readme_content MATCHES "WebSocketProtocolFailure" OR
   NOT readme_content MATCHES "section-5[.]2" OR
   NOT readme_content MATCHES "section-5[.]4" OR
   NOT readme_content MATCHES "WebSocketLifecycleOptions" OR
   NOT readme_content MATCHES "RST_STREAM[(]CANCEL[)]" OR
   NOT agents_content MATCHES "WsClosePhase" OR
   NOT agents_content MATCHES "std::optional<WsEvent>" OR
   NOT agents_content MATCHES "std::nullopt" OR
   NOT agents_content MATCHES "std::variant" OR
   NOT agents_content MATCHES "WsMessageEvent" OR
   NOT agents_content MATCHES "WsCloseEvent" OR
   NOT agents_content MATCHES "WsProtocolErrorEvent" OR
   NOT agents_content MATCHES "WsTransportEndEvent" OR
   NOT agents_content MATCHES "section-5[.]5[.]1" OR
   NOT agents_content MATCHES "section-5[.]5[.]2" OR
   NOT agents_content MATCHES "section-7[.]1[.]5" OR
   NOT agents_content MATCHES "section-7[.]4[.]1" OR
   NOT agents_content MATCHES "WebSocketFrameReadResult" OR
   NOT agents_content MATCHES "WebSocketInboundResult" OR
   NOT agents_content MATCHES "WebSocketProtocolFailure" OR
   NOT agents_content MATCHES "section-5[.]2" OR
   NOT agents_content MATCHES "section-5[.]4" OR
   NOT agents_content MATCHES "close-handshake timeout")
    boundary_error("WebSocket close/liveness boundary is undocumented"
        "README and AGENTS must pin typed frame/reassembly outcomes, input events, protocol-owned close plans, and Web-owned timeout policy")
endif()
if(NOT readme_content MATCHES "HttpFinalResponseControlPlanResult" OR
   NOT readme_content MATCHES "HttpFinalResponseControlPlanFailure" OR
   NOT readme_content MATCHES "HttpFinalResponseControlPlanError" OR
   NOT readme_content MATCHES "Http1FinalResponseControl" OR
   NOT readme_content MATCHES "Http2FinalResponseControl" OR
   NOT readme_content MATCHES "section-8[.]2[.]2" OR
   NOT readme_content MATCHES "silently dropping" OR
   NOT readme_content MATCHES "200[.][.]599" OR
   NOT readme_content MATCHES "HttpInterimResponseHead" OR
   NOT readme_content MATCHES "Http1InterimResponseWriter" OR
   NOT readme_content MATCHES "submitInterimResponseHead" OR
   NOT agents_content MATCHES "HttpFinalResponseControlPlanResult" OR
   NOT agents_content MATCHES "HttpFinalResponseControlPlanFailure" OR
   NOT agents_content MATCHES "HttpFinalResponseControlPlanError" OR
   NOT agents_content MATCHES "Http1FinalResponseControl" OR
   NOT agents_content MATCHES "Http2FinalResponseControl" OR
   NOT agents_content MATCHES "section-8[.]2[.]2" OR
   NOT agents_content MATCHES "静默过滤" OR
   NOT agents_content MATCHES "`200[.][.]599`" OR
   NOT agents_content MATCHES "HttpInterimResponseHead" OR
   NOT agents_content MATCHES "Http1InterimResponseWriter" OR
   NOT agents_content MATCHES "submitInterimResponseHead" OR
   NOT agents_content MATCHES "typed invalid message")
    boundary_error("final response control contract is undocumented"
        "README and AGENTS must pin the discriminated result/protocol alternatives, H1 parsed fields, pre-HPACK HTTP/2 connection-field rejection, status classes, dedicated 101, and version-specific 426 handling")
endif()
if(NOT readme_content MATCHES "Context::ResponseInit" OR
   NOT readme_content MATCHES "httpReasonPhrase[(][)]" OR
   NOT readme_content MATCHES "RFC 9112" OR
   NOT readme_content MATCHES "`:status`" OR
   NOT readme_content MATCHES "HttpErrorInfo::statusText" OR
   NOT agents_content MATCHES "Context::ResponseInit" OR
   NOT agents_content MATCHES "httpReasonPhrase[(][)]" OR
   NOT agents_content MATCHES "HttpStatusEntry" OR
   NOT agents_content MATCHES "RFC 9112" OR
   NOT agents_content MATCHES "`:status`" OR
   NOT agents_content MATCHES "HttpErrorInfo::statusText")
    boundary_error("version-neutral response status contract is undocumented"
        "README and AGENTS must keep reason phrases in HTTP/1 serialization and Web labels out of wire messages")
endif()
if(NOT readme_content MATCHES "HttpRequestExpectations" OR
   NOT readme_content MATCHES "#expectation" OR
   NOT readme_content MATCHES "HttpServerExpectationAction" OR
   NOT readme_content MATCHES "kExpectationFailed" OR
   NOT readme_content MATCHES "submitInterimResponseHead" OR
   NOT agents_content MATCHES "HttpRequestExpectations" OR
   NOT agents_content MATCHES "#expectation" OR
   NOT agents_content MATCHES "HttpServerExpectationAction" OR
   NOT agents_content MATCHES "kExpectationFailed" OR
   NOT agents_content MATCHES "submitInterimResponseHead")
    boundary_error("cross-version server Expect contract is undocumented"
        "README and AGENTS must pin extensible list parsing, Web-owned 417, and H1/H2 typed 100 emission")
endif()
if(NOT readme_content MATCHES "HttpProtocolVersion" OR
   NOT readme_content MATCHES "protocolVersion[(][)]" OR
   NOT readme_content MATCHES "HttpRequest::httpVersion[(][)]" OR
   NOT readme_content MATCHES "HttpResponseProtocolVersion" OR
   NOT agents_content MATCHES "HttpProtocolVersion" OR
   NOT agents_content MATCHES "kHttp10" OR
   NOT agents_content MATCHES "kHttp11" OR
   NOT agents_content MATCHES "kHttp2" OR
   NOT agents_content MATCHES "protocolVersion[(][)]" OR
   NOT agents_content MATCHES "httpVersion[(][)]" OR
   NOT agents_content MATCHES "httpVersion_" OR
   NOT agents_content MATCHES "HttpResponseProtocolVersion")
    boundary_error("typed HTTP protocol-version contract is undocumented"
        "README and AGENTS must pin the sole enum, H1 conversion, H2 connection source, and removed string/parallel state")
endif()
if(NOT readme_content MATCHES "`Server` product identity" OR
   NOT readme_content MATCHES "section-10[.]2[.]4" OR
   NOT readme_content MATCHES "section-6[.]6[.]1" OR
   NOT agents_content MATCHES "`Server` product identity" OR
   NOT agents_content MATCHES "Server: ruvia" OR
   NOT agents_content MATCHES "section-10[.]2[.]4" OR
   NOT agents_content MATCHES "section-6[.]6[.]1")
    boundary_error("explicit Server product policy is undocumented"
        "README and AGENTS must keep Server optional/application-owned while retaining required Date generation")
endif()
if(NOT readme_content MATCHES "VCPKG_ROOT/scripts/buildsystems/vcpkg[.]cmake" OR
   NOT agents_content MATCHES "VCPKG_ROOT/scripts/buildsystems/vcpkg[.]cmake")
    boundary_error("VCPKG_ROOT build guidance is missing"
        "README and AGENTS must derive CMAKE_TOOLCHAIN_FILE from VCPKG_ROOT")
endif()
if(NOT readme_content MATCHES "Each library has its own installed CMake export" OR
   NOT readme_content MATCHES "ruvia_AVAILABLE_COMPONENTS" OR
   NOT readme_content MATCHES "OPTIONAL_COMPONENTS" OR
   NOT agents_content MATCHES "独立安装 export" OR
   NOT agents_content MATCHES "依赖闭包" OR
   NOT agents_content MATCHES "ruvia_AVAILABLE_COMPONENTS")
    boundary_error("component-scoped package loading is undocumented"
        "README and AGENTS must pin independent exports, actual availability, and optional-component semantics")
endif()
if(NOT readme_content MATCHES "MultipartBoundary" OR
   NOT readme_content MATCHES "MultipartChunkPhase" OR
   NOT readme_content MATCHES "HttpMultipartBoundaryParseResult" OR
   NOT readme_content MATCHES "HttpMultipartPartHeaderParseResult" OR
   NOT readme_content MATCHES "HttpMultipartDelimiterResult" OR
   NOT readme_content MATCHES "MultipartPollResult" OR
   NOT readme_content MATCHES "MultipartPollNeedInput" OR
   NOT readme_content MATCHES "MultipartPollDone" OR
   NOT readme_content MATCHES "MultipartPollFailure" OR
   NOT readme_content MATCHES "MultipartParseError" OR
   NOT readme_content MATCHES "HttpProtocolError" OR
   NOT readme_content MATCHES "finishInput[(][)]" OR
   NOT readme_content MATCHES "rfc2046[.]html#section-5[.]1[.]1" OR
   NOT readme_content MATCHES "rfc7578[.]html#section-4[.]1" OR
   NOT agents_content MATCHES "MultipartBoundary" OR
   NOT agents_content MATCHES "MultipartChunkPhase" OR
   NOT agents_content MATCHES "HttpMultipartBoundaryParseResult" OR
   NOT agents_content MATCHES "HttpMultipartPartHeaderParseResult" OR
   NOT agents_content MATCHES "HttpMultipartDelimiterResult" OR
   NOT agents_content MATCHES "MultipartPollResult" OR
   NOT agents_content MATCHES "MultipartPollNeedInput" OR
   NOT agents_content MATCHES "MultipartPollDone" OR
   NOT agents_content MATCHES "MultipartPollFailure" OR
   NOT agents_content MATCHES "MultipartParseError" OR
   NOT agents_content MATCHES "HttpProtocolError" OR
   NOT agents_content MATCHES "finishInput[(][)]" OR
   NOT agents_content MATCHES "rfc2046[.]html#section-5[.]1[.]1" OR
   NOT agents_content MATCHES "rfc7578[.]html#section-4[.]1")
    boundary_error("multipart boundary/input lifecycle is undocumented"
        "README and AGENTS must pin discriminated boundary/header/delimiter/poll results, chunk phase, RFC grammar, and explicit EOF")
endif()
if(NOT readme_content MATCHES "HttpAuthorityView" OR
   NOT readme_content MATCHES "IPvFuture" OR
   NOT readme_content MATCHES "absent/empty" OR
   NOT readme_content MATCHES "decodes only percent-encoded unreserved" OR
   NOT readme_content MATCHES "Encoded reserved characters remain distinct" OR
   NOT readme_content MATCHES "malformed or userinfo-bearing" OR
   NOT readme_content MATCHES "rvalue string factories are deleted" OR
   NOT agents_content MATCHES "HttpAuthorityView" OR
   NOT agents_content MATCHES "IPvFuture" OR
   NOT agents_content MATCHES "absent/empty/numeric" OR
   NOT agents_content MATCHES "percent-encoded reserved" OR
   NOT agents_content MATCHES "malformed/userinfo" OR
   NOT agents_content MATCHES "rvalue `basic_string`")
    boundary_error("HTTP authority/origin normalization is undocumented"
        "README and AGENTS must pin typed port states, IP-literal grammar, and shared comparison")
endif()
if(NOT readme_content MATCHES "HttpClientRedirect[.]h" OR
   NOT readme_content MATCHES "HttpClientResponseHeaderLookupResult" OR
   NOT readme_content MATCHES "HttpClientResponseHeaderAbsent" OR
   NOT readme_content MATCHES "HttpClientResponseHeaderFound" OR
   NOT readme_content MATCHES "HttpClientResponseHeaderRepeated" OR
   NOT readme_content MATCHES "HttpClientRedirectTargetResult" OR
   NOT readme_content MATCHES "HttpClientRedirectTargetFailure" OR
   NOT readme_content MATCHES "HttpClientRedirectTargetError" OR
   NOT readme_content MATCHES "rfc9110[.]html#section-10[.]2[.]2" OR
   NOT readme_content MATCHES "rfc3986[.]html#section-5[.]2" OR
   NOT readme_content MATCHES "rfc3986[.]html#section-5[.]3" OR
   NOT agents_content MATCHES "HttpClientRedirect[.]h" OR
   NOT agents_content MATCHES "HttpClientResponseHeaderLookupResult" OR
   NOT agents_content MATCHES "HttpClientResponseHeaderAbsent" OR
   NOT agents_content MATCHES "HttpClientResponseHeaderFound" OR
   NOT agents_content MATCHES "HttpClientResponseHeaderRepeated" OR
   NOT agents_content MATCHES "HttpClientRedirectTargetResult" OR
   NOT agents_content MATCHES "HttpClientRedirectTargetFailure" OR
   NOT agents_content MATCHES "HttpClientRedirectTargetError" OR
   NOT agents_content MATCHES "rfc9110[.]html#section-10[.]2[.]2" OR
   NOT agents_content MATCHES "rfc3986[.]html#section-5[.]2" OR
   NOT agents_content MATCHES "rfc3986[.]html#section-5[.]3")
    boundary_error("public redirect result ownership is undocumented"
        "README and AGENTS must pin the public header, discriminated field/target results, PMR ownership, and RFC reference resolution")
endif()
if(NOT readme_content MATCHES "Http1RequestParser" OR
   NOT readme_content MATCHES "Http1RequestNeedMore" OR
   NOT readme_content MATCHES "Http1ParsedRequest" OR
   NOT readme_content MATCHES "Http1RequestParseFailure" OR
   NOT readme_content MATCHES "wireBody" OR
   NOT agents_content MATCHES "Http1RequestParser" OR
   NOT agents_content MATCHES "requiredTotalBytes" OR
   NOT agents_content MATCHES "wireBody")
    boundary_error("public HTTP/1 discriminated parse contract is undocumented"
        "README and AGENTS must distinguish input sizing, successful framing bytes, and protocol failure")
endif()

get_property(boundary_failed GLOBAL PROPERTY RUVIA_BOUNDARY_FAILED)
if(boundary_failed)
    message(FATAL_ERROR "Ruvia layer-boundary checks failed")
endif()
message(STATUS "layer boundaries OK")
