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
set(RULE_STALE_WS_SERVER_NEGOTIATION
    "bool[ \t]*&[ \t]*permessageDeflate|bool[ \t]+permessageDeflate|[.]permessageDeflate|[.]enabled|echoServerMaxWindowBits|http2ChooseWebSocketSubprotocol|submitWebSocketHandshake[ \t\r\n]*[(][^)]*std::string_view|http2EncodeWebSocketHandshakeHeaders[ \t\r\n]*[(][^)]*std::string_view|webSocketNegotiatePermessageDeflate[ \t]*[(]|chooseWebSocketSubprotocol[ \t]*[(]|webSocketDeflateResponseExtensions[ \t]*[(]")
set(RULE_STALE_WS_DEFLATE_PRODUCT
    "bool[ \t]+(enabled|echoServerMaxWindowBits)")
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
set(RULE_STALE_ACCESS_LOG_PROTOCOL_SPLIT
    "bool[ \t]+http2|http2_|[.]http2[ \t]*[(]|method_|knownMethod_|path_")
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
set(RULE_STALE_HTTP_CONTENT_DECODE_CHAIN
    "decodeRequestContentEncoding|inline[ \t]+void[ \t\r\n]+decodeHttpClientResponseContentEncoding|kMaxDecodedRequestBodyBytes|zlibInflateRequestBody|brotliInflateRequestBody|zstdInflateRequestBody|StreamingContentDecoder|HttpStreamingDecoder[.]h")
set(RULE_STALE_HTTP_CONTENT_ENCODE_CHAIN
    "bool[ \t\r\n]+encodeHttpContent|encodeHttpContent[ \t\r\n]*[(][^)]*std::pmr::string[ \t]*&|compressResponseBodyIfAccepted|bodyBorrowsCompressionScratch|compressionScratch|response[.]setBodyView[ \t]*[(]")
set(RULE_STALE_STATIC_FILE_REPRESENTATION_SPLIT
    "borrowNativePath|selectStaticEncodingVariant|std::string_view[ \t]*&[ \t]*contentEncoding|struct[ \t]+StaticRootEntryView|[.]found[(][)]")
set(RULE_STALE_URL_DECODE_CHAIN
    "decodeUrlComponentToString|decodeFormComponent|StringT[ \t]*&[ \t]*output")
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
set(RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT
    "BodyKind|bodyKind_|fileBody_|responseHasFileBody|responseFileBody|responseBodyBytes|responseBodySize")
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
set(RULE_HTTP2_WEB_RUNTIME_IN_CORE
    "#[ 	]*include[ 	]*[<\"]coroutine|std::coroutine_handle|HttpRequestBodyMode|Http2StreamBody(Policy|Queue)|Http2ConnectionLimits|deferStreamWindowRelease|releaseStreamWindow|responseCompressionScratch|requestBody(View|Size|Empty)|enqueueBufferedRequestBodyChunk|queuedBodyBytes")
set(RULE_HTTP2_PARALLEL_WEB_DISPATCH_STATE
    "streamSignals|make_unique<Http2SansIoStreamSignal>|std::vector[<]std::pair[<]std::uint32_t|inFlight")
set(RULE_STALE_ROUTE_MODE_SPLIT
    "ResponseBodyMode|RouteDisposition|registerStreamRoute|responseStreamKindForRouteMode|HttpResponseStreamKindAdapter|routeScratch")
set(RULE_STALE_ROUTE_RESOLUTION_TUPLE
    "RouteResolution::found(Static|Dynamic)|resolution[.](found|route|match|allowedMethods)[ \t]*[(]|resolve[ \t\r\n]*[(][^)]*RouteMatch[ \t]*&")
set(RULE_STALE_REQUEST_DISPATCHER
    "RequestDispatcher|virtual[ \t\r\n]+(RouteResolution|Task[ \t]*[<])|virtual[ \t\r\n]+~")
set(RULE_STALE_CONTEXT_CAPABILITY_SPLIT
    "withBodyReader|withBodyLoader|BodyReader[ \t]*[*][ \t]*bodyReader_|RequestBodyLoader[ \t]*[*][ \t]*bodyLoader_|WebSocket[ \t]*[*][ \t]*webSocket_|ResponseStreamWriter[ \t]*[*][ \t]*responseStream_")
set(RULE_STALE_CONN_INFO_SCALARS
    "withTransport[ \t\r\n]*[(]|routeServices[ \t\r\n]*[(]|bool[ \t]+secure|secure_|remoteAddress_|clientCertificateSubject_|clientCertificate_|kTlsStream")
set(RULE_STALE_HTTP2_BODY_MODE_SPLIT
    "RequestBodyMode[ \t]+mode_[ \t]*[{]|bool[ \t]+modeSelected_|body[(][)][.]selectMode")
set(RULE_STALE_HTTP2_SESSION_ENV
    "Http2SansIoSessionEnv|kDefaultOptions|localScannerEntry|env[.](databases|redis|rateLimiter|options|scannerEntry|clientCertificate|serverStarted)|Http2SansIoSessionContext[ \t\r\n]+session[ \t\r\n]*=[ \t\r\n]*[{]|const[ \t]+std::atomic_bool[*][ \t]+serverStarted[ \t]*=[ \t]*nullptr")
set(RULE_ROUTER_CONNECTION_POLICY
    "closeConnection(OnError)?|\"Connection\"[ \t]*,[ \t]*\"close\"")
set(RULE_STALE_ERROR_API
    "ruvia/http/Error\.h|defaultStatusText|makeErrorResponse")
set(RULE_CORE_PROTOCOL "ruvia/http/|Http[A-Z]|WebSocket|websocket")
set(RULE_STALE_POOL_WAITER_TUPLE
    "bool[ \t]*&[ \t]*(ready|timedOut)|bool[*][ \t]*(ready_|timedOut_)|std::size_t[*][ \t]+index_|sentinelIndex|PoolWaiter[ \t\r\n]*[(][^)]*ready|WaiterAwaiter|setHandle[ \t]*[(]|[.]bind[ \t]*[(]|PoolWaiterResult[*][ \t]+result[ \t]*[(]|closeAll[ \t\r\n]*[(][^)]*(slots_|connections_|sentinel|std::size_t)")
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
set(RULE_STALE_H2_RESPONSE_TRAILER_STAGING
    "submitResponseTrailerSection|Http2ResponseTrailerSubmitStatus|responseTrailerBlock|responseTrailers[ 	]*[(]")
set(RULE_IMPLICIT_H2_RESPONSE_FINISH
    "finishResponse[ 	\r\n]*[(][ 	\r\n]*[A-Za-z0-9_]+[ 	\r\n]*[)]")
set(RULE_STALE_RESPONSE_STREAM_COMMIT_BOOL
    "markCommitted[ 	]*\\([ 	]*(true|false)|bodySuppressed_")
set(RULE_STALE_RESPONSE_STREAM_STATUS_SPLIT
    "RouteStreamDispatchOutcome|responseStreamDispatched|trailerFraming_|ResponseStreamDispatchResult::(streamed|abortedByPeer|abortedAfterCommit)[ 	]*\\([ 	]*HttpResponse|result[.](streamed|abortedByPeer|abortedAfterCommit|hasBufferedResponse|takeResponse)[ 	]*\\(")
set(RULE_STALE_NEXT_CONTINUATION_TYPE_ERASURE
    "const void[ 	]*[*][ 	]+(table|route)|void[ 	]*[*][ 	]+outcome|[.]outcome[ 	]*=|state[.]outcome|StreamMiddlewareDisposition")
set(RULE_STALE_HTTP1_SESSION_COMPLETION
    "HttpResponseStream(RouteResult|BufferedRoute|CommittedRoute)|enum class[ 	]+HttpWebSocketRouteResult|HttpWebSocketRouteResult::k(WriteBufferedResponse|SessionFinished)|committedStreamStatus|bufferAlreadyCompacted|Task<void>[ 	\r\n]+dispatchHttp(Buffered|Stream)BodyRoute|std::size_t&[ 	]+consumedBytes|Http1ServerConnectionPlan&[ 	]+connectionPlan")
set(RULE_STALE_HTTP1_REQUEST_SEQUENCE_SCALARS
    "nextHttp1ResponseClosePolicy|applyRequestLimit|requestLimitReached|std::size_t[ 	]*&[ 	]*requestCount|std::size_t[ 	]+requestCount[ 	]*=|[+][+][ 	]*requestCount|std::size_t[ 	]+keepaliveRequests[ 	]*[,)]")
set(RULE_LATE_RESPONSE_STREAM_END
    "co_await[ 	]+responseStream[.]end|HttpResponse[ 	]+response[ 	]*[(][ 	]*requestMemory[.]resource[(][)]")
set(RULE_LOOSE_BUFFERED_RESPONSE_PLAN
    "httpBufferedResponseWritePlan[ \t\r\n]*[(][ \t\r\n]*(const[ \t]+HttpResponseBodyPlan[&]|bodyPlan[ \t\r\n]*,|[A-Za-z_][A-Za-z0-9_]*BodyPlan[ \t\r\n]*,)")
set(RULE_STALE_H2_BUFFERED_COMPLETION
    "response[.]status[(][)][ \t\r\n]*,[ \t\r\n]*requestStart|submitResponse[ \t]*=[^\n]*Task<void>")
set(RULE_STALE_H2_UNPREPARED_BUFFERED_HEAD
    "[(]void[)][ \t]*prepareBufferedHttpResponse|auto[ \t]+writePlan[ \t]*=[ \t\r\n]*httpBufferedResponseWritePlan|submitResponseHead[ \t\r\n]*[(][ \t\r\n]*std::uint32_t[ \t]+streamId[ \t]*,[ \t\r\n]*const HttpResponse&[ \t]+response[ \t\r\n]*[)]")
set(RULE_STALE_H1_BUFFERED_COMPLETION
    "Task<void>[ \t\r\n]+writeResponse(WithScratch|WithLocalHead)?[ \t\r\n]*[(]|writeResponse(WithScratch|WithLocalHead)?[ \t\r\n]*[(][^)]*std::error_code[ \t]*&|response[.]status[(][)][ \t\r\n]*,[ \t\r\n]*requestStart")
set(RULE_STALE_HTTP_FILE_WRITE_COMPLETION
    "Task<void>[ \t\r\n]+writeFile(ZeroCopy|Fallback|FallbackWithLocalChunk|Chunk)[ \t\r\n]*[(]|writeFile(ZeroCopy|Fallback|FallbackWithLocalChunk|Chunk)[ \t\r\n]*[(][^)]*std::error_code[ \t]*&|operation_not_supported")
set(RULE_STALE_DB_MIGRATION_REPORT_SIDE_CHANNEL
    "Task<void>[ \t\r\n]+run[ \t\r\n]*[(][^)]*DbMigrationReport[ \t]*&")
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
    expect_match("split WebSocket server negotiation"
        "${RULE_STALE_WS_SERVER_NEGOTIATION}"
        "bool& permessageDeflate")
    expect_match("WebSocket deflate boolean product"
        "${RULE_STALE_WS_DEFLATE_PRODUCT}"
        "bool echoServerMaxWindowBits = false;")
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
    expect_match("request-only bool/out-parameter content decoder"
        "${RULE_STALE_HTTP_CONTENT_DECODE_CHAIN}"
        "decodeRequestContentEncoding(coding, input, output, limit)")
    expect_match("duplicate streaming content decoder"
        "${RULE_STALE_HTTP_CONTENT_DECODE_CHAIN}"
        "class StreamingContentDecoder final {};")
    expect_match("in-place client response content decoding"
        "${RULE_STALE_HTTP_CONTENT_DECODE_CHAIN}"
        "inline void decodeHttpClientResponseContentEncoding() {}")
    expect_match("bool/out-parameter HTTP content encoder"
        "${RULE_STALE_HTTP_CONTENT_ENCODE_CHAIN}"
        "bool encodeHttpContent(Coding, View, std::pmr::string& output, std::size_t limit);")
    expect_match("response compression scratch lifetime side channel"
        "${RULE_STALE_HTTP_CONTENT_ENCODE_CHAIN}"
        "if (preparation.bodyBorrowsCompressionScratch()) compressionScratch.clear();")
    expect_match("compressed response body borrowing external storage"
        "${RULE_STALE_HTTP_CONTENT_ENCODE_CHAIN}"
        "response.setBodyView(encodedBytes);")
    expect_match("file response path lifetime stored as a boolean tuple"
        "${RULE_STALE_STATIC_FILE_REPRESENTATION_SPLIT}"
        "struct FileResponsePath { Path* path; Char* nativePath; bool borrowNativePath; };")
    expect_match("static encoding selection returned split output parameters"
        "${RULE_STALE_STATIC_FILE_REPRESENTATION_SPLIT}"
        "bool selectStaticEncodingVariant(View, Entry& variant, std::string_view& contentEncoding);")
    expect_match("static index encoded absence inside a default view"
        "${RULE_STALE_STATIC_FILE_REPRESENTATION_SPLIT}"
        "struct StaticRootEntryView { bool found() const; };")
    expect_match("URL decoding returned bool plus partial output"
        "${RULE_STALE_URL_DECODE_CHAIN}"
        "bool decodeUrlComponent(View input, StringT& output, Mode mode);")
    expect_match("form decoding duplicated the URL decoder"
        "${RULE_STALE_URL_DECODE_CHAIN}"
        "bool decodeFormComponent(View input, StringT& output);")
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
    expect_match("split HttpResponse body storage"
        "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}"
        "BodyKind bodyKind_; std::optional<FileBody> fileBody_;")
    expect_match("two-stage HttpResponse file-body read"
        "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}"
        "if (responseHasFileBody(response)) use(responseFileBody(response));")
    expect_match("split connection transport metadata"
        "${RULE_STALE_CONN_INFO_SCALARS}"
        "std::string_view clientCertificateSubject_; bool secure_;")
    expect_match("boolean connection transport refinement"
        "${RULE_STALE_CONN_INFO_SCALARS}"
        "services.withTransport(remote, certificate, secure);")
    expect_match("access log protocol boolean"
        "${RULE_STALE_ACCESS_LOG_PROTOCOL_SPLIT}"
        "bool http2_; bool http2() const;")
    expect_match("access log copied request tuple"
        "${RULE_STALE_ACCESS_LOG_PROTOCOL_SPLIT}"
        "std::string_view method_; HttpKnownMethod knownMethod_; std::string_view path_;")
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
    expect_match("Web request-body runtime leaked into HTTP/2 core"
        "${RULE_HTTP2_WEB_RUNTIME_IN_CORE}"
        "std::coroutine_handle<> bodyWaiter; HttpRequestBodyMode bodyMode;")
    expect_match("parallel HTTP/2 Web dispatch ownership"
        "${RULE_HTTP2_PARALLEL_WEB_DISPATCH_STATE}"
        "std::vector streamSignals; int inFlight;")
    expect_match("split route handler/mode contract"
        "${RULE_STALE_ROUTE_MODE_SPLIT}"
        "registerStreamRoute(handler, ResponseBodyMode::kWebSocket);")
    expect_match("route-resolution status/payload side channel"
        "${RULE_STALE_ROUTE_RESOLUTION_TUPLE}"
        "if (resolution.found()) use(resolution.route());")
    expect_match("request-time virtual route dispatcher"
        "${RULE_STALE_REQUEST_DISPATCHER}"
        "class RequestDispatcher { virtual RouteResolution resolve() = 0; };")
    expect_match("manual Context body-capability refinement"
        "${RULE_STALE_CONTEXT_CAPABILITY_SPLIT}"
        "services.withBodyReader(reader);")
    expect_match("parallel nullable Context output slots"
        "${RULE_STALE_CONTEXT_CAPABILITY_SPLIT}"
        "WebSocket* webSocket_; ResponseStreamWriter* responseStream_;")
    expect_match("HTTP/2 default body mode plus selection flag"
        "${RULE_STALE_HTTP2_BODY_MODE_SPLIT}"
        "RequestBodyMode mode_{RequestBodyMode::kBuffered}; bool modeSelected_; ")
    expect_match("nullable/default HTTP/2 session environment"
        "${RULE_STALE_HTTP2_SESSION_ENV}"
        "Http2SansIoSessionEnv env = {}; static HttpServerOptions kDefaultOptions;")
    expect_match("connection policy in Router" "${RULE_ROUTER_CONNECTION_POLICY}"
        "bool closeConnectionOnError")
    expect_match("removed mixed-layer error API" "${RULE_STALE_ERROR_API}"
        "#include \"ruvia/http/Error.h\"")
    expect_match("protocol semantics in core" "${RULE_CORE_PROTOCOL}"
        "#include \"ruvia/http/HttpParser.h\"")
    expect_match("split pool waiter completion tuple"
        "${RULE_STALE_POOL_WAITER_TUPLE}"
        "PoolWaiter(bool& ready, bool& timedOut, std::size_t& index);")
    expect_match("parallel pool waiter result accessor"
        "${RULE_STALE_POOL_WAITER_TUPLE}"
        "const PoolWaiterResult* result() const noexcept;")
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
    expect_match("staged HTTP/2 response trailer section"
        "${RULE_STALE_H2_RESPONSE_TRAILER_STAGING}"
        "connection.submitResponseTrailerSection(streamId, trailers);")
    expect_match("implicit HTTP/2 response finish without terminal section"
        "${RULE_IMPLICIT_H2_RESPONSE_FINISH}"
        "connection.finishResponse(streamId);")
    expect_match("stale response-stream commit boolean"
        "${RULE_STALE_RESPONSE_STREAM_COMMIT_BOOL}"
        "state.markCommitted(true);")
    expect_match("response stream reconstructed status from a dummy response"
        "${RULE_STALE_RESPONSE_STREAM_STATUS_SPLIT}"
        "if (result.streamed()) record(response.status());")
    expect_match("response stream route outcome/payload tuple"
        "${RULE_STALE_RESPONSE_STREAM_STATUS_SPLIT}"
        "RouteStreamDispatchOutcome outcome;")
    expect_match("Next restored an untyped stream outcome pointer"
        "${RULE_STALE_NEXT_CONTINUATION_TYPE_ERASURE}"
        "void* outcome;")
    expect_match("Next restored an untyped route-owner pointer"
        "${RULE_STALE_NEXT_CONTINUATION_TYPE_ERASURE}"
        "const void* table;")
    expect_match("HTTP/1 session parallel completion tuple"
        "${RULE_STALE_HTTP1_SESSION_COMPLETION}"
        "std::optional<std::uint16_t> committedStreamStatus;")
    expect_match("HTTP/1 body route completion out-parameters"
        "${RULE_STALE_HTTP1_SESSION_COMPLETION}"
        "Task<void> dispatchHttpBufferedBodyRoute(std::size_t& consumedBytes);")
    expect_match("HTTP/1 request limit split count and maximum"
        "${RULE_STALE_HTTP1_REQUEST_SEQUENCE_SCALARS}"
        "void complete(std::size_t& requestCount, std::size_t keepaliveRequests);")
    expect_match("HTTP/1 request completion incremented outside its owner"
        "${RULE_STALE_HTTP1_REQUEST_SEQUENCE_SCALARS}"
        "++requestCount;")
    expect_match("response stream ended after its bound Context lifetime"
        "${RULE_LATE_RESPONSE_STREAM_END}"
        "co_await responseStream.end();")
    expect_match("handled response stream constructed a dummy response"
        "${RULE_LATE_RESPONSE_STREAM_END}"
        "HttpResponse response(requestMemory.resource());")
    expect_match("buffered response accepted a loose body plan"
        "${RULE_LOOSE_BUFFERED_RESPONSE_PLAN}"
        "httpBufferedResponseWritePlan(bodyPlan, response);")
    expect_match("HTTP/2 buffered access log reconstructed response status"
        "${RULE_STALE_H2_BUFFERED_COMPLETION}"
        "recordHttpAccess(log, request, remote, response.status(), requestStart);")
    expect_match("HTTP/2 buffered response discarded its prepared plan"
        "${RULE_STALE_H2_UNPREPARED_BUFFERED_HEAD}"
        "(void)prepareBufferedHttpResponse(request, response, options, scratch);")
    expect_match("HTTP/2 core restored hidden buffered response planning"
        "${RULE_STALE_H2_UNPREPARED_BUFFERED_HEAD}"
        "auto writePlan = httpBufferedResponseWritePlan(stream->requestKnownMethod(), response);")
    expect_match("HTTP/1 buffered writer restored void plus error side channel"
        "${RULE_STALE_H1_BUFFERED_COMPLETION}"
        "Task<void> writeResponse(Stream&, std::error_code& ec);")
    expect_match("HTTP file writer restored void plus error side channel"
        "${RULE_STALE_HTTP_FILE_WRITE_COMPLETION}"
        "Task<void> writeFileZeroCopy(Socket&, File, std::error_code& ec);")
    expect_match("database migration restored a report output parameter"
        "${RULE_STALE_DB_MIGRATION_REPORT_SIDE_CHANNEL}"
        "Task<void> run(Config, DbMigrationReport& report);")
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
check_files_no_match("ruvia-http HTTP/2 core must not own Web request-body runtime"
    "${RULE_HTTP2_WEB_RUNTIME_IN_CORE}" ${HTTP_SOURCE})
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

set(WEB_ROUTE_MODES
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/RouteModes.h")
set(WEB_ROUTE_RESOLUTION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteResolution.h")
set(WEB_ROUTE_TABLE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteTable.h")
set(WEB_STALE_REQUEST_DISPATCHER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RequestDispatcher.h")
set(WEB_CONTROLLER_MACROS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Controller.h")
set(WEB_ROUTE_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_ROUTE_RESPONSE_STREAM_DISPATCH
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h")
set(WEB_ROUTE_WEBSOCKET_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketSession.h")
set(WEB_STALE_STREAM_KIND_ADAPTER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamKindAdapter.h")
set(WEB_ROUTE_RESOLUTION_TEST
    "${RUVIA_ROOT}/tests/unit_route_resolution.cpp")
foreach(route_contract_file IN ITEMS
        "${WEB_ROUTE_MODES}"
        "${WEB_ROUTE_RESOLUTION}"
        "${WEB_ROUTE_TABLE}"
        "${WEB_CONTROLLER_MACROS}"
        "${WEB_ROUTE_HTTP2_SESSION}"
        "${WEB_ROUTE_RESPONSE_STREAM_DISPATCH}"
        "${WEB_ROUTE_WEBSOCKET_SESSION}"
        "${WEB_ROUTE_RESOLUTION_TEST}")
    if(NOT EXISTS "${route_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${route_contract_file}")
        boundary_error("typed Web route contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_STALE_STREAM_KIND_ADAPTER}")
    boundary_error("split response route-mode adapter was restored"
        "stream sinks must consume the ResponseStreamKind owned by the typed endpoint")
endif()
if(EXISTS "${WEB_STALE_REQUEST_DISPATCHER}")
    boundary_error("request-time virtual route dispatcher was restored"
        "HTTP/1, HTTP/2, streaming, and WebSocket dispatch must call the concrete RouteTable directly")
endif()
if(EXISTS "${WEB_ROUTE_MODES}" AND EXISTS "${WEB_ROUTE_RESOLUTION}" AND
   EXISTS "${WEB_ROUTE_TABLE}" AND
   EXISTS "${WEB_CONTROLLER_MACROS}" AND EXISTS "${WEB_ROUTE_HTTP2_SESSION}" AND
   EXISTS "${WEB_ROUTE_RESPONSE_STREAM_DISPATCH}" AND
   EXISTS "${WEB_ROUTE_WEBSOCKET_SESSION}" AND
   EXISTS "${WEB_ROUTE_RESOLUTION_TEST}")
    file(READ "${WEB_ROUTE_MODES}" web_route_modes)
    file(READ "${WEB_ROUTE_RESOLUTION}" web_route_resolution)
    file(READ "${WEB_ROUTE_TABLE}" web_route_table)
    file(READ "${WEB_CONTROLLER_MACROS}" web_controller_macros)
    file(READ "${WEB_ROUTE_HTTP2_SESSION}" web_route_http2_session)
    file(READ "${WEB_ROUTE_RESPONSE_STREAM_DISPATCH}"
        web_route_response_stream_dispatch)
    file(READ "${WEB_ROUTE_WEBSOCKET_SESSION}"
        web_route_websocket_session)
    file(READ "${WEB_ROUTE_RESOLUTION_TEST}" web_route_resolution_test)
    if(web_route_modes MATCHES "ResponseBodyMode" OR
       web_route_resolution MATCHES "RouteDisposition" OR
       web_route_table MATCHES "ResponseBodyMode" OR
       web_route_table MATCHES
           "resolve[ \t\r\n]*[(][^)]*RouteMatch[ \t]*&")
        boundary_error("Web routing restored handler/mode or caller-scratch split state"
            "endpoint kind, handler shape, route match, and resolution outcome must each have one owner")
    endif()
    if(NOT web_route_resolution MATCHES "class RouteNotFound final" OR
       NOT web_route_resolution MATCHES
           "class RouteMethodNotAllowed final" OR
       NOT web_route_resolution MATCHES "class ResolvedRoute final" OR
       NOT web_route_resolution MATCHES "using Value = std::variant" OR
       NOT web_route_resolution MATCHES "std::get_if<ResolvedRoute>" OR
       NOT web_route_resolution MATCHES
           "std::get_if<RouteMethodNotAllowed>" OR
       NOT web_route_resolution MATCHES "std::get_if<RouteNotFound>")
        boundary_error("RouteResolution lost its exclusive result alternatives"
            "resolved, 405, and 404 payloads must not be readable through a shared tuple")
    endif()
    if(NOT web_route_table MATCHES
           "class BufferedRouteEndpoint final" OR
       NOT web_route_table MATCHES
           "class ResponseStreamRouteEndpoint final" OR
       NOT web_route_table MATCHES
           "class WebSocketRouteEndpoint final" OR
       NOT web_route_table MATCHES "class RouteEndpoint final" OR
       NOT web_route_table MATCHES
           "std::get_if<BufferedRouteEndpoint>" OR
       NOT web_route_table MATCHES
           "std::get_if<ResponseStreamRouteEndpoint>" OR
       NOT web_route_table MATCHES
           "std::get_if<WebSocketRouteEndpoint>" OR
       NOT web_route_table MATCHES "RouteEndpoint endpoint_" OR
       NOT web_route_table MATCHES "const ResolvedRoute& route")
        boundary_error("route endpoint lost its discriminated handler contract"
            "buffered, response-stream, and WebSocket routes must bind handler and metadata in one alternative")
    endif()
    if(NOT web_route_table MATCHES "class RouteTable final[ \t\r\n]*[{]" OR
       web_route_table MATCHES "${RULE_STALE_REQUEST_DISPATCHER}")
        boundary_error("RouteTable lost its concrete request-time dispatch contract"
            "the startup-frozen route table must not inherit or expose a virtual dispatch interface")
    endif()
    if(NOT web_route_http2_session MATCHES "const RouteTable& routes" OR
       NOT web_route_response_stream_dispatch MATCHES
           "const RouteTable& routes" OR
       NOT web_route_websocket_session MATCHES "const RouteTable& routes")
        boundary_error("Web runtime bypasses the concrete RouteTable dispatch chain"
            "HTTP/2, response streaming, and WebSocket sessions must receive the startup-frozen RouteTable directly")
    endif()
    if(NOT web_controller_macros MATCHES
           "ruviaAddResponseStreamRoute" OR
       NOT web_controller_macros MATCHES "ruviaAddSseRoute" OR
       NOT web_controller_macros MATCHES "ruviaAddWebSocketRoute" OR
       NOT web_route_resolution_test MATCHES
           "route_endpoint_binds_handler_shape_and_only_relevant_metadata" OR
       NOT web_route_resolution_test MATCHES
           "route_endpoint_rejects_empty_handlers_and_invalid_discriminants" OR
       NOT web_route_resolution_test MATCHES
           "route_resolution_method_not_allowed_vs_not_found")
        boundary_error("typed route contract lacks registration or regression coverage"
            "distinct macro paths and value-level endpoint/resolution tests must remain")
    endif()
endif()
check_files_no_match("Web routing restored split endpoint or resolution APIs"
    "${RULE_STALE_ROUTE_MODE_SPLIT}|${RULE_STALE_ROUTE_RESOLUTION_TUPLE}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Controller.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ControllerDescriptors.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ControllerRuntime.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouterInternal.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterBuild.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterIndex.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterRegistration.cpp")
check_files_no_match("Web routing restored request-time virtual dispatch"
    "${RULE_STALE_REQUEST_DISPATCHER}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteTable.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketSession.h")

set(WEB_CONTEXT_CAPABILITIES
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextCapabilities.h")
set(WEB_CONTEXT_SERVICES
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextServices.h")
set(WEB_CONTEXT_INTERNAL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextInternal.h")
set(WEB_CONTEXT_CAPABILITY_CONTEXT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h")
set(WEB_CONTEXT_REQUEST_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
set(WEB_CONTEXT_ROUTER_DISPATCH
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp")
set(WEB_CONTEXT_LAZY_BODY_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h")
set(WEB_CONTEXT_STREAM_BODY_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h")
set(WEB_CONTEXT_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_CONTEXT_CAPABILITY_TEST
    "${RUVIA_ROOT}/tests/unit_context_capabilities.cpp")
set(WEB_CONTEXT_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
foreach(context_capability_file IN ITEMS
        "${WEB_CONTEXT_CAPABILITIES}"
        "${WEB_CONTEXT_SERVICES}"
        "${WEB_CONTEXT_INTERNAL}"
        "${WEB_CONTEXT_CAPABILITY_CONTEXT}"
        "${WEB_CONTEXT_REQUEST_SOURCE}"
        "${WEB_CONTEXT_ROUTER_DISPATCH}"
        "${WEB_CONTEXT_LAZY_BODY_ROUTE}"
        "${WEB_CONTEXT_STREAM_BODY_ROUTE}"
        "${WEB_CONTEXT_HTTP2_SESSION}"
        "${WEB_CONTEXT_CAPABILITY_TEST}"
        "${WEB_CONTEXT_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${context_capability_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${context_capability_file}")
        boundary_error("typed Context capability contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_CONTEXT_CAPABILITIES}" AND
   EXISTS "${WEB_CONTEXT_SERVICES}" AND
   EXISTS "${WEB_CONTEXT_INTERNAL}" AND
   EXISTS "${WEB_CONTEXT_CAPABILITY_CONTEXT}" AND
   EXISTS "${WEB_CONTEXT_REQUEST_SOURCE}" AND
   EXISTS "${WEB_CONTEXT_ROUTER_DISPATCH}" AND
   EXISTS "${WEB_CONTEXT_LAZY_BODY_ROUTE}" AND
   EXISTS "${WEB_CONTEXT_STREAM_BODY_ROUTE}" AND
   EXISTS "${WEB_CONTEXT_HTTP2_SESSION}" AND
   EXISTS "${WEB_CONTEXT_CAPABILITY_TEST}" AND
   EXISTS "${WEB_CONTEXT_PACKAGE_CONSUMER}")
    file(READ "${WEB_CONTEXT_CAPABILITIES}" web_context_capabilities)
    file(READ "${WEB_CONTEXT_SERVICES}" web_context_services)
    file(READ "${WEB_CONTEXT_INTERNAL}" web_context_internal)
    file(READ "${WEB_CONTEXT_CAPABILITY_CONTEXT}" web_context_header)
    file(READ "${WEB_CONTEXT_REQUEST_SOURCE}" web_context_request_source)
    file(READ "${WEB_CONTEXT_ROUTER_DISPATCH}" web_context_router_dispatch)
    file(READ "${WEB_CONTEXT_LAZY_BODY_ROUTE}" web_context_lazy_body_route)
    file(READ "${WEB_CONTEXT_STREAM_BODY_ROUTE}" web_context_stream_body_route)
    file(READ "${WEB_CONTEXT_HTTP2_SESSION}" web_context_http2_session)
    file(READ "${WEB_CONTEXT_CAPABILITY_TEST}" web_context_capability_test)
    file(READ "${WEB_CONTEXT_PACKAGE_CONSUMER}" web_context_package_consumer)
    if(NOT web_context_capabilities MATCHES
           "class ContextBufferedRequestBodySource final" OR
       NOT web_context_capabilities MATCHES
           "class ContextLazyRequestBodySource final" OR
       NOT web_context_capabilities MATCHES
           "class ContextStreamingRequestBodySource final" OR
       NOT web_context_capabilities MATCHES
           "class ContextRequestBodySource final" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextBufferedRequestBodySource>" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextLazyRequestBodySource>" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextStreamingRequestBodySource>" OR
       NOT web_context_capabilities MATCHES
           "class ContextBufferedResponseOutput final" OR
       NOT web_context_capabilities MATCHES
           "class ContextResponseStreamOutput final" OR
       NOT web_context_capabilities MATCHES
           "class ContextWebSocketOutput final" OR
       NOT web_context_capabilities MATCHES
           "class ContextResponseOutput final" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextBufferedResponseOutput>" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextResponseStreamOutput>" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextWebSocketOutput>")
        boundary_error("Context capabilities lost their exclusive alternatives"
            "request body and response output must each be one explicit discriminated value")
    endif()
    if(web_context_services MATCHES
           "${RULE_STALE_CONTEXT_CAPABILITY_SPLIT}" OR
       web_context_header MATCHES
           "${RULE_STALE_CONTEXT_CAPABILITY_SPLIT}" OR
       web_context_internal MATCHES
           "${RULE_STALE_CONTEXT_CAPABILITY_SPLIT}" OR
       NOT web_context_services MATCHES
           "ContextRequestBodySource requestBodySource_" OR
       NOT web_context_services MATCHES
           "ContextResponseOutput responseOutput_" OR
       NOT web_context_services MATCHES "withLazyRequestBody" OR
       NOT web_context_services MATCHES "withStreamingRequestBody" OR
       NOT web_context_header MATCHES
           "ContextRequestBodySource requestBodySource_" OR
       NOT web_context_header MATCHES
           "ContextResponseOutput responseOutput_" OR
       NOT web_context_internal MATCHES
           "services[.]requestBodySource[(][)]" OR
       NOT web_context_internal MATCHES
           "services[.]responseOutput[(][)]")
        boundary_error("Context restored parallel nullable capability slots"
            "ContextServices and Context must carry the two discriminated values without manual pointer clearing")
    endif()
    if(NOT web_context_request_source MATCHES
           "requestBodySource_[.]lazy[(][)]" OR
       NOT web_context_request_source MATCHES
           "requestBodySource_[.]streaming[(][)]" OR
       NOT web_context_request_source MATCHES
           "responseOutput_[.]responseStream[(][)]" OR
       NOT web_context_request_source MATCHES
           "responseOutput_[.]webSocket[(][)]" OR
       NOT web_context_router_dispatch MATCHES
           "services[.]responseOutput[(][)][.]responseStream[(][)]" OR
       NOT web_context_router_dispatch MATCHES
           "services[.]responseOutput[(][)][.]webSocket[(][)]" OR
       NOT web_context_lazy_body_route MATCHES "withLazyRequestBody" OR
       NOT web_context_stream_body_route MATCHES
           "withStreamingRequestBody" OR
       NOT web_context_http2_session MATCHES "withStreamingRequestBody")
        boundary_error("Context runtime bypasses the typed capability chain"
            "H1, H2, router dispatch, and public Context access must consume the active alternatives")
    endif()
    if(NOT web_context_capability_test MATCHES
           "context_request_body_source_has_one_active_alternative" OR
       NOT web_context_capability_test MATCHES
           "context_response_output_has_one_active_alternative" OR
       NOT web_context_capability_test MATCHES
           "context_copies_typed_capabilities_into_public_facades" OR
       NOT web_context_package_consumer MATCHES
           "HasSplitContextCapabilityAccessors" OR
       NOT web_context_package_consumer MATCHES
           "ContextRequestBodySource" OR
       NOT web_context_package_consumer MATCHES "ContextResponseOutput")
        boundary_error("typed Context capabilities lack regression coverage"
            "unit and installed-package tests must pin exclusivity, propagation, and removal of split accessors")
    endif()
endif()

set(WEB_CONTENT_DECODE_SERVER_ENTRY
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerSessionEntry.inl")
if(EXISTS "${WEB_CONTENT_DECODE_SERVER_ENTRY}" AND
   EXISTS "${WEB_CONTEXT_SERVICES}" AND
   EXISTS "${WEB_CONTEXT_INTERNAL}" AND
   EXISTS "${WEB_CONTEXT_REQUEST_SOURCE}")
    file(READ "${WEB_CONTENT_DECODE_SERVER_ENTRY}"
        web_content_decode_server_entry)
    if(NOT web_content_decode_server_entry MATCHES
           "options_[.]maxBufferedBodyBytes" OR
       NOT web_context_services MATCHES "maxDecodedBodyBytes" OR
       NOT web_context_internal MATCHES
           "maxDecodedBodyBytes_[(]services[.]maxDecodedBodyBytes[(][)][)]" OR
       NOT web_context_request_source MATCHES
           "decodeHttpContent" OR
       NOT web_context_request_source MATCHES
           "maxDecodedBodyBytes_")
        boundary_error("Web decoded-body limit is not wired end to end"
            "the configured buffered-body limit must reach the HTTP decoder through ContextServices")
    endif()
endif()

set(WEB_CONN_INFO
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ConnInfo.h")
set(WEB_CONN_CONTEXT_SERVICES
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextServices.h")
set(WEB_CONN_CONTEXT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h")
set(WEB_CONN_CONTEXT_INTERNAL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextInternal.h")
set(WEB_CONN_CONTEXT_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
set(WEB_CONN_SERVER_ENTRY
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerSessionEntry.inl")
set(WEB_CONN_HTTP1_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_CONN_HTTP2_ENTRY
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerCleartextHttp2.h")
set(WEB_CONN_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_CONN_TEST "${RUVIA_ROOT}/tests/unit_conn_info.cpp")
set(WEB_CONN_TLS_TEST "${RUVIA_ROOT}/tests/unit_sansio_tls.cpp")
set(WEB_CONN_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
set(WEB_CONN_API_SURFACE "${RUVIA_ROOT}/examples/api_surface.cpp")
foreach(conn_info_contract_file IN ITEMS
        "${WEB_CONN_INFO}"
        "${WEB_CONN_CONTEXT_SERVICES}"
        "${WEB_CONN_CONTEXT}"
        "${WEB_CONN_CONTEXT_INTERNAL}"
        "${WEB_CONN_CONTEXT_SOURCE}"
        "${WEB_CONN_SERVER_ENTRY}"
        "${WEB_CONN_HTTP1_SESSION}"
        "${WEB_CONN_HTTP2_ENTRY}"
        "${WEB_CONN_HTTP2_SESSION}"
        "${WEB_CONN_TEST}"
        "${WEB_CONN_TLS_TEST}"
        "${WEB_CONN_PACKAGE_CONSUMER}"
        "${WEB_CONN_API_SURFACE}")
    if(NOT EXISTS "${conn_info_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${conn_info_contract_file}")
        boundary_error("typed connection metadata contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_CONN_INFO}" AND
   EXISTS "${WEB_CONN_CONTEXT_SERVICES}" AND
   EXISTS "${WEB_CONN_CONTEXT}" AND
   EXISTS "${WEB_CONN_CONTEXT_INTERNAL}" AND
   EXISTS "${WEB_CONN_CONTEXT_SOURCE}" AND
   EXISTS "${WEB_CONN_SERVER_ENTRY}" AND
   EXISTS "${WEB_CONN_HTTP1_SESSION}" AND
   EXISTS "${WEB_CONN_HTTP2_ENTRY}" AND
   EXISTS "${WEB_CONN_HTTP2_SESSION}" AND
   EXISTS "${WEB_CONN_TEST}" AND
   EXISTS "${WEB_CONN_TLS_TEST}" AND
   EXISTS "${WEB_CONN_PACKAGE_CONSUMER}" AND
   EXISTS "${WEB_CONN_API_SURFACE}")
    file(READ "${WEB_CONN_INFO}" web_conn_info)
    file(READ "${WEB_CONN_CONTEXT_SERVICES}" web_conn_services)
    file(READ "${WEB_CONN_CONTEXT}" web_conn_context)
    file(READ "${WEB_CONN_CONTEXT_INTERNAL}" web_conn_context_internal)
    file(READ "${WEB_CONN_CONTEXT_SOURCE}" web_conn_context_source)
    file(READ "${WEB_CONN_SERVER_ENTRY}" web_conn_server_entry)
    file(READ "${WEB_CONN_HTTP1_SESSION}" web_conn_http1_session)
    file(READ "${WEB_CONN_HTTP2_ENTRY}" web_conn_http2_entry)
    file(READ "${WEB_CONN_HTTP2_SESSION}" web_conn_http2_session)
    file(READ "${WEB_CONN_TEST}" web_conn_test)
    file(READ "${WEB_CONN_TLS_TEST}" web_conn_tls_test)
    file(READ "${WEB_CONN_PACKAGE_CONSUMER}" web_conn_package_consumer)
    file(READ "${WEB_CONN_API_SURFACE}" web_conn_api_surface)
    if(NOT web_conn_info MATCHES
           "class PlainConnectionTransport final" OR
       NOT web_conn_info MATCHES
           "class TlsConnectionTransport final" OR
       NOT web_conn_info MATCHES "class ConnInfo final" OR
       NOT web_conn_info MATCHES
           "std::variant<PlainConnectionTransport, TlsConnectionTransport>" OR
       NOT web_conn_info MATCHES
           "std::get_if<PlainConnectionTransport>" OR
       NOT web_conn_info MATCHES
           "std::get_if<TlsConnectionTransport>" OR
       NOT web_conn_info MATCHES
           "TlsConnectionTransport[\t\r\n ]+transport" OR
       web_conn_info MATCHES "secure[ \t\r\n]*[(]" OR
       web_conn_info MATCHES
           "std::(function|shared_ptr|unique_ptr)")
        boundary_error("ConnInfo lost its exclusive transport alternatives"
            "plain or TLS must be one allocation-free discriminated value, with certificate identity owned only by TLS")
    endif()
    string(REGEX MATCHALL "const && = delete"
        conn_deleted_rvalue_accessors "${web_conn_info}")
    list(LENGTH conn_deleted_rvalue_accessors
        conn_deleted_rvalue_accessor_count)
    if(conn_deleted_rvalue_accessor_count LESS 2 OR
       NOT web_conn_test MATCHES "ExposesRvalueTransportPointer" OR
       NOT web_conn_package_consumer MATCHES
           "ExposesRvalueTransportPointer" OR
       NOT web_conn_api_surface MATCHES
           "HasRvalueConnInfoTransportAccess")
        boundary_error("ConnInfo exposes alternative pointers from temporaries"
            "plain/tls pointer access must remain lvalue-only in source, installed consumers, and the public API surface")
    endif()
    if(web_conn_services MATCHES "${RULE_STALE_CONN_INFO_SCALARS}" OR
       web_conn_context MATCHES "${RULE_STALE_CONN_INFO_SCALARS}" OR
       web_conn_context_internal MATCHES
           "${RULE_STALE_CONN_INFO_SCALARS}" OR
       web_conn_context_source MATCHES
           "${RULE_STALE_CONN_INFO_SCALARS}" OR
       NOT web_conn_services MATCHES "const ConnInfo& connInfo" OR
       NOT web_conn_services MATCHES "withPlainTransport" OR
       NOT web_conn_services MATCHES "withTlsTransport" OR
       NOT web_conn_services MATCHES "ConnInfo connInfo_" OR
       NOT web_conn_context MATCHES "ConnInfo connInfo_" OR
       NOT web_conn_context_internal MATCHES
           "connInfo_[(]services[.]connInfo[(][)][)]" OR
       NOT web_conn_context_source MATCHES
           "return context[.]connInfo_")
        boundary_error("Context restored split connection metadata"
            "ContextServices, Context, and getConnInfo must pass one ConnInfo value without scalar reconstruction")
    endif()
    string(REGEX MATCHALL
        "std::basic_string<char, Traits, Allocator>&&"
        conn_deleted_rvalue_refinements
        "${web_conn_services}")
    list(LENGTH conn_deleted_rvalue_refinements
        conn_deleted_rvalue_refinement_count)
    if(conn_deleted_rvalue_refinement_count LESS 3 OR
       NOT web_conn_test MATCHES "AcceptsRvaluePlainTransport" OR
       NOT web_conn_test MATCHES "AcceptsRvalueTlsAddress" OR
       NOT web_conn_test MATCHES "AcceptsRvalueTlsCertificate" OR
       NOT web_conn_package_consumer MATCHES
           "AcceptsRvaluePlainTransport" OR
       NOT web_conn_package_consumer MATCHES
           "AcceptsRvalueTlsAddress" OR
       NOT web_conn_package_consumer MATCHES
           "AcceptsRvalueTlsCertificate")
        boundary_error("borrowed connection metadata accepts temporary owners"
            "plain/TLS address and certificate rvalue owning strings must remain deleted in source and installed consumers")
    endif()
    string(REGEX MATCHALL "remote_endpoint[(]" conn_remote_reads
        "${web_conn_server_entry}")
    list(LENGTH conn_remote_reads conn_remote_read_count)
    if(NOT conn_remote_read_count EQUAL 1 OR
       NOT web_conn_server_entry MATCHES "withPlainTransport" OR
       NOT web_conn_server_entry MATCHES "withTlsTransport" OR
       web_conn_http1_session MATCHES "remote_endpoint[(]" OR
       web_conn_http2_entry MATCHES "remote_endpoint[(]" OR
       web_conn_http2_session MATCHES "remote_endpoint[(]" OR
       NOT web_conn_http1_session MATCHES
           "ContextServices baseRouteServices" OR
       NOT web_conn_http1_session MATCHES
           "baseRouteServices[.]connInfo[(][)][.]remote[(][)][.]address[(][)]" OR
       NOT web_conn_http2_entry MATCHES "ContextServices services" OR
       NOT web_conn_http2_session MATCHES
           "const ContextServices& services[(][)]" OR
       NOT web_conn_http2_session MATCHES
           "const auto& baseServices = session[.]services[(][)]")
        boundary_error("server runtimes re-derived connection identity"
            "the accepted socket/handshake must classify one ConnInfo reused by HTTP/1, cleartext HTTP/2, and ALPN HTTP/2")
    endif()
    if(NOT web_conn_test MATCHES
           "conn_info_transport_has_one_active_alternative" OR
       NOT web_conn_test MATCHES
           "context_preserves_typed_connection_info_for_url_and_handler" OR
       NOT web_conn_test MATCHES
           "HasBooleanTransportRefinement" OR
       NOT web_conn_tls_test MATCHES "TlsConnectionObservation" OR
       NOT web_conn_tls_test MATCHES "withTlsTransport" OR
       NOT web_conn_tls_test MATCHES "info[.]tls[(][)]" OR
       NOT web_conn_package_consumer MATCHES
           "HasLegacyConnInfoScalarAccessors" OR
       NOT web_conn_package_consumer MATCHES
           "PlainConnectionTransport" OR
       NOT web_conn_package_consumer MATCHES
           "TlsConnectionTransport" OR
       NOT web_conn_api_surface MATCHES
           "HasLegacyConnInfoScalarAccessors")
        boundary_error("typed connection metadata lacks regression coverage"
            "unit, installed-package, and public API checks must pin alternatives, propagation, and removed scalar access")
    endif()
endif()
check_files_no_match("Web connection metadata restored scalar transport state"
    "${RULE_STALE_CONN_INFO_SCALARS}"
    "${WEB_CONN_CONTEXT_SERVICES}"
    "${WEB_CONN_CONTEXT}"
    "${WEB_CONN_CONTEXT_INTERNAL}"
    "${WEB_CONN_CONTEXT_SOURCE}"
    "${WEB_CONN_HTTP1_SESSION}"
    "${WEB_CONN_HTTP2_ENTRY}"
    "${WEB_CONN_HTTP2_SESSION}")

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

set(WEB_ACCESS_LOG_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/HttpServerOptions.h")
set(WEB_ACCESS_LOG_ACCESS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/app/AppAccess.h")
set(WEB_ACCESS_LOG_RECORDER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerAccessLog.h")
set(WEB_ACCESS_LOG_HTTP1
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_ACCESS_LOG_HTTP2
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_ACCESS_LOG_TEST "${RUVIA_ROOT}/tests/unit_access_log.cpp")
set(WEB_ACCESS_LOG_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
set(WEB_ACCESS_LOG_API_SURFACE "${RUVIA_ROOT}/examples/api_surface.cpp")
foreach(access_log_contract_file IN ITEMS
        "${WEB_ACCESS_LOG_MODEL}"
        "${WEB_ACCESS_LOG_ACCESS}"
        "${WEB_ACCESS_LOG_RECORDER}"
        "${WEB_ACCESS_LOG_HTTP1}"
        "${WEB_ACCESS_LOG_HTTP2}"
        "${WEB_ACCESS_LOG_TEST}"
        "${WEB_ACCESS_LOG_PACKAGE_CONSUMER}"
        "${WEB_ACCESS_LOG_API_SURFACE}")
    if(NOT EXISTS "${access_log_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${access_log_contract_file}")
        boundary_error("typed access-log protocol contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_ACCESS_LOG_MODEL}" AND
   EXISTS "${WEB_ACCESS_LOG_ACCESS}" AND
   EXISTS "${WEB_ACCESS_LOG_RECORDER}" AND
   EXISTS "${WEB_ACCESS_LOG_HTTP1}" AND
   EXISTS "${WEB_ACCESS_LOG_HTTP2}" AND
   EXISTS "${WEB_ACCESS_LOG_TEST}" AND
   EXISTS "${WEB_ACCESS_LOG_PACKAGE_CONSUMER}" AND
   EXISTS "${WEB_ACCESS_LOG_API_SURFACE}")
    file(READ "${WEB_ACCESS_LOG_MODEL}" web_access_log_model)
    file(READ "${WEB_ACCESS_LOG_ACCESS}" web_access_log_access)
    file(READ "${WEB_ACCESS_LOG_RECORDER}" web_access_log_recorder)
    file(READ "${WEB_ACCESS_LOG_HTTP1}" web_access_log_http1)
    file(READ "${WEB_ACCESS_LOG_HTTP2}" web_access_log_http2)
    file(READ "${WEB_ACCESS_LOG_TEST}" web_access_log_test)
    file(READ "${WEB_ACCESS_LOG_PACKAGE_CONSUMER}"
        web_access_log_package_consumer)
    file(READ "${WEB_ACCESS_LOG_API_SURFACE}"
        web_access_log_api_surface)
    if(web_access_log_model MATCHES
           "${RULE_STALE_ACCESS_LOG_PROTOCOL_SPLIT}" OR
       web_access_log_access MATCHES
           "${RULE_STALE_ACCESS_LOG_PROTOCOL_SPLIT}" OR
       web_access_log_recorder MATCHES
           "${RULE_STALE_ACCESS_LOG_PROTOCOL_SPLIT}" OR
       NOT web_access_log_model MATCHES
           "const HttpRequest& request_" OR
       NOT web_access_log_model MATCHES
           "HttpProtocolVersion protocolVersion[(][)] const noexcept" OR
       NOT web_access_log_model MATCHES
           "return request_[.]protocolVersion[(][)]" OR
       NOT web_access_log_model MATCHES
           "return request_[.]method[(][)]" OR
       NOT web_access_log_model MATCHES
           "return request_[.]knownMethod[(][)]" OR
       NOT web_access_log_model MATCHES
           "return request_[.]path[(][)]" OR
       NOT web_access_log_access MATCHES
           "const HttpRequest& request" OR
       NOT web_access_log_access MATCHES
           "AccessLogRecord[(][ \t\r\n]*request" OR
       NOT web_access_log_recorder MATCHES
           "AccessLogRecordAccess::make[(][ \t\r\n]*request")
        boundary_error("access log restored copied request facts or a protocol boolean"
            "AccessLogRecord must borrow one HttpRequest and derive method/path/version from it")
    endif()
    if(web_access_log_http1 MATCHES
           "recordHttpAccess[(][^;]*(true|false)[)]" OR
       web_access_log_http2 MATCHES
           "recordHttpAccess[(][^;]*(true|false)[)]" OR
       web_access_log_recorder MATCHES
           "HttpProtocolVersion[ \t]+protocolVersion|bool[ \t]+http2")
        boundary_error("server access-log calls re-derived protocol version"
            "recordHttpAccess must consume request.protocolVersion through the record without H1/H2 flags")
    endif()
    if(NOT web_access_log_test MATCHES
           "access_log_record_borrows_one_typed_request" OR
       NOT web_access_log_test MATCHES
           "access_log_preserves_all_protocol_versions_without_transport_bool" OR
       NOT web_access_log_test MATCHES "RecordHttpAccessFunction" OR
       NOT web_access_log_package_consumer MATCHES
           "HasLegacyAccessLogHttp2Flag" OR
       NOT web_access_log_package_consumer MATCHES
           "RecordHttpAccessFunction" OR
       NOT web_access_log_api_surface MATCHES
           "HasLegacyAccessLogHttp2Flag")
        boundary_error("typed access-log protocol contract lacks regression coverage"
            "unit, installed-package, and public API checks must pin request borrowing, all versions, and removed bool access")
    endif()
endif()
check_files_no_match("AccessLogRecord restored copied request or bool protocol state"
    "${RULE_STALE_ACCESS_LOG_PROTOCOL_SPLIT}"
    "${WEB_ACCESS_LOG_MODEL}"
    "${WEB_ACCESS_LOG_ACCESS}"
    "${WEB_ACCESS_LOG_RECORDER}")

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
check_files_no_match("WebSocket server negotiation must remain one immutable HTTP value"
    "${RULE_STALE_WS_SERVER_NEGOTIATION}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketServerHandshake.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2WebSocketHandshake.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsConnection.h"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketHandshake.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnection.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("WebSocket deflate negotiation must use exclusive alternatives"
    "${RULE_STALE_WS_DEFLATE_PRODUCT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h")
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
check_files_no_match("HTTP content decoding must use one owning typed result"
    "${RULE_STALE_HTTP_CONTENT_DECODE_CHAIN}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpContentCoding.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/RequestBodyDecoding.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientContentEncoding.h"
    "${RUVIA_ROOT}/ruvia-http/src/HttpContentCoding.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
check_files_no_match("HTTP content encoding must own its result and response lifetime"
    "${RULE_STALE_HTTP_CONTENT_ENCODE_CHAIN}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpContentCoding.h"
    "${RUVIA_ROOT}/ruvia-http/src/HttpContentCoding.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseCompression.h"
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpResponseCompression.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpBufferedResponse.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpConnectionState.h"
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpConnectionState.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(HTTP_CONTENT_CODING_CONTRACT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpContentCoding.h")
set(WEB_RESPONSE_COMPRESSION_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpResponseCompression.cpp")
set(WEB_RESPONSE_COMPRESSION_TEST
    "${RUVIA_ROOT}/tests/unit_response_compression.cpp")
if(EXISTS "${HTTP_CONTENT_CODING_CONTRACT}" AND
   EXISTS "${WEB_RESPONSE_COMPRESSION_SOURCE}" AND
   EXISTS "${WEB_RESPONSE_COMPRESSION_TEST}")
    file(READ "${HTTP_CONTENT_CODING_CONTRACT}"
        http_content_coding_contract)
    file(READ "${WEB_RESPONSE_COMPRESSION_SOURCE}"
        web_response_compression_source)
    file(READ "${WEB_RESPONSE_COMPRESSION_TEST}"
        web_response_compression_test)
    if(NOT http_content_coding_contract MATCHES
           "class HttpEncodedContent final" OR
       NOT http_content_coding_contract MATCHES
           "class HttpContentEncodeFailure final" OR
       NOT http_content_coding_contract MATCHES
           "class HttpContentEncodeResult final" OR
       NOT http_content_coding_contract MATCHES
           "std::variant<HttpEncodedContent, HttpContentEncodeFailure>" OR
       NOT web_response_compression_source MATCHES
           "setResponseBodyOwned" OR
       NOT web_response_compression_source MATCHES
           "takeBytes[(][)]" OR
       NOT web_response_compression_test MATCHES
           "responseBody[(]response[)][.]ownedBytes[(][)]")
        boundary_error("HTTP response compression lost encoded-byte ownership"
            "the HTTP encoder must return one owning alternative and Web must move it into HttpResponse")
    endif()
endif()
if(EXISTS
       "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpStreamingDecoder.h")
    boundary_error(
        "HTTP content decoding must have one implementation owner"
        "stale HttpStreamingDecoder.h exists")
endif()
set(WEB_STATIC_FILE_RESPONSE_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextFileResponse.cpp")
set(WEB_STATIC_FILE_INDEX_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/StaticFiles.cpp")
set(WEB_STATIC_FILE_INDEX_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/StaticFilesInternal.h")
set(WEB_STATIC_FILE_REPRESENTATION_TEST
    "${RUVIA_ROOT}/tests/unit_content_range.cpp")
set(WEB_STATIC_FILE_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
check_files_no_match("static file representation must use one typed selection"
    "${RULE_STALE_STATIC_FILE_REPRESENTATION_SPLIT}"
    "${WEB_STATIC_FILE_RESPONSE_SOURCE}"
    "${WEB_STATIC_FILE_INDEX_CONTRACT}")
if(EXISTS "${WEB_STATIC_FILE_RESPONSE_SOURCE}" AND
   EXISTS "${WEB_STATIC_FILE_INDEX_SOURCE}" AND
   EXISTS "${WEB_STATIC_FILE_INDEX_CONTRACT}" AND
   EXISTS "${WEB_STATIC_FILE_REPRESENTATION_TEST}" AND
   EXISTS "${WEB_STATIC_FILE_PACKAGE_CONSUMER}")
    file(READ "${WEB_STATIC_FILE_RESPONSE_SOURCE}"
        web_static_file_response_source)
    file(READ "${WEB_STATIC_FILE_INDEX_SOURCE}"
        web_static_file_index_source)
    file(READ "${WEB_STATIC_FILE_INDEX_CONTRACT}"
        web_static_file_index_contract)
    file(READ "${WEB_STATIC_FILE_REPRESENTATION_TEST}"
        web_static_file_representation_test)
    file(READ "${WEB_STATIC_FILE_PACKAGE_CONSUMER}"
        web_static_file_package_consumer)
    if(NOT web_static_file_response_source MATCHES
           "std::variant<[ \t\r\n]*FileResponseCopiedPath,[ \t\r\n]*FileResponseBorrowedNativePath>" OR
       NOT web_static_file_response_source MATCHES
           "class StaticFileRepresentation final" OR
       NOT web_static_file_response_source MATCHES
           "HttpContentCoding contentCoding_" OR
       NOT web_static_file_response_source MATCHES
           "httpContentCodingToken[(]contentCoding[)]" OR
       NOT web_static_file_index_contract MATCHES
           "class StaticRootEntryView final" OR
       NOT web_static_file_index_contract MATCHES
           "std::optional<StaticRootEntryView>" OR
       NOT web_static_file_index_source MATCHES
           "mime[.]contentType[.]empty[(][)]" OR
       NOT web_static_file_representation_test MATCHES
           "static_file_selects_precompressed_representation_atomically" OR
       NOT web_static_file_representation_test MATCHES
           "static_root_rejects_empty_custom_mime_type" OR
       NOT web_static_file_package_consumer MATCHES
           "!std::default_initializable<[ \t\r\n]*ruvia::detail::StaticRootEntryView>" OR
       NOT web_static_file_package_consumer MATCHES
           "std::optional<ruvia::detail::StaticRootEntryView>")
        boundary_error("static file representation ownership was split"
            "path lifetime, selected entry, and HTTP content coding must remain typed and atomically tested")
    endif()
endif()
set(HTTP_URL_ENCODING_CONTRACT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/UrlEncoding.h")
set(WEB_FORM_DECODING_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/FormParser.h")
set(WEB_FORM_DECODING_VISITOR
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/RequestFieldVisitors.h")
set(WEB_FORM_DECODING_TEST
    "${RUVIA_ROOT}/tests/unit_form_parser.cpp")
set(HTTP_URL_ENCODING_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("URL decoding must return one owning transactional result"
    "${RULE_STALE_URL_DECODE_CHAIN}"
    "${HTTP_URL_ENCODING_CONTRACT}"
    "${WEB_FORM_DECODING_CONTRACT}"
    "${WEB_FORM_DECODING_VISITOR}"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextFileResponse.cpp")
if(EXISTS "${HTTP_URL_ENCODING_CONTRACT}" AND
   EXISTS "${WEB_FORM_DECODING_CONTRACT}" AND
   EXISTS "${WEB_FORM_DECODING_TEST}" AND
   EXISTS "${HTTP_URL_ENCODING_PACKAGE_CONSUMER}")
    file(READ "${HTTP_URL_ENCODING_CONTRACT}"
        http_url_encoding_contract)
    file(READ "${WEB_FORM_DECODING_CONTRACT}"
        web_form_decoding_contract)
    file(READ "${WEB_FORM_DECODING_TEST}"
        web_form_decoding_test)
    file(READ "${HTTP_URL_ENCODING_PACKAGE_CONSUMER}"
        http_url_encoding_package_consumer)
    if(NOT http_url_encoding_contract MATCHES
           "std::optional<std::pmr::string>[ \t\r\n]+decodeUrlComponent" OR
       NOT web_form_decoding_contract MATCHES
           "value[.]assignOwned[(]std::move[(][*]decoded[)][)]" OR
       NOT web_form_decoding_test MATCHES
           "form_string_decode_failure_preserves_existing_value" OR
       NOT http_url_encoding_package_consumer MATCHES
           "AcceptsUrlDecodeOutputParameter" OR
       NOT http_url_encoding_package_consumer MATCHES
           "std::optional<std::pmr::string>")
        boundary_error("URL decoding lost transactional ownership"
            "HTTP must own the decoded optional and Web model fields must commit only after success")
    endif()
endif()
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

set(HTTP_RESPONSE_BODY_STORAGE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseBody.h")
set(HTTP_RESPONSE_FILE_BODY_VIEW
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseFileBody.h")
set(HTTP_RESPONSE_MODEL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpResponse.h")
set(HTTP_RESPONSE_BODY_ACCESS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseBodyAccess.h")
set(HTTP_RESPONSE_FILE_ACCESS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseFileAccess.h")
set(HTTP_RESPONSE_STORAGE_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/HttpResponse.cpp")
set(HTTP_RESPONSE_WRITE_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseWritePlan.h")
set(HTTP_RESPONSE_H2_CONNECTION
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(WEB_BUFFERED_RESPONSE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h")
set(WEB_RESPONSE_COMPRESSION
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpResponseCompression.cpp")
set(WEB_RESPONSE_H2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(HTTP_RESPONSE_BODY_TEST
    "${RUVIA_ROOT}/tests/unit_http_response_body.cpp")
set(HTTP_RESPONSE_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
foreach(response_body_contract_file IN ITEMS
        "${HTTP_RESPONSE_BODY_STORAGE}"
        "${HTTP_RESPONSE_FILE_BODY_VIEW}"
        "${HTTP_RESPONSE_MODEL}"
        "${HTTP_RESPONSE_BODY_ACCESS}"
        "${HTTP_RESPONSE_FILE_ACCESS}"
        "${HTTP_RESPONSE_STORAGE_SOURCE}"
        "${HTTP_RESPONSE_WRITE_PLAN}"
        "${HTTP_RESPONSE_H2_CONNECTION}"
        "${WEB_BUFFERED_RESPONSE_WRITER}"
        "${WEB_RESPONSE_COMPRESSION}"
        "${WEB_RESPONSE_H2_SESSION}"
        "${HTTP_RESPONSE_BODY_TEST}"
        "${HTTP_RESPONSE_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${response_body_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${response_body_contract_file}")
        boundary_error("typed HttpResponse body contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_RESPONSE_BODY_STORAGE}" AND
   EXISTS "${HTTP_RESPONSE_FILE_BODY_VIEW}" AND
   EXISTS "${HTTP_RESPONSE_MODEL}" AND
   EXISTS "${HTTP_RESPONSE_BODY_ACCESS}" AND
   EXISTS "${HTTP_RESPONSE_FILE_ACCESS}" AND
   EXISTS "${HTTP_RESPONSE_STORAGE_SOURCE}" AND
   EXISTS "${HTTP_RESPONSE_WRITE_PLAN}" AND
   EXISTS "${HTTP_RESPONSE_H2_CONNECTION}" AND
   EXISTS "${WEB_BUFFERED_RESPONSE_WRITER}" AND
   EXISTS "${WEB_RESPONSE_COMPRESSION}" AND
   EXISTS "${WEB_RESPONSE_H2_SESSION}" AND
   EXISTS "${HTTP_RESPONSE_BODY_TEST}" AND
   EXISTS "${HTTP_RESPONSE_PACKAGE_CONSUMER}")
    file(READ "${HTTP_RESPONSE_BODY_STORAGE}" http_response_body_storage)
    file(READ "${HTTP_RESPONSE_FILE_BODY_VIEW}" http_response_file_body_view)
    file(READ "${HTTP_RESPONSE_MODEL}" http_response_storage_model)
    file(READ "${HTTP_RESPONSE_BODY_ACCESS}" http_response_body_access)
    file(READ "${HTTP_RESPONSE_FILE_ACCESS}" http_response_file_access)
    file(READ "${HTTP_RESPONSE_STORAGE_SOURCE}" http_response_storage_source)
    file(READ "${HTTP_RESPONSE_WRITE_PLAN}" http_response_storage_write_plan)
    file(READ "${HTTP_RESPONSE_H2_CONNECTION}" http_response_storage_h2)
    file(READ "${WEB_BUFFERED_RESPONSE_WRITER}" web_buffered_response_writer)
    file(READ "${WEB_RESPONSE_COMPRESSION}" web_response_compression)
    file(READ "${WEB_RESPONSE_H2_SESSION}" web_response_h2_session)
    file(READ "${HTTP_RESPONSE_BODY_TEST}" http_response_body_test)
    file(READ "${HTTP_RESPONSE_PACKAGE_CONSUMER}"
        http_response_package_consumer)
    if(NOT http_response_body_storage MATCHES
           "class HttpEmptyResponseBody final" OR
       NOT http_response_body_storage MATCHES
           "class HttpBorrowedResponseBytes final" OR
       NOT http_response_body_storage MATCHES
           "class HttpStaticResponseBytes final" OR
       NOT http_response_body_storage MATCHES
           "class HttpOwnedResponseBytes final" OR
       NOT http_response_body_storage MATCHES
           "class HttpOwnedResponseFile final" OR
       NOT http_response_body_storage MATCHES
           "class HttpBorrowedResponseFile final" OR
       NOT http_response_body_storage MATCHES
           "class HttpResponseBody final" OR
       NOT http_response_body_storage MATCHES "using Value = std::variant" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpEmptyResponseBody>" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpBorrowedResponseBytes>" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpStaticResponseBytes>" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpOwnedResponseBytes>" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpOwnedResponseFile>" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpBorrowedResponseFile>" OR
       NOT http_response_body_storage MATCHES
           "std::optional<ResponseFileBody> file[(][)] const noexcept")
        boundary_error("HttpResponse body lost its exclusive storage alternatives"
            "empty, borrowed/static/owned bytes, and owned/borrowed files must remain one discriminated value")
    endif()
    if(http_response_storage_model MATCHES
           "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}" OR
       http_response_storage_source MATCHES
           "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}" OR
       NOT http_response_storage_model MATCHES
           "detail::HttpResponseBody body_" OR
       NOT http_response_file_body_view MATCHES
           "class ResponseFileBody final" OR
       NOT http_response_file_body_view MATCHES
           "friend class HttpResponseBody" OR
       NOT http_response_file_body_view MATCHES
           "std::uint64_t length[(][)] const noexcept")
        boundary_error("HttpResponse restored enum plus parallel payload storage"
            "the model must own only HttpResponseBody and file descriptors must come from its active file alternative")
    endif()
    if(http_response_body_access MATCHES
           "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}" OR
       http_response_file_access MATCHES
           "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}" OR
       NOT http_response_body_access MATCHES
           "const HttpResponseBody& responseBody" OR
       NOT http_response_storage_write_plan MATCHES
           "responseBody[(]response[)][.]size[(][)]" OR
       NOT http_response_storage_h2 MATCHES
           "const auto& body = responseBody[(]response[)]" OR
       NOT web_buffered_response_writer MATCHES
           "const auto& responseContent = responseBody[(]response[)]" OR
       NOT web_buffered_response_writer MATCHES
           "responseContent[.]file[(][)]" OR
       NOT web_response_compression MATCHES
           "const auto& responseContent = responseBody[(]response[)]" OR
       NOT web_response_h2_session MATCHES
           "const auto& responseContent = responseBody[(]response[)]")
        boundary_error("response writers bypass the unified body read contract"
            "HTTP planning, H1/H2 drivers, and compression must derive bytes/file/size from responseBody(response)")
    endif()
    if(NOT http_response_body_test MATCHES
           "response_body_has_one_storage_alternative" OR
       NOT http_response_body_test MATCHES
           "response_body_materializes_only_ephemeral_borrow" OR
       NOT http_response_body_test MATCHES
           "response_body_file_view_is_atomic_and_non_default" OR
       NOT http_response_body_test MATCHES
           "response_body_file_transition_validates_before_replacement" OR
       NOT http_response_package_consumer MATCHES
           "const ruvia::detail::HttpResponseBody&" OR
       NOT http_response_package_consumer MATCHES
           "HttpBorrowedResponseFile" OR
       NOT http_response_package_consumer MATCHES "ResponseFileBody")
        boundary_error("typed HttpResponse body lacks regression coverage"
            "unit and installed-package consumers must pin alternatives, materialization, atomic file views, and removed default states")
    endif()
endif()
check_files_no_match("HttpResponse restored split body storage or read side channels"
    "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}"
    "${HTTP_RESPONSE_MODEL}"
    "${HTTP_RESPONSE_BODY_ACCESS}"
    "${HTTP_RESPONSE_FILE_ACCESS}"
    "${HTTP_RESPONSE_STORAGE_SOURCE}"
    "${HTTP_RESPONSE_WRITE_PLAN}"
    "${HTTP_RESPONSE_H2_CONNECTION}"
    "${WEB_BUFFERED_RESPONSE_WRITER}"
    "${WEB_RESPONSE_COMPRESSION}"
    "${WEB_RESPONSE_H2_SESSION}")
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
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
check_files_no_match("HTTP/2 remote content accounting must use exclusive alternatives"
    "${RULE_STALE_H2_REMOTE_CONTENT_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RemoteContentState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
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
check_files_no_match("HTTP/2 response trailers must not have staged per-stream ownership"
    "${RULE_STALE_H2_RESPONSE_TRAILER_STAGING}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamHeaderBlocks.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
check_files_no_match("HTTP/2 response finish must receive the complete terminal section explicitly"
    "${RULE_IMPLICIT_H2_RESPONSE_FINISH}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
check_files_no_match("response-stream runtime must consume the typed commit plan"
    "${RULE_STALE_RESPONSE_STREAM_COMMIT_BOOL}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamState.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
check_files_no_match("response-stream status must follow exclusive commit results"
    "${RULE_STALE_RESPONSE_STREAM_STATUS_SPLIT}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteStreamResult.h"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterMiddlewareDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamState.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("Next continuation state must remain fully typed"
    "${RULE_STALE_NEXT_CONTINUATION_TYPE_ERASURE}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Next.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteStreamResult.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteTable.h"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterMiddlewareDispatch.cpp")
check_files_no_match("HTTP/1 session completion must not split wire, connection, and buffer state"
    "${RULE_STALE_HTTP1_SESSION_COMPLETION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1SessionRequestCompletion.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBufferedRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpConnectionState.h"
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpConnectionState.cpp")
check_files_no_match("HTTP/1 request limit must use one connection-private sequence"
    "${RULE_STALE_HTTP1_REQUEST_SEQUENCE_SCALARS}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1RequestSequence.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseState.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBufferedRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/tests/unit_response_head_emit.cpp")
check_files_no_match("response stream must end in Context scope without dummy payload"
    "${RULE_LATE_RESPONSE_STREAM_END}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h")

set(HTTP_RESPONSE_STREAM_COMMIT_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseStreamHead.h")
set(HTTP1_RESPONSE_STREAM_COMMIT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h")
set(HTTP2_RESPONSE_STREAM_COMMIT
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(WEB_ROUTE_STREAM_RESULT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteStreamResult.h")
set(WEB_ROUTE_STREAM_DISPATCH_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp")
set(WEB_RESPONSE_STREAM_DISPATCH_RESULT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h")
set(WEB_RESPONSE_STREAM_STATE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamState.h")
set(WEB_HTTP1_RESPONSE_STREAM_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h")
set(WEB_HTTP1_SESSION_REQUEST_COMPLETION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1SessionRequestCompletion.h")
set(WEB_HTTP1_STREAM_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_HTTP2_STREAM_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(RESPONSE_STREAM_STATUS_TEST
    "${RUVIA_ROOT}/tests/unit_response_stream_dispatch.cpp")
set(HTTP1_SESSION_COMPLETION_TEST
    "${RUVIA_ROOT}/tests/unit_connection_read_buffer.cpp")
set(RESPONSE_STREAM_H2_RUNTIME_TEST
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
set(RESPONSE_STREAM_HTTP_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
set(RESPONSE_STREAM_WEB_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
foreach(response_stream_status_contract IN ITEMS
        "${HTTP_RESPONSE_STREAM_COMMIT_PLAN}"
        "${HTTP1_RESPONSE_STREAM_COMMIT}"
        "${HTTP2_RESPONSE_STREAM_COMMIT}"
        "${WEB_ROUTE_STREAM_RESULT}"
        "${WEB_ROUTE_STREAM_DISPATCH_SOURCE}"
        "${WEB_RESPONSE_STREAM_DISPATCH_RESULT}"
        "${WEB_RESPONSE_STREAM_STATE}"
        "${WEB_HTTP1_RESPONSE_STREAM_ROUTE}"
        "${WEB_HTTP1_SESSION_REQUEST_COMPLETION}"
        "${WEB_HTTP1_STREAM_SESSION}"
        "${WEB_HTTP2_STREAM_SESSION}"
        "${RESPONSE_STREAM_STATUS_TEST}"
        "${HTTP1_SESSION_COMPLETION_TEST}"
        "${RESPONSE_STREAM_H2_RUNTIME_TEST}"
        "${RESPONSE_STREAM_HTTP_PACKAGE_CONSUMER}"
        "${RESPONSE_STREAM_WEB_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${response_stream_status_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${response_stream_status_contract}")
        boundary_error("response-stream status contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_RESPONSE_STREAM_COMMIT_PLAN}" AND
   EXISTS "${HTTP1_RESPONSE_STREAM_COMMIT}" AND
   EXISTS "${HTTP2_RESPONSE_STREAM_COMMIT}" AND
   EXISTS "${WEB_ROUTE_STREAM_RESULT}" AND
   EXISTS "${WEB_ROUTE_STREAM_DISPATCH_SOURCE}" AND
   EXISTS "${WEB_RESPONSE_STREAM_DISPATCH_RESULT}" AND
   EXISTS "${WEB_RESPONSE_STREAM_STATE}" AND
   EXISTS "${WEB_HTTP1_RESPONSE_STREAM_ROUTE}" AND
   EXISTS "${WEB_HTTP1_SESSION_REQUEST_COMPLETION}" AND
   EXISTS "${WEB_HTTP1_STREAM_SESSION}" AND
   EXISTS "${WEB_HTTP2_STREAM_SESSION}" AND
   EXISTS "${RESPONSE_STREAM_STATUS_TEST}" AND
   EXISTS "${HTTP1_SESSION_COMPLETION_TEST}" AND
   EXISTS "${RESPONSE_STREAM_H2_RUNTIME_TEST}" AND
   EXISTS "${RESPONSE_STREAM_HTTP_PACKAGE_CONSUMER}" AND
   EXISTS "${RESPONSE_STREAM_WEB_PACKAGE_CONSUMER}")
    file(READ "${HTTP_RESPONSE_STREAM_COMMIT_PLAN}"
        response_stream_commit_plan)
    file(READ "${HTTP1_RESPONSE_STREAM_COMMIT}"
        http1_response_stream_commit)
    file(READ "${HTTP2_RESPONSE_STREAM_COMMIT}"
        http2_response_stream_commit)
    file(READ "${WEB_ROUTE_STREAM_RESULT}"
        web_route_stream_result)
    file(READ "${WEB_ROUTE_STREAM_DISPATCH_SOURCE}"
        web_route_stream_dispatch_source)
    file(READ "${WEB_RESPONSE_STREAM_DISPATCH_RESULT}"
        web_response_stream_dispatch_result)
    file(READ "${WEB_RESPONSE_STREAM_STATE}"
        web_response_stream_state)
    file(READ "${WEB_HTTP1_RESPONSE_STREAM_ROUTE}"
        web_http1_response_stream_route)
    file(READ "${WEB_HTTP1_SESSION_REQUEST_COMPLETION}"
        web_http1_session_request_completion)
    file(READ "${WEB_HTTP1_STREAM_SESSION}"
        web_http1_stream_session)
    file(READ "${WEB_HTTP2_STREAM_SESSION}"
        web_http2_stream_session)
    file(READ "${RESPONSE_STREAM_STATUS_TEST}"
        response_stream_status_test)
    file(READ "${HTTP1_SESSION_COMPLETION_TEST}"
        http1_session_completion_test)
    file(READ "${RESPONSE_STREAM_H2_RUNTIME_TEST}"
        response_stream_h2_runtime_test)
    file(READ "${RESPONSE_STREAM_HTTP_PACKAGE_CONSUMER}"
        response_stream_http_package_consumer)
    file(READ "${RESPONSE_STREAM_WEB_PACKAGE_CONSUMER}"
        response_stream_web_package_consumer)

    if(NOT response_stream_commit_plan MATCHES
           "std::uint16_t responseStatus[(][)] const noexcept" OR
       NOT response_stream_commit_plan MATCHES
           "ResponseStreamFraming framing[(][)] const noexcept" OR
       NOT response_stream_commit_plan MATCHES
           "HttpKnownMethod requestMethod" OR
       NOT response_stream_commit_plan MATCHES
           "response[.]status[(][)] != commitPlan[.]responseStatus[(][)]" OR
       NOT http1_response_stream_commit MATCHES
           "httpResponseStreamCommitPlan" OR
       NOT http1_response_stream_commit MATCHES
           "plan[.]framing[(][)]" OR
       NOT http1_response_stream_commit MATCHES
           "response[.]status[(][)]" OR
       NOT http2_response_stream_commit MATCHES
           "ResponseStreamFraming::kHttp2Frames" OR
       NOT http2_response_stream_commit MATCHES
           "head[.]status[(][)]")
        boundary_error("response-stream protocol commit lost the final status"
            "the HTTP commit plan must bind response status, framing, method-derived body semantics, and the prepared head")
    endif()

    if(NOT web_route_stream_result MATCHES
           "StreamRouteHandled" OR
       NOT web_route_stream_result MATCHES
           "StreamRouteBufferedResponse" OR
       NOT web_route_stream_result MATCHES
           "std::variant" OR
       NOT web_route_stream_dispatch_source MATCHES
           "responseStreamOutput->writer[(][)][.]end[(][)]" OR
       NOT web_route_stream_dispatch_source MATCHES
           "Context is local to this coroutine" OR
       NOT web_response_stream_dispatch_result MATCHES
           "ResponseStreamPeerAbortedBeforeCommit" OR
       NOT web_response_stream_dispatch_result MATCHES
           "ResponseStreamPeerAbortedAfterCommit" OR
       NOT web_response_stream_dispatch_result MATCHES
           "ResponseStreamFailedAfterCommit" OR
       NOT web_response_stream_dispatch_result MATCHES
           "committedResponseStreamStatus" OR
       NOT web_response_stream_state MATCHES
           "std::optional<CommittedState> committed_" OR
       NOT web_response_stream_state MATCHES
           "const ResponseStreamCommitPlan[*] commitPlan[(][)] const noexcept" OR
       NOT web_http1_response_stream_route MATCHES
           "Task<Http1SessionRequestCompletion>" OR
       NOT web_http1_response_stream_route MATCHES
           "makeCommittedStream" OR
       NOT web_http1_response_stream_route MATCHES
           "completed->status[(][)]")
        boundary_error("Web response-stream outcomes restored a status/payload tuple"
            "handled, buffered, committed, pre-commit abort, and committed failure must remain exclusive alternatives")
    endif()

    if(NOT web_http1_session_request_completion MATCHES
           "class Http1SessionRequestCompletion final" OR
       NOT web_http1_session_request_completion MATCHES
           "class Http1CommittedStreamResponse final" OR
       NOT web_http1_session_request_completion MATCHES
           "class Http1RequestBufferCompletion final" OR
       NOT web_http1_session_request_completion MATCHES
           "Http1RequestBufferDiscarded" OR
       NOT web_http1_session_request_completion MATCHES
           "Http1RequestBufferCompaction" OR
       NOT web_http1_session_request_completion MATCHES
           "Http1RequestBufferRestored" OR
       NOT web_http1_session_request_completion MATCHES
           "makeBufferedClosing" OR
       NOT web_http1_session_request_completion MATCHES
           "makeBufferedUnrestored" OR
       NOT web_http1_session_request_completion MATCHES
           "makeBufferedRestored" OR
       NOT web_http1_session_request_completion MATCHES
           "makeCommittedStream" OR
       NOT web_http1_session_request_completion MATCHES
           "connectionPlan[(][)] const noexcept" OR
       NOT web_http1_session_request_completion MATCHES
           "bufferCompletion[(][)] const noexcept" OR
       NOT web_http1_stream_session MATCHES
           "std::optional<Http1SessionRequestCompletion> requestCompletion" OR
       NOT web_http1_stream_session MATCHES
           "committed->status[(][)]" OR
       NOT web_http1_stream_session MATCHES
           "applyReusableHttp1RequestBufferCompletion" OR
       NOT web_http1_stream_session MATCHES
           "Http1ConnectionDisposition::kClose" OR
       NOT web_http2_stream_session MATCHES
           "failed->status[(][)]" OR
       NOT web_http2_stream_session MATCHES
           "peer->status[(][)]" OR
       NOT web_http2_stream_session MATCHES
           "completed->status[(][)]" OR
       web_http2_stream_session MATCHES
           "completed streamed response [(]status 200[)]")
        boundary_error("server runtime re-derived streamed access-log status"
            "H1 must consume one request completion carrying wire status, connection disposition, and buffer cleanup; H2 must log exact committed status before close/reset")
    endif()

    if(NOT response_stream_status_test MATCHES
           "response_stream_dispatch_preserves_exact_committed_status" OR
       NOT response_stream_status_test MATCHES
           "response_stream_dispatch_distinguishes_precommit_peer_abort" OR
       NOT response_stream_status_test MATCHES
           "response_stream_dispatch_distinguishes_committed_peer_abort" OR
       NOT response_stream_status_test MATCHES
           "response_stream_dispatch_end_commits_bodyless_status" OR
       NOT response_stream_status_test MATCHES
           "response_stream_dispatch_preserves_committed_failure_status" OR
       NOT http1_session_completion_test MATCHES
           "http1_session_request_completion_owns_wire_and_buffer_outcome" OR
       NOT http1_session_completion_test MATCHES
           "http1_request_buffer_completion_applies_exactly_one_cleanup" OR
       NOT response_stream_h2_runtime_test MATCHES
           "sansio_driver_h2_stream_trailers_emitted" OR
       NOT response_stream_h2_runtime_test MATCHES
           ":status=207;" OR
       NOT response_stream_h2_runtime_test MATCHES
           "accessObservation[.]status" OR
       NOT response_stream_http_package_consumer MATCHES
           "ResponseStreamCommitPlanner" OR
       NOT response_stream_web_package_consumer MATCHES
           "HasLegacyStreamedPredicate" OR
       NOT response_stream_web_package_consumer MATCHES
           "Http1SessionRequestCompletion" OR
       NOT response_stream_web_package_consumer MATCHES
           "Http1RequestBufferCompaction" OR
       NOT response_stream_web_package_consumer MATCHES
           "HttpWebSocketRouteResult")
        boundary_error("response-stream status propagation lacks regression coverage"
            "unit and installed-package checks must pin exact status and exclusive terminal alternatives")
    endif()
endif()
check_files_no_match("buffered response planning must derive body semantics from method and response"
    "${RULE_LOOSE_BUFFERED_RESPONSE_PLAN}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseWritePlan.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpBufferedResponse.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
check_files_no_match("HTTP/2 buffered completion must not reconstruct status from HttpResponse"
    "${RULE_STALE_H2_BUFFERED_COMPLETION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("HTTP/2 buffered submission must consume the prepared Web plan"
    "${RULE_STALE_H2_UNPREPARED_BUFFERED_HEAD}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("HTTP/1 buffered completion must own its commit boundary and plan status"
    "${RULE_STALE_H1_BUFFERED_COMPLETION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
check_files_no_match("HTTP file writes must return results instead of error side channels"
    "${RULE_STALE_HTTP_FILE_WRITE_COMPLETION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpFileFallback.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpFileZeroCopy.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h"
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpFileZeroCopy.cpp")
check_files_no_match("database migration must return its owned report"
    "${RULE_STALE_DB_MIGRATION_REPORT_SIDE_CHANNEL}"
    "${RUVIA_ROOT}/ruvia-web/src/db/DbMigration.cpp")

set(DB_MIGRATION_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/db/DbMigration.cpp")
set(DB_MIGRATION_TEST
    "${RUVIA_ROOT}/tests/unit_db_api_surface.cpp")
if(EXISTS "${DB_MIGRATION_SOURCE}" AND EXISTS "${DB_MIGRATION_TEST}")
    file(READ "${DB_MIGRATION_SOURCE}" db_migration_source)
    file(READ "${DB_MIGRATION_TEST}" db_migration_test)
    if(NOT db_migration_source MATCHES
           "Task<DbMigrationReport>[ \t]+run" OR
       NOT db_migration_source MATCHES
           "TaskCompletionResult<DbMigrationReport>" OR
       NOT db_migration_source MATCHES "return std::move[(][*]report[)]" OR
       NOT db_migration_test MATCHES
           "db_migrator_validates_before_opening_connection")
        boundary_error("database migration report ownership is incomplete"
            "the runner must return its report and feature-on tests must cover pre-I/O validation")
    endif()
endif()

set(HTTP_BUFFERED_RESPONSE_WRITE_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseWritePlan.h")
set(HTTP1_BUFFERED_RESPONSE_HEAD_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/server/HttpResponseHead.cpp")
set(HTTP2_BUFFERED_RESPONSE_HEAD_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeadPlan.h")
set(HTTP2_BUFFERED_RESPONSE_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeaders.h")
set(HTTP2_BUFFERED_RESPONSE_CONNECTION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
set(HTTP2_BUFFERED_RESPONSE_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(WEB_HTTP2_BUFFERED_RESPONSE_RESULT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2BufferedResponseDispatch.h")
set(WEB_HTTP2_BUFFERED_RESPONSE_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_HTTP1_BUFFERED_RESPONSE_RESULT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1BufferedResponseWrite.h")
set(WEB_HTTP1_BUFFERED_RESPONSE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h")
set(WEB_HTTP1_BUFFERED_RESPONSE_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(BUFFERED_RESPONSE_PLAN_TEST
    "${RUVIA_ROOT}/tests/unit_response_head_policy.cpp")
set(BUFFERED_RESPONSE_H1_TEST
    "${RUVIA_ROOT}/tests/unit_response_head_emit.cpp")
set(BUFFERED_RESPONSE_H1_RESULT_TEST
    "${RUVIA_ROOT}/tests/unit_http1_buffered_response_write.cpp")
set(BUFFERED_RESPONSE_H2_PLAN_TEST
    "${RUVIA_ROOT}/tests/unit_http2_response_headers.cpp")
set(BUFFERED_RESPONSE_H2_RESULT_TEST
    "${RUVIA_ROOT}/tests/unit_http2_buffered_response_dispatch.cpp")
set(BUFFERED_RESPONSE_H2_RUNTIME_TEST
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
set(BUFFERED_RESPONSE_H2_CONNECTION_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
set(BUFFERED_RESPONSE_HTTP_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
set(BUFFERED_RESPONSE_WEB_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
set(BUFFERED_RESPONSE_PACKAGE_VERIFY
    "${RUVIA_ROOT}/tests/verify_package_consumers.cmake.in")
foreach(buffered_response_status_contract IN ITEMS
        "${HTTP_BUFFERED_RESPONSE_WRITE_PLAN}"
        "${HTTP1_BUFFERED_RESPONSE_HEAD_SOURCE}"
        "${HTTP2_BUFFERED_RESPONSE_HEAD_PLAN}"
        "${HTTP2_BUFFERED_RESPONSE_HEADERS}"
        "${HTTP2_BUFFERED_RESPONSE_CONNECTION_HEADER}"
        "${HTTP2_BUFFERED_RESPONSE_CONNECTION_SOURCE}"
        "${WEB_HTTP2_BUFFERED_RESPONSE_RESULT}"
        "${WEB_HTTP2_BUFFERED_RESPONSE_SESSION}"
        "${WEB_HTTP1_BUFFERED_RESPONSE_RESULT}"
        "${WEB_HTTP1_BUFFERED_RESPONSE_WRITER}"
        "${WEB_HTTP1_BUFFERED_RESPONSE_SESSION}"
        "${BUFFERED_RESPONSE_PLAN_TEST}"
        "${BUFFERED_RESPONSE_H1_TEST}"
        "${BUFFERED_RESPONSE_H1_RESULT_TEST}"
        "${BUFFERED_RESPONSE_H2_PLAN_TEST}"
        "${BUFFERED_RESPONSE_H2_RESULT_TEST}"
        "${BUFFERED_RESPONSE_H2_RUNTIME_TEST}"
        "${BUFFERED_RESPONSE_H2_CONNECTION_TEST}"
        "${BUFFERED_RESPONSE_HTTP_PACKAGE_CONSUMER}"
        "${BUFFERED_RESPONSE_WEB_PACKAGE_CONSUMER}"
        "${BUFFERED_RESPONSE_PACKAGE_VERIFY}")
    if(NOT EXISTS "${buffered_response_status_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${buffered_response_status_contract}")
        boundary_error("buffered response status contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_BUFFERED_RESPONSE_WRITE_PLAN}" AND
   EXISTS "${HTTP1_BUFFERED_RESPONSE_HEAD_SOURCE}" AND
   EXISTS "${HTTP2_BUFFERED_RESPONSE_HEAD_PLAN}" AND
   EXISTS "${HTTP2_BUFFERED_RESPONSE_HEADERS}" AND
   EXISTS "${HTTP2_BUFFERED_RESPONSE_CONNECTION_HEADER}" AND
   EXISTS "${HTTP2_BUFFERED_RESPONSE_CONNECTION_SOURCE}" AND
   EXISTS "${WEB_HTTP2_BUFFERED_RESPONSE_RESULT}" AND
   EXISTS "${WEB_HTTP2_BUFFERED_RESPONSE_SESSION}" AND
   EXISTS "${WEB_HTTP1_BUFFERED_RESPONSE_RESULT}" AND
   EXISTS "${WEB_HTTP1_BUFFERED_RESPONSE_WRITER}" AND
   EXISTS "${WEB_HTTP1_BUFFERED_RESPONSE_SESSION}" AND
   EXISTS "${BUFFERED_RESPONSE_PLAN_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H1_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H1_RESULT_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H2_PLAN_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H2_RESULT_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H2_RUNTIME_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H2_CONNECTION_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_HTTP_PACKAGE_CONSUMER}" AND
   EXISTS "${BUFFERED_RESPONSE_WEB_PACKAGE_CONSUMER}" AND
   EXISTS "${BUFFERED_RESPONSE_PACKAGE_VERIFY}")
    file(READ "${HTTP_BUFFERED_RESPONSE_WRITE_PLAN}"
        buffered_response_write_plan)
    file(READ "${HTTP1_BUFFERED_RESPONSE_HEAD_SOURCE}"
        buffered_response_h1_source)
    file(READ "${HTTP2_BUFFERED_RESPONSE_HEAD_PLAN}"
        buffered_response_h2_head_plan)
    file(READ "${HTTP2_BUFFERED_RESPONSE_HEADERS}"
        buffered_response_h2_headers)
    file(READ "${HTTP2_BUFFERED_RESPONSE_CONNECTION_HEADER}"
        buffered_response_h2_connection_header)
    file(READ "${HTTP2_BUFFERED_RESPONSE_CONNECTION_SOURCE}"
        buffered_response_h2_connection_source)
    file(READ "${WEB_HTTP2_BUFFERED_RESPONSE_RESULT}"
        buffered_response_h2_result)
    file(READ "${WEB_HTTP2_BUFFERED_RESPONSE_SESSION}"
        buffered_response_h2_session)
    file(READ "${WEB_HTTP1_BUFFERED_RESPONSE_RESULT}"
        buffered_response_h1_result)
    file(READ "${WEB_HTTP1_BUFFERED_RESPONSE_WRITER}"
        buffered_response_h1_writer)
    file(READ "${WEB_HTTP1_BUFFERED_RESPONSE_SESSION}"
        buffered_response_h1_session)
    file(READ "${BUFFERED_RESPONSE_PLAN_TEST}"
        buffered_response_plan_test)
    file(READ "${BUFFERED_RESPONSE_H1_TEST}"
        buffered_response_h1_test)
    file(READ "${BUFFERED_RESPONSE_H1_RESULT_TEST}"
        buffered_response_h1_result_test)
    file(READ "${BUFFERED_RESPONSE_H2_PLAN_TEST}"
        buffered_response_h2_plan_test)
    file(READ "${BUFFERED_RESPONSE_H2_RESULT_TEST}"
        buffered_response_h2_result_test)
    file(READ "${BUFFERED_RESPONSE_H2_RUNTIME_TEST}"
        buffered_response_h2_runtime_test)
    file(READ "${BUFFERED_RESPONSE_H2_CONNECTION_TEST}"
        buffered_response_h2_connection_test)
    file(READ "${BUFFERED_RESPONSE_HTTP_PACKAGE_CONSUMER}"
        buffered_response_http_package_consumer)
    file(READ "${BUFFERED_RESPONSE_WEB_PACKAGE_CONSUMER}"
        buffered_response_web_package_consumer)
    file(READ "${BUFFERED_RESPONSE_PACKAGE_VERIFY}"
        buffered_response_package_verify)

    if(NOT buffered_response_write_plan MATCHES
           "std::uint16_t responseStatus[(][)] const noexcept" OR
       NOT buffered_response_write_plan MATCHES
           "return bodyPlan_[.]responseStatus[(][)]" OR
       NOT buffered_response_write_plan MATCHES
           "HttpKnownMethod requestMethod" OR
       NOT buffered_response_write_plan MATCHES
           "HttpKnownMethod requestMethod[(][)] const noexcept" OR
       NOT buffered_response_write_plan MATCHES
           "matchesResponse" OR
       NOT buffered_response_write_plan MATCHES
           "bufferedRepresentationLength" OR
       NOT buffered_response_h1_source MATCHES
           "response[.]status[(][)] != bodyPlan[.]responseStatus[(][)]" OR
       NOT buffered_response_h1_source MATCHES
           "response plan representation does not match response" OR
       NOT buffered_response_h1_source MATCHES
           "bodyPlan[.]bufferedRepresentationLength[(]response[)]" OR
       NOT buffered_response_h1_source MATCHES
           "httpReasonPhrase[(]responseStatus[)]" OR
       NOT buffered_response_h2_head_plan MATCHES
           "kResponseStatusMismatch" OR
       NOT buffered_response_h2_head_plan MATCHES
           "writePlan[.]responseStatus[(][)] != response[.]status[(][)]" OR
       NOT buffered_response_h2_head_plan MATCHES
           "kResponseRepresentationMismatch" OR
       NOT buffered_response_h2_headers MATCHES
           "plan[.]bodyPlan[(][)][.]responseStatus[(][)]")
        boundary_error("buffered protocol plan lost exact response status ownership"
            "body/write/head plans must bind one status and reject response-plan mismatch before wire mutation")
    endif()

    if(NOT buffered_response_h2_connection_header MATCHES
           "kResponsePlanMismatch" OR
       NOT buffered_response_h2_connection_header MATCHES
           "HttpBufferedResponseWritePlan writePlan" OR
       NOT buffered_response_h2_connection_source MATCHES
           "writePlan[.]requestMethod[(][)] != stream->requestKnownMethod[(][)]" OR
       NOT buffered_response_h2_connection_source MATCHES
           "!writePlan[.]matchesResponse[(]response[)]" OR
       buffered_response_h2_connection_source MATCHES
           "auto[ \t]+writePlan[ \t]*=[ \t\r\n]*httpBufferedResponseWritePlan" OR
       NOT buffered_response_h2_session MATCHES
           "const auto responsePreparation = prepareBufferedHttpResponse" OR
       NOT buffered_response_h2_session MATCHES
           "responsePreparation[.]writePlan[(][)]")
        boundary_error("HTTP/2 buffered submission stopped consuming the prepared response plan"
            "Web preparation must flow into core submission and method/status/representation drift must fail transactionally")
    endif()

    if(NOT buffered_response_h2_result MATCHES
           "Http2BufferedResponsePeerAbortedBeforeCommit" OR
       NOT buffered_response_h2_result MATCHES
           "Http2BufferedResponsePeerAbortedAfterCommit" OR
       NOT buffered_response_h2_result MATCHES
           "Http2BufferedResponseFailedBeforeCommit" OR
       NOT buffered_response_h2_result MATCHES
           "Http2BufferedResponseFailedAfterCommit" OR
       NOT buffered_response_h2_result MATCHES
           "std::variant" OR
       NOT buffered_response_h2_session MATCHES
           "Task<Http2BufferedResponseDispatchResult>" OR
       NOT buffered_response_h2_session MATCHES
           "const auto committedStatus = writePlan[.]responseStatus[(][)]" OR
       NOT buffered_response_h2_session MATCHES
           "All valid buffered branches converge here" OR
       NOT buffered_response_h2_session MATCHES
           "prepareBufferedHttpResponse" OR
       NOT buffered_response_h2_session MATCHES
           "result[.]peerAbortedBeforeCommit[(][)]")
        boundary_error("HTTP/2 buffered completion restored a loose status/result path"
            "all valid buffered responses must share preparation and exclusive pre/post-commit outcomes")
    endif()

    if(NOT buffered_response_h1_result MATCHES
           "Http1BufferedResponseWriteCompleted" OR
       NOT buffered_response_h1_result MATCHES
           "Http1BufferedResponseWriteFailedBeforeCommit" OR
       NOT buffered_response_h1_result MATCHES
           "Http1BufferedResponseWriteFailedAfterCommit" OR
       NOT buffered_response_h1_result MATCHES
           "using Value = std::variant" OR
       NOT buffered_response_h1_result MATCHES
           "plan[.]writePlan[(][)][.]responseStatus[(][)]" OR
       NOT buffered_response_h1_writer MATCHES
           "Task<Http1BufferedResponseWriteResult> writeResponseWithScratch" OR
       NOT buffered_response_h1_writer MATCHES
           "asyncResult<std::size_t>" OR
       NOT buffered_response_h1_writer MATCHES
           "classifyHttp1BufferedResponseWrite" OR
       NOT buffered_response_h1_session MATCHES
           "writeResult[.]completed[(][)]" OR
       NOT buffered_response_h1_session MATCHES
           "writeResult[.]failedBeforeCommit[(][)]" OR
       NOT buffered_response_h1_session MATCHES
           "writeResult[.]failedAfterCommit[(][)]" OR
       buffered_response_h1_session MATCHES
           "response[.]status[(][)][ \t\r\n]*,[ \t\r\n]*requestStart")
        boundary_error("HTTP/1 buffered completion restored a loose write/error/status path"
            "the writer must classify the complete-head byte boundary and logging must consume only committed plan status")
    endif()

    if(NOT buffered_response_plan_test MATCHES
           "AcceptsLooseBufferedResponseBodyPlan" OR
       NOT buffered_response_h1_test MATCHES
           "http1_response_head_rejects_status_plan_mismatch" OR
       NOT buffered_response_h1_test MATCHES
           "http1_response_head_rejects_representation_plan_mismatch" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_write_partial_head_has_no_status" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_write_body_failure_keeps_committed_status" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_scatter_write_keeps_committed_status" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_write_cannot_complete_without_a_full_head" OR
       NOT buffered_response_h1_result_test MATCHES
           "http_file_zero_copy_result_distinguishes_capability" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_file_fallback_completion_owns_status" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_file_open_failure_preserves_committed_status" OR
       NOT buffered_response_h2_plan_test MATCHES
           "http2_response_head_rejects_status_plan_mismatch" OR
       NOT buffered_response_h2_plan_test MATCHES
           "http2_response_head_rejects_representation_plan_mismatch" OR
       NOT buffered_response_h2_result_test MATCHES
           "http2_buffered_response_dispatch_result_owns_only_committed_status" OR
       NOT buffered_response_h2_runtime_test MATCHES
           "sansio_driver_h2_buffered_access_uses_only_committed_plan_status" OR
       NOT buffered_response_h2_connection_test MATCHES
           "http2_connection_buffered_response_requires_matching_prepared_plan" OR
       NOT buffered_response_h2_runtime_test MATCHES
           "sansio_driver_h2_buffered_peer_abort_before_commit_has_no_status" OR
       NOT buffered_response_h2_runtime_test MATCHES
           "accessObservation[.]calls, std::size_t[{]1[}]" OR
       NOT buffered_response_h2_runtime_test MATCHES
           "accessObservation[.]calls, std::size_t[{]0[}]" OR
       NOT buffered_response_http_package_consumer MATCHES
           "AcceptsLooseBufferedResponseBodyPlan" OR
       NOT buffered_response_http_package_consumer MATCHES
           "AcceptsUnpreparedBufferedResponseHead" OR
       NOT buffered_response_web_package_consumer MATCHES
           "Http2BufferedResponseDispatchResult" OR
       NOT buffered_response_web_package_consumer MATCHES
           "Http1BufferedResponseWriteResult" OR
       NOT buffered_response_web_package_consumer MATCHES
           "HttpFileZeroCopyResult" OR
       NOT buffered_response_package_verify MATCHES
           "installed buffered response status ownership" OR
       NOT buffered_response_package_verify MATCHES
           "installed HTTP/2 prepared buffered response plan ownership" OR
       NOT buffered_response_package_verify MATCHES
           "installed HTTP/1 buffered response completion ownership")
        boundary_error("buffered response status ownership lacks regression coverage"
            "unit, integration, source-boundary, and installed-package checks must pin the exact committed status path")
    endif()
endif()
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
           "httpReasonPhrase[(]responseStatus[)]" OR
       NOT http1_response_head_source MATCHES "sink[.]append[(]' '[)]" OR
       NOT http1_interim_response_writer MATCHES
           "httpReasonPhrase[(]response[.]status[(][)][)]" OR
       NOT http1_interim_response_writer MATCHES
           "[*]cursor[+][+][ \t]*=[ \t]*' '" OR
       NOT http2_status_response_headers MATCHES
           "plan[.]bodyPlan[(][)][.]responseStatus[(][)]" OR
       NOT web_context_status_header MATCHES
           "struct ResponseInit final[ \t\r\n]*[{][ \t\r\n]*std::uint16_t status" OR
       NOT web_context_status_header MATCHES "ResponseHeaderInit headers" OR
       NOT web_error_status_normalize MATCHES "statusText = \"HTTP Error\"")
        boundary_error("response status and reason-phrase ownership split again"
            "the body/head plan must own the final status; H1 derives an optional phrase, H2 emits :status, and Web labels stay presentation-only")
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
set(HTTP2_STALE_WEB_RUNTIME_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2BodyState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2BodyQueue.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamBodyQueue.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamBodyPolicy.h")
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
elseif(NOT EXISTS "${HTTP2_REQUEST_HEADERS}" OR
       NOT EXISTS "${HTTP2_STREAM_STATE}" OR
       NOT EXISTS "${HTTP2_CONNECTION_SOURCE}")
    boundary_error("HTTP/2 remote content call chain is incomplete"
        "header decode, DATA preflight, and stream state must consume one remote-content contract")
else()
    file(READ "${HTTP2_REMOTE_CONTENT_STATE}" http2_remote_content_state)
    file(READ "${HTTP2_REQUEST_HEADERS}" http2_request_headers)
    file(READ "${HTTP2_STREAM_STATE}" http2_remote_stream_state)
    file(READ "${HTTP2_CONNECTION_SOURCE}" http2_remote_content_connection)
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
       NOT http2_remote_content_connection MATCHES
           "stream->accountRemoteContent[(]data[.]size[(][)][)]" OR
       NOT http2_remote_content_connection MATCHES
           "Http2RemoteContentAccountingResult::kDeclaredLengthExceeded" OR
       NOT http2_remote_content_connection MATCHES
           "Http2RemoteContentAccountingResult::kContentForbidden" OR
       NOT http2_request_headers MATCHES "declareRemoteContentLength")
        boundary_error("HTTP/2 remote content accounting lost its discriminated transaction"
            "content allowance and length must be exclusive, DATA accounting must be atomic, and metadata-only responses must reject payload")
    endif()
endif()
foreach(http2_stale_web_runtime_header IN LISTS
        HTTP2_STALE_WEB_RUNTIME_HEADERS)
    if(EXISTS "${http2_stale_web_runtime_header}")
        file(RELATIVE_PATH relative
            "${RUVIA_ROOT}" "${http2_stale_web_runtime_header}")
        boundary_error("ruvia-http regained Web request-body runtime state"
            "${relative} must remain absent; route storage, buffering, queues, and coroutine wakeups belong to ruvia-web")
    endif()
endforeach()
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
set(HTTP2_WEB_STREAM_RUNTIME_TEST
    "${RUVIA_ROOT}/tests/unit_http2_sansio_stream_runtime.cpp")
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
           "http2_connect_open_tunnel_replenishes_owner_released_stream_window" OR
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
if(NOT EXISTS "${HTTP2_REMOTE_CONTENT_TEST}")
    boundary_error("HTTP/2 remote content alternatives are untested"
        "unit tests must pin typed length ownership and transactional DATA acceptance")
elseif(EXISTS "${HTTP2_EVENT_TEST}" AND EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_REMOTE_CONTENT_TEST}" http2_remote_content_test)
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
       NOT http2_remote_connection_test MATCHES
           "http2_connection_feed_data_emits_body_chunk_and_end" OR
       NOT http2_remote_connection_test MATCHES
           "http2_connection_same_feed_data_credit_waits_for_owner_batch_release" OR
       NOT http2_remote_connection_test MATCHES
           "releaseReceivedData[(]1[)]" OR
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
if(NOT EXISTS "${HTTP2_WEB_STREAM_RUNTIME_TEST}")
    boundary_error("Web-owned HTTP/2 stream runtime is untested"
        "unit_http2_sansio_stream_runtime.cpp must pin route/body storage, dispatch leases, concurrent wakeups, and stable per-stream ownership")
else()
    file(READ "${HTTP2_WEB_STREAM_RUNTIME_TEST}"
        http2_web_stream_runtime_test)
    if(NOT http2_web_stream_runtime_test MATCHES
           "http2_web_body_queue_preserves_fifo_and_tracks_backlog" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_request_body_runtime_selects_storage_before_data" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_request_body_runtime_enforces_total_and_backlog_limits" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_stream_runtime_table_keeps_active_storage_stable" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_stream_runtime_table_owns_dispatch_signal_and_lease" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_stream_signal_wakes_concurrent_waiters_without_self_cancel" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_stream_runtime_keeps_overflow_signal_reference_stable")
        boundary_error("Web-owned HTTP/2 stream runtime is under-tested"
            "FIFO/backlog accounting, stable storage, table-owned dispatch leases, and concurrent signal waiters must remain explicit")
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
    if(NOT http2_connection_header MATCHES "Http2Role role" OR
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
    if(NOT http2_connection_header MATCHES "releaseReceivedData" OR
       http2_connection_header MATCHES "Http2ConnectionLimits" OR
       http2_connection_header MATCHES "deferStreamWindowRelease" OR
       http2_connection_header MATCHES "releaseStreamWindow")
        boundary_error("HTTP/2 DATA flow control regained implicit runtime policy"
            "non-empty DATA events must retain receive credit until the owner calls releaseReceivedData; route limits and defer-mode toggles do not belong in the protocol core")
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

set(WEB_HTTP2_STREAM_RUNTIME
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoStreamRuntime.h")
set(WEB_HTTP2_WS_TRANSPORT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
set(WEB_HTTP2_RESPONSE_STREAM_SINK
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
if(NOT EXISTS "${WEB_HTTP2_STREAM_RUNTIME}" OR
   NOT EXISTS "${WEB_HTTP2_WS_TRANSPORT}" OR
   NOT EXISTS "${WEB_HTTP2_RESPONSE_STREAM_SINK}")
    boundary_error("Web-owned HTTP/2 stream runtime is missing"
        "stable per-stream route/body/signal storage and asynchronous consumers must live under ruvia-web/include/ruvia/web/detail/http2")
else()
    file(READ "${WEB_HTTP2_STREAM_RUNTIME}" web_http2_stream_runtime)
    file(READ "${WEB_HTTP2_WS_TRANSPORT}" web_http2_ws_transport)
    file(READ "${WEB_HTTP2_RESPONSE_STREAM_SINK}"
        web_http2_response_stream_sink)
    if(NOT web_http2_stream_runtime MATCHES
           "class Http2SansIoStreamSignal final" OR
       NOT web_http2_stream_runtime MATCHES
           "class Http2SansIoBodyQueue final" OR
       NOT web_http2_stream_runtime MATCHES
           "class Http2RequestBodyRuntime final" OR
       NOT web_http2_stream_runtime MATCHES
           "class Http2SansIoStreamRuntimeTable final" OR
       NOT web_http2_stream_runtime MATCHES "RequestBodyMode" OR
       NOT web_http2_stream_runtime MATCHES "kModeNotSelected" OR
       NOT web_http2_stream_runtime MATCHES "modeSelected" OR
       NOT web_http2_stream_runtime MATCHES "selectedMode" OR
       NOT web_http2_stream_runtime MATCHES
           "std::optional<RequestBodyMode>" OR
       NOT web_http2_stream_runtime MATCHES
           "std::optional<RouteResolution>" OR
       NOT web_http2_stream_runtime MATCHES "selectRoute" OR
       NOT web_http2_stream_runtime MATCHES "streamingBacklogLimit" OR
       NOT web_http2_stream_runtime MATCHES
           "std::optional<Http2SansIoStreamSignal>" OR
       NOT web_http2_stream_runtime MATCHES
           "friend class Http2SansIoStreamRuntimeTable" OR
       NOT web_http2_stream_runtime MATCHES "beginDispatch" OR
       NOT web_http2_stream_runtime MATCHES "dispatchedCount" OR
       NOT web_http2_stream_runtime MATCHES "void forEach" OR
       NOT web_http2_stream_runtime MATCHES "makePmrObject" OR
       NOT web_http2_ws_transport MATCHES "releaseReceivedData" OR
       NOT web_http2_ws_transport MATCHES "Http2SansIoStreamSignal&" OR
       NOT web_http2_response_stream_sink MATCHES
           "Http2SansIoStreamSignal&" OR
       web_http2_ws_transport MATCHES "Http2SansIoStreamSignal[*]" OR
       web_http2_response_stream_sink MATCHES
           "Http2SansIoStreamSignal[*]" OR
       web_http2_stream_runtime MATCHES
           "${RULE_STALE_HTTP2_BODY_MODE_SPLIT}" OR
       web_http2_ws_transport MATCHES
           "Http2BodyQueue|Http2StreamBodyQueue|class Http2SansIoStreamSignal final")
        boundary_error("HTTP/2 Web stream runtime lost its ownership boundary"
            "route-selected storage, PMR-stable stream state, dispatch signal/lease, Web queues, and consume-time receive-credit release must remain one Web-owned object")
    endif()
endif()
if(EXISTS "${HTTP2_REQUEST_BUILDER}")
    file(READ "${HTTP2_REQUEST_BUILDER}" http2_external_body_builder)
    if(NOT http2_external_body_builder MATCHES
           "std::string_view[ \t\r\n]+body[ \t\r\n]*[)]" OR
       http2_external_body_builder MATCHES
           "std::string_view[ \t\r\n]+body[ \t\r\n]*=" OR
       NOT http2_external_body_builder MATCHES
           "setBody[(]request, body[)]")
        boundary_error("HTTP/2 request builder regained protocol-owned body storage"
            "the external runtime must pass an explicit body view; an empty default would hide the ownership boundary")
    endif()
endif()

set(WEB_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_HTTP2_SERVER_ENTRY
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerCleartextHttp2.h")
set(WEB_HTTP2_SESSION_FIXTURE
    "${RUVIA_ROOT}/tests/http2_sansio_session_fixture.h")
set(WEB_HTTP2_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
if(NOT EXISTS "${WEB_HTTP2_SESSION}" OR
   NOT EXISTS "${WEB_HTTP2_SERVER_ENTRY}" OR
   NOT EXISTS "${WEB_HTTP2_SESSION_FIXTURE}" OR
   NOT EXISTS "${WEB_HTTP2_PACKAGE_CONSUMER}")
    boundary_error("HTTP/2 session wiring contract is incomplete"
        "production context, server entry, test fixture, and installed-package assertion are all required")
else()
    file(READ "${WEB_HTTP2_SESSION}" web_http2_session)
    file(READ "${WEB_HTTP2_SERVER_ENTRY}" web_http2_server_entry)
    file(READ "${WEB_HTTP2_SESSION_FIXTURE}" web_http2_session_fixture)
    file(READ "${WEB_HTTP2_PACKAGE_CONSUMER}" web_http2_package_consumer)
    file(READ "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp"
        web_http2_session_test)
    if(NOT web_http2_session MATCHES
           "class Http2SansIoSessionContext final" OR
       NOT web_http2_session MATCHES "ContextServices services" OR
       NOT web_http2_session MATCHES "const HttpServerOptions& options" OR
       NOT web_http2_session MATCHES
           "ConnectionScanner::Entry& scannerEntry" OR
       NOT web_http2_session MATCHES
           "const std::atomic_bool& serverStarted" OR
       NOT web_http2_session MATCHES
           "const ContextServices& services[(][)]" OR
       NOT web_http2_session MATCHES
           "Http2SansIoSessionContext session" OR
       web_http2_session MATCHES "${RULE_STALE_HTTP2_SESSION_ENV}" OR
       NOT web_http2_server_entry MATCHES
           "Http2SansIoSessionContext[(]" OR
       NOT web_http2_server_entry MATCHES
           "ContextServices services" OR
       web_http2_server_entry MATCHES
           "${RULE_STALE_HTTP2_SESSION_ENV}" OR
       NOT web_http2_session_fixture MATCHES
           "class Http2SansIoSessionFixture final" OR
       NOT web_http2_session_fixture MATCHES
           "runBareHttp2SansIoSession" OR
       NOT web_http2_session_fixture MATCHES
           "runBarePlainHttp2SansIoSession" OR
       NOT web_http2_package_consumer MATCHES
           "!std::is_default_constructible_v<[ \t\r\n]*ruvia::detail::Http2SansIoSessionContext")
        boundary_error("HTTP/2 session restored nullable or test-shaped wiring"
            "the coroutine must receive one non-default context with mandatory options/scanner/shutdown references; bare defaults belong only to tests")
    endif()
    if(NOT web_http2_session MATCHES "event->tunnelData[(][)]" OR
       NOT web_http2_session MATCHES "event->tunnelEnd[(][)]")
        boundary_error("ruvia-web collapses tunnel bytes back into HTTP message content"
            "the HTTP/2 driver must consume the core's dedicated tunnel DATA and FIN events")
    endif()
    if(NOT web_http2_session MATCHES "event->streamClosed[(][)]" OR
       NOT web_http2_session MATCHES "eraseStreamRuntime[(]streamId[)]" OR
       NOT web_http2_session MATCHES "already-removed core state")
        boundary_error("ruvia-web re-derived an already-closed HTTP/2 stream"
            "stream-close cleanup must use the typed event ID without querying erased protocol state")
    endif()
    if(NOT web_http2_session MATCHES
           "Http2SansIoStreamRuntimeTable" OR
       NOT web_http2_session MATCHES
           "streamRuntime->body[(][)][.]store" OR
       NOT web_http2_session MATCHES "requestBodyByteLimit" OR
       NOT web_http2_session MATCHES "releaseReceivedData" OR
       NOT web_http2_session MATCHES "markBufferedBodyCopied" OR
       NOT web_http2_session MATCHES "unmarkBufferedBodyCopied" OR
       NOT web_http2_session MATCHES "resetEventStream" OR
       NOT web_http2_session MATCHES "Owner-side reset" OR
       NOT web_http2_session MATCHES
           "streamRuntimes[.]beginDispatch" OR
       NOT web_http2_session MATCHES "runtime->selectRoute" OR
       NOT web_http2_session MATCHES "routes[.]resolve[(]method, path[)]" OR
       NOT web_http2_session MATCHES
           "streamRuntimes[.]dispatchedCount" OR
       NOT web_http2_session MATCHES "streamRuntimes[.]size" OR
       NOT web_http2_session MATCHES "streamRuntimes[.]forEach" OR
       NOT web_http2_session MATCHES "http2SansIoInactivityPhase" OR
       NOT web_http2_session_test MATCHES
           "sansio_driver_h2_inactivity_phase_counts_predispatch_runtime" OR
       web_http2_session MATCHES
           "${RULE_HTTP2_PARALLEL_WEB_DISPATCH_STATE}" OR
       web_http2_session MATCHES "${RULE_STALE_ROUTE_MODE_SPLIT}" OR
       web_http2_session MATCHES "${RULE_STALE_HTTP2_BODY_MODE_SPLIT}" OR
       web_http2_session MATCHES
           "Http2ConnectionLimits|HttpRequestBodyMode|setBodyMode|usesStreamRequestBody")
        boundary_error("ruvia-web HTTP/2 session bypasses Web-owned body storage"
            "one stable runtime must own route/body/signal/dispatch lease, admission must precede co_spawn, owner resets must reclaim undispatched runtimes, and protocol streams must remain policy-free")
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
    if(NOT web_http1_stream_route MATCHES
       "requestSequence[.]nextResponseClosePolicy[(][)]")
        boundary_error("ruvia-web HTTP/1 stream limit is recomputed after commit"
            "the request limit must enter the pre-commit close policy before response bytes are emitted")
    endif()
    if(NOT web_http1_stream_route MATCHES
       "requestSequence[.]completeCommittedResponse[(]connectionPlan[)]")
        boundary_error("ruvia-web HTTP/1 stream completion bypasses its request sequence"
            "the same connection-private owner must record a successfully committed response")
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
set(RESPONSE_TRAILER_H2_CONNECTION
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
set(RESPONSE_TRAILER_H2_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(RESPONSE_TRAILER_H2_STREAM_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h")
set(RESPONSE_TRAILER_H2_HEADER_BLOCKS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamHeaderBlocks.h")
set(RESPONSE_TRAILER_H2_SINK
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
set(RESPONSE_TRAILER_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
set(RESPONSE_TRAILER_PACKAGE_VERIFY
    "${RUVIA_ROOT}/tests/verify_package_consumers.cmake.in")
if(EXISTS "${RESPONSE_TRAILER_H2_TEST}" AND
   EXISTS "${RESPONSE_TRAILER_H1_TEST}" AND
   EXISTS "${RESPONSE_TRAILER_H2_CONNECTION}" AND
   EXISTS "${RESPONSE_TRAILER_H2_CONNECTION_SOURCE}" AND
   EXISTS "${RESPONSE_TRAILER_H2_STREAM_STATE}" AND
   EXISTS "${RESPONSE_TRAILER_H2_HEADER_BLOCKS}" AND
   EXISTS "${RESPONSE_TRAILER_H2_SINK}" AND
   EXISTS "${RESPONSE_TRAILER_PACKAGE_CONSUMER}" AND
   EXISTS "${RESPONSE_TRAILER_PACKAGE_VERIFY}")
    file(READ "${RESPONSE_TRAILER_H2_TEST}" response_trailer_h2_test)
    file(READ "${RESPONSE_TRAILER_H1_TEST}" response_trailer_h1_test)
    file(READ "${RESPONSE_TRAILER_H2_CONNECTION}"
        response_trailer_h2_connection)
    file(READ "${RESPONSE_TRAILER_H2_CONNECTION_SOURCE}"
        response_trailer_h2_connection_source)
    file(READ "${RESPONSE_TRAILER_H2_STREAM_STATE}"
        response_trailer_h2_stream_state)
    file(READ "${RESPONSE_TRAILER_H2_HEADER_BLOCKS}"
        response_trailer_h2_header_blocks)
    file(READ "${RESPONSE_TRAILER_H2_SINK}"
        response_trailer_h2_sink)
    file(READ "${RESPONSE_TRAILER_PACKAGE_CONSUMER}"
        response_trailer_package_consumer)
    file(READ "${RESPONSE_TRAILER_PACKAGE_VERIFY}"
        response_trailer_package_verify)
    if(NOT response_trailer_h2_connection MATCHES
           "kInvalidTrailerSection" OR
       NOT response_trailer_h2_connection MATCHES
           "std::span<const HttpHeaderView> trailers" OR
       NOT response_trailer_h2_connection_source MATCHES
           "std::pmr::string trailerBlock[(]resource_[)]" OR
       NOT response_trailer_h2_connection_source MATCHES
           "pending[.]trailerBlock[.]swap[(]trailerBlock[)]" OR
       response_trailer_h2_stream_state MATCHES "responseTrailerBlock" OR
       response_trailer_h2_header_blocks MATCHES "responseTrailers" OR
       NOT response_trailer_h2_sink MATCHES
           "finishResponse[(]streamId_, trailers[)]" OR
       NOT response_trailer_h2_sink MATCHES
           "responseTrailerSectionValid[(]trailers[)]" OR
       NOT response_trailer_package_consumer MATCHES
           "AcceptsStagedResponseTrailerSection" OR
       NOT response_trailer_package_consumer MATCHES
           "HasStagedResponseTrailerBlock" OR
       NOT response_trailer_package_verify MATCHES
           "installed HTTP/2 response finish restored staged trailer ownership" OR
       NOT response_trailer_package_verify MATCHES
           "installed Web HTTP/2 sink restored staged trailer submission")
        boundary_error("HTTP/2 response finish lost atomic trailer ownership"
            "the HTTP preflight must preserve pre-commit failure, then finishResponse must receive the whole section, encode detached, queue behind DATA, and expose no per-stream staging API")
    endif()
    if(NOT response_trailer_h2_test MATCHES
           "http2_connection_head_response_can_end_with_trailers_only" OR
       NOT response_trailer_h2_test MATCHES
           "http2_response_finish_owns_trailer_section_atomically" OR
       NOT response_trailer_h2_test MATCHES
           "http2_connection_trailers_wait_for_blocked_body" OR
       NOT response_trailer_h1_test MATCHES
           "http1_stream_commit_plan_exposes_exact_trailer_capability")
        boundary_error("response trailer terminal contract is under-tested"
            "tests must pin H1 framing capability plus H2 trailers-only, phase refusal, atomic validation, and DATA-before-trailers ordering")
    endif()
endif()


set(POOL_WAITER_HEADER
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/PoolWaiterQueue.h")
set(POOL_WAITER_DB_SLOTS "${RUVIA_ROOT}/ruvia-web/src/db/DbPoolSlots.cpp")
set(POOL_WAITER_DB_LIFECYCLE
    "${RUVIA_ROOT}/ruvia-web/src/db/DbPoolLifecycle.cpp")
set(POOL_WAITER_REDIS_SLOTS
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisPoolSlots.cpp")
set(POOL_WAITER_REDIS_LIFECYCLE
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisPoolLifecycle.cpp")
set(POOL_WAITER_TEST "${RUVIA_ROOT}/tests/unit_pool_waiter_queue.cpp")
set(POOL_WAITER_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/core.cpp")
foreach(required IN ITEMS
    "${POOL_WAITER_HEADER}"
    "${POOL_WAITER_DB_SLOTS}"
    "${POOL_WAITER_DB_LIFECYCLE}"
    "${POOL_WAITER_REDIS_SLOTS}"
    "${POOL_WAITER_REDIS_LIFECYCLE}"
    "${POOL_WAITER_TEST}"
    "${POOL_WAITER_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${required}")
        boundary_error("typed pool waiter completion is missing" "${required}")
    endif()
endforeach()
if(EXISTS "${POOL_WAITER_HEADER}" AND
   EXISTS "${POOL_WAITER_DB_SLOTS}" AND
   EXISTS "${POOL_WAITER_DB_LIFECYCLE}" AND
   EXISTS "${POOL_WAITER_REDIS_SLOTS}" AND
   EXISTS "${POOL_WAITER_REDIS_LIFECYCLE}" AND
   EXISTS "${POOL_WAITER_TEST}" AND
   EXISTS "${POOL_WAITER_PACKAGE_CONSUMER}")
    file(READ "${POOL_WAITER_HEADER}" pool_waiter_header)
    file(READ "${POOL_WAITER_DB_SLOTS}" pool_waiter_db_slots)
    file(READ "${POOL_WAITER_DB_LIFECYCLE}" pool_waiter_db_lifecycle)
    file(READ "${POOL_WAITER_REDIS_SLOTS}" pool_waiter_redis_slots)
    file(READ "${POOL_WAITER_REDIS_LIFECYCLE}"
        pool_waiter_redis_lifecycle)
    file(READ "${POOL_WAITER_TEST}" pool_waiter_test)
    file(READ "${POOL_WAITER_PACKAGE_CONSUMER}"
        pool_waiter_package_consumer)
    if(NOT pool_waiter_header MATCHES "class PoolWaiterAcquired final" OR
       NOT pool_waiter_header MATCHES "class PoolWaiterTimedOut final" OR
       NOT pool_waiter_header MATCHES "class PoolWaiterClosed final" OR
       NOT pool_waiter_header MATCHES "class PoolWaiterResult final" OR
       NOT pool_waiter_header MATCHES "using Value = std::variant" OR
       NOT pool_waiter_header MATCHES "std::get_if<PoolWaiterAcquired>" OR
       NOT pool_waiter_header MATCHES "std::get_if<PoolWaiterTimedOut>" OR
       NOT pool_waiter_header MATCHES "std::get_if<PoolWaiterClosed>" OR
       NOT pool_waiter_header MATCHES
           "std::optional<PoolWaiterResult> result_" OR
       NOT pool_waiter_header MATCHES "bool await_ready[(][)] const noexcept" OR
       NOT pool_waiter_header MATCHES
           "void await_suspend[(]std::coroutine_handle<> handle[)] noexcept" OR
       NOT pool_waiter_header MATCHES
           "const PoolWaiterResult& await_resume[(][)] const noexcept" OR
       NOT pool_waiter_header MATCHES "void completeAcquired" OR
       NOT pool_waiter_header MATCHES "void completeTimedOut" OR
       NOT pool_waiter_header MATCHES "void completeClosed" OR
       NOT pool_waiter_header MATCHES "PoolWaiter[*] closedHead" OR
       NOT pool_waiter_header MATCHES "void closeAll[(][)] noexcept")
        boundary_error("pool waiter lost its discriminated await result"
            "pending must remain optional; acquired, timeout, and closure must be exclusive completion alternatives, and closeAll must commit its entire queue before resuming")
    endif()
    if(NOT pool_waiter_db_slots MATCHES
           "const auto& result = co_await waiter" OR
       NOT pool_waiter_db_slots MATCHES "result[.]timedOut[(][)]" OR
       NOT pool_waiter_db_slots MATCHES "result[.]closed[(][)]" OR
       NOT pool_waiter_db_slots MATCHES "result[.]acquired[(][)]" OR
       NOT pool_waiter_redis_slots MATCHES
           "const auto& result = co_await waiter" OR
       NOT pool_waiter_redis_slots MATCHES "result[.]timedOut[(][)]" OR
       NOT pool_waiter_redis_slots MATCHES "result[.]closed[(][)]" OR
       NOT pool_waiter_redis_slots MATCHES "result[.]acquired[(][)]" OR
       NOT pool_waiter_db_lifecycle MATCHES "waiters_[.]closeAll[(][)]" OR
       NOT pool_waiter_redis_lifecycle MATCHES "waiters_[.]closeAll[(][)]")
        boundary_error("DB/Redis pool waits stopped consuming one core completion"
            "both integrations must co_await PoolWaiter and map only its typed timeout, closed, or acquired outcome")
    endif()
    if(NOT pool_waiter_test MATCHES
           "pool_waiter_is_its_own_typed_awaiter" OR
       NOT pool_waiter_test MATCHES
           "pool_waiter_queue_close_all_wakes_with_closed_result" OR
       NOT pool_waiter_test MATCHES
           "observeWaiterThenTryResumeNext" OR
       NOT pool_waiter_test MATCHES "PoolWaiterTimedOut" OR
       NOT pool_waiter_package_consumer MATCHES
           "AcceptsLoosePoolWaiterTuple" OR
       NOT pool_waiter_package_consumer MATCHES
           "AcceptsPoolCloseSentinel" OR
       NOT pool_waiter_package_consumer MATCHES
           "HasParallelPoolWaiterResultAccessor" OR
       NOT pool_waiter_package_consumer MATCHES
           "PoolWaiterResult")
        boundary_error("typed pool waiter completion is insufficiently pinned"
            "runtime tests and installed-core compile contracts must reject the former flags/sentinel tuple")
    endif()
endif()

check_files_no_match("normal responses must not reintroduce a dynamic streaming-body bypass"
    "${RULE_DYNAMIC_RESPONSE_BODY_STREAM}" ${EDGE_REFERENCE_SOURCE})
check_files_no_match("ruvia-http CMake contains stale mixed-responsibility names"
    "src/Streaming\.cpp|include/ruvia/http/(JsonUtils|HttpBodyStream)\.h"
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt")
check_files_no_match("Router/error mapping must not decide HTTP/1 connection persistence"
    "${RULE_ROUTER_CONNECTION_POLICY}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteTable.h"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterMiddlewareDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/Error.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextErrorResponse.cpp")
check_files_no_match("ruvia-core must not contain HTTP/WebSocket semantics"
    "${RULE_CORE_PROTOCOL}" ${CORE_SOURCE})
check_files_no_match("pool wait completion must not restore readiness flags or a close sentinel"
    "${RULE_STALE_POOL_WAITER_TUPLE}"
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/PoolWaiterQueue.h"
    "${RUVIA_ROOT}/ruvia-web/src/db/DbPoolSlots.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/db/DbPoolLifecycle.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisPoolSlots.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisPoolLifecycle.cpp")
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

set(WS_DEFLATE_NEGOTIATION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h")
set(WS_SERVER_NEGOTIATION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WebSocketServerNegotiation.h")
set(WS_H1_HANDSHAKE_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketServerHandshake.h")
set(WS_H2_HANDSHAKE_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2WebSocketHandshake.h")
set(WS_H2_CONNECTION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
set(WS_H1_HANDSHAKE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketHandshake.h")
set(WS_H1_ROUTE_DRIVER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h")
set(WS_H2_ROUTE_DRIVER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
foreach(required IN ITEMS
    "${WS_DEFLATE_NEGOTIATION_HEADER}"
    "${WS_SERVER_NEGOTIATION_HEADER}"
    "${WS_H1_HANDSHAKE_HEADER}"
    "${WS_H2_HANDSHAKE_HEADER}"
    "${WS_H2_CONNECTION_HEADER}"
    "${WS_H1_HANDSHAKE_WRITER}"
    "${WS_H1_ROUTE_DRIVER}"
    "${WS_H2_ROUTE_DRIVER}")
    if(NOT EXISTS "${required}")
        boundary_error("immutable WebSocket server negotiation is missing"
            "${required}")
    endif()
endforeach()
if(EXISTS "${WS_DEFLATE_NEGOTIATION_HEADER}" AND
   EXISTS "${WS_SERVER_NEGOTIATION_HEADER}" AND
   EXISTS "${WS_H1_HANDSHAKE_HEADER}" AND
   EXISTS "${WS_H2_HANDSHAKE_HEADER}" AND
   EXISTS "${WS_H2_CONNECTION_HEADER}" AND
   EXISTS "${WS_H1_HANDSHAKE_WRITER}" AND
   EXISTS "${WS_H1_ROUTE_DRIVER}" AND
   EXISTS "${WS_H2_ROUTE_DRIVER}")
    file(READ "${WS_DEFLATE_NEGOTIATION_HEADER}" ws_deflate_negotiation)
    file(READ "${WS_SERVER_NEGOTIATION_HEADER}" ws_server_negotiation)
    file(READ "${WS_H1_HANDSHAKE_HEADER}" ws_h1_handshake)
    file(READ "${WS_H2_HANDSHAKE_HEADER}" ws_h2_handshake)
    file(READ "${WS_H2_CONNECTION_HEADER}" ws_h2_connection)
    file(READ "${WS_H1_HANDSHAKE_WRITER}" ws_h1_writer)
    file(READ "${WS_H1_ROUTE_DRIVER}" ws_h1_route)
    file(READ "${WS_H2_ROUTE_DRIVER}" ws_h2_route)
    if(NOT ws_deflate_negotiation MATCHES
           "enum class WebSocketDeflateNegotiation" OR
       NOT ws_deflate_negotiation MATCHES "kDisabled" OR
       NOT ws_deflate_negotiation MATCHES "kAccepted" OR
       NOT ws_deflate_negotiation MATCHES
           "kAcceptedWithServerMaxWindowBits" OR
       NOT ws_deflate_negotiation MATCHES "webSocketDeflateNegotiated" OR
       NOT ws_deflate_negotiation MATCHES
           "webSocketDeflateResponseExtensions" OR
       NOT ws_server_negotiation MATCHES
           "class WebSocketServerNegotiation final" OR
       NOT ws_server_negotiation MATCHES "std::string_view subprotocol[(][)]" OR
       NOT ws_server_negotiation MATCHES
           "WebSocketDeflateNegotiation deflate[(][)]" OR
       NOT ws_server_negotiation MATCHES "std::string_view extensions[(][)]" OR
       NOT ws_server_negotiation MATCHES "makeWebSocketServerNegotiation" OR
       NOT ws_h1_handshake MATCHES
           "class HttpWebSocketServerHandshake final" OR
       NOT ws_h1_handshake MATCHES "const WebSocketServerNegotiation&" OR
       NOT ws_h1_handshake MATCHES "makeHttpWebSocketServerHandshake" OR
       NOT ws_h2_handshake MATCHES
           "const WebSocketServerNegotiation& negotiation" OR
       NOT ws_h2_connection MATCHES
           "class Http2SubmittedWebSocketHandshake final" OR
       NOT ws_h2_connection MATCHES
           "class Http2WebSocketHandshakeSubmitFailure final" OR
       NOT ws_h2_connection MATCHES
           "class Http2WebSocketHandshakeSubmitResult final" OR
       NOT ws_h2_connection MATCHES
           "std::get_if<Http2SubmittedWebSocketHandshake>" OR
       NOT ws_h2_connection MATCHES
           "WebSocketServerNegotiation negotiation" OR
       NOT ws_h1_writer MATCHES
           "const HttpWebSocketServerHandshake& handshake" OR
       NOT ws_h1_route MATCHES "makeHttpWebSocketServerHandshake" OR
       NOT ws_h1_route MATCHES
           "handshake[.]negotiation[(][)][.]deflate[(][)]" OR
       NOT ws_h2_route MATCHES "makeWebSocketServerNegotiation" OR
       NOT ws_h2_route MATCHES "handshakeResult[.]submitted[(][)]" OR
       NOT ws_h2_route MATCHES
           "submittedHandshake->negotiation[(][)][.]deflate[(][)]")
        boundary_error("WebSocket server negotiation lost its single committed value"
            "HTTP/1 and RFC 8441 response metadata plus WsConnection compression must consume the same immutable negotiation")
    endif()
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
    if(NOT ws_protocol MATCHES "WebSocketDeflateNegotiation deflate" OR
       NOT ws_protocol_source MATCHES
           "webSocketDeflateNegotiated[(]deflate[)]" OR
       NOT ws_runtime MATCHES "WebSocketDeflateNegotiation deflate" OR
       NOT ws_runtime MATCHES "protocol_[(]buffer_, maxMessageBytes, deflate[)]")
        boundary_error("WebSocket frame core lost the typed negotiation handoff"
            "the runtime and sans-I/O core must consume the committed deflate alternative, never a boolean")
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

set(WS_H1_HANDSHAKE_TEST
    "${RUVIA_ROOT}/tests/unit_websocket_handshake.cpp")
set(WS_H2_HANDSHAKE_TEST
    "${RUVIA_ROOT}/tests/unit_http2_websocket_handshake.cpp")
set(WS_H2_CONNECTION_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
if(EXISTS "${WS_H1_HANDSHAKE_TEST}" AND
   EXISTS "${WS_H2_HANDSHAKE_TEST}" AND
   EXISTS "${WS_H2_CONNECTION_TEST}" AND
   EXISTS "${WS_RUNTIME_TEST}" AND
   EXISTS "${WS_PACKAGE_CONSUMER}")
    file(READ "${WS_H1_HANDSHAKE_TEST}" ws_h1_handshake_test)
    file(READ "${WS_H2_HANDSHAKE_TEST}" ws_h2_handshake_test)
    file(READ "${WS_H2_CONNECTION_TEST}" ws_h2_connection_test)
    file(READ "${WS_RUNTIME_TEST}" ws_negotiation_runtime_test)
    file(READ "${WS_PACKAGE_CONSUMER}" ws_negotiation_package_consumer)
    if(NOT ws_h1_handshake_test MATCHES
           "ws_server_handshake_response_serialization_is_http_owned" OR
       NOT ws_h1_handshake_test MATCHES
           "kAcceptedWithServerMaxWindowBits" OR
       NOT ws_h2_handshake_test MATCHES
           "makeWebSocketServerNegotiation" OR
       NOT ws_h2_connection_test MATCHES
           "duplicateHandshakeResult[.]failure[(][)]->error[(][)]" OR
       NOT ws_negotiation_runtime_test MATCHES
           "WebSocketDeflateNegotiation::kAccepted" OR
       NOT ws_negotiation_package_consumer MATCHES
           "HasLooseWebSocketDeflateFields" OR
       NOT ws_negotiation_package_consumer MATCHES
           "HasLooseWebSocketNegotiationFields" OR
       NOT ws_negotiation_package_consumer MATCHES
           "AcceptsLooseWebSocketHandshakeSubmit" OR
       NOT ws_negotiation_package_consumer MATCHES
           "Http2WebSocketHandshakeSubmitResult")
        boundary_error("immutable WebSocket server negotiation is insufficiently tested"
            "H1/H2 serialization, committed submission, typed frame handoff, and installed compile contracts must stay pinned")
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
check_files_no_match("README must describe the current product, not migration history"
    "former implementation|former API|no longer|was removed|migration history|旧实现|迁移历史"
    "${RUVIA_ROOT}/README.md")

# Keep the two root documents intentionally small and role-specific. Protocol
# implementation invariants belong to source, unit tests, package consumers, and
# the checks above; requiring every internal type name in both documents caused
# them to become duplicate change logs.
file(READ "${RUVIA_ROOT}/README.md" readme_content)
file(READ "${RUVIA_ROOT}/AGENTS.md" agents_content)
string(REGEX REPLACE "[^\n]" "" readme_newlines "${readme_content}")
string(REGEX REPLACE "[^\n]" "" agents_newlines "${agents_content}")
string(LENGTH "${readme_newlines}" readme_line_count)
string(LENGTH "${agents_newlines}" agents_line_count)
if(NOT readme_content STREQUAL "" AND NOT readme_content MATCHES "\n$")
    math(EXPR readme_line_count "${readme_line_count} + 1")
endif()
if(NOT agents_content STREQUAL "" AND NOT agents_content MATCHES "\n$")
    math(EXPR agents_line_count "${agents_line_count} + 1")
endif()
if(readme_line_count GREATER 400)
    boundary_error("README exceeded its user-document scope"
        "README.md has ${readme_line_count} lines; keep it under 400 and move executable invariants to tests/guards")
endif()
if(agents_line_count GREATER 400)
    boundary_error("AGENTS exceeded its contributor-guide scope"
        "AGENTS.md has ${agents_line_count} lines; keep it under 400 and do not append per-refactor type catalogs")
endif()

if(NOT readme_content MATCHES "## Targets" OR
   NOT readme_content MATCHES "ruvia::core" OR
   NOT readme_content MATCHES "ruvia::http" OR
   NOT readme_content MATCHES "ruvia::web" OR
   NOT readme_content MATCHES "## Build" OR
   NOT readme_content MATCHES "## Install and Consume" OR
   NOT readme_content MATCHES "## Minimal Web App" OR
   NOT readme_content MATCHES
       "VCPKG_ROOT/scripts/buildsystems/vcpkg[.]cmake")
    boundary_error("README lost its user-facing contract"
        "README must retain targets, build/install guidance, a minimal app, and VCPKG_ROOT-based configuration")
endif()
if(NOT agents_content MATCHES "README 面向使用者" OR
   NOT agents_content MATCHES "AGENTS 面向贡献者" OR
   NOT agents_content MATCHES "## 目录规则" OR
   NOT agents_content MATCHES "## Target 边界" OR
   NOT agents_content MATCHES "ruvia-web  -> ruvia-core [+] ruvia-http" OR
   NOT agents_content MATCHES "## 性能原则" OR
   NOT agents_content MATCHES "## 验证要求" OR
   NOT agents_content MATCHES
       "VCPKG_ROOT/scripts/buildsystems/vcpkg[.]cmake")
    boundary_error("AGENTS lost its contributor-guide contract"
        "AGENTS must retain document roles, directory/target/dependency rules, performance constraints, verification, and VCPKG_ROOT guidance")
endif()
get_property(boundary_failed GLOBAL PROPERTY RUVIA_BOUNDARY_FAILED)
if(boundary_failed)
    message(FATAL_ERROR "Ruvia layer-boundary checks failed")
endif()
message(STATUS "layer boundaries OK")
