#include <array>
#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <ruvia/http/HttpClient.h>
#include <ruvia/http/HttpClientRedirect.h>
#include <ruvia/http/Http1ClientRequestWriter.h>
#include <ruvia/http/Http1ClientResponseParser.h>
#include <ruvia/http/Http1InterimResponseWriter.h>
#include <ruvia/http/Http1RequestParser.h>
#include <ruvia/http/HttpInterimResponse.h>
#include <ruvia/http/HttpProtocolError.h>
#include <ruvia/http/HttpProtocolVersion.h>
#include <ruvia/http/HttpResponse.h>
#include <ruvia/http/MultipartParser.h>
#include <ruvia/http/detail/AsciiCase.h>
#include <ruvia/http/detail/client/HttpOrigin.h>
#include <ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h>
#include <ruvia/http/detail/http1/Http1ServerRequestParser.h>
#include <ruvia/http/detail/http1/Http1ServerSemantics.h>
#include <ruvia/http/detail/http2/Http2Connection.h>
#include <ruvia/http/detail/http2/Http2Event.h>
#include <ruvia/http/detail/http2/Http2PeerSettings.h>
#include <ruvia/http/detail/MultipartParsing.h>
#include <ruvia/http/detail/parser/HttpChunkParser.h>
#include <ruvia/http/detail/server/HttpResponseWritePlan.h>
#include <ruvia/http/detail/websocket/WsConnection.h>
#include <ruvia/http/detail/websocket/WsEvent.h>

template <typename T>
concept HasHttp2EventError = requires(const T& event) {
    { event.error() } -> std::same_as<ruvia::detail::Http2ErrorCode>;
};

template <typename T>
concept HasFeedStatusField = requires(const T& result) {
    result.status;
};

template <typename T>
concept HasFeedConsumedField = requires(const T& result) {
    result.consumed;
};

template <typename T>
concept HasRequestHeadStatusAccessor = requires(const T& result) {
    result.status();
};

template <typename T>
concept HasRequestHeadAcceptedAccessor = requires(const T& result) {
    result.accepted();
};

template <typename T>
concept HasRequestHeadStreamIdAccessor = requires(const T& result) {
    { result.streamId() } -> std::same_as<std::uint32_t>;
};

template <typename T>
concept HasRequestHeadErrorAccessor = requires(const T& result) {
    { result.error() } ->
        std::same_as<ruvia::detail::Http2RequestHeadSubmitError>;
};

template <typename T>
concept HasResponseHeadStatusAccessor = requires(const T& result) {
    result.status();
};

template <typename T>
concept HasResponseHeadAcceptedAccessor = requires(const T& result) {
    result.accepted();
};

template <typename T>
concept HasResponseHeadPlanAccessor = requires(const T& result) {
    result.plan();
};

template <typename T>
concept HasResponseHeadErrorAccessor = requires(const T& result) {
    { result.error() } ->
        std::same_as<ruvia::detail::Http2ResponseHeadSubmitError>;
};

template <typename T>
concept HasPeerSettingStatusField = requires(const T& result) {
    result.status;
};

template <typename T>
concept HasPeerSettingChangedField = requires(const T& result) {
    result.initialWindowChanged;
};

template <typename T>
concept HasPeerSettingDeltaField = requires(const T& result) {
    result.initialWindowDelta;
};

template <typename T>
concept HasPeerSettingDeltaAccessor = requires(const T& result) {
    { result.delta() } -> std::same_as<std::int64_t>;
};

template <typename T>
concept HasPeerSettingErrorAccessor = requires(const T& result) {
    { result.error() } ->
        std::same_as<ruvia::detail::Http2PeerSettingError>;
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
concept HasWsFrameReadStatusField = requires(const T& result) {
    result.status;
};

template <typename T>
concept HasWsRequiredBytesField = requires(const T& result) {
    result.requiredBytes;
};

template <typename T>
concept HasWsCleanEofAllowedField = requires(const T& result) {
    result.cleanEofAllowed;
};

template <typename T>
concept HasWsInboundActionAccessor = requires(const T& result) {
    result.action();
};

template <typename T>
concept HasWsProtocolFailure = requires(const T& result) {
    { result.error() } ->
        std::same_as<ruvia::detail::WebSocketProtocolFailure>;
};

template <typename T>
concept HasConsumedBytes = requires(const T& result) {
    { result.consumedBytes() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasChunkScanError = requires(const T& result) {
    { result.error() } -> std::same_as<ruvia::detail::HttpChunkScanError>;
};

template <typename T>
concept HasMultipartStatus = requires(const T& result) {
    result.status();
};

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
    { result.error() } -> std::same_as<ruvia::MultipartParseError>;
};

template <typename T>
concept HasHttpClientHeaderValue = requires(const T& result) {
    { result.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttpClientRedirectError = requires(const T& result) {
    { result.error() } ->
        std::same_as<ruvia::HttpClientRedirectTargetError>;
};

template <typename T>
concept HasHttpClientRedirectStatus = requires(const T& result) {
    result.status();
};

static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http2Connection&>().nextEvent()),
    std::optional<ruvia::detail::Http2Event>>);
static_assert(!std::default_initializable<ruvia::detail::Http2Event>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http2Event&>().streamClosed()),
    const ruvia::detail::Http2StreamClosedEvent*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http2Event&>().goaway()),
    const ruvia::detail::Http2GoawayEvent*>);
static_assert(HasHttp2EventError<ruvia::detail::Http2StreamClosedEvent>);
static_assert(HasHttp2EventError<ruvia::detail::Http2GoawayEvent>);
static_assert(!HasHttp2EventError<ruvia::detail::Http2RequestUnprocessedEvent>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http2Connection&>().feed(
        std::string_view{})),
    ruvia::detail::Http2FeedResult>);
static_assert(std::is_enum_v<ruvia::detail::Http2FeedResult>);
static_assert(!HasFeedStatusField<ruvia::detail::Http2FeedResult>);
static_assert(!HasFeedConsumedField<ruvia::detail::Http2FeedResult>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http2PeerSettings&>().apply(
        ruvia::detail::Http2SettingId::kHeaderTableSize,
        std::uint32_t{})),
    ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2PeerSettingApplyResult&>().applied()),
    const ruvia::detail::Http2PeerSettingApplied*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2PeerSettingApplyResult&>().initialWindowChange()),
    const ruvia::detail::Http2PeerInitialWindowChange*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2PeerSettingApplyResult&>().failure()),
    const ruvia::detail::Http2PeerSettingFailure*>);
static_assert(!HasPeerSettingStatusField<
    ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingChangedField<
    ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingDeltaField<
    ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingDeltaAccessor<
    ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingErrorAccessor<
    ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingDeltaAccessor<
    ruvia::detail::Http2PeerSettingApplied>);
static_assert(!HasPeerSettingErrorAccessor<
    ruvia::detail::Http2PeerSettingApplied>);
static_assert(HasPeerSettingDeltaAccessor<
    ruvia::detail::Http2PeerInitialWindowChange>);
static_assert(!HasPeerSettingErrorAccessor<
    ruvia::detail::Http2PeerInitialWindowChange>);
static_assert(!HasPeerSettingDeltaAccessor<
    ruvia::detail::Http2PeerSettingFailure>);
static_assert(HasPeerSettingErrorAccessor<
    ruvia::detail::Http2PeerSettingFailure>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2PeerSettingApplied>);
static_assert(!std::constructible_from<
    ruvia::detail::Http2PeerInitialWindowChange,
    std::int64_t>);
static_assert(!std::constructible_from<
    ruvia::detail::Http2PeerSettingFailure,
    ruvia::detail::Http2PeerSettingError>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RequestHeadSubmitResult>);
static_assert(std::same_as<
    decltype(std::declval<
        const ruvia::detail::Http2RequestHeadSubmitResult&>().submitted()),
    const ruvia::detail::Http2SubmittedRequestHead*>);
static_assert(std::same_as<
    decltype(std::declval<
        const ruvia::detail::Http2RequestHeadSubmitResult&>().failure()),
    const ruvia::detail::Http2RequestHeadSubmitFailure*>);
static_assert(!HasRequestHeadStatusAccessor<
    ruvia::detail::Http2RequestHeadSubmitResult>);
static_assert(!HasRequestHeadAcceptedAccessor<
    ruvia::detail::Http2RequestHeadSubmitResult>);
static_assert(!HasRequestHeadStreamIdAccessor<
    ruvia::detail::Http2RequestHeadSubmitResult>);
static_assert(!std::constructible_from<
    ruvia::detail::Http2SubmittedRequestHead,
    std::uint32_t>);
static_assert(HasRequestHeadStreamIdAccessor<
    ruvia::detail::Http2SubmittedRequestHead>);
static_assert(!HasRequestHeadErrorAccessor<
    ruvia::detail::Http2SubmittedRequestHead>);
static_assert(HasRequestHeadErrorAccessor<
    ruvia::detail::Http2RequestHeadSubmitFailure>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http2Connection&>()
        .submitResponseHead(
            std::uint32_t{},
            std::declval<const ruvia::HttpResponse&>())),
    ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http2Connection&>()
        .submitStreamingResponseHead(
            std::uint32_t{},
            std::declval<ruvia::HttpResponse>(),
            ruvia::detail::ResponseStreamKind::kGeneric,
            ruvia::detail::ResponseTrailerIntent::kNone)),
    ruvia::detail::Http2StreamingResponseHeadSubmitResult>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2StreamingResponseHeadSubmitResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2BufferedResponseHeadSubmitResult&>().submitted()),
    const ruvia::detail::Http2SubmittedBufferedResponseHead*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2StreamingResponseHeadSubmitResult&>().submitted()),
    const ruvia::detail::Http2SubmittedStreamingResponseHead*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2BufferedResponseHeadSubmitResult&>().failure()),
    const ruvia::detail::Http2ResponseHeadSubmitFailure*>);
static_assert(!HasResponseHeadStatusAccessor<
    ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadAcceptedAccessor<
    ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadPlanAccessor<
    ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadErrorAccessor<
    ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2SubmittedBufferedResponseHead&>().plan()),
    const ruvia::detail::HttpBufferedResponseWritePlan&>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2SubmittedStreamingResponseHead&>().plan()),
    const ruvia::detail::ResponseStreamCommitPlan&>);
static_assert(HasResponseHeadPlanAccessor<
    ruvia::detail::Http2SubmittedBufferedResponseHead>);
static_assert(!HasResponseHeadErrorAccessor<
    ruvia::detail::Http2SubmittedBufferedResponseHead>);
static_assert(HasResponseHeadErrorAccessor<
    ruvia::detail::Http2ResponseHeadSubmitFailure>);
static_assert(!HasResponseHeadPlanAccessor<
    ruvia::detail::Http2ResponseHeadSubmitFailure>);
static_assert(!std::constructible_from<
    ruvia::detail::Http2SubmittedBufferedResponseHead,
    ruvia::detail::HttpBufferedResponseWritePlan>);
static_assert(!std::constructible_from<
    ruvia::detail::Http2SubmittedStreamingResponseHead,
    ruvia::detail::ResponseStreamCommitPlan>);
static_assert(!std::constructible_from<
    ruvia::detail::Http2ResponseHeadSubmitFailure,
    ruvia::detail::Http2ResponseHeadSubmitError>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::WsConnection&>().poll()),
    std::optional<ruvia::detail::WsEvent>>);
static_assert(!std::default_initializable<ruvia::detail::WsEvent>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::WsEvent&>().message()),
    const ruvia::detail::WsMessageEvent*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::WsEvent&>().close()),
    const ruvia::detail::WsCloseEvent*>);
static_assert(!HasWsCloseCode<ruvia::detail::WsMessageEvent>);
static_assert(HasWsCloseCode<ruvia::detail::WsCloseEvent>);
static_assert(HasWsCloseCode<ruvia::detail::WsProtocolErrorEvent>);
static_assert(HasWsReason<ruvia::detail::WsCloseEvent>);
static_assert(!HasWsReason<ruvia::detail::WsProtocolErrorEvent>);
static_assert(!std::default_initializable<
    ruvia::detail::WebSocketFrameReadResult>);
static_assert(std::same_as<
    decltype(std::declval<
        const ruvia::detail::WebSocketFrameReadResult&>().needInput()),
    const ruvia::detail::WebSocketFrameNeedInput*>);
static_assert(std::same_as<
    decltype(std::declval<
        const ruvia::detail::WebSocketFrameReadResult&>().frame()),
    const ruvia::detail::WebSocketFrameView*>);
static_assert(std::same_as<
    decltype(std::declval<
        const ruvia::detail::WebSocketFrameReadResult&>().failure()),
    const ruvia::detail::WebSocketFrameReadFailure*>);
static_assert(!HasWsFrameReadStatusField<
    ruvia::detail::WebSocketFrameReadResult>);
static_assert(!HasWsRequiredBytesField<
    ruvia::detail::WebSocketFrameReadResult>);
static_assert(!HasWsCleanEofAllowedField<
    ruvia::detail::WebSocketFrameReadResult>);
static_assert(!HasWsProtocolFailure<
    ruvia::detail::WebSocketFrameReadResult>);
static_assert(HasWsProtocolFailure<
    ruvia::detail::WebSocketFrameReadFailure>);
static_assert(!std::default_initializable<
    ruvia::detail::WebSocketInboundResult>);
static_assert(std::same_as<
    decltype(std::declval<
        const ruvia::detail::WebSocketInboundResult&>().continueReading()),
    const ruvia::detail::WebSocketInboundContinue*>);
static_assert(std::same_as<
    decltype(std::declval<
        const ruvia::detail::WebSocketInboundResult&>().controlFrame()),
    const ruvia::detail::WebSocketInboundControlFrame*>);
static_assert(std::same_as<
    decltype(std::declval<
        const ruvia::detail::WebSocketInboundResult&>().message()),
    const ruvia::detail::WebSocketInboundMessage*>);
static_assert(std::same_as<
    decltype(std::declval<
        const ruvia::detail::WebSocketInboundResult&>().failure()),
    const ruvia::detail::WebSocketInboundFailure*>);
static_assert(!HasWsInboundActionAccessor<
    ruvia::detail::WebSocketInboundResult>);
static_assert(!HasWsProtocolFailure<
    ruvia::detail::WebSocketInboundResult>);
static_assert(HasWsProtocolFailure<
    ruvia::detail::WebSocketInboundFailure>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http1ChunkedBodyDecoder&>().decode(
        std::string_view{})),
    ruvia::detail::Http1ChunkDecodeResult>);
static_assert(!std::default_initializable<ruvia::detail::Http1ChunkDecodeResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http1ChunkDecodeResult&>()
        .bodyChunk()),
    const ruvia::detail::Http1ChunkDecodeBodyChunk*>);
static_assert(HasConsumedBytes<ruvia::detail::Http1ChunkDecodeNeedMore>);
static_assert(HasConsumedBytes<ruvia::detail::Http1ChunkDecodeBodyChunk>);
static_assert(HasConsumedBytes<ruvia::detail::Http1ChunkDecodeComplete>);
static_assert(std::same_as<
    decltype(ruvia::detail::scanHttpChunkedBody(std::string_view{})),
    ruvia::detail::HttpChunkScanResult>);
static_assert(!std::default_initializable<ruvia::detail::HttpChunkScanResult>);
static_assert(!HasConsumedBytes<ruvia::detail::HttpChunkScanNeedMore>);
static_assert(HasConsumedBytes<ruvia::detail::HttpChunkScanComplete>);
static_assert(!HasConsumedBytes<ruvia::detail::HttpChunkScanFailure>);
static_assert(!HasChunkScanError<ruvia::detail::HttpChunkScanComplete>);
static_assert(HasChunkScanError<ruvia::detail::HttpChunkScanFailure>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::MultipartParser&>().poll()),
    ruvia::MultipartPollResult>);
static_assert(!std::default_initializable<ruvia::MultipartPollResult>);
static_assert(!HasMultipartStatus<ruvia::MultipartPollResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::MultipartPollResult&>().part()),
    const ruvia::MultipartStreamPart*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::MultipartPollResult&>().failure()),
    const ruvia::MultipartPollFailure*>);
static_assert(!HasMultipartParseError<ruvia::MultipartPollNeedInput>);
static_assert(!HasMultipartParseError<ruvia::MultipartStreamPart>);
static_assert(!HasMultipartParseError<ruvia::MultipartPollDone>);
static_assert(HasMultipartParseError<ruvia::MultipartPollFailure>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpMultipartDelimiterResult>);
static_assert(!HasMultipartStatus<
    ruvia::detail::HttpMultipartDelimiterResult>);
static_assert(!HasMultipartOffset<
    ruvia::detail::HttpMultipartDelimiterNoMatch>);
static_assert(HasMultipartOffset<
    ruvia::detail::HttpMultipartDelimiterNeedInput>);
static_assert(!HasMultipartLineBytes<
    ruvia::detail::HttpMultipartDelimiterNeedInput>);
static_assert(HasMultipartLineBytes<
    ruvia::detail::HttpMultipartPartDelimiter>);
static_assert(HasMultipartLineBytes<
    ruvia::detail::HttpMultipartCloseDelimiter>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpMultipartBoundaryParseResult>);
static_assert(!HasMultipartStatus<
    ruvia::detail::HttpMultipartBoundaryParseResult>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpMultipartPartHeaderParseResult>);
static_assert(std::same_as<
    decltype(ruvia::lookupUniqueHttpClientResponseHeader(
        std::declval<const ruvia::HttpClientResponse&>(),
        std::string_view{})),
    ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!std::default_initializable<
    ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!HasHttpClientRedirectStatus<
    ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!HasHttpClientHeaderValue<
    ruvia::HttpClientResponseHeaderAbsent>);
static_assert(HasHttpClientHeaderValue<
    ruvia::HttpClientResponseHeaderFound>);
static_assert(!HasHttpClientHeaderValue<
    ruvia::HttpClientResponseHeaderRepeated>);
static_assert(std::same_as<
    decltype(ruvia::resolveHttpClientSameOriginRedirectTarget(
        std::declval<const ruvia::HttpOrigin&>(),
        std::string_view{},
        std::string_view{},
        std::declval<std::pmr::memory_resource*>())),
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!std::default_initializable<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!std::copy_constructible<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!HasHttpClientRedirectStatus<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!HasHttpClientRedirectError<
    ruvia::HttpClientRedirectTarget>);
static_assert(HasHttpClientRedirectError<
    ruvia::HttpClientRedirectTargetFailure>);

int main() {
    const auto outboundOrigin = ruvia::HttpOrigin::https("example.test");
    ruvia::HttpClientRequest outboundRequest;
    outboundRequest.method = "POST";
    outboundRequest.target = "/submit";
    outboundRequest.content = ruvia::HttpClientRequestContent::bytes("payload");
    std::array<char, 512> outboundHeadBuffer;
    const auto outboundPrepared = ruvia::Http1ClientRequestWriter().prepare(
        outboundOrigin, outboundRequest, outboundHeadBuffer);
    const auto* outboundWire = outboundPrepared.prepared();
    if (outboundOrigin.scheme() != ruvia::HttpScheme::kHttps ||
        outboundOrigin.host() != "example.test" || outboundOrigin.port() != 443 ||
        outboundRequest.target != "/submit" || outboundWire == nullptr ||
        outboundWire->head().find("Content-Length: 7\r\n") ==
            std::string_view::npos ||
        outboundWire->contentPlan().bytes() != "payload" ||
        outboundWire->contentPlan().disposition() !=
            ruvia::Http1ClientRequestContentDisposition::kImmediate) {
        return 17;
    }
    std::array<char, 512> expectHeadBuffer;
    const auto expectPrepared = ruvia::Http1ClientRequestWriter().prepare(
        outboundOrigin,
        outboundRequest,
        expectHeadBuffer,
        ruvia::Http1ClientRequestWirePolicy::expectContinue());
    const auto* expectWire = expectPrepared.prepared();
    if (expectWire == nullptr ||
        expectWire->contentPlan().disposition() !=
            ruvia::Http1ClientRequestContentDisposition::kContinueGated ||
        expectWire->head().find("Expect: 100-continue\r\n") ==
            std::string_view::npos) {
        return 25;
    }
    ruvia::Http1ClientResponseParser expectParser(*expectWire);
    const auto continueResult = expectParser.parse(
        "HTTP/1.1 100 Continue\r\n\r\n");
    const auto* continueHead = continueResult.parsed();
    if (continueHead == nullptr ||
        continueHead->response().protocolVersion() !=
            ruvia::HttpProtocolVersion::kHttp11 ||
        continueHead->plan().requestContentSignal() !=
            ruvia::Http1ClientRequestContentSignal::kContinue) {
        return 25;
    }
    if (expectParser.completeRequestContent() !=
        ruvia::Http1ClientRequestContentCompletionStatus::kCompleted) {
        return 25;
    }
    const auto expectFinalResult = expectParser.parse(
        "HTTP/1.1 204 No Content\r\n\r\n");
    const auto* expectFinalHead = expectFinalResult.parsed();
    if (expectFinalHead == nullptr ||
        expectFinalHead->plan().requestContentSignal() !=
            ruvia::Http1ClientRequestContentSignal::kExchangeComplete) {
        return 25;
    }
    const auto zeroPortOrigin = ruvia::HttpOrigin::http("example.test", 0);
    const auto zeroPortAuthority = ruvia::detail::makeHttpOriginAuthority(
        zeroPortOrigin, std::pmr::get_default_resource());
    if (zeroPortAuthority != "example.test:0") {
        return 20;
    }
    bool invalidOriginRejected = false;
    try {
        (void)ruvia::HttpOrigin::http("");
    } catch (const std::invalid_argument&) {
        invalidOriginRejected = true;
    }
    if (!invalidOriginRejected) {
        return 21;
    }

    const auto redirectTarget =
        ruvia::resolveHttpClientSameOriginRedirectTarget(
            outboundOrigin,
            "/base/current",
            "../next?x=1",
            std::pmr::get_default_resource());
    if (redirectTarget.target() == nullptr ||
        redirectTarget.failure() != nullptr ||
        redirectTarget.target()->value() != "/next?x=1") {
        return 28;
    }
    const auto crossOrigin =
        ruvia::resolveHttpClientSameOriginRedirectTarget(
            outboundOrigin,
            "/base/current",
            "https://other.test/next",
            std::pmr::get_default_resource());
    if (crossOrigin.target() != nullptr || crossOrigin.failure() == nullptr ||
        crossOrigin.failure()->error() !=
            ruvia::HttpClientRedirectTargetError::kNotSameOrigin) {
        return 29;
    }

    const ruvia::HttpProtocolError error(400, "bad request");
    if (error.status() != 400) {
        return 2;
    }
    std::pmr::string wsInput(std::pmr::get_default_resource());
    std::size_t wsOffset = 0;
    std::size_t wsPendingCompactUntil = 0;
    const auto wsNeedInput = ruvia::detail::webSocketTryReadFrame(
        wsInput, wsOffset, wsPendingCompactUntil, 1024, false);
    if (wsNeedInput.needInput() == nullptr ||
        wsNeedInput.frame() != nullptr ||
        wsNeedInput.failure() != nullptr) {
        return 31;
    }
    const auto multipart = ruvia::parseMultipartBody(
        "--x--\r\n", ruvia::MultipartBoundary("x"));
    if (!multipart.empty()) {
        return 3;
    }
    ruvia::MultipartParser multipartParser(
        ruvia::MultipartBoundary("x"),
        std::pmr::get_default_resource());
    multipartParser.feed("--x--");
    if (multipartParser.poll().needInput() == nullptr) {
        return 18;
    }
    multipartParser.finishInput();
    if (multipartParser.poll().done() == nullptr) {
        return 19;
    }
    constexpr std::string_view chunkedRequest =
        "POST / HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "1\r\nx\r\n0\r\n\r\n";
    const auto parseResult = ruvia::Http1RequestParser().parse(chunkedRequest);
    const auto* parsed = parseResult.parsed();
    if (parsed == nullptr || !parsed->bodyPlan().isChunked() ||
        parsed->request().protocolVersion() !=
            ruvia::HttpProtocolVersion::kHttp11 ||
        parsed->wireBody() != "1\r\nx\r\n0\r\n\r\n" ||
        parsed->consumedBytes() != chunkedRequest.size()) {
        return 1;
    }

    ruvia::detail::Http1ServerRequestParser serverParser;
    const auto extensionMethod = serverParser.parseMessage(
        "PROPFIND /dav HTTP/1.1\r\nHost: example.test\r\n\r\n");
    if (!extensionMethod.messageReady() ||
        extensionMethod.request.method() != "PROPFIND" ||
        extensionMethod.request.knownMethod() != ruvia::HttpKnownMethod::kUnknown) {
        return 22;
    }
    const auto transferCoded = serverParser.parseMessage(
        "POST / HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n"
        "1\r\nx\r\n0\r\n\r\n");
    if (!transferCoded.messageReady() ||
        !transferCoded.bodyPlan.isChunked() ||
        transferCoded.bodyPlan.transferCodings().count != 1 ||
        transferCoded.bodyPlan.transferCodings().values[0] !=
            ruvia::detail::HttpTransferCoding::kGzip) {
        return 14;
    }

    ruvia::HttpClientRequest getRequest;
    getRequest.method = "GET";
    std::array<char, 256> getHeadBuffer;
    const auto getPrepared = ruvia::Http1ClientRequestWriter().prepare(
        outboundOrigin, getRequest, getHeadBuffer);
    const auto* getWire = getPrepared.prepared();
    constexpr std::string_view closeDelimitedHead =
        "HTTP/1.1 200 OK\r\n\r\n";
    if (getWire == nullptr) {
        return 15;
    }
    ruvia::Http1ClientResponseParser clientParser(*getWire);
    const auto clientResult = clientParser.parse(closeDelimitedHead);
    const auto* clientHead = clientResult.parsed();
    if (clientHead == nullptr ||
        !clientHead->plan().isCloseDelimited() ||
        clientHead->consumedBytes() != closeDelimitedHead.size() ||
        clientHead->plan().connectionDisposition() !=
            ruvia::Http1ClientConnectionDisposition::kClose) {
        return 15;
    }

    std::array<char, 256> tunnelHeadBuffer;
    const auto tunnelPrepared = ruvia::Http1ClientRequestWriter().prepareConnect(
        outboundOrigin, {}, tunnelHeadBuffer);
    const auto* tunnelWire = tunnelPrepared.prepared();
    if (tunnelWire == nullptr) {
        return 16;
    }
    ruvia::Http1ClientResponseParser tunnelParser(*tunnelWire);
    const auto tunnelResult = tunnelParser.parse(
        "HTTP/1.1 200 Connection Established\r\n"
        "Content-Length: invalid\r\n\r\ntunnel bytes");
    const auto* tunnelHead = tunnelResult.parsed();
    if (tunnelHead == nullptr || !tunnelHead->plan().isOpaque() ||
        !tunnelHead->plan().isConnectTunnel() ||
        tunnelHead->plan().connectionDisposition() !=
            ruvia::Http1ClientConnectionDisposition::kConnectTunnel) {
        return 16;
    }

    const ruvia::HttpHeaderView upgradeRequestHeaders[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::HttpClientRequest upgradeRequest;
    upgradeRequest.method = "GET";
    upgradeRequest.headers = upgradeRequestHeaders;
    std::array<char, 512> upgradeHeadBuffer;
    const auto upgradePrepared = ruvia::Http1ClientRequestWriter().prepare(
        outboundOrigin, upgradeRequest, upgradeHeadBuffer);
    const auto* upgradeWire = upgradePrepared.prepared();
    if (upgradeWire == nullptr) {
        return 24;
    }
    ruvia::Http1ClientResponseParser upgradeParser(*upgradeWire);
    const auto upgradeResult = upgradeParser.parse(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: WebSocket\r\n\r\nopaque bytes");
    const auto* upgradeHead = upgradeResult.parsed();
    if (upgradeHead == nullptr || !upgradeHead->plan().isOpaque() ||
        !upgradeHead->plan().isUpgrade() ||
        upgradeHead->plan().connectionDisposition() !=
            ruvia::Http1ClientConnectionDisposition::kUpgrade) {
        return 24;
    }

    const auto streamPlan = ruvia::detail::http1PlanResponseStream(
        serverParser.parseMessage("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n"),
        ruvia::detail::Http1ServerClosePolicy::kAllowReuse);
    if (streamPlan.framing() != ruvia::detail::ResponseStreamFraming::kHttp1Chunked ||
        streamPlan.requestConnectionPlan().disposition() !=
            ruvia::detail::Http1ConnectionDisposition::kReuse ||
        streamPlan.requestConnectionPlan().responseSignal() !=
            ruvia::detail::Http1ResponseConnectionSignal::kImplicitPersistence ||
        streamPlan.closePolicy() != ruvia::detail::Http1ServerClosePolicy::kAllowReuse) {
        return 4;
    }

    ruvia::HttpResponse streamResponse;
    streamResponse.header("Connection", "close");
    const auto preparedStream = ruvia::detail::prepareHttp1ResponseStreamHead(
        std::move(streamResponse),
        ruvia::detail::ResponseStreamKind::kGeneric,
        streamPlan,
        ruvia::detail::ResponseTrailerIntent::kNone);
    if (preparedStream.connectionPlan().disposition() !=
            ruvia::detail::Http1ConnectionDisposition::kClose ||
        preparedStream.response().header("Connection") != "close") {
        return 5;
    }

    const auto http10Plan = ruvia::detail::http1PlanResponseStream(
        serverParser.parseMessage(
            "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"),
        ruvia::detail::Http1ServerClosePolicy::kAllowReuse);
    ruvia::HttpResponse resetContentStream;
    resetContentStream.status(205);
    const auto preparedHttp10 = ruvia::detail::prepareHttp1ResponseStreamHead(
        std::move(resetContentStream),
        ruvia::detail::ResponseStreamKind::kGeneric,
        http10Plan,
        ruvia::detail::ResponseTrailerIntent::kNone);
    if (!preparedHttp10.commitPlan().bodyPlan().bodySuppressed() ||
        preparedHttp10.commitPlan().headDisposition() !=
            ruvia::detail::ResponseStreamHeadDisposition::kMessageEnded ||
        preparedHttp10.connectionPlan().disposition() !=
            ruvia::detail::Http1ConnectionDisposition::kReuse ||
        preparedHttp10.response().header("Connection") != "keep-alive") {
        return 13;
    }

    ruvia::HttpResponse response;
    response.setBodyCopy("body");
    const auto writePlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpKnownMethod::kHead, response);
    if (!writePlan.bodySuppressed() || writePlan.sendBody() ||
        writePlan.contentLength() != 4) {
        return 6;
    }

    response.status(205);
    const auto resetContentPlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpKnownMethod::kGet, response);
    if (resetContentPlan.statusAllowsBody() ||
        !resetContentPlan.bodySuppressed() ||
        resetContentPlan.sendBody() ||
        resetContentPlan.contentLength() != 0) {
        return 9;
    }

    const ruvia::HttpHeaderView earlyHintFields[] = {
        {"Link", "</style.css>; rel=preload"},
    };
    const ruvia::HttpInterimResponseHead earlyHints(103, earlyHintFields);
    if (earlyHints.status() != 103 || earlyHints.headers().size() != 1) {
        return 26;
    }
    std::array<char, 128> earlyHintsWireBuffer{};
    const auto earlyHintsWire = ruvia::Http1InterimResponseWriter().prepare(
        earlyHints, earlyHintsWireBuffer);
    if (earlyHintsWire.prepared() == nullptr ||
        earlyHintsWire.prepared()->head().find(
            "HTTP/1.1 103 Early Hints\r\n") != 0) {
        return 27;
    }

    ruvia::detail::Http2PeerSettings installedPeerSettings(
        ruvia::detail::Http2Role::kServer);
    const auto ordinarySetting = installedPeerSettings.apply(
        ruvia::detail::Http2SettingId::kHeaderTableSize, 8192);
    const auto windowSetting = installedPeerSettings.apply(
        ruvia::detail::Http2SettingId::kInitialWindowSize,
        static_cast<std::uint32_t>(
            ruvia::detail::kHttp2DefaultInitialWindowSize));
    const auto invalidSetting = installedPeerSettings.apply(
        ruvia::detail::Http2SettingId::kMaxFrameSize, 0);
    if (ordinarySetting.applied() == nullptr ||
        ordinarySetting.initialWindowChange() != nullptr ||
        ordinarySetting.failure() != nullptr ||
        windowSetting.applied() != nullptr ||
        windowSetting.initialWindowChange() == nullptr ||
        windowSetting.initialWindowChange()->delta() != 0 ||
        windowSetting.failure() != nullptr ||
        invalidSetting.applied() != nullptr ||
        invalidSetting.initialWindowChange() != nullptr ||
        invalidSetting.failure() == nullptr ||
        invalidSetting.failure()->error() !=
            ruvia::detail::Http2PeerSettingError::kInvalidMaxFrameSize) {
        return 32;
    }

    ruvia::detail::Http2Connection h2(
        std::pmr::get_default_resource(), ruvia::detail::Http2Role::kClient);
    if (h2.feed({}) !=
        ruvia::detail::Http2FeedResult::kConnectionNotStarted) {
        return 30;
    }
    h2.beginConnection();
    if (h2.connectionError().has_value()) {
        return 12;
    }
    const auto request = h2.submitRegularRequestHead(
        "PROPFIND",
        "https",
        "example.test",
        "/",
        {},
        ruvia::detail::Http2RequestContent::none());
    const auto* submittedRequest = request.submitted();
    if (submittedRequest == nullptr || request.failure() != nullptr) {
        return 7;
    }
    const auto streamId = submittedRequest->streamId();
    const auto* extensionStream = h2.stream(streamId);
    if (extensionStream == nullptr ||
        extensionStream->requestMethod() != "PROPFIND" ||
        extensionStream->requestKnownMethod() != ruvia::HttpKnownMethod::kUnknown) {
        return 23;
    }
    if (h2.submitData(
            streamId, "forbidden", ruvia::detail::Http2EndStream::kEndStream) !=
        ruvia::detail::Http2DataSubmitStatus::kInvalidState) {
        return 8;
    }

    const auto connect = h2.submitConnectRequestHead("example.test:443");
    const auto* submittedConnect = connect.submitted();
    if (submittedConnect == nullptr || connect.failure() != nullptr ||
        h2.submitData(
            submittedConnect->streamId(),
            "too early",
            ruvia::detail::Http2EndStream::kKeepOpen) !=
            ruvia::detail::Http2DataSubmitStatus::kInvalidState) {
        return 10;
    }

    const auto unavailable = h2.submitExtendedConnectRequestHead(
        "connect-udp",
        "https",
        "example.test",
        "/masque");
    return unavailable.submitted() == nullptr &&
            unavailable.failure() != nullptr &&
            unavailable.failure()->error() ==
                ruvia::detail::Http2RequestHeadSubmitError::kPeerCapabilityUnavailable
        ? 0
        : 11;
}
