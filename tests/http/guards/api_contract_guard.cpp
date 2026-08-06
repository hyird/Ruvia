#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <ruvia/http/HttpCache.h>
#include <ruvia/http/Cookies.h>
#include <ruvia/http/ProtocolByteLimit.h>
#include <ruvia/http/HttpHeader.h>
#include <ruvia/http/HttpClient.h>
#include <ruvia/http/HttpClientRedirect.h>
#include <ruvia/http/Http1ClientRequestWriter.h>
#include <ruvia/http/Http1ClientResponseParser.h>
#include <ruvia/http/Http1InterimResponseWriter.h>
#include <ruvia/http/Http1RequestParser.h>
#include <ruvia/http/HttpInterimResponse.h>
#include <ruvia/http/HttpKnownMethod.h>
#include <ruvia/http/HttpProtocolError.h>
#include <ruvia/http/HttpProtocolVersion.h>
#include <ruvia/http/HttpRequest.h>
#include <ruvia/http/HttpResponse.h>
#include <ruvia/http/MultipartParser.h>
#include <ruvia/http/Sse.h>
#include <ruvia/http/UrlEncoding.h>
#include <ruvia/http/detail/util/AsciiCase.h>
#include <ruvia/http/detail/util/BorrowedView.h>
#include <ruvia/http/detail/cookie/CookieValidation.h>
#include <ruvia/http/detail/field/HttpMediaType.h>
#include <ruvia/http/detail/field/HttpQualityValue.h>
#include <ruvia/http/detail/field/HeaderTokenUtils.h>
#include <ruvia/http/detail/field/HttpByteRange.h>
#include <ruvia/http/detail/coding/HttpContentCoding.h>
#include <ruvia/http/detail/coding/HttpAcceptEncoding.h>
#include <ruvia/http/detail/coding/HttpContentLength.h>
#include <ruvia/http/detail/field/HttpEntityTag.h>
#include <ruvia/http/detail/field/HttpExpectations.h>
#include <ruvia/http/detail/util/HttpOws.h>
#include <ruvia/http/detail/request/HttpRequestBodyFailure.h>
#include <ruvia/http/detail/coding/HttpRequestContentSemantics.h>
#include <ruvia/http/detail/coding/HttpTransferEncoding.h>
#include <ruvia/http/detail/response/HttpResponseBody.h>
#include <ruvia/http/detail/response/HttpResponseBodyAccess.h>
#include <ruvia/http/detail/coding/HttpResponseContentSemantics.h>
#include <ruvia/http/detail/response/HttpResponseFileBody.h>
#include <ruvia/http/detail/request/RequestBodyDecoding.h>
#include <ruvia/http/detail/client/HttpClientContentEncoding.h>
#include <ruvia/http/detail/client/HttpOrigin.h>
#include <ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h>
#include <ruvia/http/detail/http1/Http1ChunkedFraming.h>
#include <ruvia/http/detail/coding/HttpTransferCodingDecoder.h>
#include <ruvia/http/detail/http1/Http1ResponseHeadPlan.h>
#include <ruvia/http/detail/http1/Http1ServerRequestParser.h>
#include <ruvia/http/detail/http1/Http1ServerSemantics.h>
#include <ruvia/http/detail/http2/Http2Connection.h>
#include <ruvia/http/detail/http2/stream/Http2ClosedStreams.h>
#include <ruvia/http/detail/http2/Http2Event.h>
#include <ruvia/http/detail/http2/frame/Http2FramePayload.h>
#include <ruvia/http/detail/http2/hpack/Http2HeaderList.h>
#include <ruvia/http/detail/http2/hpack/Http2Hpack.h>
#include <ruvia/http/detail/http2/message/Http2LocalSendState.h>
#include <ruvia/http/detail/http2/frame/Http2OutputBuffer.h>
#include <ruvia/http/detail/http2/settings/Http2PeerSettings.h>
#include <ruvia/http/detail/http2/frame/Http2PayloadSlice.h>
#include <ruvia/http/detail/http2/message/Http2RemoteContentState.h>
#include <ruvia/http/detail/http2/message/Http2RemoteReceiveState.h>
#include <ruvia/http/detail/http2/message/Http2RequestBuilder.h>
#include <ruvia/http/detail/http2/message/Http2ResponseHeadPlan.h>
#include <ruvia/http/detail/http2/stream/Http2StreamHeaderBlocks.h>
#include <ruvia/http/detail/http2/stream/Http2StreamLifecycle.h>
#include <ruvia/http/detail/http2/stream/Http2StreamRequestData.h>
#include <ruvia/http/detail/http2/stream/Http2StreamRequestState.h>
#include <ruvia/http/detail/http2/stream/Http2StreamState.h>
#include <ruvia/http/detail/http2/stream/Http2StreamTable.h>
#include <ruvia/http/detail/http2/stream/Http2TunnelState.h>
#include <ruvia/http/detail/parser/MultipartPartAccess.h>
#include <ruvia/http/detail/parser/MimeFieldGrammar.h>
#include <ruvia/http/detail/parser/MultipartBoundary.h>
#include <ruvia/http/detail/parser/MultipartDelimiter.h>
#include <ruvia/http/detail/parser/MultipartPartHeaders.h>
#include <ruvia/http/detail/parser/MultipartStreamPartAccess.h>
#include <ruvia/http/detail/cookie/SetCookiePlan.h>
#include <ruvia/http/detail/parser/HttpChunkParser.h>
#include <ruvia/http/detail/parser/HttpHeaderBlockParser.h>
#include <ruvia/http/detail/parser/HttpRequestTarget.h>
#include <ruvia/http/detail/server/HttpFinalResponseControlPlan.h>
#include <ruvia/http/detail/server/HttpResponseHeadBuffer.h>
#include <ruvia/http/detail/server/HttpResponseWritePlan.h>
#include <ruvia/http/detail/websocket/handshake/HttpWebSocketServerHandshake.h>
#include <ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeFields.h>
#include <ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeValidation.h>
#include <ruvia/http/detail/websocket/message/HttpWebSocketMessageAccess.h>
#include <ruvia/http/detail/websocket/frame/HttpWebSocketClosePayload.h>
#include <ruvia/http/detail/websocket/frame/HttpWebSocketFrameCodec.h>
#include <ruvia/http/detail/websocket/frame/HttpWebSocketFrameReader.h>
#include <ruvia/http/detail/websocket/frame/HttpWebSocketFrameView.h>
#include <ruvia/http/detail/websocket/message/HttpWebSocketInboundAssembler.h>
#include <ruvia/http/detail/websocket/frame/HttpWebSocketPayloadValidation.h>
#include <ruvia/http/detail/websocket/handshake/WebSocketServerNegotiation.h>
#include <ruvia/http/detail/websocket/WsConnection.h>
#include <ruvia/http/detail/websocket/WsEvent.h>

template <typename T>
concept HasLegacyResponseBodyCopy = requires(T& response) { response.setBodyCopy(std::string_view{}); };

static_assert(ruvia::detail::httpBorrowedCStringView(nullptr).empty());
static_assert(ruvia::CookieOptions::BorrowedText(nullptr).empty());
static_assert(ruvia::SseMessage::BorrowedText(nullptr).empty());
static_assert(ruvia::HttpClientRequest::BorrowedText(nullptr).view().empty());

struct MatchAnyHeaderToken final {
    [[nodiscard]] constexpr bool operator()(std::string_view) const noexcept {
        return true;
    }
};

template <typename Input>
concept AcceptsAnyBorrowedHttpSubviewInput = requires(Input&& input) { ruvia::detail::httpTrimOws(std::forward<Input>(input)); } || requires(Input&& input) { ruvia::detail::httpTrimQuotes(std::forward<Input>(input)); } || requires(Input&& input) { ruvia::detail::httpFindHeaderToken(std::forward<Input>(input), MatchAnyHeaderToken{}); } || requires(Input&& input) { ruvia::detail::httpHeaderTokenBeforeParameters(std::forward<Input>(input)); } || requires(Input&& input) { ruvia::detail::httpMediaTypeOnly(std::forward<Input>(input)); } || requires(Input&& input) { ruvia::detail::httpTrimWeakEtagPrefix(std::forward<Input>(input)); } || requires(const ruvia::HttpRequest& request, Input&& input) { ruvia::detail::chooseWebSocketSubprotocol(request, std::forward<Input>(input)); };

template <typename Input>
concept AcceptsAllBorrowedHttpSubviewInputs = requires(const ruvia::HttpRequest& request, Input&& input) {
    ruvia::detail::httpTrimOws(std::forward<Input>(input));
    ruvia::detail::httpTrimQuotes(std::forward<Input>(input));
    ruvia::detail::httpFindHeaderToken(std::forward<Input>(input), MatchAnyHeaderToken{});
    ruvia::detail::httpHeaderTokenBeforeParameters(std::forward<Input>(input));
    ruvia::detail::httpMediaTypeOnly(std::forward<Input>(input));
    ruvia::detail::httpTrimWeakEtagPrefix(std::forward<Input>(input));
    ruvia::detail::chooseWebSocketSubprotocol(request, std::forward<Input>(input));
};

static_assert(!AcceptsAnyBorrowedHttpSubviewInput<std::string>);
static_assert(!AcceptsAnyBorrowedHttpSubviewInput<const std::string>);
static_assert(!AcceptsAnyBorrowedHttpSubviewInput<std::pmr::string>);
static_assert(AcceptsAllBorrowedHttpSubviewInputs<std::string&>);
static_assert(AcceptsAllBorrowedHttpSubviewInputs<std::pmr::string&>);
static_assert(AcceptsAllBorrowedHttpSubviewInputs<std::string_view>);

template <typename Input>
concept AcceptsAnyBorrowedHttpParserOutputInput = requires(Input&& input, ruvia::detail::HttpMediaTypeParts& mediaType) { ruvia::detail::httpParseMediaTypeParts(std::forward<Input>(input), false, mediaType); } || requires(Input&& input, ruvia::detail::HttpMediaTypeParts& mediaType) { ruvia::detail::httpParseMediaType(std::forward<Input>(input), false, mediaType); } || requires(Input&& input, std::string_view& first, std::string_view& second) { ruvia::detail::httpParseMimeParameter(std::forward<Input>(input), first, second); } || requires(Input&& input, std::string_view& first, bool& flag) { ruvia::detail::httpParseTransferCodingSyntax(std::forward<Input>(input), first, flag); } || requires(Input&& input, ruvia::detail::HttpUpgradeProtocol& protocol) { ruvia::detail::httpParseUpgradeProtocol(std::forward<Input>(input), protocol); } || requires(Input&& input, const ruvia::detail::Http2FrameHeader& frame, std::string_view& first) { ruvia::detail::http2StripPadAndPriority(frame, std::forward<Input>(input), false, first); } || requires(Input&& input, const ruvia::detail::Http2FrameHeader& frame, std::string_view& first) { ruvia::detail::http2DecodeHeadersPayload(frame, std::forward<Input>(input), first); } || requires(Input&& input, const ruvia::detail::Http2FrameHeader& frame, std::string_view& first) { ruvia::detail::http2DecodeDataPayload(frame, std::forward<Input>(input), first); };

template <typename Input>
concept AcceptsAllBorrowedHttpParserOutputInputs = requires(Input&& input, ruvia::detail::HttpMediaTypeParts& mediaType, std::string_view& first, std::string_view& second, bool& flag, ruvia::detail::HttpUpgradeProtocol& protocol, const ruvia::detail::Http2FrameHeader& frame) {
    ruvia::detail::httpParseMediaTypeParts(std::forward<Input>(input), false, mediaType);
    ruvia::detail::httpParseMediaType(std::forward<Input>(input), false, mediaType);
    ruvia::detail::httpParseMimeParameter(std::forward<Input>(input), first, second);
    ruvia::detail::httpParseTransferCodingSyntax(std::forward<Input>(input), first, flag);
    ruvia::detail::httpParseUpgradeProtocol(std::forward<Input>(input), protocol);
    ruvia::detail::http2StripPadAndPriority(frame, std::forward<Input>(input), false, first);
    ruvia::detail::http2DecodeHeadersPayload(frame, std::forward<Input>(input), first);
    ruvia::detail::http2DecodeDataPayload(frame, std::forward<Input>(input), first);
};

static_assert(!AcceptsAnyBorrowedHttpParserOutputInput<std::string>);
static_assert(!AcceptsAnyBorrowedHttpParserOutputInput<const std::string>);
static_assert(!AcceptsAnyBorrowedHttpParserOutputInput<std::pmr::string>);
static_assert(AcceptsAllBorrowedHttpParserOutputInputs<std::string&>);
static_assert(AcceptsAllBorrowedHttpParserOutputInputs<std::pmr::string&>);
static_assert(AcceptsAllBorrowedHttpParserOutputInputs<std::string_view>);

template <typename Text>
concept AcceptsSseData = requires(Text&& text) { ruvia::SseMessage{.data = std::forward<Text>(text)}; };

template <typename Text>
concept AcceptsSseEvent = requires(Text&& text) { ruvia::SseMessage{.event = std::forward<Text>(text)}; };

template <typename Text>
concept AcceptsSseId = requires(Text&& text) { ruvia::SseMessage{.id = std::forward<Text>(text)}; };

template <typename Text>
concept AcceptsAnySseTextAssignment = requires(ruvia::SseMessage& message, Text&& text) { message.data = std::forward<Text>(text); } || requires(ruvia::SseMessage& message, Text&& text) { message.event = std::forward<Text>(text); } || requires(ruvia::SseMessage& message, Text&& text) { message.id = std::forward<Text>(text); };

template <typename Text>
concept AcceptsAllSseTextAssignments = requires(ruvia::SseMessage& message, Text&& text) {
    message.data = std::forward<Text>(text);
    message.event = std::forward<Text>(text);
    message.id = std::forward<Text>(text);
};

static_assert(!AcceptsSseData<std::string>);
static_assert(!AcceptsSseData<const std::string>);
static_assert(!AcceptsSseData<std::pmr::string>);
static_assert(AcceptsSseData<std::string&>);
static_assert(AcceptsSseData<std::pmr::string&>);
static_assert(AcceptsSseData<std::string_view>);
static_assert(!AcceptsSseEvent<std::string>);
static_assert(!AcceptsSseEvent<const std::string>);
static_assert(!AcceptsSseEvent<std::pmr::string>);
static_assert(AcceptsSseEvent<std::string&>);
static_assert(AcceptsSseEvent<std::pmr::string&>);
static_assert(AcceptsSseEvent<std::string_view>);
static_assert(!AcceptsSseId<std::string>);
static_assert(!AcceptsSseId<const std::string>);
static_assert(!AcceptsSseId<std::pmr::string>);
static_assert(AcceptsSseId<std::string&>);
static_assert(AcceptsSseId<std::pmr::string&>);
static_assert(AcceptsSseId<std::string_view>);
static_assert(!AcceptsAnySseTextAssignment<std::string>);
static_assert(!AcceptsAnySseTextAssignment<const std::string>);
static_assert(!AcceptsAnySseTextAssignment<std::pmr::string>);
static_assert(AcceptsAllSseTextAssignments<std::string&>);
static_assert(AcceptsAllSseTextAssignments<std::pmr::string&>);
static_assert(AcceptsAllSseTextAssignments<std::string_view>);
static_assert(std::is_aggregate_v<ruvia::SseMessage>);
constexpr ruvia::SseMessage kLiteralSseMessage{.data = "data", .event = "event", .id = "id"};
static_assert(kLiteralSseMessage.data->view() == "data");
static_assert(kLiteralSseMessage.event == "event");
static_assert("event" == kLiteralSseMessage.event);
static_assert(kLiteralSseMessage.id->view() == "id");

template <typename T>
concept ExposesRvalueHttpRequestHeaders = requires(T&& request) { std::move(request).headers(); };

static_assert(!ExposesRvalueHttpRequestHeaders<ruvia::HttpRequest>);

template <typename T>
concept ExposesRvalueHttp1ChunkHeaderView = requires(T&& header) { std::move(header).view(); };

template <typename T>
concept ExposesRvalueResponseHeadBufferStorage = requires(T&& buffer) { std::move(buffer).view(); } || requires(T&& buffer) { std::move(buffer).stackCursor(std::size_t{}); };

template <typename T>
concept ExposesRvalueHttp2OutputBuffer = requires(T&& output) { std::move(output).pending(); };

template <typename T>
concept ExposesRvalueHttp2ConnectionStorage = requires(T&& connection) { std::move(connection).pendingOutput(); } || requires(T&& connection) { std::move(connection).takeDrainedDataStreams(); } || requires(T&& connection) { std::move(connection).stream(std::uint32_t{}); };

template <typename T>
concept ExposesRvalueEncodedClosePayloadBytes = requires(T&& payload) { std::move(payload).bytes(); };

static_assert(!ExposesRvalueHttp1ChunkHeaderView<ruvia::detail::Http1ChunkHeader>);
static_assert(!ExposesRvalueResponseHeadBufferStorage<ruvia::detail::ResponseHeadBuffer>);
static_assert(!ExposesRvalueHttp2OutputBuffer<ruvia::detail::Http2OutputBuffer>);
static_assert(!ExposesRvalueHttp2ConnectionStorage<ruvia::detail::Http2Connection>);
static_assert(!ExposesRvalueEncodedClosePayloadBytes<ruvia::detail::WebSocketEncodedClosePayload>);

template <typename T>
concept ExposesRvalueHttp2HeaderListStorage = requires(T&& list) { std::move(list).at(std::size_t{}); };

template <typename T>
concept ExposesRvalueHttp2StreamRequestDataStorage = requires(T&& data) { std::move(data).method(); } || requires(T&& data) { std::move(data).scheme(); } || requires(T&& data) { std::move(data).authority(); } || requires(T&& data) { std::move(data).path(); } || requires(T&& data) { std::move(data).protocol(); } || requires(T&& data) { std::move(data).cookie(); } || requires(T&& data) { std::move(data).headerAt(std::size_t{}); };

template <typename T>
concept ExposesRvalueHttp2StreamRequestStateStorage = requires(T&& state) { std::move(state).responseStatus(); };

template <typename T>
concept ExposesRvalueHttp2StreamHeaderBlocksStorage = requires(T&& blocks) { std::move(blocks).request(); } || requires(const T&& blocks) { std::move(blocks).request(); } || requires(T&& blocks) { std::move(blocks).response(); } || requires(const T&& blocks) { std::move(blocks).response(); };

template <typename T>
concept ExposesRvalueHttp2StreamLifecycleStorage = requires(T&& lifecycle) { std::move(lifecycle).localSend(); } || requires(T&& lifecycle) { std::move(lifecycle).remoteReceive(); };

template <typename T>
concept ExposesRvalueHttp2StreamStateStorage = requires(T&& stream) { std::move(stream).receiveWindowCredit(); } || requires(T&& stream) { std::move(stream).requestHeaderBlock(); } || requires(const T&& stream) { std::move(stream).requestHeaderBlock(); } || requires(T&& stream) { std::move(stream).responseHeaderBlock(); } || requires(const T&& stream) { std::move(stream).responseHeaderBlock(); } || requires(T&& stream) { std::move(stream).remoteContent(); } || requires(T&& stream) { std::move(stream).localContent(); } || requires(T&& stream) { std::move(stream).localSend(); } || requires(T&& stream) { std::move(stream).remoteReceive(); } || requires(T&& stream) { std::move(stream).requestMethod(); } || requires(T&& stream) { std::move(stream).requestAuthority(); } || requires(T&& stream) { std::move(stream).requestPath(); } || requires(T&& stream) { std::move(stream).requestProtocol(); } || requires(T&& stream) { std::move(stream).requestCookie(); } || requires(T&& stream) { std::move(stream).requestHeaderAt(std::size_t{}); } || requires(T&& stream) { std::move(stream).requestScheme(); } || requires(T&& stream) { std::move(stream).tunnel(); } || requires(T&& stream) { std::move(stream).responseStatus(); };

template <typename T>
concept ExposesRvalueHttp2StreamTableStorage = requires(T&& table) { std::move(table).find(std::uint32_t{}); } || requires(const T&& table) { std::move(table).find(std::uint32_t{}); } || requires(T&& table) { std::move(table).create(std::uint32_t{}, std::int32_t{}); };

static_assert(!ExposesRvalueHttp2HeaderListStorage<ruvia::detail::Http2HeaderList>);
static_assert(!ExposesRvalueHttp2StreamRequestDataStorage<ruvia::detail::Http2StreamRequestData>);
static_assert(!ExposesRvalueHttp2StreamRequestStateStorage<ruvia::detail::Http2StreamRequestState>);
static_assert(!ExposesRvalueHttp2StreamHeaderBlocksStorage<ruvia::detail::Http2StreamHeaderBlocks>);
static_assert(!ExposesRvalueHttp2StreamLifecycleStorage<ruvia::detail::Http2StreamLifecycle>);
static_assert(!ExposesRvalueHttp2StreamStateStorage<ruvia::detail::Http2StreamState>);
static_assert(!ExposesRvalueHttp2StreamTableStorage<ruvia::detail::Http2StreamTable>);

template <typename T>
concept ExposesRvalueMultipartInputStorage = requires(T&& input) { std::move(input).borrowed(); } || requires(T&& input) { std::move(input).streamingOpen(); } || requires(T&& input) { std::move(input).streamingEof(); } || requires(T&& input) { std::move(input).view(); };

template <typename T>
concept ExposesRvalueWsConnectionStorage = requires(T&& connection) { std::move(connection).poll(); } || requires(T&& connection) { std::move(connection).outputPlan(); };

static_assert(!ExposesRvalueMultipartInputStorage<ruvia::detail::MultipartInputLifecycle>);
static_assert(!ExposesRvalueWsConnectionStorage<ruvia::detail::WsConnection>);

template <typename Input>
concept AcceptsHttp1BorrowedParseInput = requires(const ruvia::Http1RequestParser& parser, Input&& input) { parser.parse(std::forward<Input>(input)); };

template <typename Input>
concept AcceptsHttp2BorrowedFeedInput = requires(ruvia::detail::Http2Connection& connection, Input&& input) { connection.feed(std::forward<Input>(input)); };

template <typename Input>
concept AcceptsHttp1ChunkBorrowedInput = requires(ruvia::detail::Http1ChunkedBodyDecoder& decoder, Input&& input) { decoder.decode(std::forward<Input>(input)); };

template <typename Input>
concept AcceptsMultipartBorrowedInput = requires(Input&& input) { ruvia::parseMultipartBody(std::forward<Input>(input), ruvia::MultipartBoundary("x")); };

template <typename Input>
concept AcceptsCopiedMultipartMetadata = requires(Input&& input) {
    ruvia::detail::MultipartPartAccess::make(std::forward<Input>(input), {}, {}, {}, std::pmr::get_default_resource());
    ruvia::detail::MultipartPartAccess::make({}, std::forward<Input>(input), {}, {}, std::pmr::get_default_resource());
    ruvia::detail::MultipartPartAccess::makeDecoded(std::forward<Input>(input), {}, {}, {}, std::pmr::get_default_resource());
    ruvia::detail::MultipartPartAccess::makeDecoded({}, std::forward<Input>(input), {}, {}, std::pmr::get_default_resource());
};

template <typename Input>
concept AcceptsAnyBufferedMultipartBorrow = requires(Input&& input) { ruvia::detail::MultipartPartAccess::make({}, {}, std::forward<Input>(input), {}, std::pmr::get_default_resource()); } || requires(Input&& input) { ruvia::detail::MultipartPartAccess::make({}, {}, {}, std::forward<Input>(input), std::pmr::get_default_resource()); } || requires(Input&& input) { ruvia::detail::MultipartPartAccess::makeDecoded({}, {}, std::forward<Input>(input), {}, std::pmr::get_default_resource()); } || requires(Input&& input) { ruvia::detail::MultipartPartAccess::makeDecoded({}, {}, {}, std::forward<Input>(input), std::pmr::get_default_resource()); };

template <typename Input>
concept AcceptsAnyStreamMultipartBorrow = requires(Input&& input) { ruvia::detail::MultipartStreamPartAccess::make(std::forward<Input>(input), {}, {}, {}, ruvia::MultipartChunkPhase::kComplete); } || requires(Input&& input) { ruvia::detail::MultipartStreamPartAccess::make({}, std::forward<Input>(input), {}, {}, ruvia::MultipartChunkPhase::kComplete); } || requires(Input&& input) { ruvia::detail::MultipartStreamPartAccess::make({}, {}, std::forward<Input>(input), {}, ruvia::MultipartChunkPhase::kComplete); } || requires(Input&& input) { ruvia::detail::MultipartStreamPartAccess::make({}, {}, {}, std::forward<Input>(input), ruvia::MultipartChunkPhase::kComplete); };

template <typename Input>
concept AcceptsAuthorityBorrowedInput = requires(Input&& input) { ruvia::detail::parseHttpAuthority(std::forward<Input>(input)); };

template <typename Input>
concept AcceptsRequestTargetBorrowedInput = requires(Input&& input, ruvia::detail::RequestTargetView& output) { ruvia::detail::parseRequestTarget(ruvia::HttpKnownMethod::kGet, std::forward<Input>(input), output); };

template <typename Input>
concept AcceptsFirstHttp2PayloadPart = requires(Input&& input) { ruvia::detail::http2SliceTwoPartPayload(std::forward<Input>(input), std::string_view{}, 0, 0); };

template <typename Input>
concept AcceptsSecondHttp2PayloadPart = requires(Input&& input) { ruvia::detail::http2SliceTwoPartPayload(std::string_view{}, std::forward<Input>(input), 0, 0); };

template <typename Input>
concept AcceptsUrlValueBorrowedInput = requires(Input&& input) { ruvia::detail::findUrlEncodedValue(std::forward<Input>(input), "name", ruvia::detail::UrlDecodeMode::kForm); };

template <typename Input>
concept AcceptsParameterBorrowedInput = requires(Input&& input) { ruvia::detail::httpFindSemicolonParameter(std::forward<Input>(input), "name"); };

template <typename Input>
concept AcceptsServerMessageBorrowedInput = requires(const ruvia::detail::Http1ServerRequestParser& parser, Input&& input) { parser.parseMessage(std::forward<Input>(input)); };

template <typename Input>
concept AcceptsHeaderSliceBorrowedInput = requires(const ruvia::detail::HttpHeaderSlice& slice, Input&& input) { slice.bind(std::forward<Input>(input)); };

template <typename Input>
concept AcceptsMultipartHeaderBorrowedInput = requires(Input&& input) { ruvia::detail::httpHeaderValueInBlock(std::forward<Input>(input), "content-type"); };

static_assert(ruvia::detail::kIsHttpOwningCharString<std::string>);
static_assert(ruvia::detail::kIsHttpOwningCharString<std::pmr::string>);
static_assert(!ruvia::detail::kIsHttpOwningCharString<std::string_view>);
static_assert(AcceptsHttp1BorrowedParseInput<std::string&>);
static_assert(AcceptsHttp1BorrowedParseInput<std::pmr::string&>);
static_assert(AcceptsHttp1BorrowedParseInput<std::string_view>);
static_assert(!AcceptsHttp1BorrowedParseInput<std::string>);
static_assert(!AcceptsHttp1BorrowedParseInput<std::pmr::string>);
static_assert(AcceptsHttp2BorrowedFeedInput<std::string&>);
static_assert(AcceptsHttp2BorrowedFeedInput<std::string_view>);
static_assert(!AcceptsHttp2BorrowedFeedInput<std::string>);
static_assert(!AcceptsHttp2BorrowedFeedInput<std::pmr::string>);
static_assert(AcceptsHttp1ChunkBorrowedInput<std::string&>);
static_assert(AcceptsHttp1ChunkBorrowedInput<std::string_view>);
static_assert(!AcceptsHttp1ChunkBorrowedInput<std::string>);
static_assert(!AcceptsHttp1ChunkBorrowedInput<std::pmr::string>);
static_assert(AcceptsMultipartBorrowedInput<std::string&>);
static_assert(AcceptsMultipartBorrowedInput<std::string_view>);
static_assert(!AcceptsMultipartBorrowedInput<std::string>);
static_assert(!AcceptsMultipartBorrowedInput<std::pmr::string>);
static_assert(AcceptsCopiedMultipartMetadata<std::string>);
static_assert(AcceptsCopiedMultipartMetadata<std::pmr::string>);
static_assert(AcceptsAnyBufferedMultipartBorrow<std::string&>);
static_assert(AcceptsAnyBufferedMultipartBorrow<std::pmr::string&>);
static_assert(AcceptsAnyBufferedMultipartBorrow<std::string_view>);
static_assert(!AcceptsAnyBufferedMultipartBorrow<std::string>);
static_assert(!AcceptsAnyBufferedMultipartBorrow<const std::string>);
static_assert(!AcceptsAnyBufferedMultipartBorrow<std::pmr::string>);
static_assert(AcceptsAnyStreamMultipartBorrow<std::string&>);
static_assert(AcceptsAnyStreamMultipartBorrow<std::pmr::string&>);
static_assert(AcceptsAnyStreamMultipartBorrow<std::string_view>);
static_assert(!AcceptsAnyStreamMultipartBorrow<std::string>);
static_assert(!AcceptsAnyStreamMultipartBorrow<const std::string>);
static_assert(!AcceptsAnyStreamMultipartBorrow<std::pmr::string>);
static_assert(AcceptsAuthorityBorrowedInput<std::string&>);
static_assert(AcceptsAuthorityBorrowedInput<std::string_view>);
static_assert(!AcceptsAuthorityBorrowedInput<std::string>);
static_assert(!AcceptsAuthorityBorrowedInput<std::pmr::string>);
static_assert(AcceptsRequestTargetBorrowedInput<std::string&>);
static_assert(AcceptsRequestTargetBorrowedInput<std::pmr::string&>);
static_assert(AcceptsRequestTargetBorrowedInput<std::string_view>);
static_assert(!AcceptsRequestTargetBorrowedInput<std::string>);
static_assert(!AcceptsRequestTargetBorrowedInput<const std::string>);
static_assert(!AcceptsRequestTargetBorrowedInput<std::pmr::string>);
static_assert(AcceptsFirstHttp2PayloadPart<std::string&>);
static_assert(AcceptsFirstHttp2PayloadPart<std::pmr::string&>);
static_assert(AcceptsFirstHttp2PayloadPart<std::string_view>);
static_assert(!AcceptsFirstHttp2PayloadPart<std::string>);
static_assert(!AcceptsFirstHttp2PayloadPart<const std::string>);
static_assert(!AcceptsFirstHttp2PayloadPart<std::pmr::string>);
static_assert(AcceptsSecondHttp2PayloadPart<std::string&>);
static_assert(AcceptsSecondHttp2PayloadPart<std::pmr::string&>);
static_assert(AcceptsSecondHttp2PayloadPart<std::string_view>);
static_assert(!AcceptsSecondHttp2PayloadPart<std::string>);
static_assert(!AcceptsSecondHttp2PayloadPart<const std::string>);
static_assert(!AcceptsSecondHttp2PayloadPart<std::pmr::string>);
static_assert(AcceptsUrlValueBorrowedInput<std::string&>);
static_assert(!AcceptsUrlValueBorrowedInput<std::string>);
static_assert(!AcceptsUrlValueBorrowedInput<std::pmr::string>);
static_assert(AcceptsParameterBorrowedInput<std::string&>);
static_assert(!AcceptsParameterBorrowedInput<std::string>);
static_assert(!AcceptsParameterBorrowedInput<std::pmr::string>);
static_assert(AcceptsServerMessageBorrowedInput<std::string&>);
static_assert(AcceptsServerMessageBorrowedInput<std::string_view>);
static_assert(!AcceptsServerMessageBorrowedInput<std::string>);
static_assert(!AcceptsServerMessageBorrowedInput<std::pmr::string>);
static_assert(AcceptsHeaderSliceBorrowedInput<std::string&>);
static_assert(!AcceptsHeaderSliceBorrowedInput<std::string>);
static_assert(!AcceptsHeaderSliceBorrowedInput<std::pmr::string>);
static_assert(AcceptsMultipartHeaderBorrowedInput<std::string&>);
static_assert(!AcceptsMultipartHeaderBorrowedInput<std::string>);
static_assert(!AcceptsMultipartHeaderBorrowedInput<std::pmr::string>);
static_assert(std::default_initializable<ruvia::detail::Http2LocalConnectionState>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2LocalConnectionState&>().open()), const ruvia::detail::Http2LocalConnectionOpen*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2LocalConnectionState&>().gracefulDrain()), const ruvia::detail::Http2LocalConnectionGracefulDrain*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2LocalConnectionState&>().fatalFailure()), const ruvia::detail::Http2LocalConnectionFatalFailure*>);

template <typename T>
concept HasLegacyResponseBodyView = requires(T& response) { response.setBodyView(std::string_view{}); };

static_assert(std::same_as<decltype(std::declval<const ruvia::HttpRequest&>().header(std::string_view{})), std::optional<std::string_view>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpResponse&>().header(std::string_view{})), std::optional<std::string_view>>);
static_assert(std::constructible_from<ruvia::HttpHeaderView, const std::string&, const std::string&>);
static_assert(!std::constructible_from<ruvia::HttpHeaderView, std::string&&, std::string_view>);
static_assert(!std::constructible_from<ruvia::HttpHeaderView, std::string_view, std::string&&>);
static_assert(!std::constructible_from<ruvia::HttpHeaderView, std::string&&, std::string&&>);
static_assert(!std::constructible_from<ruvia::HttpHeaderView, const std::string&&, std::string_view>);
static_assert(!std::constructible_from<ruvia::HttpHeaderView, std::string_view, const std::string&&>);
static_assert(!std::constructible_from<ruvia::HttpHeaderView, std::pmr::string&&, std::string_view>);
static_assert(!std::constructible_from<ruvia::HttpHeaderView, std::string_view, const std::pmr::string&&>);

template <typename T>
concept HasHttp2EventError = requires(const T& event) {
    { event.error() } -> std::same_as<ruvia::detail::Http2ErrorCode>;
};

template <typename T>
concept ExposesAnyRvalueSansIoEventBorrow = requires(T&& event) { std::move(event).messageHead(); } || requires(T&& event) { std::move(event).messageBodyChunk(); } || requires(T&& event) { std::move(event).messageEnd(); } || requires(T&& event) { std::move(event).tunnelData(); } || requires(T&& event) { std::move(event).tunnelEnd(); } || requires(T&& event) { std::move(event).streamClosed(); } || requires(T&& event) { std::move(event).requestUnprocessed(); } || requires(T&& event) { std::move(event).goaway(); } || requires(T&& event) { std::move(event).peerGoaway(); } || requires(T&& event) { std::move(event).message(); } || requires(T&& event) { std::move(event).ping(); } || requires(T&& event) { std::move(event).pong(); } || requires(T&& event) { std::move(event).close(); } || requires(T&& event) { std::move(event).protocolError(); } || requires(T&& event) { std::move(event).transportEnd(); };

template <typename T>
concept HasSharedCacheFreshnessPolicy = requires(const T& directives) { directives.sharedFreshnessLifetime(); };

template <typename T>
concept HasFeedStatusField = requires(const T& result) { result.status; };

template <typename T>
concept HasFeedConsumedField = requires(const T& result) { result.consumed; };

template <typename T>
concept ExposesRvalueDecodedContent = requires(T&& result) { std::move(result).decoded(); };

template <typename T>
concept ExposesRvalueDecodeFailure = requires(const T&& result) { std::move(result).failure(); };

template <typename T>
concept HasRawContentDecodeError = requires(const T& result) { result.error(); };

template <typename T>
concept ExposesRvalueHttp2RequestBuildAlternative = requires(const T&& result) { std::move(result).built(); } || requires(const T&& result) { std::move(result).failure(); };

template <typename T>
concept ExposesRvalueEncodedContent = requires(T&& result) { std::move(result).encoded(); };

template <typename T>
concept ExposesRvalueEncodeFailure = requires(const T&& result) { std::move(result).failure(); };

template <typename T>
concept ExposesRvalueContentCoding = requires(const T&& result) { std::move(result).coding(); };

template <typename T>
concept ExposesAnyRvalueWebSocketFrameReadAccessor = requires(T&& result) { std::move(result).needInput(); } || requires(T&& result) { std::move(result).frame(); } || requires(T&& result) { std::move(result).failure(); };

template <typename String>
concept AcceptsAnyTemporaryWebSocketFramePayload = requires(String&& payload) { ruvia::detail::WebSocketFrameView::text(std::move(payload), true); } || requires(String&& payload) { ruvia::detail::WebSocketFrameView::binary(std::move(payload), true); } || requires(String&& payload) { ruvia::detail::WebSocketFrameView::continuation(std::move(payload), true); } || requires(String&& payload) { ruvia::detail::WebSocketFrameView::close(std::move(payload)); } || requires(String&& payload) { ruvia::detail::WebSocketFrameView::ping(std::move(payload)); } || requires(String&& payload) { ruvia::detail::WebSocketFrameView::pong(std::move(payload)); };

template <typename Payload>
concept AcceptsWebSocketMessagePayload = requires(Payload&& payload) { ruvia::detail::WebSocketMessageAccess::make(ruvia::WebSocketOpcode::kText, std::forward<Payload>(payload)); };

template <typename T>
concept ExposesAnyRvalueWebSocketInboundAccessor = requires(T&& result) { std::move(result).continueReading(); } || requires(T&& result) { std::move(result).controlFrame(); } || requires(T&& result) { std::move(result).message(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept ExposesRvalueWebSocketInboundMessageMember = requires(T&& message) { std::move(message).message(); };

template <typename T>
concept ExposesAnyRvalueHttpOperationResultAccessor = requires(T&& result) { std::move(result).committed(); } || requires(T&& result) { std::move(result).prepared(); } || requires(T&& result) { std::move(result).submitted(); } || requires(T&& result) { std::move(result).applied(); } || requires(T&& result) { std::move(result).initialWindowChange(); } || requires(T&& result) { std::move(result).plan(); } || requires(T&& result) { std::move(result).section(); } || requires(T&& result) { std::move(result).failure(); };

static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<ruvia::detail::Http1FinalResponseCommitResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<ruvia::detail::PreparedHttp1ResponseStreamResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<ruvia::detail::Http2WebSocketHandshakeSubmitResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<ruvia::detail::Http2RequestHeadSubmitResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<ruvia::detail::Http2StreamingResponseHeadSubmitResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<ruvia::detail::Http2ResponseHeadPlanResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<ruvia::detail::Http1FinalResponseControlPlanResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<ruvia::detail::Http2FinalResponseControlPlanResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<ruvia::detail::HttpResponseTrailerSectionResult>);

template <typename T>
concept ExposesRvalueUnsupportedContentCoding = requires(const T&& result) { std::move(result).unsupported(); };

template <typename T>
concept ExposesRvalueInvalidContentCoding = requires(const T&& result) { std::move(result).invalid(); };

template <typename T>
concept HasContentLengthPresent = requires(const T& state) { state.present(); };

template <typename T>
concept HasStaleTransferEncodingAccessors = requires(const T& state) {
    state.present();
    state.finalChunked();
    state.codings();
};

template <typename T>
concept ExposesRvalueFinalChunked = requires(const T&& value) { std::move(value).finalChunked(); };

template <typename T>
concept ExposesRvalueNonChunked = requires(const T&& value) { std::move(value).nonChunked(); };

template <typename Output>
concept AcceptsUrlDecodeOutputParameter = requires(Output& output) { ruvia::detail::decodeUrlComponent(std::string_view{}, output, ruvia::detail::UrlDecodeMode::kPercent); };

template <typename T>
concept HasRequestHeadStatusAccessor = requires(const T& result) { result.status(); };

template <typename T>
concept HasRequestHeadAcceptedAccessor = requires(const T& result) { result.accepted(); };

template <typename T>
concept HasRequestHeadStreamIdAccessor = requires(const T& result) {
    { result.streamId() } -> std::same_as<std::uint32_t>;
};

template <typename T>
concept HasRequestHeadErrorAccessor = requires(const T& result) {
    { result.error() } -> std::same_as<ruvia::detail::Http2RequestHeadSubmitError>;
};

template <typename T>
concept HasResponseHeadStatusAccessor = requires(const T& result) { result.status(); };

template <typename T>
concept HasResponseHeadAcceptedAccessor = requires(const T& result) { result.accepted(); };

template <typename T>
concept HasResponseHeadPlanAccessor = requires(const T& result) { result.plan(); };

template <typename T>
concept HasResponseHeadErrorAccessor = requires(const T& result) { result.error(); };

template <typename T>
concept HasResponseHeadFailureContract = requires(const T& failure) {
    { failure.peerClosed() } -> std::same_as<bool>;
    { failure.exception() } -> std::same_as<ruvia::detail::Http2ResponseHeadSubmitError>;
};

template <typename Connection>
concept AcceptsUnpreparedBufferedResponseHead = requires(Connection& connection, const ruvia::HttpResponse& response) { connection.submitResponseHead(std::uint32_t{}, response); };

template <typename Connection>
concept AcceptsStagedResponseTrailerSection = requires(Connection& connection, std::span<const ruvia::HttpHeaderView> trailers) { connection.submitResponseTrailerSection(std::uint32_t{}, trailers); };

template <typename Connection>
concept AcceptsImplicitResponseFinish = requires(Connection& connection) { connection.finishResponse(std::uint32_t{}); };

template <typename Connection>
concept AcceptsRawResponseTrailerFinish = requires(Connection& connection, std::span<const ruvia::HttpHeaderView> trailers) { connection.finishResponse(std::uint32_t{}, trailers); };

template <typename Headers>
concept AcceptsResponseTrailerRange = requires(Headers&& headers) {
    ruvia::detail::httpResponseTrailerSection(std::forward<Headers>(headers));
};

template <typename Headers>
concept AcceptsValidatedResponseTrailerRange = requires(Headers&& headers) {
    ruvia::detail::validatedResponseTrailerSection(std::forward<Headers>(headers));
};

template <typename T>
concept HasResponseTrailerSectionAlternatives = requires(const T& result) {
    { result.section() } -> std::same_as<const ruvia::detail::HttpResponseTrailerSection*>;
    { result.failure() } -> std::same_as<const ruvia::detail::HttpResponseTrailerSectionFailure*>;
};

template <typename Stream>
concept HasStagedResponseTrailerBlock = requires(Stream& stream) { stream.responseTrailerBlock(); };

template <typename HeaderBlocks>
concept HasStagedResponseTrailers = requires(HeaderBlocks& blocks) { blocks.responseTrailers(); };

template <typename BodyPlan>
concept AcceptsLooseResponseStreamBodyPlan = requires(BodyPlan bodyPlan) { ruvia::detail::httpResponseStreamCommitPlan(ruvia::detail::ResponseStreamFraming::kHttp1Chunked, bodyPlan, ruvia::detail::ResponseTrailerIntent::kNone); };

template <typename BodyPlan>
concept AcceptsLooseBufferedResponseBodyPlan = requires(BodyPlan bodyPlan, const ruvia::HttpResponse& response) { ruvia::detail::httpBufferedResponseWritePlan(bodyPlan, response); };

template <typename T>
concept HasPeerSettingStatusField = requires(const T& result) { result.status; };

template <typename T>
concept HasPeerSettingChangedField = requires(const T& result) { result.initialWindowChanged; };

template <typename T>
concept HasPeerSettingDeltaField = requires(const T& result) { result.initialWindowDelta; };

template <typename T>
concept HasPeerSettingDeltaAccessor = requires(const T& result) {
    { result.delta() } -> std::same_as<std::int64_t>;
};

template <typename T>
concept HasPeerSettingErrorAccessor = requires(const T& result) {
    { result.error() } -> std::same_as<ruvia::detail::Http2PeerSettingError>;
};

template <typename T>
concept HasByteRangeOutcomeField = requires(const T& result) { result.outcome; };

template <typename T>
concept HasByteRangePayloadField = requires(const T& result) { result.range; };

template <typename T>
concept HasByteRangeOffsetAccessor = requires(const T& result) {
    { result.offset() } -> std::same_as<std::uint64_t>;
};

template <typename T>
concept HasByteRangeLengthAccessor = requires(const T& result) {
    { result.length() } -> std::same_as<std::uint64_t>;
};

template <typename T>
concept HasWsCloseCode = requires(const T& event) {
    { event.closeCode() } -> std::same_as<std::uint16_t>;
};

template <typename T>
concept HasWsReason = requires(const T& event) {
    { event.reason() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasWsSubmitMessageAlias = requires(T& connection) { connection.submitMessage(ruvia::WebSocketOpcode::kText, std::string_view{}); };

template <typename T>
concept HasWsSubmitPingAlias = requires(T& connection) { connection.submitPing(std::string_view{}); };

template <typename T>
concept HasWsSubmitPongAlias = requires(T& connection) { connection.submitPong(std::string_view{}); };

template <typename T>
concept HasWsApplicationFrameStateSideChannel = requires(const T& connection) { connection.acceptsApplicationFrames(); };

template <typename T>
concept HasWsEndsTransportAlias = requires(const T& plan) { plan.endsTransport(); };

template <typename T>
concept HasWsTransportEndPendingSideChannel = requires(const T& connection) { connection.transportEndPending(); };

template <typename T>
concept HasWsClosedStateSideChannel = requires(const T& connection) { connection.closed(); };

template <typename T>
concept HasWsClosePhaseSideChannel = requires(const T& connection) { connection.closePhase(); };

template <typename T>
concept HasWebSocketNegotiationAccessor = requires(const T& value) {
    { value.negotiation() } -> std::same_as<const ruvia::detail::WebSocketServerNegotiation&>;
};

template <typename T>
concept HasWebSocketHandshakeErrorAccessor = requires(const T& value) {
    { value.error() } -> std::same_as<ruvia::detail::Http2WebSocketHandshakeSubmitError>;
};

template <typename T>
concept ExposesRvalueWebSocketHandshakeValidationAlternative = requires(const T&& result) { std::move(result).accepted(); } || requires(const T&& result) { std::move(result).failure(); };

template <typename T>
concept AppliesRequiredWebSocketResponseHeaders = requires(const T& failure, ruvia::HttpResponse& response) { failure.applyRequiredResponseHeaders(response); };

template <typename T>
concept HasLooseWebSocketNegotiationFields = requires(T& value) {
    value.subprotocol;
    value.extensions;
    value.permessageDeflate;
};

template <typename T>
concept HasLooseWebSocketDeflateFields = requires(T& value) {
    value.enabled;
    value.echoServerMaxWindowBits;
};

template <typename T>
concept HasFallibleWebSocketDeflateState = requires(const T& value) {
    { value.ok() } -> std::same_as<bool>;
};

template <typename T>
concept ExposesRvalueWebSocketServerSubprotocol = requires(T&& negotiation) { std::move(negotiation).subprotocol(); };

template <typename T>
concept AcceptsLooseWebSocketHandshakeSubmit = requires(T& connection) { connection.submitWebSocketHandshake(std::uint32_t{}, std::string_view{}, std::string_view{}); };

template <typename T>
concept HasWsFrameReadStatusField = requires(const T& result) { result.status; };

template <typename T>
concept HasWsRequiredBytesField = requires(const T& result) { result.requiredBytes; };

template <typename T>
concept HasWsCleanEofAllowedField = requires(const T& result) { result.cleanEofAllowed; };

template <typename T>
concept HasLooseWebSocketFrameFields = requires(T& frame) {
    frame.opcode;
    frame.payload;
    frame.fin;
    frame.continuation;
    frame.rsv1;
};

template <typename T>
concept AcceptsMutableWebSocketFrameStartDecode = requires(T& frame) { ruvia::detail::decodeWebSocketFrameStart(static_cast<unsigned char>(0x81), static_cast<unsigned char>(0x80), frame, false); };

template <typename T>
concept HasWsInboundActionAccessor = requires(const T& result) { result.action(); };

template <typename T>
concept HasWsProtocolFailure = requires(const T& result) {
    { result.error() } -> std::same_as<ruvia::detail::WebSocketProtocolFailure>;
};

template <typename T>
concept HasConsumedBytes = requires(const T& result) {
    { result.consumedBytes() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasTransferOutputBytes = requires(const T& result) {
    { result.bytes() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasTransferDecodeError = requires(const T& result) {
    { result.error() } -> std::same_as<ruvia::detail::TransferCodingDecodeError>;
};

template <typename T>
concept HasProtocolError = requires(const T& result) { result.protocolError(); };

template <typename T>
concept HasChunkScanError = requires(const T& result) {
    { result.error() } -> std::same_as<ruvia::detail::HttpChunkScanError>;
};

template <typename T>
concept HasAnyRvalueHttpChunkScanAccessor = requires(T&& result) { std::move(result).needMore(); } || requires(T&& result) { std::move(result).complete(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueHttp1ChunkDecodeAccessor = requires(T&& result) { std::move(result).needMore(); } || requires(T&& result) { std::move(result).bodyChunk(); } || requires(T&& result) { std::move(result).complete(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueTransferCodingDecodeAccessor = requires(T&& result) { std::move(result).needInput(); } || requires(T&& result) { std::move(result).output(); } || requires(T&& result) { std::move(result).complete(); } || requires(T&& result) { std::move(result).protocolFailure(); } || requires(T&& result) { std::move(result).decoderFailure(); };

template <typename T>
concept HasAnyRvalueRequestContentDecodeAccessor = requires(T&& result) { std::move(result).decoded(); } || requires(T&& result) { std::move(result).protocolFailure(); } || requires(T&& result) { std::move(result).decoderFailure(); };

template <typename T>
concept HasMultipartStatus = requires(const T& result) { result.status(); };

template <typename T>
concept HasMultipartOffset = requires(const T& result) {
    { result.offset() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasMultipartLineBytes = requires(const T& result) {
    { result.lineBytes() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasMultipartParseError = requires(const T& result) {
    { result.parseError() } -> std::same_as<ruvia::MultipartParseError>;
};

template <typename T>
concept HasMultipartProtocolError = requires(const T& result) {
    { result.protocolError() } -> std::same_as<ruvia::HttpProtocolError>;
};

template <typename T>
concept HasHttpClientHeaderValue = requires(const T& result) {
    { result.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttpClientRedirectError = requires(const T& result) {
    { result.error() } -> std::same_as<ruvia::HttpClientRedirectTargetError>;
};

template <typename T>
concept HasHttpClientRedirectStatus = requires(const T& result) { result.status(); };

template <typename T>
concept HasHttpClientRequestContentAlternatives = requires(const T& content) {
    { content.withoutContent() } -> std::same_as<const ruvia::HttpClientRequestWithoutContent*>;
    { content.borrowedBytes() } -> std::same_as<const ruvia::HttpClientRequestBytes*>;
};

template <typename T>
concept HasAnyRvalueHttpClientRequestContentAccessor = requires(T&& content) { std::move(content).withoutContent(); } || requires(T&& content) { std::move(content).borrowedBytes(); };

template <typename String>
concept AcceptsAnyTemporaryHttpClientRequestText = requires(String&& value) { ruvia::HttpClientRequest{.method = std::forward<String>(value)}; } || requires(String&& value) { ruvia::HttpClientRequest{.target = std::forward<String>(value)}; } || requires(ruvia::HttpClientRequest& request, String&& value) { request.method = std::forward<String>(value); } || requires(ruvia::HttpClientRequest& request, String&& value) { request.target = std::forward<String>(value); };

template <typename String>
concept AcceptsLvalueHttpClientRequestText = requires(ruvia::HttpClientRequest& request, String& value) {
    ruvia::HttpClientRequest{.method = value, .target = value};
    request.method = value;
    request.target = value;
};

template <typename Headers>
concept AcceptsHttp1ConnectHeaders = requires(const ruvia::Http1ClientRequestWriter& writer, const ruvia::HttpOrigin& origin, std::array<char, 512>& buffer, Headers&& headers) { writer.prepareConnect(origin, std::forward<Headers>(headers), buffer); };

template <typename T>
concept HasStaleHttpClientContentMode = requires(const T& content) { content.mode(); };

template <typename T>
concept HasHttpClientRequestContentValue = requires(const T& content) {
    { content.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttp1PreparedContentAlternatives = requires(const T& plan) {
    { plan.withoutContent() } -> std::same_as<const ruvia::Http1ClientRequestWithoutContent*>;
    { plan.immediate() } -> std::same_as<const ruvia::Http1ClientImmediateRequestContent*>;
    { plan.continueGated() } -> std::same_as<const ruvia::Http1ClientContinueGatedRequestContent*>;
};

template <typename T>
concept HasAnyRvalueHttp1ClientRequestContentPlanAccessor = requires(T&& plan) { std::move(plan).withoutContent(); } || requires(T&& plan) { std::move(plan).immediate(); } || requires(T&& plan) { std::move(plan).continueGated(); };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestWirePolicyAccessor = requires(T&& policy) { std::move(policy).noExpectation(); } || requires(T&& policy) { std::move(policy).continueExpectation(); };

template <typename T>
concept HasHttp1ClientExpectationAlternatives = requires(const T& policy) {
    { policy.noExpectation() } -> std::same_as<const ruvia::Http1ClientNoRequestExpectation*>;
    { policy.continueExpectation() } -> std::same_as<const ruvia::Http1ClientContinueExpectation*>;
};

template <typename T>
concept HasHttp1PreparedContentDisposition = requires(const T& plan) { plan.disposition(); };

template <typename T>
concept HasHttp1PreparedContentBytes = requires(const T& content) {
    { content.bytes() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttp1ResponseHeadAlternatives = requires(const T& plan) {
    { plan.buffered() } -> std::same_as<const ruvia::detail::Http1BufferedResponseHead*>;
    { plan.knownLengthStream() } -> std::same_as<const ruvia::detail::Http1KnownLengthResponseStreamHead*>;
    { plan.chunkedStream() } -> std::same_as<const ruvia::detail::Http1ChunkedResponseStreamHead*>;
    { plan.closeDelimitedStream() } -> std::same_as<const ruvia::detail::Http1CloseDelimitedResponseStreamHead*>;
};

template <typename T>
concept HasHttp1ProtocolVersion = requires(const T& plan) {
    { plan.protocolVersion() } -> std::same_as<ruvia::HttpProtocolVersion>;
};

template <typename T>
concept HasHttp1BufferedContentLength = requires(const T& buffered) {
    { buffered.contentLength() } -> std::same_as<std::uint64_t>;
};

template <typename T>
concept HasHttp1BufferedPlanContract = requires(const T& plan) {
    { plan.bodyPlan() } -> std::same_as<ruvia::detail::HttpResponseBodyPlan>;
    { plan.responseStatus() } -> std::same_as<ruvia::HttpStatusCode>;
    { plan.contentLength() } -> std::same_as<std::uint64_t>;
    { plan.sendBody() } -> std::same_as<bool>;
    { plan.headPlan() } -> std::same_as<const ruvia::detail::Http1ResponseHeadPlan&>;
};

template <typename T>
concept HasStaleHttp1BufferedWritePlanForwarder = requires(const T& plan) { plan.writePlan(); };

template <typename T>
concept HasStaleHttp1ResponseSignal = requires(const T& plan) { plan.responseSignal(); };

template <typename T>
concept HasStaleHttp1ResponseHeadScalar = requires(const T& plan) { plan.suppressAutoContentLength(); };

template <typename T>
concept HasHttp1ServerParseAlternatives = requires(const T& state) {
    { state.needRequestHead() } -> std::same_as<const ruvia::detail::Http1ServerNeedRequestHead*>;
    { state.headReady() } -> std::same_as<const ruvia::detail::Http1ServerRequestHeadReady*>;
    { state.needRequestBody() } -> std::same_as<const ruvia::detail::Http1ServerNeedRequestBody*>;
    { state.messageReady() } -> std::same_as<const ruvia::detail::Http1ServerRequestMessageReady*>;
    { state.failure() } -> std::same_as<const ruvia::detail::Http1ServerRequestParseFailure*>;
};

template <typename T>
concept HasAnyRvalueHttp1ServerParseAccessor = requires(T&& state) { std::move(state).needRequestHead(); } || requires(T&& state) { std::move(state).headReady(); } || requires(T&& state) { std::move(state).needRequestBody(); } || requires(T&& state) { std::move(state).messageReady(); } || requires(T&& state) { std::move(state).failure(); };

template <typename T>
concept HasAnyRvalueHttp1RequestParseAccessor = requires(T&& result) { std::move(result).needMore(); } || requires(T&& result) { std::move(result).parsed(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueHttp1ClientResponseParseAccessor = requires(T&& result) { std::move(result).needMore(); } || requires(T&& result) { std::move(result).parsed(); } || requires(const T&& result) { std::move(result).parsed(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueMultipartPollAccessor = requires(T&& result) { std::move(result).needInput(); } || requires(T&& result) { std::move(result).part(); } || requires(T&& result) { std::move(result).done(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueMultipartDelimiterAccessor = requires(T&& result) { std::move(result).noMatch(); } || requires(T&& result) { std::move(result).needInput(); } || requires(T&& result) { std::move(result).part(); } || requires(T&& result) { std::move(result).close(); };

template <typename T>
concept HasAnyRvalueMultipartPartHeaderAccessor = requires(T&& result) { std::move(result).headers(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueMultipartBoundaryAccessor = requires(T&& result) { std::move(result).boundary(); } || requires(T&& result) { std::move(result).notApplicable(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept ExposesAnyRvalueMultipartOwnedView = requires(T&& value) { std::move(value).value(); } || requires(T&& value) { std::move(value).name(); } || requires(T&& value) { std::move(value).filename(); };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestPrepareAccessor = requires(T&& result) { std::move(result).bufferTooSmall(); } || requires(T&& result) { std::move(result).prepared(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueHttp1InterimResponsePrepareAccessor = requires(T&& result) { std::move(result).bufferTooSmall(); } || requires(T&& result) { std::move(result).prepared(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasResultKindDiscriminator = requires(const T& result) { result.kind(); };

template <typename T>
concept HasStaleHttp1ServerParseScalars = requires(const T& state) { state.phase(); } || requires(const T& state) { state.headerBytes; } || requires(const T& state) { state.messageBytes; } || requires(const T& state) { state.requiredTotalBytes; };

template <typename T>
concept HasLegacyResponseCodingField = requires(T& state) { state.responseCoding.has_value(); };

template <typename T>
concept HasRawResponseCodingSelector = requires(const T& qualities) {
    httpSelectResponseCodingFromQualities(qualities);
};

template <typename T>
concept HasRawResponseCodingScore = requires(const T& quality) {
    httpAcceptedEncodingScore(quality);
};

template <typename T>
concept HasRawResponseIdentityScore = requires(const T& quality) {
    httpAcceptedIdentityScore(quality);
};

template <typename Selection>
concept HasTypedResponseCodingSelector = requires(const ruvia::detail::HttpResponseCodingQualities& qualities) {
    { Selection::select(qualities) } -> std::same_as<ruvia::detail::HttpResponseCodingSelectionResult>;
};

template <typename Selection>
concept HasResponseCodingAcceptability = requires(const Selection& selection, ruvia::detail::HttpContentCoding coding) {
    { selection.accepts(coding) } -> std::same_as<bool>;
};

template <typename Selection>
concept HasLegacyOptionalResponseCodingSelector = requires(const ruvia::detail::HttpResponseCodingQualities& qualities) {
    { Selection::select(qualities) } -> std::same_as<std::optional<Selection>>;
};

template <typename Result>
concept HasTypedResponseCodingResult = requires(const Result& result) {
    { result.selected() } -> std::same_as<const ruvia::detail::HttpResponseCodingSelection*>;
    { result.failure() } -> std::same_as<const ruvia::detail::HttpResponseCodingSelectionFailure*>;
};

template <typename Candidates>
concept HasTypedResponseCodingCandidates = requires(Candidates& candidates, ruvia::detail::HttpContentCoding coding) {
    { Candidates::empty() } -> std::same_as<Candidates>;
    { Candidates::identityOnly() } -> std::same_as<Candidates>;
    { Candidates::all() } -> std::same_as<Candidates>;
    { candidates.include(coding) } -> std::same_as<Candidates&>;
    { candidates.contains(coding) } -> std::same_as<bool>;
};

template <typename Selection, typename Candidates>
concept HasAvailableResponseCodingSelector = requires(const ruvia::detail::HttpResponseCodingQualities& qualities) {
    { Selection::select(qualities, Candidates::identityOnly()) } -> std::same_as<ruvia::detail::HttpResponseCodingSelectionResult>;
};

template <typename T>
concept HasStalePreparedStreamPolicy = requires(const T& prepared) { prepared.policy(); };

template <typename T, typename Control, typename Failure>
concept HasFinalResponseControlResult = requires(const T& result) {
    { result.control() } -> std::same_as<const Control*>;
    { result.failure() } -> std::same_as<const Failure*>;
};

template <typename T>
concept HasHttp1FinalResponseControlFields = requires(const T& plan) {
    { plan.connectionOptions() } -> std::same_as<ruvia::detail::HttpConnectionOptions>;
    { plan.upgradeProtocols() } -> std::same_as<ruvia::detail::HttpUpgradeProtocols>;
} && requires(const T&& plan) {
    { std::move(plan).connectionOptions() } -> std::same_as<ruvia::detail::HttpConnectionOptions>;
    { std::move(plan).upgradeProtocols() } -> std::same_as<ruvia::detail::HttpUpgradeProtocols>;
};

template <typename T>
concept HasStaleFinalResponseControlStatus = requires(const T& result) {
    result.status();
    result.accepted();
};

template <typename T>
concept HasStaleTopLevelUpgradeProtocols = requires(const T& plan) { plan.upgradeProtocols(); };

template <typename T>
concept HasHttp1FinalCommitAlternatives = requires(const T& result) {
    { result.committed() } -> std::same_as<const ruvia::detail::Http1ServerConnectionPlan*>;
    { result.failure() } -> std::same_as<const ruvia::detail::Http1FinalResponseCommitFailure*>;
};

template <typename T>
concept HasPreparedHttp1StreamAlternatives = requires(const T& result) {
    { result.prepared() } -> std::same_as<const ruvia::detail::PreparedHttp1ResponseStream*>;
    { result.failure() } -> std::same_as<const ruvia::detail::Http1FinalResponseCommitFailure*>;
};

template <typename T>
concept HasRawHttp1FinalCommitError = requires(const T& failure) { failure.error(); };

template <typename T>
concept HasUncheckedPreparedHttp1StreamExtraction = requires(T&& result) { std::move(result).takePrepared(); };

template <typename T>
concept HasHttp2ResponseHeadExecutionPlan = requires(const T& plan) {
    { plan.contentLength() } -> std::same_as<std::optional<std::uint64_t>>;
    { plan.streamingContentLength() } -> std::same_as<std::optional<std::uint64_t>>;
};

template <typename T>
concept HasHttp2RequestContentAlternatives = requires(const T& content) {
    { content.withoutContent() } -> std::same_as<const ruvia::detail::Http2RequestWithoutContent*>;
    { content.knownLengthContent() } -> std::same_as<const ruvia::detail::Http2KnownLengthRequestContent*>;
    { content.streamingContent() } -> std::same_as<const ruvia::detail::Http2StreamingRequestContent*>;
};

template <typename T>
concept HasStaleHttp2ContentMode = requires(const T& content) { content.mode(); };

template <typename T>
concept HasHttp2RequestContentLength = requires(const T& content) {
    { content.length() } -> std::same_as<std::uint64_t>;
};

template <typename T>
concept HasHttp2LocalContentAlternatives = requires(const T& content) {
    { content.unset() } -> std::same_as<const ruvia::detail::Http2LocalContentUnset*>;
    { content.forbidden() } -> std::same_as<const ruvia::detail::Http2LocalContentForbidden*>;
    { content.unbounded() } -> std::same_as<const ruvia::detail::Http2LocalContentUnbounded*>;
    { content.knownLength() } -> std::same_as<const ruvia::detail::Http2LocalContentKnownLength*>;
};

template <typename T>
concept HasAnyRvalueHttp2LocalContentAccessor = requires(T&& content) { std::move(content).unset(); } || requires(T&& content) { std::move(content).forbidden(); } || requires(T&& content) { std::move(content).unbounded(); } || requires(T&& content) { std::move(content).knownLength(); };

template <typename T>
concept HasStaleHttp2LocalModeAccessor = requires(const T& content) { content.mode(); };

template <typename T>
concept HasHttp2LocalDeclaredLength = requires(const T& content) {
    { content.declaredLength() } -> std::same_as<std::uint64_t>;
};

template <typename T>
concept HasStaleHttp2StreamLocalContentForwarders = requires(const T& stream) {
    stream.localContentMode();
    stream.localContentHasKnownLength();
    stream.localContentDeclaredLength();
    stream.localContentAcceptedBytes();
    stream.localContentCommittedBytes();
    stream.localContentLengthComplete();
};

template <typename T>
concept HasHttp2RemoteContentAlternatives = requires(const T& content) {
    { content.allowedWithoutLength() } -> std::same_as<const ruvia::detail::Http2RemoteContentAllowedWithoutLength*>;
    { content.allowedKnownLength() } -> std::same_as<const ruvia::detail::Http2RemoteContentAllowedKnownLength*>;
    { content.metadataOnlyWithoutLength() } -> std::same_as<const ruvia::detail::Http2RemoteContentMetadataOnlyWithoutLength*>;
    { content.metadataOnlyKnownLength() } -> std::same_as<const ruvia::detail::Http2RemoteContentMetadataOnlyKnownLength*>;
};

template <typename T>
concept HasAnyRvalueHttp2RemoteContentAccessor = requires(T&& content) { std::move(content).allowedWithoutLength(); } || requires(T&& content) { std::move(content).allowedKnownLength(); } || requires(T&& content) { std::move(content).metadataOnlyWithoutLength(); } || requires(T&& content) { std::move(content).metadataOnlyKnownLength(); };

template <typename T>
concept HasHttp2RemoteDeclaredLength = requires(const T& content) {
    { content.declaredLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasHttp2RemoteReceivedBytes = requires(const T& content) {
    { content.receivedBytes() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasStaleHttp2RemoteContentTuple = requires(const T& content) {
    content.hasContentLength();
    content.contentLength();
};

template <typename T>
concept HasStaleHttp2RemoteCheckAcceptSplit = requires(T& content) {
    content.checkAccept(std::size_t{1});
    content.accept(std::size_t{1});
};

template <typename T>
concept HasValueSemanticResponseContentSemantics = requires(const T& plan, const T&& temporary) {
    { plan.contentSemantics() } -> std::same_as<ruvia::detail::HttpResponseContentSemantics>;
    { temporary.contentSemantics() } -> std::same_as<ruvia::detail::HttpResponseContentSemantics>;
};

template <typename T>
concept HasValueSemanticResponseBodyPlan = requires(const T& plan, const T&& temporary) {
    { plan.bodyPlan() } -> std::same_as<ruvia::detail::HttpResponseBodyPlan>;
    { temporary.bodyPlan() } -> std::same_as<ruvia::detail::HttpResponseBodyPlan>;
};

template <typename T>
concept ExposesAnyRvalueHttpProtocolPlanBorrow = requires(T&& value) { std::move(value).ignored(); } || requires(T&& value) { std::move(value).unsatisfiable(); } || requires(T&& value) { std::move(value).resolved(); } || requires(T&& value) { std::move(value).withoutBody(); } || requires(T&& value) { std::move(value).knownLength(); } || requires(T&& value) { std::move(value).chunked(); } || requires(T&& value) { std::move(value).buffered(); } || requires(T&& value) { std::move(value).knownLengthStream(); } || requires(T&& value) { std::move(value).chunkedStream(); } || requires(T&& value) { std::move(value).closeDelimitedStream(); } || requires(T&& value) { std::move(value).knownLengthContent(); } || requires(T&& value) { std::move(value).streamingContent(); } || requires(T&& value) { std::move(value).control(); } || requires(T&& value) { std::move(value).writePlan(); } || requires(T&& value) { std::move(value).headPlan(); };

static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<ruvia::detail::HttpByteRangeResolution>);
static_assert(std::is_enum_v<ruvia::detail::HttpResponseContentSemantics>);
static_assert(HasValueSemanticResponseContentSemantics<ruvia::detail::HttpResponseBodyPlan>);
static_assert(HasValueSemanticResponseBodyPlan<ruvia::detail::HttpBufferedResponseWritePlan>);
static_assert(HasValueSemanticResponseBodyPlan<ruvia::detail::Http1ResponseHeadPlan>);
static_assert(HasValueSemanticResponseBodyPlan<ruvia::detail::Http2ResponseHeadPlan>);
static_assert(HasValueSemanticResponseBodyPlan<ruvia::detail::ResponseStreamCommitPlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<ruvia::detail::Http1RequestBodyPlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<ruvia::detail::Http1ChunkedRequestBody>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<ruvia::detail::Http1ResponseHeadPlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<ruvia::detail::Http1BufferedResponsePlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<ruvia::detail::Http2RequestContent>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<ruvia::detail::Http2ResponseHeadPlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<ruvia::detail::Http1FinalResponseControlPlanResult>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<ruvia::detail::Http2FinalResponseControlPlanResult>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<ruvia::detail::Http1FinalResponseControl>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<ruvia::detail::ResponseStreamCommitPlan>);

template <typename T>
concept ExposesAnyRvalueHttpOperationPayloadBorrow = requires(T&& value) { std::move(value).request(); } || requires(T&& value) { std::move(value).head(); } || requires(const T&& value) { std::move(value).head(); } || requires(T&& value) { std::move(value).response(); } || requires(const T&& value) { std::move(value).response(); } || requires(T&& value) { std::move(value).bodyPlan(); } || requires(T&& value) { std::move(value).plan(); } || requires(T&& value) { std::move(value).contentPlan(); } || requires(T&& value) { std::move(value).responseHeadPlan(); } || requires(T&& value) { std::move(value).commitPlan(); } || requires(T&& value) { std::move(value).negotiation(); };

static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<ruvia::Http1ParsedRequest>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<ruvia::Http1ParsedClientResponseHead>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<ruvia::PreparedHttp1ClientRequest>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<ruvia::detail::ResponseStreamHead>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<ruvia::detail::PreparedHttp1ResponseStream>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<ruvia::detail::HttpWebSocketServerHandshake>);

template <typename T>
concept HasStaleHttp2StreamRemoteContentForwarders = requires(const T& stream) {
    stream.hasContentLength();
    stream.contentLength();
    stream.receivedBodyBytes();
    stream.receivedBodyExceedsContentLength();
    stream.bodyLengthComplete();
};

template <typename T>
concept HasStaleHttp2WebRuntimeState = requires(T& stream) {
    stream.bodyMode();
    stream.usesStreamRequestBody();
    stream.requestBodyView();
    stream.enqueueBodyChunk(std::string_view{});
    stream.responseCompressionScratch();
};

template <typename T>
concept HasHttp2ReceiveDataRelease = requires(T& connection) {
    { connection.releaseReceivedData(std::uint32_t{1}) } -> std::same_as<void>;
};

template <typename T>
concept HasStaleHttp2ReceiveDeferral = requires(T& connection) {
    connection.deferStreamWindowRelease(std::uint32_t{1});
    connection.releaseStreamWindow(std::uint32_t{1});
};

template <typename T>
concept HasHttp2TunnelAlternatives = requires(const T& state) {
    { state.notConnect() } -> std::same_as<const ruvia::detail::Http2NotConnect*>;
    { state.pending() } -> std::same_as<const ruvia::detail::Http2ConnectPending*>;
    { state.open() } -> std::same_as<const ruvia::detail::Http2TunnelOpen*>;
    { state.rejected() } -> std::same_as<const ruvia::detail::Http2ConnectRejected*>;
};

template <typename T>
concept HasAnyRvalueHttp2TunnelAccessor = requires(T&& state) { std::move(state).notConnect(); } || requires(T&& state) { std::move(state).pending(); } || requires(T&& state) { std::move(state).open(); } || requires(T&& state) { std::move(state).rejected(); };

template <typename T>
concept HasHttp2ConnectForm = requires(const T& state) {
    { state.form() } -> std::same_as<ruvia::detail::Http2ConnectForm>;
};

template <typename T>
concept HasStaleHttp2TunnelKindPhase = requires(const T& state) {
    state.kind();
    state.phase();
};

template <typename T>
concept HasStaleHttp2StreamTunnelForwarders = requires(const T& stream) {
    stream.standardConnect();
    stream.extendedConnect();
    stream.extendedConnectWebSocket();
    stream.connectRequest();
    stream.connectPending();
    stream.tunnelOpen();
    stream.connectRejected();
};

template <typename T>
concept HasHttp2LocalSendAlternatives = requires(const T& state) {
    { state.headPending() } -> std::same_as<const ruvia::detail::Http2LocalHeadPending*>;
    { state.requestContentOpen() } -> std::same_as<const ruvia::detail::Http2LocalRequestContentOpen*>;
    { state.responseContentOpen() } -> std::same_as<const ruvia::detail::Http2LocalResponseContentOpen*>;
    { state.responseTrailersOnly() } -> std::same_as<const ruvia::detail::Http2LocalResponseTrailersOnly*>;
    { state.connectPending() } -> std::same_as<const ruvia::detail::Http2LocalConnectPending*>;
    { state.tunnelOpen() } -> std::same_as<const ruvia::detail::Http2LocalTunnelOpen*>;
    { state.endStreamQueued() } -> std::same_as<const ruvia::detail::Http2LocalEndStreamQueued*>;
    { state.endStreamCommitted() } -> std::same_as<const ruvia::detail::Http2LocalEndStreamCommitted*>;
    { state.aborted() } -> std::same_as<const ruvia::detail::Http2StreamAborted*>;
};

template <typename T>
concept HasAnyRvalueHttp2LocalSendAccessor = requires(T&& state) { std::move(state).headPending(); } || requires(T&& state) { std::move(state).requestContentOpen(); } || requires(T&& state) { std::move(state).responseContentOpen(); } || requires(T&& state) { std::move(state).responseTrailersOnly(); } || requires(T&& state) { std::move(state).connectPending(); } || requires(T&& state) { std::move(state).tunnelOpen(); } || requires(T&& state) { std::move(state).endStreamQueued(); } || requires(T&& state) { std::move(state).endStreamCommitted(); } || requires(T&& state) { std::move(state).aborted(); };

template <typename T>
concept HasHttp2RemoteReceiveAlternatives = requires(const T& state) {
    { state.headPending() } -> std::same_as<const ruvia::detail::Http2RemoteHeadPending*>;
    { state.headEndStreamPending() } -> std::same_as<const ruvia::detail::Http2RemoteHeadEndStreamPending*>;
    { state.contentOpen() } -> std::same_as<const ruvia::detail::Http2RemoteContentOpen*>;
    { state.connectPending() } -> std::same_as<const ruvia::detail::Http2RemoteConnectPending*>;
    { state.connectPendingEndStream() } -> std::same_as<const ruvia::detail::Http2RemoteConnectPendingEndStream*>;
    { state.connectRejectedAwaitingEndStream() } -> std::same_as<const ruvia::detail::Http2RemoteConnectRejectedAwaitingEndStream*>;
    { state.tunnelOpen() } -> std::same_as<const ruvia::detail::Http2RemoteTunnelOpen*>;
    { state.endStream() } -> std::same_as<const ruvia::detail::Http2RemoteEndStream*>;
    { state.aborted() } -> std::same_as<const ruvia::detail::Http2RemoteAborted*>;
};

template <typename T>
concept HasAnyRvalueHttp2RemoteReceiveAccessor = requires(T&& state) { std::move(state).headPending(); } || requires(T&& state) { std::move(state).headEndStreamPending(); } || requires(T&& state) { std::move(state).contentOpen(); } || requires(T&& state) { std::move(state).connectPending(); } || requires(T&& state) { std::move(state).connectPendingEndStream(); } || requires(T&& state) { std::move(state).connectRejectedAwaitingEndStream(); } || requires(T&& state) { std::move(state).tunnelOpen(); } || requires(T&& state) { std::move(state).endStream(); } || requires(T&& state) { std::move(state).aborted(); };

template <typename T>
concept HasStaleHttp2BodyEnded = requires(const T& stream) { stream.bodyEnded(); };

template <typename T>
concept HasStaleHttp2PeerEndStream = requires(const T& stream) { stream.peerEndStream(); };

template <typename T>
concept HasStaleHttp2HeadersDecoded = requires(const T& stream) { stream.headersDecoded(); };

template <typename T>
concept HasHttp2LocalCloseSource = requires(const T& state) {
    { state.source() } -> std::same_as<ruvia::detail::Http2StreamCloseSource>;
};

template <typename T>
concept HasStaleHttp2LocalSendProduct = requires(const T& state) {
    state.localSendPhase();
    state.localMessageKind();
    state.localEndStreamCommitted();
};

template <typename T>
concept HasStaleHttp2StreamLocalSendForwarders = requires(const T& stream) {
    stream.localSendPhase();
    stream.localMessageKind();
    stream.localEndStream();
    stream.localEndStreamCommitted();
    stream.canSubmitLocalHead();
    stream.localBodyOpen();
    stream.localTrailersOnly();
};

template <typename T>
concept HasHttp2AbortLifecycle = requires(T& stream) {
    { stream.isAborted() } -> std::same_as<bool>;
    { stream.abort(ruvia::detail::Http2StreamCloseSource::kLocal) } -> std::same_as<bool>;
};

template <typename T>
concept HasStaleHttp2IsReset = requires(const T& stream) { stream.isReset(); };

template <typename T>
concept HasStaleHttp2MarkReset = requires(T& stream) { stream.markReset(ruvia::detail::Http2StreamCloseSource::kLocal); };

template <typename T>
concept HasStaleHttp2MarkClosed = requires(T& stream) { stream.markClosed(ruvia::detail::Http2StreamCloseSource::kLocal); };

template <typename T>
concept HasHttp2RemoveAborted = requires(T& table) { table.removeAborted([](const ruvia::detail::Http2StreamState&) noexcept {}); };

template <typename T>
concept HasStaleHttp2RemoveReset = requires(T& table) { table.removeReset([](const ruvia::detail::Http2StreamState&) noexcept {}); };

using HttpResponseBodySetter = void (ruvia::HttpResponse::*)(std::string_view);
using HttpResponseHeadersGetter = const ruvia::HttpResponseHeaders& (ruvia::HttpResponse::*)() const& noexcept;

template <typename T>
concept ExposesAnyRvalueResponseView = requires(T&& value) { std::move(value).headers(); } || requires(T&& value) { std::move(value).header(std::string_view{}); } || requires(T&& value) { std::move(value).begin(); } || requires(T&& value) { std::move(value).end(); } || requires(T&& value) { std::move(value).cbegin(); } || requires(T&& value) { std::move(value).cend(); };

template <typename T>
concept ExposesAnyRvalueResponseBodyBorrow = requires(T&& value) { std::move(value).empty(); } || requires(T&& value) { std::move(value).borrowedBytes(); } || requires(T&& value) { std::move(value).staticBytes(); } || requires(T&& value) { std::move(value).ownedBytes(); } || requires(T&& value) { std::move(value).ownedFile(); } || requires(T&& value) { std::move(value).borrowedFile(); } || requires(T&& value) { std::move(value).bytes(); } || requires(T&& value) { std::move(value).file(); } || requires(T&& value) { std::move(value).nativePathCStr(); };

template <typename T>
concept ExposesRvalueResponseBodyAccess = requires(T&& response) { ruvia::detail::responseBody(std::move(response)); } || requires(T&& response) { ruvia::detail::HttpResponseBodyAccess::body(std::move(response)); };

template <typename T>
concept ExposesRvalueResponseFileIdentityWords = requires(T&& identity) { std::move(identity).words(); };

static_assert(!HasLegacyResponseBodyCopy<ruvia::HttpResponse>);
static_assert(!HasLegacyResponseBodyView<ruvia::HttpResponse>);
static_assert(!std::is_copy_constructible_v<ruvia::HttpResponse>);
static_assert(!std::is_copy_assignable_v<ruvia::HttpResponse>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::HttpResponse>);
static_assert(std::is_nothrow_move_assignable_v<ruvia::HttpResponse>);
static_assert(std::same_as<decltype(static_cast<HttpResponseBodySetter>(&ruvia::HttpResponse::body)), HttpResponseBodySetter>);
static_assert(std::same_as<decltype(static_cast<HttpResponseHeadersGetter>(&ruvia::HttpResponse::headers)), HttpResponseHeadersGetter>);
static_assert(!std::default_initializable<ruvia::HttpResponseHeaders>);
static_assert(!std::constructible_from<ruvia::HttpResponseHeaders, std::pmr::memory_resource*>);
static_assert(!ExposesAnyRvalueResponseView<ruvia::HttpResponse>);
static_assert(!ExposesAnyRvalueResponseView<ruvia::HttpResponseHeaders>);
static_assert(!ExposesAnyRvalueResponseBodyBorrow<ruvia::detail::HttpResponseBody>);
static_assert(!ExposesAnyRvalueResponseBodyBorrow<ruvia::detail::HttpOwnedResponseBytes>);
static_assert(!ExposesAnyRvalueResponseBodyBorrow<ruvia::detail::HttpOwnedResponseFile>);
static_assert(!ExposesRvalueResponseFileIdentityWords<ruvia::detail::ResponseFileIdentity>);
static_assert(!ExposesRvalueResponseBodyAccess<ruvia::HttpResponse>);
static_assert(!HasSharedCacheFreshnessPolicy<ruvia::CacheControl>);
static_assert(std::same_as<decltype(ruvia::CacheControl::sMaxAge), std::optional<std::uint64_t>>);
static_assert(std::same_as<decltype(ruvia::CacheControl::noTransform), bool>);
static_assert(std::same_as<decltype(std::declval<ruvia::CacheControlFieldParser&>().update(std::string_view{})), void>);
static_assert(std::same_as<decltype(std::declval<const ruvia::CacheControlFieldParser&>().finish()), ruvia::CacheControl>);
static_assert(std::same_as<decltype(ruvia::CookieOptions{}.sameSite), std::optional<ruvia::CookieSameSite>>);
static_assert(std::same_as<decltype(ruvia::CookieOptions{}.priority), std::optional<ruvia::CookiePriority>>);
static_assert(std::same_as<decltype(ruvia::CookieOptions{}.prefix), std::optional<ruvia::CookiePrefix>>);
static_assert(std::same_as<decltype(ruvia::CookieOptions{}.maxAge), std::optional<std::chrono::seconds>>);
template <typename Text>
concept CookiePathAccepts = requires(ruvia::CookieOptions& options, Text&& text) { options.path = std::forward<Text>(text); };
template <typename Text>
concept CookieDomainAccepts = requires(ruvia::CookieOptions& options, Text&& text) { options.domain = std::forward<Text>(text); };
template <typename Name, typename Value, typename Options>
concept CanConstructSetCookiePlan = requires(Name&& name, Value&& value, Options&& options) { ruvia::detail::SetCookiePlan(std::forward<Name>(name), std::forward<Value>(value), std::forward<Options>(options)); };
static_assert(CookiePathAccepts<std::string&>);
static_assert(CookieDomainAccepts<const std::string&>);
static_assert(CookiePathAccepts<std::pmr::string&>);
static_assert(CookieDomainAccepts<const std::pmr::string&>);
static_assert(!CookiePathAccepts<std::string>);
static_assert(!CookiePathAccepts<const std::string>);
static_assert(!CookieDomainAccepts<std::string>);
static_assert(!CookieDomainAccepts<const std::string>);
static_assert(!CookiePathAccepts<std::pmr::string>);
static_assert(!CookieDomainAccepts<std::pmr::string>);
static_assert(CanConstructSetCookiePlan<std::string&, const std::string&, ruvia::CookieOptions&>);
static_assert(!CanConstructSetCookiePlan<std::string, std::string_view, ruvia::CookieOptions&>);
static_assert(!CanConstructSetCookiePlan<std::string_view, const std::string, ruvia::CookieOptions&>);
static_assert(!CanConstructSetCookiePlan<std::pmr::string, std::string_view, ruvia::CookieOptions&>);
static_assert(!CanConstructSetCookiePlan<std::string_view, std::string_view, ruvia::CookieOptions>);
static_assert(!CanConstructSetCookiePlan<std::string_view, std::string_view, const ruvia::CookieOptions>);
constexpr ruvia::CookieOptions kLiteralCookieOptions{.path = "/app", .domain = "example.com"};
static_assert(kLiteralCookieOptions.path.view() == "/app");
static_assert(kLiteralCookieOptions.domain.view() == "example.com");
static_assert(HasHttpClientRequestContentAlternatives<ruvia::HttpClientRequestContent>);
static_assert(!HasStaleHttpClientContentMode<ruvia::HttpClientRequestContent>);
static_assert(!HasHttpClientRequestContentValue<ruvia::HttpClientRequestContent>);
static_assert(HasHttpClientRequestContentValue<ruvia::HttpClientRequestBytes>);
static_assert(!HasHttpClientRequestContentValue<ruvia::HttpClientRequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestContent>);
static_assert(!std::is_constructible_v<ruvia::HttpClientRequest::HeaderInit, std::array<ruvia::HttpHeaderView, 1>&&>);
static_assert(!std::is_assignable_v<ruvia::HttpClientRequest::HeaderInit&, std::array<ruvia::HttpHeaderView, 1>&&>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestBytes>);

static_assert(HasHttp1PreparedContentAlternatives<ruvia::Http1ClientRequestContentPlan>);
static_assert(HasHttp1ClientExpectationAlternatives<ruvia::Http1ClientRequestWirePolicy>);
static_assert(!HasHttp1PreparedContentDisposition<ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasHttp1PreparedContentBytes<ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasHttp1PreparedContentBytes<ruvia::Http1ClientRequestWithoutContent>);
static_assert(HasHttp1PreparedContentBytes<ruvia::Http1ClientImmediateRequestContent>);
static_assert(HasHttp1PreparedContentBytes<ruvia::Http1ClientContinueGatedRequestContent>);
static_assert(!std::default_initializable<ruvia::Http1ClientRequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::Http1ClientImmediateRequestContent>);
static_assert(!std::default_initializable<ruvia::Http1ClientContinueGatedRequestContent>);

static_assert(HasHttp1ResponseHeadAlternatives<ruvia::detail::Http1ResponseHeadPlan>);
static_assert(HasHttp1ProtocolVersion<ruvia::detail::Http1ServerConnectionPlan>);
static_assert(HasHttp1ProtocolVersion<ruvia::detail::Http1ResponseHeadPlan>);
static_assert(HasHttp1BufferedContentLength<ruvia::detail::Http1BufferedResponseHead>);
static_assert(HasHttp1BufferedContentLength<ruvia::detail::Http1KnownLengthResponseStreamHead>);
static_assert(HasHttp1BufferedPlanContract<ruvia::detail::Http1BufferedResponsePlan>);
static_assert(!HasStaleHttp1BufferedWritePlanForwarder<ruvia::detail::Http1BufferedResponsePlan>);
static_assert(std::is_trivially_copyable_v<ruvia::detail::Http1BufferedResponsePlan>);
static_assert(sizeof(ruvia::detail::Http1BufferedResponsePlan) == sizeof(ruvia::detail::Http1ResponseHeadPlan));
static_assert(!HasStaleHttp1ResponseSignal<ruvia::detail::Http1ServerConnectionPlan>);
static_assert(!HasStaleHttp1ResponseHeadScalar<ruvia::detail::Http1ResponseHeadPlan>);
static_assert(!std::default_initializable<ruvia::detail::Http1ServerConnectionPlan>);
static_assert(!std::default_initializable<ruvia::detail::Http1ResponseHeadPlan>);
static_assert(!std::default_initializable<ruvia::detail::Http1BufferedResponsePlan>);
static_assert(!std::default_initializable<ruvia::detail::Http1BufferedResponseHead>);
static_assert(!std::default_initializable<ruvia::detail::Http1KnownLengthResponseStreamHead>);
static_assert(!std::default_initializable<ruvia::detail::Http1ChunkedResponseStreamHead>);
static_assert(!std::default_initializable<ruvia::detail::Http1CloseDelimitedResponseStreamHead>);
static_assert(!HasStalePreparedStreamPolicy<ruvia::detail::PreparedHttp1ResponseStream>);

static_assert(HasFinalResponseControlResult<ruvia::detail::Http1FinalResponseControlPlanResult, ruvia::detail::Http1FinalResponseControl, ruvia::detail::Http1FinalResponseControlPlanFailure>);
static_assert(HasFinalResponseControlResult<ruvia::detail::Http2FinalResponseControlPlanResult, ruvia::detail::Http2FinalResponseControl, ruvia::detail::Http2FinalResponseControlPlanFailure>);
static_assert(HasHttp1FinalResponseControlFields<ruvia::detail::Http1FinalResponseControl>);
static_assert(!HasHttp1FinalResponseControlFields<ruvia::detail::Http2FinalResponseControl>);
static_assert(!HasStaleFinalResponseControlStatus<ruvia::detail::Http1FinalResponseControlPlanResult>);
static_assert(!HasStaleFinalResponseControlStatus<ruvia::detail::Http2FinalResponseControlPlanResult>);
static_assert(!HasStaleTopLevelUpgradeProtocols<ruvia::detail::Http1FinalResponseControlPlanResult>);
static_assert(!std::default_initializable<ruvia::detail::Http1FinalResponseControl>);
static_assert(!std::default_initializable<ruvia::detail::Http2FinalResponseControl>);
static_assert(!std::default_initializable<ruvia::detail::Http1FinalResponseControlPlanFailure>);
static_assert(!std::default_initializable<ruvia::detail::Http2FinalResponseControlPlanFailure>);
static_assert(!std::default_initializable<ruvia::detail::Http1FinalResponseControlPlanResult>);
static_assert(!std::default_initializable<ruvia::detail::Http2FinalResponseControlPlanResult>);
static_assert(std::is_trivially_copyable_v<ruvia::detail::Http1FinalResponseControlPlanResult>);
static_assert(sizeof(ruvia::detail::Http1FinalResponseControlPlanResult) <= 8);
static_assert(std::is_trivially_copyable_v<ruvia::detail::Http2FinalResponseControlPlanResult>);
static_assert(sizeof(ruvia::detail::Http2FinalResponseControlPlanResult) <= 2);
static_assert(HasHttp1FinalCommitAlternatives<ruvia::detail::Http1FinalResponseCommitResult>);
static_assert(HasPreparedHttp1StreamAlternatives<ruvia::detail::PreparedHttp1ResponseStreamResult>);
static_assert(!HasUncheckedPreparedHttp1StreamExtraction<ruvia::detail::PreparedHttp1ResponseStreamResult>);
static_assert(!std::default_initializable<ruvia::detail::Http1FinalResponseCommitFailure>);
static_assert(std::derived_from<ruvia::detail::Http1FinalResponseCommitError, std::exception>);
static_assert(!HasRawHttp1FinalCommitError<ruvia::detail::Http1FinalResponseCommitFailure>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1FinalResponseCommitFailure&>().exception()), ruvia::detail::Http1FinalResponseCommitError>);
static_assert(std::is_trivially_copyable_v<ruvia::detail::Http1FinalResponseCommitResult>);
static_assert(sizeof(ruvia::detail::Http1FinalResponseCommitResult) <= 8);
static_assert(!std::default_initializable<ruvia::detail::Http1FinalResponseCommitResult>);
static_assert(!std::default_initializable<ruvia::detail::PreparedHttp1ResponseStreamResult>);

static_assert(HasHttp2ResponseHeadExecutionPlan<ruvia::detail::Http2ResponseHeadPlan>);
static_assert(!std::default_initializable<ruvia::detail::Http2ResponseHeadPlan>);
static_assert(std::is_trivially_copyable_v<ruvia::detail::Http2ResponseHeadPlan>);
static_assert(sizeof(ruvia::detail::Http2ResponseHeadPlan) <= 24);
static_assert(!std::default_initializable<ruvia::detail::Http2ResponseHeadPlanResult>);
static_assert(HasHttp2RequestContentAlternatives<ruvia::detail::Http2RequestContent>);
static_assert(!HasStaleHttp2ContentMode<ruvia::detail::Http2RequestContent>);
static_assert(!HasHttp2RequestContentLength<ruvia::detail::Http2RequestContent>);
static_assert(!HasHttp2RequestContentLength<ruvia::detail::Http2RequestWithoutContent>);
static_assert(HasHttp2RequestContentLength<ruvia::detail::Http2KnownLengthRequestContent>);
static_assert(!HasHttp2RequestContentLength<ruvia::detail::Http2StreamingRequestContent>);
static_assert(!std::default_initializable<ruvia::detail::Http2RequestContent>);
static_assert(!std::default_initializable<ruvia::detail::Http2RequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::detail::Http2KnownLengthRequestContent>);
static_assert(!std::default_initializable<ruvia::detail::Http2StreamingRequestContent>);
static_assert(HasHttp2LocalContentAlternatives<ruvia::detail::Http2LocalContentState>);
static_assert(!HasAnyRvalueHttp2LocalContentAccessor<ruvia::detail::Http2LocalContentState>);
static_assert(!HasStaleHttp2LocalModeAccessor<ruvia::detail::Http2LocalContentState>);
static_assert(!HasHttp2LocalDeclaredLength<ruvia::detail::Http2LocalContentState>);
static_assert(!HasHttp2LocalDeclaredLength<ruvia::detail::Http2LocalContentUnset>);
static_assert(!HasHttp2LocalDeclaredLength<ruvia::detail::Http2LocalContentForbidden>);
static_assert(!HasHttp2LocalDeclaredLength<ruvia::detail::Http2LocalContentUnbounded>);
static_assert(HasHttp2LocalDeclaredLength<ruvia::detail::Http2LocalContentKnownLength>);
static_assert(std::default_initializable<ruvia::detail::Http2LocalContentState>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalContentUnset>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalContentForbidden>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalContentUnbounded>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalContentKnownLength>);
static_assert(!HasStaleHttp2StreamLocalContentForwarders<ruvia::detail::Http2StreamState>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2StreamState&>().localContent()), const ruvia::detail::Http2LocalContentState&>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2StreamState&>().responseStatus()), const ruvia::HttpStatusCode*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2ClosedStreamHistory&>().source(std::uint32_t{})), std::optional<ruvia::detail::Http2StreamCloseSource>>);
static_assert(!std::default_initializable<ruvia::detail::HpackDecodeResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HpackDecodeResult&>().decoded()), const ruvia::detail::HpackDecoded*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HpackDecodeResult&>().failure()), const ruvia::detail::HpackDecodeFailure*>);
static_assert(!ExposesRvalueDecodedContent<ruvia::detail::HpackDecodeResult>);
static_assert(!ExposesRvalueDecodeFailure<ruvia::detail::HpackDecodeResult>);
static_assert(!std::default_initializable<ruvia::Http1RequestParseFailure>);
static_assert(!HasRawContentDecodeError<ruvia::Http1RequestParseFailure>);
static_assert(std::same_as<decltype(std::declval<const ruvia::Http1RequestParseFailure&>().protocolError()), ruvia::HttpProtocolError>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1ServerRequestParseState&>().failure()), const ruvia::detail::Http1ServerRequestParseFailure*>);
static_assert(HasHttp1ServerParseAlternatives<ruvia::detail::Http1ServerRequestParseState>);
static_assert(!HasAnyRvalueHttp1ServerParseAccessor<ruvia::detail::Http1ServerRequestParseState>);
static_assert(!HasStaleHttp1ServerParseScalars<ruvia::detail::Http1ServerRequestParseState>);
static_assert(!HasLegacyResponseCodingField<ruvia::detail::Http1ServerRequestParseState>);
static_assert(!std::default_initializable<ruvia::detail::HttpResponseCodingSelection>);
static_assert(!HasRawResponseCodingSelector<ruvia::detail::HttpResponseCodingQualities>);
static_assert(!HasRawResponseCodingScore<ruvia::detail::HttpAcceptedEncodingQuality>);
static_assert(!HasRawResponseIdentityScore<ruvia::detail::HttpAcceptedEncodingQuality>);
static_assert(HasTypedResponseCodingSelector<ruvia::detail::HttpResponseCodingSelection>);
static_assert(HasResponseCodingAcceptability<ruvia::detail::HttpResponseCodingSelection>);
static_assert(!HasLegacyOptionalResponseCodingSelector<ruvia::detail::HttpResponseCodingSelection>);
static_assert(!std::default_initializable<ruvia::detail::HttpResponseCodingSelectionResult>);
static_assert(HasTypedResponseCodingResult<ruvia::detail::HttpResponseCodingSelectionResult>);
static_assert(!std::default_initializable<ruvia::detail::HttpResponseCodingCandidates>);
static_assert(HasTypedResponseCodingCandidates<ruvia::detail::HttpResponseCodingCandidates>);
static_assert(HasAvailableResponseCodingSelector<ruvia::detail::HttpResponseCodingSelection, ruvia::detail::HttpResponseCodingCandidates>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1ServerRequestParseState&>().responseCodingSelection()), ruvia::detail::HttpResponseCodingSelectionResult>);
static_assert(HasHttp2RemoteContentAlternatives<ruvia::detail::Http2RemoteContentState>);
static_assert(!HasAnyRvalueHttp2RemoteContentAccessor<ruvia::detail::Http2RemoteContentState>);
static_assert(!HasStaleHttp2RemoteContentTuple<ruvia::detail::Http2RemoteContentState>);
static_assert(!HasHttp2RemoteDeclaredLength<ruvia::detail::Http2RemoteContentState>);
static_assert(!HasHttp2RemoteReceivedBytes<ruvia::detail::Http2RemoteContentState>);
static_assert(HasHttp2RemoteReceivedBytes<ruvia::detail::Http2RemoteContentAllowedWithoutLength>);
static_assert(HasHttp2RemoteReceivedBytes<ruvia::detail::Http2RemoteContentAllowedKnownLength>);
static_assert(!HasHttp2RemoteReceivedBytes<ruvia::detail::Http2RemoteContentMetadataOnlyWithoutLength>);
static_assert(!HasHttp2RemoteReceivedBytes<ruvia::detail::Http2RemoteContentMetadataOnlyKnownLength>);
static_assert(!HasHttp2RemoteDeclaredLength<ruvia::detail::Http2RemoteContentAllowedWithoutLength>);
static_assert(HasHttp2RemoteDeclaredLength<ruvia::detail::Http2RemoteContentAllowedKnownLength>);
static_assert(!HasHttp2RemoteDeclaredLength<ruvia::detail::Http2RemoteContentMetadataOnlyWithoutLength>);
static_assert(HasHttp2RemoteDeclaredLength<ruvia::detail::Http2RemoteContentMetadataOnlyKnownLength>);
static_assert(std::default_initializable<ruvia::detail::Http2RemoteContentState>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteContentAllowedWithoutLength>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteContentAllowedKnownLength>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteContentMetadataOnlyWithoutLength>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteContentMetadataOnlyKnownLength>);
static_assert(!HasStaleHttp2RemoteCheckAcceptSplit<ruvia::detail::Http2RemoteContentState>);
static_assert(!HasStaleHttp2StreamRemoteContentForwarders<ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2WebRuntimeState<ruvia::detail::Http2StreamState>);
static_assert(HasHttp2ReceiveDataRelease<ruvia::detail::Http2Connection>);
static_assert(!HasStaleHttp2ReceiveDeferral<ruvia::detail::Http2Connection>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2StreamState&>().remoteContent()), const ruvia::detail::Http2RemoteContentState&>);
static_assert(std::is_enum_v<ruvia::detail::HttpResponseContentSemantics>);
static_assert(sizeof(ruvia::detail::HttpResponseContentSemantics) == 1);
static_assert(std::is_enum_v<ruvia::detail::HttpRequestContentSemantics>);
static_assert(sizeof(ruvia::detail::HttpRequestContentSemantics) == 1);
static_assert(ruvia::detail::httpClientExpectationIsValid(false, ruvia::detail::HttpRequestContentIndication::kNoContent));
static_assert(!ruvia::detail::httpClientExpectationIsValid(true, ruvia::detail::HttpRequestContentIndication::kNoContent));
static_assert(ruvia::detail::httpClientExpectationIsValid(true, ruvia::detail::HttpRequestContentIndication::kWillFollow));
static_assert(ruvia::detail::httpRequestContentSemantics("TRACE") == ruvia::detail::HttpRequestContentSemantics::kForbidden);
static_assert(ruvia::detail::httpRequestContentSemantics("CONNECT") == ruvia::detail::HttpRequestContentSemantics::kForbidden);
static_assert(ruvia::detail::httpRequestContentSemantics("trace") == ruvia::detail::HttpRequestContentSemantics::kNoAdditionalRequirements);
static_assert(ruvia::detail::httpRequestContentSemantics("OPTIONS") == ruvia::detail::HttpRequestContentSemantics::kContentTypeRequired);
static_assert(ruvia::detail::httpRequestContentSemantics("POST") == ruvia::detail::HttpRequestContentSemantics::kNoAdditionalRequirements);
static_assert(std::is_trivially_copyable_v<ruvia::detail::HttpResponseBodyPlan>);
static_assert(sizeof(ruvia::detail::HttpResponseBodyPlan) <= 12);
static_assert(HasHttp2TunnelAlternatives<ruvia::detail::Http2TunnelState>);
static_assert(!HasAnyRvalueHttp2TunnelAccessor<ruvia::detail::Http2TunnelState>);
static_assert(!HasStaleHttp2TunnelKindPhase<ruvia::detail::Http2TunnelState>);
static_assert(!HasHttp2ConnectForm<ruvia::detail::Http2TunnelState>);
static_assert(!HasHttp2ConnectForm<ruvia::detail::Http2NotConnect>);
static_assert(HasHttp2ConnectForm<ruvia::detail::Http2ConnectPending>);
static_assert(!HasHttp2ConnectForm<ruvia::detail::Http2TunnelOpen>);
static_assert(!HasHttp2ConnectForm<ruvia::detail::Http2ConnectRejected>);
static_assert(std::default_initializable<ruvia::detail::Http2TunnelState>);
static_assert(!std::default_initializable<ruvia::detail::Http2NotConnect>);
static_assert(!std::default_initializable<ruvia::detail::Http2ConnectPending>);
static_assert(!std::default_initializable<ruvia::detail::Http2TunnelOpen>);
static_assert(!std::default_initializable<ruvia::detail::Http2ConnectRejected>);
static_assert(!HasStaleHttp2StreamTunnelForwarders<ruvia::detail::Http2StreamState>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2StreamState&>().tunnel()), const ruvia::detail::Http2TunnelState&>);
static_assert(HasHttp2LocalSendAlternatives<ruvia::detail::Http2LocalSendState>);
static_assert(!HasAnyRvalueHttp2LocalSendAccessor<ruvia::detail::Http2LocalSendState>);
static_assert(!HasStaleHttp2LocalSendProduct<ruvia::detail::Http2LocalSendState>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalSendState>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalHeadPending>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalRequestContentOpen>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalResponseContentOpen>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalResponseTrailersOnly>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalConnectPending>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalTunnelOpen>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalEndStreamQueued>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalEndStreamCommitted>);
static_assert(!std::default_initializable<ruvia::detail::Http2StreamAborted>);
static_assert(!std::constructible_from<ruvia::detail::Http2StreamAborted, ruvia::detail::Http2StreamCloseSource>);
static_assert(!HasHttp2LocalCloseSource<ruvia::detail::Http2LocalSendState>);
static_assert(!HasHttp2LocalCloseSource<ruvia::detail::Http2LocalEndStreamCommitted>);
static_assert(HasHttp2LocalCloseSource<ruvia::detail::Http2StreamAborted>);
static_assert(!HasStaleHttp2StreamLocalSendForwarders<ruvia::detail::Http2StreamState>);
static_assert(HasHttp2AbortLifecycle<ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2IsReset<ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2MarkReset<ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2MarkClosed<ruvia::detail::Http2StreamState>);
static_assert(HasHttp2RemoveAborted<ruvia::detail::Http2StreamTable>);
static_assert(!HasStaleHttp2RemoveReset<ruvia::detail::Http2StreamTable>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2StreamState&>().localSend()), const ruvia::detail::Http2LocalSendState&>);
static_assert(HasHttp2RemoteReceiveAlternatives<ruvia::detail::Http2RemoteReceiveState>);
static_assert(!HasAnyRvalueHttp2RemoteReceiveAccessor<ruvia::detail::Http2RemoteReceiveState>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteReceiveState>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteHeadPending>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteHeadEndStreamPending>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteContentOpen>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteConnectPending>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteConnectPendingEndStream>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteConnectRejectedAwaitingEndStream>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteTunnelOpen>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteEndStream>);
static_assert(!std::default_initializable<ruvia::detail::Http2RemoteAborted>);
static_assert(!HasStaleHttp2BodyEnded<ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2PeerEndStream<ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2HeadersDecoded<ruvia::detail::Http2StreamState>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2StreamState&>().remoteReceive()), const ruvia::detail::Http2RemoteReceiveState&>);

template <typename T>
concept HasHttp1RequestBodyPlanAlternatives = requires(const T& plan) {
    { plan.withoutBody() } -> std::same_as<const ruvia::detail::Http1RequestWithoutBody*>;
    { plan.knownLength() } -> std::same_as<const ruvia::detail::Http1KnownLengthRequestBody*>;
    { plan.chunked() } -> std::same_as<const ruvia::detail::Http1ChunkedRequestBody*>;
};

template <typename T>
concept HasHttp1RequestFramingAccessor = requires(const T& plan) { plan.mode(); };

template <typename T>
concept HasHttp1RequestPlanContentLength = requires(const T& framing) {
    { framing.contentLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasHttp1RequestPlanTransferCodings = requires(const T& framing) {
    { framing.transferCodings() } -> std::same_as<ruvia::detail::HttpTransferCodings>;
} && requires(const T&& framing) {
    { std::move(framing).transferCodings() } -> std::same_as<ruvia::detail::HttpTransferCodings>;
};

template <typename T>
concept HasTypedHttpServerExpectationPlan = requires(const T& state) {
    { state.expectationPlan(ruvia::detail::HttpUnsupportedExpectationPolicy::kReject) } -> std::same_as<ruvia::detail::HttpServerExpectationPlan>;
};

template <typename T>
concept ExposesRvalueHttpServerExpectationAlternative = requires(T&& plan) { std::move(plan).noAction(); } || requires(T&& plan) { std::move(plan).sendContinue(); } || requires(T&& plan) { std::move(plan).rejection(); };

template <typename T>
concept HasHttpServerExpectationAlternatives = requires(const T& plan) {
    { plan.noAction() } -> std::same_as<const ruvia::detail::HttpNoServerExpectationAction*>;
    { plan.sendContinue() } -> std::same_as<const ruvia::detail::HttpSendContinue*>;
    { plan.rejection() } -> std::same_as<const ruvia::detail::HttpUnsupportedExpectationRejection*>;
};

template <typename T>
concept HasPublicHttp1RequestBodyPlanFactories = requires {
    T::makeWithoutBody();
    T::makeKnownLength(std::size_t{});
    T::makeChunked(ruvia::detail::HttpTransferCodings{});
};

static_assert(HasHttp1RequestBodyPlanAlternatives<ruvia::detail::Http1RequestBodyPlan>);
static_assert(!HasHttp1RequestFramingAccessor<ruvia::detail::Http1RequestBodyPlan>);
static_assert(!HasHttp1RequestPlanContentLength<ruvia::detail::Http1RequestBodyPlan>);
static_assert(!HasHttp1RequestPlanTransferCodings<ruvia::detail::Http1RequestBodyPlan>);
static_assert(HasHttp1RequestPlanContentLength<ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!HasHttp1RequestPlanContentLength<ruvia::detail::Http1ChunkedRequestBody>);
static_assert(HasHttp1RequestPlanTransferCodings<ruvia::detail::Http1ChunkedRequestBody>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1RequestBodyPlan&>().expectations()), ruvia::detail::HttpRequestExpectations>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1RequestBodyPlan&&>().expectations()), ruvia::detail::HttpRequestExpectations>);
static_assert(!HasHttp1RequestPlanTransferCodings<ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1ClientRequestContext&>().connectionOptions()), ruvia::detail::HttpConnectionOptions>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1ClientRequestContext&&>().connectionOptions()), ruvia::detail::HttpConnectionOptions>);
static_assert(std::is_trivially_copyable_v<ruvia::detail::HttpUpgradeProtocols>);
static_assert(sizeof(ruvia::detail::HttpUpgradeProtocols) == 1);
static_assert(std::is_trivially_copyable_v<ruvia::detail::HttpConnectionOptions>);
static_assert(sizeof(ruvia::detail::HttpConnectionOptions) == 1);
static_assert(!HasPublicHttp1RequestBodyPlanFactories<ruvia::detail::Http1RequestBodyPlan>);
static_assert(!std::default_initializable<ruvia::detail::Http1RequestBodyPlan>);
static_assert(!std::constructible_from<ruvia::detail::Http1RequestBodyPlan, ruvia::detail::HttpRequestExpectations>);
static_assert(!std::default_initializable<ruvia::detail::Http1RequestWithoutBody>);
static_assert(!std::default_initializable<ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!std::default_initializable<ruvia::detail::Http1ChunkedRequestBody>);
static_assert(!std::constructible_from<ruvia::detail::Http1KnownLengthRequestBody, std::size_t>);
static_assert(!std::constructible_from<ruvia::detail::Http1ChunkedRequestBody, ruvia::detail::HttpTransferCodings>);
static_assert(HasTypedHttpServerExpectationPlan<ruvia::detail::Http1RequestBodyPlan>);
static_assert(HasTypedHttpServerExpectationPlan<ruvia::detail::Http2StreamState>);
static_assert(HasHttpServerExpectationAlternatives<ruvia::detail::HttpServerExpectationPlan>);
static_assert(!ExposesRvalueHttpServerExpectationAlternative<ruvia::detail::HttpServerExpectationPlan>);
static_assert(!std::default_initializable<ruvia::detail::HttpServerExpectationPlan>);
static_assert(!std::default_initializable<ruvia::detail::HttpNoServerExpectationAction>);
static_assert(!std::default_initializable<ruvia::detail::HttpSendContinue>);
static_assert(!std::default_initializable<ruvia::detail::HttpUnsupportedExpectationRejection>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpUnsupportedExpectationRejection&>().protocolError()), ruvia::HttpProtocolError>);
static_assert(!std::default_initializable<ruvia::detail::HttpRequestBodyFailure>);
static_assert(std::same_as<decltype(ruvia::detail::HttpRequestBodyFailure::tooLarge()), ruvia::detail::HttpRequestBodyFailure>);
static_assert(std::same_as<decltype(ruvia::detail::HttpRequestBodyFailure::incomplete()), ruvia::detail::HttpRequestBodyFailure>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpRequestBodyFailure&>().protocolError()), ruvia::HttpProtocolError>);
static_assert(std::same_as<decltype(ruvia::detail::httpRequestBodySizeFailure(std::size_t{}, ruvia::ProtocolByteLimit::unlimited())), std::optional<ruvia::detail::HttpRequestBodyFailure>>);
static_assert(std::same_as<decltype(ruvia::detail::httpRequestBodyAdditionFailure(std::size_t{}, std::size_t{}, ruvia::ProtocolByteLimit::unlimited())), std::optional<ruvia::detail::HttpRequestBodyFailure>>);
static_assert(!std::default_initializable<ruvia::detail::Http2RequestBuildResult>);
static_assert(!std::default_initializable<ruvia::detail::Http2RequestBuilt>);
static_assert(!std::default_initializable<ruvia::detail::Http2RequestBuildFailure>);
static_assert(!ExposesRvalueHttp2RequestBuildAlternative<ruvia::detail::Http2RequestBuildResult>);
static_assert(!HasRawContentDecodeError<ruvia::detail::Http2RequestBuildFailure>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2RequestBuildResult&>().built()), const ruvia::detail::Http2RequestBuilt*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2RequestBuildResult&>().failure()), const ruvia::detail::Http2RequestBuildFailure*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2RequestBuildFailure&>().protocolError()), ruvia::HttpProtocolError>);
static_assert(std::is_enum_v<ruvia::detail::Http1ClientRequestContentPhase>);
static_assert(sizeof(ruvia::detail::Http1ClientRequestContentPhase) == 1);
static_assert(ruvia::detail::Http1ClientRequestContentPhase::kContentCompleteAwaitingContinue != ruvia::detail::Http1ClientRequestContentPhase::kContinueReceivedContentComplete);

template <typename T>
concept HasHttp1ClientResponsePlanAlternatives = requires(const T& plan) {
    { plan.informational() } -> std::same_as<const ruvia::Http1ClientInformationalResponse*>;
    { plan.withoutContent() } -> std::same_as<const ruvia::Http1ClientResponseWithoutContent*>;
    { plan.zeroContent() } -> std::same_as<const ruvia::Http1ClientResponseWithZeroContent*>;
    { plan.knownLength() } -> std::same_as<const ruvia::Http1ClientKnownLengthResponse*>;
    { plan.chunked() } -> std::same_as<const ruvia::Http1ClientChunkedResponse*>;
    { plan.closeDelimited() } -> std::same_as<const ruvia::Http1ClientCloseDelimitedResponse*>;
    { plan.connectTunnel() } -> std::same_as<const ruvia::Http1ClientConnectTunnel*>;
    { plan.protocolUpgrade() } -> std::same_as<const ruvia::Http1ClientProtocolUpgrade*>;
    { plan.requestContentSignal() } -> std::same_as<std::optional<ruvia::Http1ClientRequestContentSignal>>;
};

template <typename T>
concept HasHttp1ClientZeroContentFraming = requires(const T& plan) {
    { plan.knownLength() } -> std::same_as<const ruvia::Http1ClientKnownLengthResponse*>;
    { plan.chunked() } -> std::same_as<const ruvia::Http1ClientChunkedResponse*>;
    { plan.closeDelimited() } -> std::same_as<const ruvia::Http1ClientCloseDelimitedResponse*>;
};

template <typename T>
concept HasAnyRvalueHttp1ClientResponsePlanAccessor = requires(T&& plan) { std::move(plan).informational(); } || requires(T&& plan) { std::move(plan).withoutContent(); } || requires(T&& plan) { std::move(plan).zeroContent(); } || requires(T&& plan) { std::move(plan).knownLength(); } || requires(T&& plan) { std::move(plan).chunked(); } || requires(T&& plan) { std::move(plan).closeDelimited(); } || requires(T&& plan) { std::move(plan).connectTunnel(); } || requires(T&& plan) { std::move(plan).protocolUpgrade(); };

template <typename T>
concept HasAnyRvalueHttpClientHeaderLookupAccessor = requires(T&& result) { std::move(result).absent(); } || requires(T&& result) { std::move(result).found(); } || requires(T&& result) { std::move(result).repeated(); };

template <typename T>
concept HasAnyRvalueHttpClientRedirectTargetAccessor = requires(T&& result) { std::move(result).target(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept ExposesAnyRvalueHttpClientOwnedView = requires(T&& value) { std::move(value).name(); } || requires(T&& value) { std::move(value).value(); } || requires(T&& value) { std::move(value).headers(); } || requires(T&& value) { std::move(value).body(); } || requires(T&& value) { std::move(value).method(); };

template <typename T>
concept HasHttpClientResponseBody = requires(const T& head) {
    { head.body() } -> std::same_as<std::string_view>;
};

template <typename T>
concept AcceptsTemporaryHttpClientResponseHeaderLookup = requires(T&& response) { ruvia::lookupUniqueHttpClientResponseHeader(std::move(response), std::string_view{}); };

template <typename T>
concept HasHttp1ClientResponseMode = requires(const T& plan) { plan.mode(); };

template <typename T>
concept HasHttp1ClientResponseConnectionAccessor = requires(const T& plan) { plan.connectionDisposition(); };

template <typename T>
concept HasHttp1ClientResponseContentLength = requires(const T& framing) {
    { framing.contentLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasHttp1ClientResponseTransferCodings = requires(const T& framing) {
    { framing.transferCodings() } -> std::same_as<ruvia::detail::HttpTransferCodings>;
} && requires(const T&& framing) {
    { std::move(framing).transferCodings() } -> std::same_as<ruvia::detail::HttpTransferCodings>;
};

template <typename T>
concept HasHttp1ClientResponsePersistence = requires(const T& framing) {
    { framing.persistence() } -> std::same_as<ruvia::Http1ClientResponsePersistence>;
};

static_assert(HasHttp1ClientResponsePlanAlternatives<ruvia::Http1ClientResponsePlan>);
static_assert(HasHttp1ClientZeroContentFraming<ruvia::Http1ClientResponseWithZeroContent>);
static_assert(!HasHttp1ClientResponseMode<ruvia::Http1ClientResponsePlan>);
static_assert(!HasHttp1ClientResponseConnectionAccessor<ruvia::Http1ClientResponsePlan>);
static_assert(!HasHttp1ClientResponseContentLength<ruvia::Http1ClientResponsePlan>);
static_assert(HasHttp1ClientResponseContentLength<ruvia::Http1ClientKnownLengthResponse>);
static_assert(!HasHttp1ClientResponseContentLength<ruvia::Http1ClientChunkedResponse>);
static_assert(HasHttp1ClientResponseTransferCodings<ruvia::Http1ClientChunkedResponse>);
static_assert(HasHttp1ClientResponseTransferCodings<ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!HasHttp1ClientResponseTransferCodings<ruvia::Http1ClientKnownLengthResponse>);
static_assert(HasHttp1ClientResponsePersistence<ruvia::Http1ClientInformationalResponse>);
static_assert(HasHttp1ClientResponsePersistence<ruvia::Http1ClientResponseWithoutContent>);
static_assert(HasHttp1ClientResponsePersistence<ruvia::Http1ClientKnownLengthResponse>);
static_assert(HasHttp1ClientResponsePersistence<ruvia::Http1ClientChunkedResponse>);
static_assert(!HasHttp1ClientResponsePersistence<ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!std::default_initializable<ruvia::Http1ClientResponsePlan>);
static_assert(!std::default_initializable<ruvia::Http1ClientInformationalResponse>);
static_assert(!std::default_initializable<ruvia::Http1ClientResponseWithoutContent>);
static_assert(!std::default_initializable<ruvia::Http1ClientResponseWithZeroContent>);
static_assert(!std::default_initializable<ruvia::Http1ClientKnownLengthResponse>);
static_assert(!std::default_initializable<ruvia::Http1ClientChunkedResponse>);
static_assert(!std::default_initializable<ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!std::default_initializable<ruvia::Http1ClientConnectTunnel>);
static_assert(!std::default_initializable<ruvia::Http1ClientProtocolUpgrade>);

static_assert(std::same_as<decltype(std::declval<ruvia::detail::Http2Connection&>().nextEvent()), std::optional<ruvia::detail::Http2Event>>);
static_assert(!std::default_initializable<ruvia::detail::Http2Event>);
static_assert(!ExposesAnyRvalueSansIoEventBorrow<ruvia::detail::Http2Event>);
static_assert(!ExposesAnyRvalueSansIoEventBorrow<ruvia::detail::Http2GoawayEvent>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2Event&>().streamClosed()), const ruvia::detail::Http2StreamClosedEvent*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2Event&>().goaway()), const ruvia::detail::Http2GoawayEvent*>);
static_assert(HasHttp2EventError<ruvia::detail::Http2StreamClosedEvent>);
static_assert(HasHttp2EventError<ruvia::detail::Http2GoawayEvent>);
static_assert(!HasHttp2EventError<ruvia::detail::Http2RequestUnprocessedEvent>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::Http2Connection&>().feed(std::string_view{})), ruvia::detail::Http2FeedResult>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::Http2Connection&>().consumeOutput(std::size_t{})), ruvia::detail::Http2OutputConsumeStatus>);
static_assert(std::is_enum_v<ruvia::detail::Http2FeedResult>);
static_assert(!HasFeedStatusField<ruvia::detail::Http2FeedResult>);
static_assert(!HasFeedConsumedField<ruvia::detail::Http2FeedResult>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::Http2PeerSettings&>().apply(ruvia::detail::Http2SettingId::kHeaderTableSize, std::uint32_t{})), ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!std::default_initializable<ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2PeerSettingApplyResult&>().applied()), const ruvia::detail::Http2PeerSettingApplied*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2PeerSettingApplyResult&>().initialWindowChange()), const ruvia::detail::Http2PeerInitialWindowChange*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2PeerSettingApplyResult&>().failure()), const ruvia::detail::Http2PeerSettingFailure*>);
static_assert(!HasPeerSettingStatusField<ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingChangedField<ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingDeltaField<ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingDeltaAccessor<ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingErrorAccessor<ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingDeltaAccessor<ruvia::detail::Http2PeerSettingApplied>);
static_assert(!HasPeerSettingErrorAccessor<ruvia::detail::Http2PeerSettingApplied>);
static_assert(HasPeerSettingDeltaAccessor<ruvia::detail::Http2PeerInitialWindowChange>);
static_assert(!HasPeerSettingErrorAccessor<ruvia::detail::Http2PeerInitialWindowChange>);
static_assert(!HasPeerSettingDeltaAccessor<ruvia::detail::Http2PeerSettingFailure>);
static_assert(HasPeerSettingErrorAccessor<ruvia::detail::Http2PeerSettingFailure>);
static_assert(!std::default_initializable<ruvia::detail::Http2PeerSettingApplied>);
static_assert(!std::constructible_from<ruvia::detail::Http2PeerInitialWindowChange, std::int64_t>);
static_assert(!std::constructible_from<ruvia::detail::Http2PeerSettingFailure, ruvia::detail::Http2PeerSettingError>);
static_assert(std::same_as<decltype(ruvia::detail::resolveHttpByteRange(std::string_view{}, std::uint64_t{})), ruvia::detail::HttpByteRangeResolution>);
static_assert(!std::default_initializable<ruvia::detail::HttpByteRangeResolution>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpByteRangeResolution&>().ignored()), const ruvia::detail::HttpByteRangeIgnored*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpByteRangeResolution&>().unsatisfiable()), const ruvia::detail::HttpByteRangeUnsatisfiable*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpByteRangeResolution&>().resolved()), const ruvia::detail::HttpResolvedByteRange*>);
static_assert(!HasByteRangeOutcomeField<ruvia::detail::HttpByteRangeResolution>);
static_assert(!HasByteRangePayloadField<ruvia::detail::HttpByteRangeResolution>);
static_assert(!HasByteRangeOffsetAccessor<ruvia::detail::HttpByteRangeResolution>);
static_assert(!HasByteRangeLengthAccessor<ruvia::detail::HttpByteRangeResolution>);
static_assert(!HasByteRangeOffsetAccessor<ruvia::detail::HttpByteRangeIgnored>);
static_assert(!HasByteRangeLengthAccessor<ruvia::detail::HttpByteRangeUnsatisfiable>);
static_assert(HasByteRangeOffsetAccessor<ruvia::detail::HttpResolvedByteRange>);
static_assert(HasByteRangeLengthAccessor<ruvia::detail::HttpResolvedByteRange>);
static_assert(!std::default_initializable<ruvia::detail::HttpByteRangeIgnored>);
static_assert(!std::default_initializable<ruvia::detail::HttpByteRangeUnsatisfiable>);
static_assert(!std::default_initializable<ruvia::detail::HttpResolvedByteRange>);
static_assert(!std::constructible_from<ruvia::detail::HttpResolvedByteRange, std::uint64_t, std::uint64_t>);
static_assert(!std::default_initializable<ruvia::detail::Http2RequestHeadSubmitResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2RequestHeadSubmitResult&>().submitted()), const ruvia::detail::Http2SubmittedRequestHead*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2RequestHeadSubmitResult&>().failure()), const ruvia::detail::Http2RequestHeadSubmitFailure*>);
static_assert(!HasRequestHeadStatusAccessor<ruvia::detail::Http2RequestHeadSubmitResult>);
static_assert(!HasRequestHeadAcceptedAccessor<ruvia::detail::Http2RequestHeadSubmitResult>);
static_assert(!HasRequestHeadStreamIdAccessor<ruvia::detail::Http2RequestHeadSubmitResult>);
static_assert(!std::constructible_from<ruvia::detail::Http2SubmittedRequestHead, std::uint32_t>);
static_assert(HasRequestHeadStreamIdAccessor<ruvia::detail::Http2SubmittedRequestHead>);
static_assert(!HasRequestHeadErrorAccessor<ruvia::detail::Http2SubmittedRequestHead>);
static_assert(HasRequestHeadErrorAccessor<ruvia::detail::Http2RequestHeadSubmitFailure>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::Http2Connection&>().submitResponseHead(std::uint32_t{}, std::declval<const ruvia::HttpResponse&>(), std::declval<ruvia::detail::HttpBufferedResponseWritePlan>())), ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!AcceptsUnpreparedBufferedResponseHead<ruvia::detail::Http2Connection>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::Http2Connection&>().finishResponse(std::uint32_t{}, std::declval<const ruvia::detail::HttpResponseTrailerSection&>())), ruvia::detail::Http2FinishSubmitStatus>);
static_assert(!AcceptsStagedResponseTrailerSection<ruvia::detail::Http2Connection>);
static_assert(!AcceptsImplicitResponseFinish<ruvia::detail::Http2Connection>);
static_assert(!AcceptsRawResponseTrailerFinish<ruvia::detail::Http2Connection>);
using ResponseTrailerHeaderArray = std::array<ruvia::HttpHeaderView, 1>;
using ResponseTrailerHeaderVector = std::vector<ruvia::HttpHeaderView>;
static_assert(AcceptsResponseTrailerRange<ResponseTrailerHeaderArray&>);
static_assert(AcceptsResponseTrailerRange<const ResponseTrailerHeaderArray&>);
static_assert(!AcceptsResponseTrailerRange<ResponseTrailerHeaderArray>);
static_assert(!AcceptsResponseTrailerRange<ResponseTrailerHeaderVector>);
static_assert(AcceptsResponseTrailerRange<std::span<const ruvia::HttpHeaderView>>);
static_assert(AcceptsValidatedResponseTrailerRange<ResponseTrailerHeaderArray&>);
static_assert(AcceptsValidatedResponseTrailerRange<const ResponseTrailerHeaderArray&>);
static_assert(!AcceptsValidatedResponseTrailerRange<ResponseTrailerHeaderArray>);
static_assert(!AcceptsValidatedResponseTrailerRange<ResponseTrailerHeaderVector>);
static_assert(AcceptsValidatedResponseTrailerRange<std::span<const ruvia::HttpHeaderView>>);
static_assert(HasResponseTrailerSectionAlternatives<ruvia::detail::HttpResponseTrailerSectionResult>);
static_assert(!std::default_initializable<ruvia::detail::HttpResponseTrailerSection>);
static_assert(!std::default_initializable<ruvia::detail::HttpResponseTrailerSectionFailure>);
static_assert(!std::default_initializable<ruvia::detail::HttpResponseTrailerSectionResult>);
static_assert(std::derived_from<ruvia::detail::HttpResponseTrailerSectionError, std::exception>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpResponseTrailerSectionFailure&>().exception()), ruvia::detail::HttpResponseTrailerSectionError>);
static_assert(!HasStagedResponseTrailerBlock<ruvia::detail::Http2StreamState>);
static_assert(!HasStagedResponseTrailers<ruvia::detail::Http2StreamHeaderBlocks>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::Http2Connection&>().submitStreamingResponseHead(std::uint32_t{}, std::declval<ruvia::HttpResponse>(), ruvia::detail::ResponseStreamKind::kGeneric, ruvia::detail::ResponseTrailerIntent::kNone)), ruvia::detail::Http2StreamingResponseHeadSubmitResult>);
static_assert(!std::default_initializable<ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!std::default_initializable<ruvia::detail::Http2StreamingResponseHeadSubmitResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2BufferedResponseHeadSubmitResult&>().submitted()), const ruvia::detail::HttpBufferedResponseWritePlan*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2StreamingResponseHeadSubmitResult&>().submitted()), const ruvia::detail::ResponseStreamCommitPlan*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2BufferedResponseHeadSubmitResult&>().failure()), const ruvia::detail::Http2ResponseHeadSubmitFailure*>);
static_assert(!HasResponseHeadStatusAccessor<ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadAcceptedAccessor<ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadPlanAccessor<ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadErrorAccessor<ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
using ResponseStreamCommitPlanner = ruvia::detail::ResponseStreamCommitPlan (*)(ruvia::detail::ResponseStreamFraming, ruvia::HttpKnownMethod, ruvia::HttpStatusCode, ruvia::detail::ResponseTrailerIntent) noexcept;
using ResponseStreamHeadPreparer = ruvia::detail::ResponseStreamHead (*)(ruvia::HttpResponse, ruvia::detail::ResponseStreamKind, ruvia::detail::ResponseStreamCommitPlan);
using BufferedResponseWritePlanner = ruvia::detail::HttpBufferedResponseWritePlan (*)(ruvia::HttpKnownMethod, const ruvia::HttpResponse&) noexcept;
static_assert(std::same_as<decltype(&ruvia::detail::httpBufferedResponseWritePlan), BufferedResponseWritePlanner>);
static_assert(std::same_as<decltype(&ruvia::detail::httpResponseStreamCommitPlan), ResponseStreamCommitPlanner>);
static_assert(std::same_as<decltype(&ruvia::detail::prepareResponseStreamHead), ResponseStreamHeadPreparer>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::ResponseStreamCommitPlan&>().responseStatus()), ruvia::HttpStatusCode>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::ResponseStreamCommitPlan&>().framing()), ruvia::detail::ResponseStreamFraming>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpResponseBodyPlan&>().responseStatus()), ruvia::HttpStatusCode>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpBufferedResponseWritePlan&>().responseStatus()), ruvia::HttpStatusCode>);
template <typename Plan>
concept HasValueSemanticResponseWritePolicy = requires(const Plan& plan) {
    { plan.policy() } -> std::same_as<ruvia::detail::ResponseWritePolicy>;
} && requires(const Plan&& plan) {
    { std::move(plan).policy() } -> std::same_as<ruvia::detail::ResponseWritePolicy>;
};
template <typename Policy>
concept ExposesAnyRvalueResponseWritePolicyAlternative = requires(const Policy&& policy) { std::move(policy).normal(); } || requires(const Policy&& policy) { std::move(policy).bodyForbidden(); } || requires(const Policy&& policy) { std::move(policy).zeroLength(); } || requires(const Policy&& policy) { std::move(policy).notModified(); };
template <typename Policy>
concept HasLegacyResponseWritePolicyFactory = requires { Policy::zeroLengthContent(); };
static_assert(!std::default_initializable<ruvia::detail::ResponseWritePolicy>);
static_assert(!std::default_initializable<ruvia::detail::ResponseNormalWrite>);
static_assert(!std::default_initializable<ruvia::detail::ResponseBodyForbiddenWrite>);
static_assert(!std::default_initializable<ruvia::detail::ResponseZeroLengthWrite>);
static_assert(!std::default_initializable<ruvia::detail::ResponseNotModifiedWrite>);
static_assert(!ExposesAnyRvalueResponseWritePolicyAlternative<ruvia::detail::ResponseWritePolicy>);
static_assert(!HasLegacyResponseWritePolicyFactory<ruvia::detail::ResponseWritePolicy>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::ResponseWritePolicy&>().normal()), const ruvia::detail::ResponseNormalWrite*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::ResponseWritePolicy&>().bodyForbidden()), const ruvia::detail::ResponseBodyForbiddenWrite*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::ResponseWritePolicy&>().zeroLength()), const ruvia::detail::ResponseZeroLengthWrite*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::ResponseWritePolicy&>().notModified()), const ruvia::detail::ResponseNotModifiedWrite*>);
static_assert(sizeof(ruvia::detail::ResponseWritePolicy) <= 2);
static_assert(HasValueSemanticResponseWritePolicy<ruvia::detail::HttpResponseBodyPlan>);
static_assert(HasValueSemanticResponseWritePolicy<ruvia::detail::HttpBufferedResponseWritePlan>);
static_assert(!AcceptsLooseResponseStreamBodyPlan<ruvia::detail::HttpResponseBodyPlan>);
static_assert(!AcceptsLooseBufferedResponseBodyPlan<ruvia::detail::HttpResponseBodyPlan>);
static_assert(!HasResponseHeadErrorAccessor<ruvia::detail::Http2ResponseHeadSubmitFailure>);
static_assert(HasResponseHeadFailureContract<ruvia::detail::Http2ResponseHeadSubmitFailure>);
static_assert(std::derived_from<ruvia::detail::Http2ResponseHeadSubmitError, std::exception>);
static_assert(std::is_trivially_copyable_v<ruvia::detail::Http2ResponseHeadSubmitFailure>);
static_assert(sizeof(ruvia::detail::Http2ResponseHeadSubmitFailure) <= 1);
static_assert(!HasResponseHeadPlanAccessor<ruvia::detail::Http2ResponseHeadSubmitFailure>);
static_assert(!std::constructible_from<ruvia::detail::Http2ResponseHeadSubmitFailure, ruvia::detail::Http2ResponseHeadSubmitError>);
using WebSocketServerNegotiator = ruvia::detail::WebSocketServerNegotiation (*)(const ruvia::HttpRequest&, std::string_view, std::pmr::memory_resource*);
using HttpWebSocketServerHandshakeFactory = ruvia::detail::HttpWebSocketServerHandshake (*)(const ruvia::HttpRequest&, std::string_view, std::pmr::memory_resource*);
using Http1WebSocketHandshakeValidator = ruvia::detail::HttpWebSocketHandshakeValidationResult (*)(const ruvia::HttpRequest&, const ruvia::detail::Http1RequestBodyPlan&) noexcept;
using Http2WebSocketHandshakeValidator = ruvia::detail::HttpWebSocketHandshakeValidationResult (*)(const ruvia::detail::Http2StreamState&, const ruvia::HttpRequest&) noexcept;
using WebSocketHandshakeFieldValidator = bool (*)(const ruvia::HttpRequest&) noexcept;
using WebSocketClientOfferHeaderValidator = bool (*)(std::span<const ruvia::HttpHeaderView>) noexcept;
static_assert(std::same_as<decltype(&ruvia::detail::makeWebSocketServerNegotiation), WebSocketServerNegotiator>);
static_assert(std::same_as<decltype(&ruvia::detail::makeHttpWebSocketServerHandshake), HttpWebSocketServerHandshakeFactory>);
static_assert(std::same_as<decltype(&ruvia::detail::validateHttp1WebSocketHandshake), Http1WebSocketHandshakeValidator>);
static_assert(std::same_as<decltype(&ruvia::detail::validateHttp2WebSocketHandshake), Http2WebSocketHandshakeValidator>);
static_assert(std::same_as<decltype(&ruvia::detail::webSocketSubprotocolOffersValid), WebSocketHandshakeFieldValidator>);
static_assert(std::same_as<decltype(&ruvia::detail::webSocketExtensionOffersValid), WebSocketHandshakeFieldValidator>);
static_assert(std::same_as<decltype(&ruvia::detail::webSocketClientOfferHeadersValid), WebSocketClientOfferHeaderValidator>);
static_assert(!std::default_initializable<ruvia::detail::HttpWebSocketHandshakeAccepted>);
static_assert(!std::default_initializable<ruvia::detail::HttpWebSocketHandshakeFailure>);
static_assert(!std::default_initializable<ruvia::detail::HttpWebSocketHandshakeValidationResult>);
static_assert(!ExposesRvalueWebSocketHandshakeValidationAlternative<ruvia::detail::HttpWebSocketHandshakeValidationResult>);
static_assert(!HasRawContentDecodeError<ruvia::detail::HttpWebSocketHandshakeFailure>);
static_assert(AppliesRequiredWebSocketResponseHeaders<ruvia::detail::HttpWebSocketHandshakeFailure>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpWebSocketHandshakeValidationResult&>().accepted()), const ruvia::detail::HttpWebSocketHandshakeAccepted*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpWebSocketHandshakeValidationResult&>().failure()), const ruvia::detail::HttpWebSocketHandshakeFailure*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpWebSocketHandshakeFailure&>().protocolError()), ruvia::HttpProtocolError>);
static_assert(std::is_enum_v<ruvia::detail::WebSocketDeflateNegotiation>);
static_assert(!std::constructible_from<ruvia::detail::WebSocketDeflateNegotiation, bool>);
static_assert(!HasLooseWebSocketDeflateFields<ruvia::detail::WebSocketDeflateNegotiation>);
static_assert(!HasFallibleWebSocketDeflateState<ruvia::detail::WebSocketDeflate>);
static_assert(!std::is_nothrow_default_constructible_v<ruvia::detail::WebSocketDeflate>);
static_assert(!std::default_initializable<ruvia::detail::WebSocketServerNegotiation>);
static_assert(!std::copy_constructible<ruvia::detail::WebSocketServerNegotiation>);
static_assert(std::move_constructible<ruvia::detail::WebSocketServerNegotiation>);
static_assert(!ExposesRvalueWebSocketServerSubprotocol<ruvia::detail::WebSocketServerNegotiation>);
static_assert(!std::constructible_from<ruvia::detail::WebSocketServerNegotiation, std::string_view, ruvia::detail::WebSocketDeflateNegotiation>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketServerNegotiation&>().subprotocol()), std::string_view>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketServerNegotiation&>().deflate()), ruvia::detail::WebSocketDeflateNegotiation>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketServerNegotiation&>().extensions()), std::string_view>);
static_assert(!std::default_initializable<ruvia::detail::HttpWebSocketServerHandshake>);
static_assert(!std::copy_constructible<ruvia::detail::HttpWebSocketServerHandshake>);
static_assert(std::move_constructible<ruvia::detail::HttpWebSocketServerHandshake>);
static_assert(HasWebSocketNegotiationAccessor<ruvia::detail::HttpWebSocketServerHandshake>);
static_assert(!HasLooseWebSocketNegotiationFields<ruvia::detail::HttpWebSocketServerHandshake>);
static_assert(!std::default_initializable<ruvia::detail::Http2WebSocketHandshakeSubmitResult>);
static_assert(!std::copy_constructible<ruvia::detail::Http2WebSocketHandshakeSubmitResult>);
static_assert(std::move_constructible<ruvia::detail::Http2WebSocketHandshakeSubmitResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2WebSocketHandshakeSubmitResult&>().submitted()), const ruvia::detail::WebSocketServerNegotiation*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2WebSocketHandshakeSubmitResult&>().failure()), const ruvia::detail::Http2WebSocketHandshakeSubmitFailure*>);
static_assert(!HasWebSocketNegotiationAccessor<ruvia::detail::Http2WebSocketHandshakeSubmitResult>);
static_assert(HasWebSocketHandshakeErrorAccessor<ruvia::detail::Http2WebSocketHandshakeSubmitFailure>);
static_assert(!HasWebSocketNegotiationAccessor<ruvia::detail::Http2WebSocketHandshakeSubmitFailure>);
static_assert(!std::constructible_from<ruvia::detail::Http2WebSocketHandshakeSubmitFailure, ruvia::detail::Http2WebSocketHandshakeSubmitError>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::Http2Connection&>().submitWebSocketHandshake(std::uint32_t{}, std::declval<ruvia::detail::WebSocketServerNegotiation>())), ruvia::detail::Http2WebSocketHandshakeSubmitResult>);
static_assert(!AcceptsLooseWebSocketHandshakeSubmit<ruvia::detail::Http2Connection>);
static_assert(!std::constructible_from<ruvia::detail::WsConnection, std::pmr::string&, std::size_t, bool>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::WsConnection&>().poll()), std::optional<ruvia::detail::WsEvent>>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::WsConnection&>().submitFrame(ruvia::WebSocketOpcode::kText, std::string_view{})), ruvia::detail::WsFrameSubmitStatus>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::WsConnection&>().submitClose(std::uint16_t{}, std::string_view{})), ruvia::detail::WsCloseSubmitStatus>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::WsConnection&>().consumeOutput(std::size_t{})), ruvia::detail::WsOutputConsumeStatus>);
static_assert(!std::convertible_to<ruvia::detail::WsOutputConsumeStatus, bool>);
static_assert(!HasWsSubmitMessageAlias<ruvia::detail::WsConnection>);
static_assert(!HasWsSubmitPingAlias<ruvia::detail::WsConnection>);
static_assert(!HasWsSubmitPongAlias<ruvia::detail::WsConnection>);
static_assert(!HasWsApplicationFrameStateSideChannel<ruvia::detail::WsConnection>);
static_assert(!HasWsEndsTransportAlias<ruvia::detail::WsOutputPlan>);
static_assert(!HasWsTransportEndPendingSideChannel<ruvia::detail::WsConnection>);
static_assert(!HasWsClosedStateSideChannel<ruvia::detail::WsConnection>);
static_assert(!HasWsClosePhaseSideChannel<ruvia::detail::WsConnection>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WsConnection&>().livenessMode()), ruvia::detail::WsLivenessMode>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::WsConnection&>().abort()), ruvia::detail::WsAbortDisposition>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WsOutputPlan&>().disposition()), ruvia::detail::WsTransportDisposition>);
static_assert(std::same_as<decltype(ruvia::detail::encodeWebSocketClosePayload(std::uint16_t{}, std::string_view{})), ruvia::detail::WebSocketClosePayloadEncodeResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketClosePayloadEncodeResult&>().encoded()), const ruvia::detail::WebSocketEncodedClosePayload*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketClosePayloadEncodeResult&>().failure()), const ruvia::detail::WebSocketClosePayloadEncodeFailure*>);
static_assert(!ExposesRvalueEncodedContent<ruvia::detail::WebSocketClosePayloadEncodeResult>);
static_assert(!ExposesRvalueEncodeFailure<ruvia::detail::WebSocketClosePayloadEncodeResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketEncodedClosePayload&>().bytes()), std::string_view>);
static_assert(!std::default_initializable<ruvia::detail::WebSocketClosePayloadEncodeResult>);
static_assert(!std::default_initializable<ruvia::detail::WsEvent>);
static_assert(!ExposesAnyRvalueSansIoEventBorrow<ruvia::detail::WsEvent>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WsEvent&>().message()), const ruvia::detail::WsMessageEvent*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WsEvent&>().close()), const ruvia::detail::WsCloseEvent*>);
static_assert(!HasWsCloseCode<ruvia::detail::WsMessageEvent>);
static_assert(HasWsCloseCode<ruvia::detail::WsCloseEvent>);
static_assert(HasWsCloseCode<ruvia::detail::WsProtocolErrorEvent>);
static_assert(HasWsReason<ruvia::detail::WsCloseEvent>);
static_assert(!HasWsReason<ruvia::detail::WsProtocolErrorEvent>);
static_assert(!std::default_initializable<ruvia::detail::WebSocketFrameReadResult>);
static_assert(!std::default_initializable<ruvia::detail::WebSocketFrameStart>);
static_assert(!std::default_initializable<ruvia::detail::WebSocketFrameView>);
static_assert(!HasLooseWebSocketFrameFields<ruvia::detail::WebSocketFrameStart>);
static_assert(!HasLooseWebSocketFrameFields<ruvia::detail::WebSocketFrameView>);
static_assert(!AcceptsMutableWebSocketFrameStartDecode<ruvia::detail::WebSocketFrameStart>);
static_assert(std::same_as<decltype(ruvia::detail::decodeWebSocketFrameStart(static_cast<unsigned char>(0x81), static_cast<unsigned char>(0x80), false)), std::optional<ruvia::detail::WebSocketFrameStart>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketFrameView&>().kind()), ruvia::detail::WebSocketFrameKind>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketFrameView&>().payload()), std::string_view>);
static_assert(std::same_as<decltype(ruvia::detail::WebSocketFrameView::continuation(std::string_view{}, false)), ruvia::detail::WebSocketFrameView>);
static_assert(std::same_as<decltype(ruvia::detail::WebSocketFrameView::close(std::string_view{})), std::optional<ruvia::detail::WebSocketFrameView>>);
static_assert(!AcceptsAnyTemporaryWebSocketFramePayload<std::string>);
static_assert(!AcceptsAnyTemporaryWebSocketFramePayload<std::pmr::string>);
static_assert(AcceptsWebSocketMessagePayload<std::string&>);
static_assert(AcceptsWebSocketMessagePayload<std::pmr::string&>);
static_assert(AcceptsWebSocketMessagePayload<std::string_view>);
static_assert(!AcceptsWebSocketMessagePayload<std::string>);
static_assert(!AcceptsWebSocketMessagePayload<const std::string>);
static_assert(!AcceptsWebSocketMessagePayload<std::pmr::string>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketFrameReadResult&>().needInput()), const ruvia::detail::WebSocketFrameNeedInput*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketFrameReadResult&>().frame()), const ruvia::detail::WebSocketFrameView*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketFrameReadResult&>().failure()), const ruvia::detail::WebSocketFrameReadFailure*>);
static_assert(!HasWsFrameReadStatusField<ruvia::detail::WebSocketFrameReadResult>);
static_assert(!HasWsRequiredBytesField<ruvia::detail::WebSocketFrameReadResult>);
static_assert(!HasWsCleanEofAllowedField<ruvia::detail::WebSocketFrameReadResult>);
static_assert(!HasWsProtocolFailure<ruvia::detail::WebSocketFrameReadResult>);
static_assert(!ExposesAnyRvalueWebSocketFrameReadAccessor<ruvia::detail::WebSocketFrameReadResult>);
static_assert(HasWsProtocolFailure<ruvia::detail::WebSocketFrameReadFailure>);
static_assert(!std::default_initializable<ruvia::detail::WebSocketInboundResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketInboundResult&>().continueReading()), const ruvia::detail::WebSocketInboundContinue*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketInboundResult&>().controlFrame()), const ruvia::detail::WebSocketInboundControlFrame*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketInboundResult&>().message()), const ruvia::detail::WebSocketInboundMessage*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketInboundResult&>().failure()), const ruvia::detail::WebSocketInboundFailure*>);
static_assert(!HasWsInboundActionAccessor<ruvia::detail::WebSocketInboundResult>);
static_assert(!HasWsProtocolFailure<ruvia::detail::WebSocketInboundResult>);
static_assert(!ExposesAnyRvalueWebSocketInboundAccessor<ruvia::detail::WebSocketInboundResult>);
static_assert(!ExposesRvalueWebSocketInboundMessageMember<ruvia::detail::WebSocketInboundMessage>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketInboundFragmented&>().opcode()), ruvia::WebSocketOpcode>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::WebSocketInboundFragmented&>().encoding()), ruvia::detail::WebSocketInboundContentEncoding>);
static_assert(HasWsProtocolFailure<ruvia::detail::WebSocketInboundFailure>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::Http1ChunkedBodyDecoder&>().decode(std::string_view{})), ruvia::detail::Http1ChunkDecodeResult>);
static_assert(std::default_initializable<ruvia::ProtocolByteLimit>);
static_assert(!std::constructible_from<ruvia::ProtocolByteLimit, std::size_t>);
static_assert(std::same_as<decltype(ruvia::ProtocolByteLimit::limited(std::size_t{1})), ruvia::ProtocolByteLimit>);
static_assert(std::same_as<decltype(std::declval<const ruvia::ProtocolByteLimit&>().maximum()), std::optional<std::size_t>>);
static_assert(!std::default_initializable<ruvia::detail::Http1ChunkDecodeResult>);
static_assert(!HasAnyRvalueHttp1ChunkDecodeAccessor<ruvia::detail::Http1ChunkDecodeResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1ChunkDecodeResult&>().bodyChunk()), const ruvia::detail::Http1ChunkDecodeBodyChunk*>);
static_assert(HasConsumedBytes<ruvia::detail::Http1ChunkDecodeNeedMore>);
static_assert(HasConsumedBytes<ruvia::detail::Http1ChunkDecodeBodyChunk>);
static_assert(HasConsumedBytes<ruvia::detail::Http1ChunkDecodeComplete>);
static_assert(HasConsumedBytes<ruvia::detail::Http1ChunkDecodeFailure>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1ChunkDecodeResult&>().failure()), const ruvia::detail::Http1ChunkDecodeFailure*>);
static_assert(HasProtocolError<ruvia::detail::Http1ChunkDecodeFailure>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1ChunkDecodeFailure&>().protocolError()), ruvia::HttpProtocolError>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::TransferCodingDecoder&>().decode(std::string_view{}, std::span<char>{})), ruvia::detail::TransferCodingDecodeResult>);
static_assert(!std::default_initializable<ruvia::detail::TransferCodingDecodeResult>);
static_assert(!HasAnyRvalueTransferCodingDecodeAccessor<ruvia::detail::TransferCodingDecodeResult>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::TransferCodingDecoder&>().finishInput()), ruvia::detail::TransferCodingDecodeResult>);
static_assert(HasConsumedBytes<ruvia::detail::TransferCodingDecodeNeedInput>);
static_assert(HasConsumedBytes<ruvia::detail::TransferCodingDecodeOutput>);
static_assert(HasConsumedBytes<ruvia::detail::TransferCodingDecodeComplete>);
static_assert(HasConsumedBytes<ruvia::detail::TransferCodingDecodeProtocolFailure>);
static_assert(HasConsumedBytes<ruvia::detail::TransferCodingDecoderFailure>);
static_assert(HasTransferOutputBytes<ruvia::detail::TransferCodingDecodeOutput>);
static_assert(!HasTransferOutputBytes<ruvia::detail::TransferCodingDecodeProtocolFailure>);
static_assert(!HasTransferDecodeError<ruvia::detail::TransferCodingDecodeProtocolFailure>);
static_assert(HasProtocolError<ruvia::detail::TransferCodingDecodeProtocolFailure>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::TransferCodingDecodeProtocolFailure&>().protocolError()), ruvia::HttpProtocolError>);
static_assert(std::same_as<decltype(ruvia::detail::scanHttpChunkedBody(std::string_view{})), ruvia::detail::HttpChunkScanResult>);
static_assert(!std::default_initializable<ruvia::detail::HttpChunkScanResult>);
static_assert(!HasAnyRvalueHttpChunkScanAccessor<ruvia::detail::HttpChunkScanResult>);
static_assert(!HasConsumedBytes<ruvia::detail::HttpChunkScanNeedMore>);
static_assert(HasConsumedBytes<ruvia::detail::HttpChunkScanComplete>);
static_assert(!HasConsumedBytes<ruvia::detail::HttpChunkScanFailure>);
static_assert(!HasChunkScanError<ruvia::detail::HttpChunkScanComplete>);
static_assert(HasChunkScanError<ruvia::detail::HttpChunkScanFailure>);
static_assert(std::same_as<decltype(std::declval<ruvia::MultipartParser&>().poll()), ruvia::MultipartPollResult>);
static_assert(!std::is_copy_constructible_v<ruvia::MultipartParser>);
static_assert(!std::is_copy_assignable_v<ruvia::MultipartParser>);
static_assert(!std::is_move_constructible_v<ruvia::MultipartParser>);
static_assert(!std::is_move_assignable_v<ruvia::MultipartParser>);
static_assert(!std::default_initializable<ruvia::MultipartPollResult>);
static_assert(!HasMultipartStatus<ruvia::MultipartPollResult>);
static_assert(!HasAnyRvalueMultipartPollAccessor<ruvia::MultipartPollResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::MultipartPollResult&>().part()), const ruvia::MultipartStreamPart*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::MultipartPollResult&>().failure()), const ruvia::MultipartPollFailure*>);
static_assert(!HasMultipartParseError<ruvia::MultipartPollNeedInput>);
static_assert(!HasMultipartParseError<ruvia::MultipartStreamPart>);
static_assert(!HasMultipartParseError<ruvia::MultipartPollDone>);
static_assert(!HasMultipartParseError<ruvia::MultipartPollFailure>);
static_assert(HasMultipartProtocolError<ruvia::MultipartPollFailure>);
static_assert(std::same_as<decltype(ruvia::parseMultipartBody(std::string_view{}, ruvia::MultipartBoundary("x"))), ruvia::MultipartBodyParseResult>);
static_assert(!std::default_initializable<ruvia::MultipartBodyParseResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::MultipartBodyParseResult&>().body()), const ruvia::MultipartBody*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::MultipartBodyParseResult&>().failure()), const ruvia::MultipartBodyParseFailure*>);
static_assert(!HasMultipartParseError<ruvia::MultipartBodyParseFailure>);
static_assert(HasMultipartProtocolError<ruvia::MultipartBodyParseFailure>);
static_assert(!std::default_initializable<ruvia::detail::HttpMultipartDelimiterResult>);
static_assert(!HasMultipartStatus<ruvia::detail::HttpMultipartDelimiterResult>);
static_assert(!HasAnyRvalueMultipartDelimiterAccessor<ruvia::detail::HttpMultipartDelimiterResult>);
static_assert(!HasMultipartOffset<ruvia::detail::HttpMultipartDelimiterNoMatch>);
static_assert(HasMultipartOffset<ruvia::detail::HttpMultipartDelimiterNeedInput>);
static_assert(!HasMultipartLineBytes<ruvia::detail::HttpMultipartDelimiterNeedInput>);
static_assert(HasMultipartLineBytes<ruvia::detail::HttpMultipartPartDelimiter>);
static_assert(HasMultipartLineBytes<ruvia::detail::HttpMultipartCloseDelimiter>);
static_assert(!std::default_initializable<ruvia::detail::HttpMultipartBoundaryParseResult>);
static_assert(!HasMultipartStatus<ruvia::detail::HttpMultipartBoundaryParseResult>);
static_assert(!HasAnyRvalueMultipartBoundaryAccessor<ruvia::detail::HttpMultipartBoundaryParseResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpMultipartBoundaryParseResult&>().notApplicable()), const ruvia::detail::HttpMultipartBoundaryNotApplicable*>);
static_assert(!ExposesAnyRvalueMultipartOwnedView<ruvia::MultipartBoundary>);
static_assert(!ExposesAnyRvalueMultipartOwnedView<ruvia::MultipartPart>);
static_assert(!HasMultipartParseError<ruvia::detail::HttpMultipartBoundaryParseFailure>);
static_assert(HasMultipartProtocolError<ruvia::detail::HttpMultipartBoundaryParseFailure>);
static_assert(!std::default_initializable<ruvia::detail::HttpMultipartPartHeaderParseResult>);
static_assert(!HasAnyRvalueMultipartPartHeaderAccessor<ruvia::detail::HttpMultipartPartHeaderParseResult>);
static_assert(HasMultipartParseError<ruvia::detail::HttpMultipartPartHeaderParseFailure>);
static_assert(std::same_as<decltype(ruvia::lookupUniqueHttpClientResponseHeader(std::declval<const ruvia::HttpClientResponseHead&>(), std::string_view{})), ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!std::default_initializable<ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(std::move_constructible<ruvia::HttpClientResponseHead>);
static_assert(!std::assignable_from<ruvia::HttpClientResponseHead&, ruvia::HttpClientResponseHead&&>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::HttpClientResponseHeader>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::HttpClientResponseHead>);
static_assert(!HasHttpClientResponseBody<ruvia::HttpClientResponseHead>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::Http1ClientChunkedResponse>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!AcceptsTemporaryHttpClientResponseHeaderLookup<ruvia::HttpClientResponseHead>);
static_assert(std::move_constructible<ruvia::Http1ParsedClientResponseHead>);
static_assert(!std::assignable_from<ruvia::Http1ParsedClientResponseHead&, ruvia::Http1ParsedClientResponseHead&&>);
static_assert(std::move_constructible<ruvia::Http1ClientResponseParseResult>);
static_assert(!std::assignable_from<ruvia::Http1ClientResponseParseResult&, ruvia::Http1ClientResponseParseResult&&>);
static_assert(!HasAnyRvalueHttp1RequestParseAccessor<ruvia::Http1RequestParseResult>);
static_assert(!HasResultKindDiscriminator<ruvia::Http1RequestParseResult>);
static_assert(!HasAnyRvalueHttp1ClientResponseParseAccessor<ruvia::Http1ClientResponseParseResult>);
static_assert(!HasResultKindDiscriminator<ruvia::Http1ClientResponseParseResult>);
static_assert(!HasAnyRvalueHttp1ClientRequestPrepareAccessor<ruvia::Http1ClientRequestPrepareResult>);
static_assert(!HasResultKindDiscriminator<ruvia::Http1ClientRequestPrepareResult>);
static_assert(!HasAnyRvalueHttp1InterimResponsePrepareAccessor<ruvia::Http1InterimResponsePrepareResult>);
static_assert(!HasResultKindDiscriminator<ruvia::Http1InterimResponsePrepareResult>);
static_assert(!HasAnyRvalueHttpClientRequestContentAccessor<ruvia::HttpClientRequestContent>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestText<std::string>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestText<const std::string>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestText<std::pmr::string>);
static_assert(AcceptsLvalueHttpClientRequestText<std::string>);
static_assert(AcceptsHttp1ConnectHeaders<std::vector<ruvia::HttpHeaderView>&>);
static_assert(AcceptsHttp1ConnectHeaders<std::array<ruvia::HttpHeaderView, 1>&>);
static_assert(AcceptsHttp1ConnectHeaders<std::span<const ruvia::HttpHeaderView>>);
static_assert(!AcceptsHttp1ConnectHeaders<std::vector<ruvia::HttpHeaderView>>);
static_assert(!AcceptsHttp1ConnectHeaders<const std::vector<ruvia::HttpHeaderView>>);
static_assert(!AcceptsHttp1ConnectHeaders<std::array<ruvia::HttpHeaderView, 1>>);
static_assert(!AcceptsHttp1ConnectHeaders<const std::array<ruvia::HttpHeaderView, 1>>);
constexpr ruvia::HttpClientRequest kLiteralHttpClientRequest{.method = "POST", .target = "/items"};
static_assert(kLiteralHttpClientRequest.method.view() == "POST");
static_assert(kLiteralHttpClientRequest.target.view() == "/items");
static_assert(!HasAnyRvalueHttp1ClientRequestContentPlanAccessor<ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasAnyRvalueHttp1ClientRequestWirePolicyAccessor<ruvia::Http1ClientRequestWirePolicy>);
static_assert(!HasAnyRvalueHttp1ClientResponsePlanAccessor<ruvia::Http1ClientResponsePlan>);
static_assert(!HasAnyRvalueHttpClientHeaderLookupAccessor<ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!HasHttpClientRedirectStatus<ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(std::same_as<decltype(ruvia::planHttpClientRedirectRequest(std::declval<const ruvia::HttpClientRequest&>(), ruvia::http_status::kTemporaryRedirect, std::declval<std::pmr::memory_resource*>())), ruvia::HttpClientRedirectRequestPlan>);
static_assert(!std::copy_constructible<ruvia::HttpClientRedirectRequestPlan>);
static_assert(std::move_constructible<ruvia::HttpClientRedirectRequestPlan>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::HttpClientRedirectRequestPlan>);
static_assert(!HasHttpClientHeaderValue<ruvia::HttpClientResponseHeaderAbsent>);
static_assert(HasHttpClientHeaderValue<ruvia::HttpClientResponseHeaderFound>);
static_assert(!HasHttpClientHeaderValue<ruvia::HttpClientResponseHeaderRepeated>);
static_assert(std::same_as<decltype(ruvia::resolveHttpClientSameOriginRedirectTarget(std::declval<const ruvia::HttpOrigin&>(), std::string_view{}, std::string_view{}, std::declval<std::pmr::memory_resource*>())), ruvia::HttpClientRedirectTargetResult>);
static_assert(!std::default_initializable<ruvia::HttpClientRedirectTargetResult>);
static_assert(!std::copy_constructible<ruvia::HttpClientRedirectTargetResult>);
static_assert(std::move_constructible<ruvia::HttpClientRedirectTargetResult>);
static_assert(!std::assignable_from<ruvia::HttpClientRedirectTargetResult&, ruvia::HttpClientRedirectTargetResult&&>);
static_assert(!HasAnyRvalueHttpClientRedirectTargetAccessor<ruvia::HttpClientRedirectTargetResult>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::HttpClientRedirectTarget>);
static_assert(std::move_constructible<ruvia::HttpClientRedirectTarget>);
static_assert(!std::assignable_from<ruvia::HttpClientRedirectTarget&, ruvia::HttpClientRedirectTarget&&>);
static_assert(!HasHttpClientRedirectStatus<ruvia::HttpClientRedirectTargetResult>);
static_assert(!HasHttpClientRedirectError<ruvia::HttpClientRedirectTarget>);
static_assert(HasHttpClientRedirectError<ruvia::HttpClientRedirectTargetFailure>);
static_assert(std::same_as<decltype(ruvia::detail::responseBody(std::declval<const ruvia::HttpResponse&>())), const ruvia::detail::HttpResponseBody&>);
static_assert(!std::copy_constructible<ruvia::detail::HttpResponseBody>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::detail::HttpResponseBody>);
static_assert(!std::is_move_assignable_v<ruvia::detail::HttpResponseBody>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::HttpResponse>);
static_assert(!std::default_initializable<ruvia::detail::HttpBorrowedResponseBytes>);
static_assert(!std::default_initializable<ruvia::detail::HttpStaticResponseBytes>);
static_assert(!std::default_initializable<ruvia::detail::HttpOwnedResponseBytes>);
static_assert(!std::default_initializable<ruvia::detail::HttpOwnedResponseFile>);
static_assert(!std::default_initializable<ruvia::detail::HttpBorrowedResponseFile>);
static_assert(!std::default_initializable<ruvia::detail::ResponseFileBody>);
static_assert(!std::default_initializable<ruvia::detail::HttpContentCodingFieldResult>);
static_assert(ruvia::detail::HttpUnsupportedContentCoding::status() == ruvia::http_status::kUnsupportedMediaType);
static_assert(ruvia::detail::HttpInvalidContentCodingField::status() == ruvia::http_status::kBadRequest);
static_assert(ruvia::detail::httpSupportedRequestContentCodings() == std::string_view("gzip, br, zstd"));
static_assert(!ExposesRvalueContentCoding<ruvia::detail::HttpContentCodingFieldResult>);
static_assert(!ExposesRvalueUnsupportedContentCoding<ruvia::detail::HttpContentCodingFieldResult>);
static_assert(!ExposesRvalueInvalidContentCoding<ruvia::detail::HttpContentCodingFieldResult>);
static_assert(std::same_as<decltype(ruvia::detail::httpContentCodingFromFieldValue(std::string_view{})), ruvia::detail::HttpContentCodingFieldResult>);
static_assert(std::same_as<decltype(ruvia::detail::httpClientResponseContentCoding(std::declval<const ruvia::HttpClientResponseHead&>())), ruvia::detail::HttpContentCodingFieldResult>);
static_assert(!HasContentLengthPresent<ruvia::detail::HttpContentLengthState>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpContentLengthState&>().value()), std::optional<std::size_t>>);
static_assert(!HasStaleTransferEncodingAccessors<ruvia::detail::HttpTransferEncodingState>);
static_assert(!std::default_initializable<ruvia::detail::HttpTransferEncodingValue>);
static_assert(!std::default_initializable<ruvia::detail::HttpNonChunkedTransferEncoding>);
static_assert(!std::default_initializable<ruvia::detail::HttpFinalChunkedTransferEncoding>);
static_assert(HasHttp1RequestPlanTransferCodings<ruvia::detail::HttpNonChunkedTransferEncoding>);
static_assert(HasHttp1RequestPlanTransferCodings<ruvia::detail::HttpFinalChunkedTransferEncoding>);
static_assert(!ExposesRvalueFinalChunked<ruvia::detail::HttpTransferEncodingValue>);
static_assert(!ExposesRvalueNonChunked<ruvia::detail::HttpTransferEncodingValue>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpTransferEncodingState&>().value()), std::optional<ruvia::detail::HttpTransferEncodingValue>>);
static_assert(!std::default_initializable<ruvia::detail::HttpContentDecodeResult>);
static_assert(!std::default_initializable<ruvia::detail::HttpRequestContentDecodeResult>);
static_assert(!std::copy_constructible<ruvia::detail::HttpRequestContentDecodeResult>);
static_assert(std::move_constructible<ruvia::detail::HttpRequestContentDecodeResult>);
static_assert(!std::is_move_assignable_v<ruvia::detail::HttpRequestContentDecodeResult>);
static_assert(!ExposesRvalueDecodedContent<ruvia::detail::HttpRequestContentDecodeResult>);
static_assert(!HasAnyRvalueRequestContentDecodeAccessor<ruvia::detail::HttpRequestContentDecodeResult>);
static_assert(!HasRawContentDecodeError<ruvia::detail::HttpRequestContentDecodeProtocolFailure>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpRequestContentDecodeProtocolFailure&>().protocolError()), ruvia::HttpProtocolError>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpRequestContentDecodeResult&>().protocolFailure()), const ruvia::detail::HttpRequestContentDecodeProtocolFailure*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpRequestContentDecodeResult&>().decoderFailure()), const ruvia::detail::HttpRequestContentDecoderFailure*>);
static_assert(std::same_as<decltype(ruvia::detail::decodeHttpRequestContent(ruvia::detail::HttpContentCoding::kGzip, std::string_view{}, std::size_t{}, std::declval<std::pmr::memory_resource*>())), ruvia::detail::HttpRequestContentDecodeResult>);
static_assert(!std::copy_constructible<ruvia::detail::HttpContentDecodeResult>);
static_assert(std::move_constructible<ruvia::detail::HttpContentDecodeResult>);
static_assert(!std::is_move_assignable_v<ruvia::detail::HttpContentDecodeResult>);
static_assert(!ExposesRvalueDecodedContent<ruvia::detail::HttpContentDecodeResult>);
static_assert(!ExposesRvalueDecodeFailure<ruvia::detail::HttpContentDecodeResult>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::HttpContentDecodeResult&>().decoded()), ruvia::detail::HttpDecodedContent*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpContentDecodeResult&>().failure()), const ruvia::detail::HttpContentDecodeFailure*>);
static_assert(std::same_as<decltype(ruvia::detail::decodeHttpContent(ruvia::detail::HttpContentCoding::kGzip, std::string_view{}, std::size_t{}, std::declval<std::pmr::memory_resource*>())), ruvia::detail::HttpContentDecodeResult>);
static_assert(std::same_as<decltype(ruvia::detail::decodeHttpClientResponseContentEncoding(std::declval<const ruvia::HttpClientResponseHead&>(), std::string_view{}, std::size_t{}, std::declval<std::pmr::memory_resource*>())), ruvia::detail::HttpContentDecodeResult>);
static_assert(!std::default_initializable<ruvia::detail::HttpContentEncodeResult>);
static_assert(!std::copy_constructible<ruvia::detail::HttpContentEncodeResult>);
static_assert(std::move_constructible<ruvia::detail::HttpContentEncodeResult>);
static_assert(!std::is_move_assignable_v<ruvia::detail::HttpContentEncodeResult>);
static_assert(!ExposesRvalueEncodedContent<ruvia::detail::HttpContentEncodeResult>);
static_assert(!ExposesRvalueEncodeFailure<ruvia::detail::HttpContentEncodeResult>);
static_assert(std::same_as<decltype(std::declval<ruvia::detail::HttpContentEncodeResult&>().encoded()), ruvia::detail::HttpEncodedContent*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::HttpContentEncodeResult&>().failure()), const ruvia::detail::HttpContentEncodeFailure*>);
static_assert(std::same_as<decltype(ruvia::detail::encodeHttpContent(ruvia::detail::HttpContentCoding::kGzip, std::string_view{}, std::size_t{}, std::declval<std::pmr::memory_resource*>())), ruvia::detail::HttpContentEncodeResult>);
static_assert(!AcceptsUrlDecodeOutputParameter<std::pmr::string>);
static_assert(std::same_as<decltype(ruvia::detail::decodeUrlComponent(std::string_view{}, ruvia::detail::UrlDecodeMode::kPercent, std::declval<std::pmr::memory_resource*>())), std::optional<std::pmr::string>>);

int main() {
    return 0;
}
