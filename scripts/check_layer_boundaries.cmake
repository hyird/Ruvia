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
set(RULE_WEB_HANDSHAKE_PROTOCOL_BYTES
    "kHttpWebSocket(SwitchingProtocolsPrefix|SubprotocolHeaderPrefix|ExtensionsHeaderPrefix)|kHttpCrlf|kHttp2UpgradeResponse(Prefix|Terminator)|cachedDateHeader|asio::buffer\\(\"\\\\r\\\\n")
set(RULE_WEB_HTTP2_TRAILER_PROTOCOL
    "HpackEncoder|httpAsciiToLower|lowerName_|HPACK-encoded")
set(RULE_WEB_TRAILER_PROTOCOL_VALIDATION "responseTrailerFieldValid")
set(RULE_WEB_WS_SPLIT_FRAME_TRANSPORT
    "writeFrame[ \t]*\\(|std::string_view[ \t]+header")
set(RULE_WEB_HTTP1_STREAM_PLAN
    "ResponseStreamFraming::|httpVersion[ \t]*\\(|parsed\\.contentLength[ \t]*==|![ \t]*parsed\\.chunked|isHttp11")
set(RULE_WEB_HTTP1_RESPONSE_FINALIZATION
    "http1ResponseWantsClose|http1MarkConnection(Close|KeepAlive)IfNeeded")
set(RULE_WEB_RESPONSE_BODY_PROTOCOL_BOOL
    "skipBody|bodyForbidden")
set(RULE_WEB_H2_RESPONSE_PLAN_DUPLICATION
    "responseWritePolicy|responseBodySize\\(response\\)|responseFileBody\\(response\\)\\.length")
set(RULE_WEB_HEAD_BODY_DECISION
    "request\\.method\\(\\)[ \t]*==[ \t]*HttpMethod::kHead")
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
    "HttpClientConfig|tlsOptions|caFile|insecureSkipVerify|certificateChainFile|privateKeyFile|privateKeyPassword|sniHost|poolSizePerWorker|proxyConnectTimeout|proxyReadTimeout|proxySendTimeout|acquireTimeout|maxResponseBodyBytes|milliseconds[ \t]+timeout")
set(RULE_SCANNER_SEMANTICS
    "http|websocket|client_header|client_body|send_timeout|keepalive")
set(RULE_HTTP1_CONNECTION "Http1Connection")
set(RULE_DELETED_H2_SESSION "Http2ServerSession")
set(RULE_STALE_DEPENDENCY
    "ruvia-web[ \t]*->[ \t]*ruvia-http[ \t]*->[ \t]*ruvia-core|http[ \t]*->[ \t]*core|http[^\r\n]*(asio/TLS|socket/TLS)[ \t]*runtime driver")

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
    expect_match("Upgrade handshake serialization in Web runtime"
        "${RULE_WEB_HANDSHAKE_PROTOCOL_BYTES}"
        "asio::buffer(kHttp2UpgradeResponsePrefix)")
    expect_match("HTTP/2 trailer encoding in Web runtime"
        "${RULE_WEB_HTTP2_TRAILER_PROTOCOL}"
        "HpackEncoder::encodeHeader(trailers, name, value)")
    expect_match("response trailer protocol validation in Web state"
        "${RULE_WEB_TRAILER_PROTOCOL_VALIDATION}"
        "responseTrailerFieldValid(name, value)")
    expect_match("split WebSocket frame transport contract"
        "${RULE_WEB_WS_SPLIT_FRAME_TRANSPORT}"
        "writeFrame(std::string_view header, std::string_view payload)")
    expect_match("HTTP/1 stream planning in Web runtime"
        "${RULE_WEB_HTTP1_STREAM_PLAN}"
        "const bool isHttp11 = request.httpVersion() == \"HTTP/1.1\";")
    expect_match("HTTP/1 response finalization in Web runtime"
        "${RULE_WEB_HTTP1_RESPONSE_FINALIZATION}"
        "if (http1ResponseWantsClose(response)) keepAlive = false;")
    expect_match("loose response body protocol bool in Web runtime"
        "${RULE_WEB_RESPONSE_BODY_PROTOCOL_BOOL}"
        "const bool skipBody = request.method() == HttpMethod::kHead;")
    expect_match("duplicated HTTP/2 response plan in Web runtime"
        "${RULE_WEB_H2_RESPONSE_PLAN_DUPLICATION}"
        "const auto policy = responseWritePolicy(response.status());")
    expect_match("HEAD response body decision in Web runtime"
        "${RULE_WEB_HEAD_BODY_DECISION}"
        "request.method() == HttpMethod::kHead")
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
    expect_match("protocol semantics in core scanner" "${RULE_SCANNER_SEMANTICS}"
        "client_header_timeout")
    expect_match("parallel HTTP/1 state machine" "${RULE_HTTP1_CONNECTION}"
        "class Http1Connection {};")
    expect_match("deleted HTTP/2 session in docs" "${RULE_DELETED_H2_SESSION}"
        "Http2ServerSession owns the socket")
    expect_match("stale dependency description" "${RULE_STALE_DEPENDENCY}"
        "ruvia-web -> ruvia-http -> ruvia-core")
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
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClientTypes.h")
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
check_files_no_match("ruvia-web handshake writers must only submit HTTP-owned parts"
    "${RULE_WEB_HANDSHAKE_PROTOCOL_BYTES}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2UpgradeHandshake.h"
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
check_files_no_match("ruvia-web must drive the HTTP-owned HTTP/1 stream plan"
    "${RULE_WEB_HTTP1_STREAM_PLAN}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h")
check_files_no_match("ruvia-web response state must use HTTP-owned persistence finalization"
    "${RULE_WEB_HTTP1_RESPONSE_FINALIZATION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseState.h")
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
    if(NOT web_h2_session MATCHES "writePlan\\.sendBody")
        boundary_error("ruvia-web HTTP/2 runtime bypasses the HTTP-owned send-body verdict"
            "Http2SansIoSession.h must drive writePlan.sendBody()")
    endif()
endif()

set(HTTP2_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
if(EXISTS "${HTTP2_CONNECTION_SOURCE}")
    file(READ "${HTTP2_CONNECTION_SOURCE}" http2_connection_source)
    if(NOT http2_connection_source MATCHES "stream->localEndStream\\(\\)")
        boundary_error("HTTP/2 core does not enforce the local END_STREAM lifecycle"
            "Http2Connection::submitData must reject a locally ended stream")
    endif()
endif()

set(WEB_HTTP1_STREAM_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h")
if(EXISTS "${WEB_HTTP1_STREAM_ROUTE}")
    file(READ "${WEB_HTTP1_STREAM_ROUTE}" web_http1_stream_route)
    if(NOT web_http1_stream_route MATCHES "http1PlanResponseStream")
        boundary_error("ruvia-web HTTP/1 stream route bypasses the protocol plan"
            "HttpServerResponseStreamRoute.h must call http1PlanResponseStream")
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
    if(NOT boundary_doc_content MATCHES "HttpResponseBodyPlan" OR
       NOT boundary_doc_content MATCHES "HttpBufferedResponseWritePlan")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${boundary_doc}")
        boundary_error("response body-plan boundary is undocumented"
            "${relative} must describe HTTP-owned response write plans")
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

set(WS_RUNTIME_HEADER "${RUVIA_ROOT}/ruvia-web/src/websocket/HttpWebSocketConnection.h")
if(EXISTS "${WS_RUNTIME_HEADER}")
    file(READ "${WS_RUNTIME_HEADER}" ws_runtime)
    if(NOT ws_runtime MATCHES "WsConnection")
        boundary_error("ruvia-web WebSocket runtime does not drive WsConnection"
            "missing WsConnection in ruvia-web/src/websocket/HttpWebSocketConnection.h")
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

set(HTTP_RUNTIME_STATE "${RUVIA_ROOT}/ruvia-web/src/server/HttpConnectionState.h")
set(HTTP_BODY_READER "${RUVIA_ROOT}/ruvia-web/src/body/HttpStreamBodyReader.h")
if(EXISTS "${HTTP_RUNTIME_STATE}" AND EXISTS "${HTTP_BODY_READER}")
    file(READ "${HTTP_RUNTIME_STATE}" runtime_state)
    file(READ "${HTTP_BODY_READER}" body_reader)
    if(NOT runtime_state MATCHES "HttpServerParser" OR
       NOT body_reader MATCHES "HttpChunkDecoder")
        boundary_error("ruvia-web HTTP/1 runtime is not driving http-owned primitives"
            "expected HttpServerParser and HttpChunkDecoder in web runtime state")
    endif()
endif()

set(BOUNDARY_DOCS "${RUVIA_ROOT}/README.md" "${RUVIA_ROOT}/AGENTS.md")
check_files_no_match("docs reference the deleted coroutine h2 server session"
    "${RULE_DELETED_H2_SESSION}" ${BOUNDARY_DOCS})
check_files_no_match("docs contain stale dependency/runtime ownership"
    "${RULE_STALE_DEPENDENCY}" ${BOUNDARY_DOCS})

get_property(boundary_failed GLOBAL PROPERTY RUVIA_BOUNDARY_FAILED)
if(boundary_failed)
    message(FATAL_ERROR "Ruvia layer-boundary checks failed")
endif()
message(STATUS "layer boundaries OK")
