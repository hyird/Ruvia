#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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
#include <ruvia/http/UrlEncoding.h>
#include <ruvia/http/detail/AsciiCase.h>
#include <ruvia/http/detail/HttpByteRange.h>
#include <ruvia/http/detail/HttpContentCoding.h>
#include <ruvia/http/detail/HttpContentLength.h>
#include <ruvia/http/detail/HttpTransferEncoding.h>
#include <ruvia/http/detail/HttpResponseBody.h>
#include <ruvia/http/detail/HttpResponseBodyAccess.h>
#include <ruvia/http/detail/HttpResponseContentSemantics.h>
#include <ruvia/http/detail/HttpResponseFileBody.h>
#include <ruvia/http/detail/client/HttpClientContentEncoding.h>
#include <ruvia/http/detail/client/HttpOrigin.h>
#include <ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h>
#include <ruvia/http/detail/body/HttpTransferCodingDecoder.h>
#include <ruvia/http/detail/http1/Http1ResponseHeadPlan.h>
#include <ruvia/http/detail/http1/Http1ServerRequestParser.h>
#include <ruvia/http/detail/http1/Http1ServerSemantics.h>
#include <ruvia/http/detail/http2/Http2Connection.h>
#include <ruvia/http/detail/http2/Http2ClosedStreams.h>
#include <ruvia/http/detail/http2/Http2Event.h>
#include <ruvia/http/detail/http2/Http2Hpack.h>
#include <ruvia/http/detail/http2/Http2LocalSendState.h>
#include <ruvia/http/detail/http2/Http2PeerSettings.h>
#include <ruvia/http/detail/http2/Http2RemoteContentState.h>
#include <ruvia/http/detail/http2/Http2RemoteReceiveState.h>
#include <ruvia/http/detail/http2/Http2ResponseHeadPlan.h>
#include <ruvia/http/detail/http2/Http2StreamHeaderBlocks.h>
#include <ruvia/http/detail/http2/Http2StreamState.h>
#include <ruvia/http/detail/http2/Http2StreamTable.h>
#include <ruvia/http/detail/http2/Http2TunnelState.h>
#include <ruvia/http/detail/MultipartParsing.h>
#include <ruvia/http/detail/SetCookiePlan.h>
#include <ruvia/http/detail/parser/HttpChunkParser.h>
#include <ruvia/http/detail/server/HttpFinalResponseControlPlan.h>
#include <ruvia/http/detail/server/HttpResponseWritePlan.h>
#include <ruvia/http/detail/websocket/HttpWebSocketServerHandshake.h>
#include <ruvia/http/detail/websocket/WebSocketServerNegotiation.h>
#include <ruvia/http/detail/websocket/WsConnection.h>
#include <ruvia/http/detail/websocket/WsEvent.h>

template <typename T>
concept HasLegacyResponseBodyCopy = requires(T& response) {
    response.setBodyCopy(std::string_view{});
};

template <typename T>
concept HasLegacyResponseBodyView = requires(T& response) {
    response.setBodyView(std::string_view{});
};

template <typename T>
concept HasHttp2EventError = requires(const T& event) {
    { event.error() } -> std::same_as<ruvia::detail::Http2ErrorCode>;
};

template <typename T>
concept ExposesAnyRvalueSansIoEventBorrow =
    requires(T&& event) { std::move(event).messageHead(); } ||
    requires(T&& event) { std::move(event).messageBodyChunk(); } ||
    requires(T&& event) { std::move(event).messageEnd(); } ||
    requires(T&& event) { std::move(event).tunnelData(); } ||
    requires(T&& event) { std::move(event).tunnelEnd(); } ||
    requires(T&& event) { std::move(event).streamClosed(); } ||
    requires(T&& event) { std::move(event).requestUnprocessed(); } ||
    requires(T&& event) { std::move(event).goaway(); } ||
    requires(T&& event) { std::move(event).peerGoaway(); } ||
    requires(T&& event) { std::move(event).message(); } ||
    requires(T&& event) { std::move(event).ping(); } ||
    requires(T&& event) { std::move(event).pong(); } ||
    requires(T&& event) { std::move(event).close(); } ||
    requires(T&& event) { std::move(event).protocolError(); } ||
    requires(T&& event) { std::move(event).transportEnd(); };

template <typename T>
concept HasSharedCacheFreshnessPolicy = requires(const T& directives) {
    directives.sharedFreshnessLifetime();
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
concept ExposesRvalueDecodedContent = requires(T&& result) {
    std::move(result).decoded();
};

template <typename T>
concept ExposesRvalueDecodeFailure = requires(const T&& result) {
    std::move(result).failure();
};

template <typename T>
concept ExposesRvalueEncodedContent = requires(T&& result) {
    std::move(result).encoded();
};

template <typename T>
concept ExposesRvalueEncodeFailure = requires(const T&& result) {
    std::move(result).failure();
};

template <typename T>
concept ExposesRvalueContentCoding = requires(const T&& result) {
    std::move(result).coding();
};

template <typename T>
concept ExposesAnyRvalueWebSocketFrameReadAccessor =
    requires(T&& result) { std::move(result).needInput(); } ||
    requires(T&& result) { std::move(result).frame(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept ExposesAnyRvalueWebSocketInboundAccessor =
    requires(T&& result) { std::move(result).continueReading(); } ||
    requires(T&& result) { std::move(result).controlFrame(); } ||
    requires(T&& result) { std::move(result).message(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept ExposesAnyRvalueHttpOperationResultAccessor =
    requires(T&& result) { std::move(result).committed(); } ||
    requires(T&& result) { std::move(result).prepared(); } ||
    requires(T&& result) { std::move(result).submitted(); } ||
    requires(T&& result) { std::move(result).applied(); } ||
    requires(T&& result) { std::move(result).initialWindowChange(); } ||
    requires(T&& result) { std::move(result).plan(); } ||
    requires(T&& result) { std::move(result).section(); } ||
    requires(T&& result) { std::move(result).failure(); };

static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<
    ruvia::detail::Http1FinalResponseCommitResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<
    ruvia::detail::PreparedHttp1ResponseStreamResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<
    ruvia::detail::Http2WebSocketHandshakeSubmitResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<
    ruvia::detail::Http2RequestHeadSubmitResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<
    ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<
    ruvia::detail::Http2StreamingResponseHeadSubmitResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<
    ruvia::detail::Http2PeerSettingApplyResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<
    ruvia::detail::Http2ResponseHeadPlanResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<
    ruvia::detail::HttpFinalResponseControlPlanResult>);
static_assert(!ExposesAnyRvalueHttpOperationResultAccessor<
    ruvia::detail::HttpResponseTrailerSectionResult>);

template <typename T>
concept ExposesRvalueUnsupportedContentCoding = requires(const T&& result) {
    std::move(result).unsupported();
};

template <typename T>
concept HasContentLengthPresent = requires(const T& state) {
    state.present();
};

template <typename T>
concept HasStaleTransferEncodingAccessors = requires(const T& state) {
    state.present();
    state.finalChunked();
    state.codings();
};

template <typename T>
concept ExposesRvalueFinalChunked = requires(const T&& value) {
    std::move(value).finalChunked();
};

template <typename T>
concept ExposesRvalueNonChunked = requires(const T&& value) {
    std::move(value).nonChunked();
};

template <typename Output>
concept AcceptsUrlDecodeOutputParameter = requires(Output& output) {
    ruvia::detail::decodeUrlComponent(
        std::string_view{},
        output,
        ruvia::detail::UrlDecodeMode::kPercent);
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

template <typename Connection>
concept AcceptsUnpreparedBufferedResponseHead = requires(
    Connection& connection,
    const ruvia::HttpResponse& response) {
    connection.submitResponseHead(std::uint32_t{}, response);
};

template <typename Connection>
concept AcceptsStagedResponseTrailerSection = requires(
    Connection& connection,
    std::span<const ruvia::HttpHeaderView> trailers) {
    connection.submitResponseTrailerSection(std::uint32_t{}, trailers);
};

template <typename Connection>
concept AcceptsImplicitResponseFinish = requires(Connection& connection) {
    connection.finishResponse(std::uint32_t{});
};

template <typename Connection>
concept AcceptsRawResponseTrailerFinish = requires(
    Connection& connection,
    std::span<const ruvia::HttpHeaderView> trailers) {
    connection.finishResponse(std::uint32_t{}, trailers);
};

template <typename T>
concept HasResponseTrailerSectionAlternatives = requires(const T& result) {
    { result.section() } -> std::same_as<const
        ruvia::detail::HttpResponseTrailerSection*>;
    { result.failure() } -> std::same_as<const
        ruvia::detail::HttpResponseTrailerSectionFailure*>;
};

template <typename Stream>
concept HasStagedResponseTrailerBlock = requires(Stream& stream) {
    stream.responseTrailerBlock();
};

template <typename HeaderBlocks>
concept HasStagedResponseTrailers = requires(HeaderBlocks& blocks) {
    blocks.responseTrailers();
};

template <typename BodyPlan>
concept AcceptsLooseResponseStreamBodyPlan = requires(BodyPlan bodyPlan) {
    ruvia::detail::httpResponseStreamCommitPlan(
        ruvia::detail::ResponseStreamFraming::kHttp1Chunked,
        bodyPlan,
        ruvia::detail::ResponseTrailerIntent::kNone);
};

template <typename BodyPlan>
concept AcceptsLooseBufferedResponseBodyPlan = requires(
    BodyPlan bodyPlan,
    const ruvia::HttpResponse& response) {
    ruvia::detail::httpBufferedResponseWritePlan(bodyPlan, response);
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
concept HasByteRangeOutcomeField = requires(const T& result) {
    result.outcome;
};

template <typename T>
concept HasByteRangePayloadField = requires(const T& result) {
    result.range;
};

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
concept HasWsSubmitMessageAlias = requires(T& connection) {
    connection.submitMessage(
        ruvia::WebSocketOpcode::kText,
        std::string_view{});
};

template <typename T>
concept HasWsSubmitPingAlias = requires(T& connection) {
    connection.submitPing(std::string_view{});
};

template <typename T>
concept HasWsSubmitPongAlias = requires(T& connection) {
    connection.submitPong(std::string_view{});
};

template <typename T>
concept HasWsApplicationFrameStateSideChannel = requires(
    const T& connection) {
    connection.acceptsApplicationFrames();
};

template <typename T>
concept HasWsEndsTransportAlias = requires(const T& plan) {
    plan.endsTransport();
};

template <typename T>
concept HasWsTransportEndPendingSideChannel = requires(
    const T& connection) {
    connection.transportEndPending();
};

template <typename T>
concept HasWsClosedStateSideChannel = requires(const T& connection) {
    connection.closed();
};

template <typename T>
concept HasWsClosePhaseSideChannel = requires(const T& connection) {
    connection.closePhase();
};

template <typename T>
concept HasWebSocketNegotiationAccessor = requires(const T& value) {
    { value.negotiation() } ->
        std::same_as<const ruvia::detail::WebSocketServerNegotiation&>;
};

template <typename T>
concept HasWebSocketHandshakeErrorAccessor = requires(const T& value) {
    { value.error() } -> std::same_as<
        ruvia::detail::Http2WebSocketHandshakeSubmitError>;
};

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
concept AcceptsLooseWebSocketHandshakeSubmit = requires(T& connection) {
    connection.submitWebSocketHandshake(
        std::uint32_t{},
        std::string_view{},
        std::string_view{});
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
concept HasTransferOutputBytes = requires(const T& result) {
    { result.bytes() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasTransferDecodeError = requires(const T& result) {
    { result.error() } ->
        std::same_as<ruvia::detail::TransferCodingDecodeError>;
};

template <typename T>
concept HasChunkScanError = requires(const T& result) {
    { result.error() } -> std::same_as<ruvia::detail::HttpChunkScanError>;
};

template <typename T>
concept HasAnyRvalueHttpChunkScanAccessor =
    requires(T&& result) { std::move(result).needMore(); } ||
    requires(T&& result) { std::move(result).complete(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueHttp1ChunkDecodeAccessor =
    requires(T&& result) { std::move(result).needMore(); } ||
    requires(T&& result) { std::move(result).bodyChunk(); } ||
    requires(T&& result) { std::move(result).complete(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueTransferCodingDecodeAccessor =
    requires(T&& result) { std::move(result).needInput(); } ||
    requires(T&& result) { std::move(result).output(); } ||
    requires(T&& result) { std::move(result).complete(); } ||
    requires(T&& result) { std::move(result).failure(); };

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

template <typename T>
concept HasHttpClientRequestContentAlternatives = requires(const T& content) {
    { content.withoutContent() } ->
        std::same_as<const ruvia::HttpClientRequestWithoutContent*>;
    { content.borrowedBytes() } ->
        std::same_as<const ruvia::HttpClientRequestBytes*>;
};

template <typename T>
concept HasAnyRvalueHttpClientRequestContentAccessor =
    requires(T&& content) { std::move(content).withoutContent(); } ||
    requires(T&& content) { std::move(content).borrowedBytes(); };

template <typename T>
concept HasStaleHttpClientContentMode = requires(const T& content) {
    content.mode();
};

template <typename T>
concept HasHttpClientRequestContentValue = requires(const T& content) {
    { content.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttp1PreparedContentAlternatives = requires(const T& plan) {
    { plan.withoutContent() } ->
        std::same_as<const ruvia::Http1ClientRequestWithoutContent*>;
    { plan.immediate() } ->
        std::same_as<const ruvia::Http1ClientImmediateRequestContent*>;
    { plan.continueGated() } ->
        std::same_as<const ruvia::Http1ClientContinueGatedRequestContent*>;
};

template <typename T>
concept HasAnyRvalueHttp1ClientRequestContentPlanAccessor =
    requires(T&& plan) { std::move(plan).withoutContent(); } ||
    requires(T&& plan) { std::move(plan).immediate(); } ||
    requires(T&& plan) { std::move(plan).continueGated(); };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestWirePolicyAccessor =
    requires(T&& policy) { std::move(policy).noExpectation(); } ||
    requires(T&& policy) { std::move(policy).continueExpectation(); };

template <typename T>
concept HasHttp1ClientExpectationAlternatives = requires(const T& policy) {
    { policy.noExpectation() } -> std::same_as<const
        ruvia::Http1ClientNoRequestExpectation*>;
    { policy.continueExpectation() } -> std::same_as<const
        ruvia::Http1ClientContinueExpectation*>;
};

template <typename T>
concept HasHttp1PreparedContentDisposition = requires(const T& plan) {
    plan.disposition();
};

template <typename T>
concept HasHttp1PreparedContentBytes = requires(const T& content) {
    { content.bytes() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttp1ResponseHeadAlternatives = requires(const T& plan) {
    { plan.buffered() } ->
        std::same_as<const ruvia::detail::Http1BufferedResponseHead*>;
    { plan.chunkedStream() } ->
        std::same_as<const ruvia::detail::Http1ChunkedResponseStreamHead*>;
    { plan.closeDelimitedStream() } -> std::same_as<
        const ruvia::detail::Http1CloseDelimitedResponseStreamHead*>;
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
concept HasHttp1BufferedPlanComposition = requires(const T& plan) {
    { plan.writePlan() } -> std::same_as<const
        ruvia::detail::HttpBufferedResponseWritePlan&>;
    { plan.headPlan() } -> std::same_as<const
        ruvia::detail::Http1ResponseHeadPlan&>;
};

template <typename T>
concept HasStaleHttp1ResponseSignal = requires(const T& plan) {
    plan.responseSignal();
};

template <typename T>
concept HasStaleHttp1ResponseHeadScalar = requires(const T& plan) {
    plan.suppressAutoContentLength();
};

template <typename T>
concept HasHttp1ServerParseAlternatives = requires(const T& state) {
    { state.needRequestHead() } -> std::same_as<const
        ruvia::detail::Http1ServerNeedRequestHead*>;
    { state.headReady() } -> std::same_as<const
        ruvia::detail::Http1ServerRequestHeadReady*>;
    { state.needRequestBody() } -> std::same_as<const
        ruvia::detail::Http1ServerNeedRequestBody*>;
    { state.messageReady() } -> std::same_as<const
        ruvia::detail::Http1ServerRequestMessageReady*>;
    { state.failure() } -> std::same_as<const
        ruvia::detail::Http1ServerRequestParseFailure*>;
};

template <typename T>
concept HasAnyRvalueHttp1RequestParseAccessor =
    requires(T&& result) { std::move(result).needMore(); } ||
    requires(T&& result) { std::move(result).parsed(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueHttp1ClientResponseParseAccessor =
    requires(T&& result) { std::move(result).needMore(); } ||
    requires(T&& result) { std::move(result).parsed(); } ||
    requires(const T&& result) { std::move(result).parsed(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueMultipartPollAccessor =
    requires(T&& result) { std::move(result).needInput(); } ||
    requires(T&& result) { std::move(result).part(); } ||
    requires(T&& result) { std::move(result).done(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueMultipartDelimiterAccessor =
    requires(T&& result) { std::move(result).noMatch(); } ||
    requires(T&& result) { std::move(result).needInput(); } ||
    requires(T&& result) { std::move(result).part(); } ||
    requires(T&& result) { std::move(result).close(); };

template <typename T>
concept HasAnyRvalueMultipartPartHeaderAccessor =
    requires(T&& result) { std::move(result).headers(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueMultipartBoundaryAccessor =
    requires(T&& result) { std::move(result).boundary(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestPrepareAccessor =
    requires(T&& result) { std::move(result).bufferTooSmall(); } ||
    requires(T&& result) { std::move(result).prepared(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueHttp1InterimResponsePrepareAccessor =
    requires(T&& result) { std::move(result).bufferTooSmall(); } ||
    requires(T&& result) { std::move(result).prepared(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasStaleHttp1ServerParseScalars =
    requires(const T& state) { state.phase(); } ||
    requires(const T& state) { state.headerBytes; } ||
    requires(const T& state) { state.messageBytes; } ||
    requires(const T& state) { state.requiredTotalBytes; };

template <typename T>
concept HasStalePreparedStreamPolicy = requires(const T& prepared) {
    prepared.policy();
};

template <typename T>
concept HasFinalResponseControlResultAlternatives = requires(
    const T& result) {
    { result.plan() } -> std::same_as<const
        ruvia::detail::HttpFinalResponseControlPlan*>;
    { result.failure() } -> std::same_as<const
        ruvia::detail::HttpFinalResponseControlPlanFailure*>;
};

template <typename T>
concept HasFinalResponseControlProtocolAlternatives = requires(
    const T& plan) {
    { plan.http1() } -> std::same_as<const
        ruvia::detail::Http1FinalResponseControl*>;
    { plan.http2() } -> std::same_as<const
        ruvia::detail::Http2FinalResponseControl*>;
};

template <typename T>
concept HasHttp1FinalResponseControlFields = requires(const T& plan) {
    { plan.connectionOptions() } -> std::same_as<const
        ruvia::detail::HttpConnectionOptions&>;
    { plan.upgradeProtocols() } -> std::same_as<const
        ruvia::detail::HttpUpgradeProtocols&>;
};

template <typename T>
concept HasStaleFinalResponseControlStatus = requires(const T& result) {
    result.status();
    result.accepted();
};

template <typename T>
concept HasStaleTopLevelUpgradeProtocols = requires(const T& plan) {
    plan.upgradeProtocols();
};

template <typename T>
concept HasHttp1FinalCommitAlternatives = requires(const T& result) {
    { result.committed() } -> std::same_as<const
        ruvia::detail::Http1FinalResponseCommit*>;
    { result.failure() } -> std::same_as<const
        ruvia::detail::Http1FinalResponseCommitFailure*>;
};

template <typename T>
concept HasPreparedHttp1StreamAlternatives = requires(const T& result) {
    { result.prepared() } -> std::same_as<const
        ruvia::detail::PreparedHttp1ResponseStream*>;
    { result.failure() } -> std::same_as<const
        ruvia::detail::Http1FinalResponseCommitFailure*>;
};

template <typename T>
concept HasHttp2ResponseHeadContentLengthAlternatives = requires(
    const T& plan) {
    { plan.canonicalContentLength() } -> std::same_as<const
        ruvia::detail::Http2CanonicalResponseContentLength*>;
    { plan.explicitContentLength() } -> std::same_as<const
        ruvia::detail::Http2ExplicitResponseContentLength*>;
    { plan.absentContentLength() } -> std::same_as<const
        ruvia::detail::Http2AbsentResponseContentLength*>;
    { plan.forbiddenContentLength() } -> std::same_as<const
        ruvia::detail::Http2ForbiddenResponseContentLength*>;
};

template <typename T>
concept HasHttp2ResponseContentLengthValue = requires(const T& length) {
    { length.value() } -> std::same_as<std::uint64_t>;
};

template <typename T>
concept HasHttp2RequestContentAlternatives = requires(const T& content) {
    { content.withoutContent() } ->
        std::same_as<const ruvia::detail::Http2RequestWithoutContent*>;
    { content.knownLengthContent() } ->
        std::same_as<const ruvia::detail::Http2KnownLengthRequestContent*>;
    { content.streamingContent() } ->
        std::same_as<const ruvia::detail::Http2StreamingRequestContent*>;
};

template <typename T>
concept HasStaleHttp2ContentMode = requires(const T& content) {
    content.mode();
};

template <typename T>
concept HasHttp2RequestContentLength = requires(const T& content) {
    { content.length() } -> std::same_as<std::uint64_t>;
};

template <typename T>
concept HasHttp2LocalContentAlternatives = requires(const T& content) {
    { content.unset() } ->
        std::same_as<const ruvia::detail::Http2LocalContentUnset*>;
    { content.forbidden() } ->
        std::same_as<const ruvia::detail::Http2LocalContentForbidden*>;
    { content.unbounded() } ->
        std::same_as<const ruvia::detail::Http2LocalContentUnbounded*>;
    { content.knownLength() } ->
        std::same_as<const ruvia::detail::Http2LocalContentKnownLength*>;
};

template <typename T>
concept HasStaleHttp2LocalModeAccessor = requires(const T& content) {
    content.mode();
};

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
    { content.allowedWithoutLength() } ->
        std::same_as<const
            ruvia::detail::Http2RemoteContentAllowedWithoutLength*>;
    { content.allowedKnownLength() } ->
        std::same_as<const
            ruvia::detail::Http2RemoteContentAllowedKnownLength*>;
    { content.metadataOnlyWithoutLength() } ->
        std::same_as<const
            ruvia::detail::Http2RemoteContentMetadataOnlyWithoutLength*>;
    { content.metadataOnlyKnownLength() } ->
        std::same_as<const
            ruvia::detail::Http2RemoteContentMetadataOnlyKnownLength*>;
};

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
concept HasHttpResponseContentAlternatives = requires(const T& semantics) {
    { semantics.informational() } -> std::same_as<const
        ruvia::detail::HttpInformationalResponseContent*>;
    { semantics.protocolSwitch() } -> std::same_as<const
        ruvia::detail::HttpProtocolSwitchResponseContent*>;
    { semantics.connectTunnel() } -> std::same_as<const
        ruvia::detail::HttpConnectTunnelResponseContent*>;
    { semantics.withoutContent() } -> std::same_as<const
        ruvia::detail::HttpResponseWithoutContent*>;
    { semantics.withContent() } -> std::same_as<const
        ruvia::detail::HttpResponseWithContent*>;
};

template <typename T>
concept ExposesAnyRvalueHttpProtocolPlanBorrow =
    requires(T&& value) { std::move(value).ignored(); } ||
    requires(T&& value) { std::move(value).unsatisfiable(); } ||
    requires(T&& value) { std::move(value).resolved(); } ||
    requires(T&& value) { std::move(value).informational(); } ||
    requires(T&& value) { std::move(value).protocolSwitch(); } ||
    requires(T&& value) { std::move(value).connectTunnel(); } ||
    requires(T&& value) { std::move(value).withoutContent(); } ||
    requires(T&& value) { std::move(value).withContent(); } ||
    requires(T&& value) { std::move(value).withoutBody(); } ||
    requires(T&& value) { std::move(value).knownLength(); } ||
    requires(T&& value) { std::move(value).chunked(); } ||
    requires(T&& value) { std::move(value).buffered(); } ||
    requires(T&& value) { std::move(value).chunkedStream(); } ||
    requires(T&& value) { std::move(value).closeDelimitedStream(); } ||
    requires(T&& value) { std::move(value).knownLengthContent(); } ||
    requires(T&& value) { std::move(value).streamingContent(); } ||
    requires(T&& value) { std::move(value).canonicalContentLength(); } ||
    requires(T&& value) { std::move(value).explicitContentLength(); } ||
    requires(T&& value) { std::move(value).absentContentLength(); } ||
    requires(T&& value) { std::move(value).forbiddenContentLength(); } ||
    requires(T&& value) { std::move(value).http1(); } ||
    requires(T&& value) { std::move(value).http2(); } ||
    requires(T&& value) { std::move(value).policy(); } ||
    requires(T&& value) { std::move(value).contentSemantics(); } ||
    requires(T&& value) { std::move(value).bodyPlan(); } ||
    requires(T&& value) { std::move(value).expectations(); } ||
    requires(T&& value) { std::move(value).transferCodings(); } ||
    requires(T&& value) { std::move(value).connectionOptions(); } ||
    requires(T&& value) { std::move(value).upgradeProtocols(); } ||
    requires(T&& value) { std::move(value).writePlan(); } ||
    requires(T&& value) { std::move(value).headPlan(); };

static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::HttpByteRangeResolution>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::HttpResponseContentSemantics>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::HttpResponseBodyPlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::HttpBufferedResponseWritePlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::Http1ChunkedRequestBody>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::Http1ResponseHeadPlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::Http1BufferedResponsePlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::Http2RequestContent>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::Http2ResponseHeadPlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::HttpFinalResponseControlPlan>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::Http1FinalResponseControl>);
static_assert(!ExposesAnyRvalueHttpProtocolPlanBorrow<
    ruvia::detail::ResponseStreamCommitPlan>);

template <typename T>
concept ExposesAnyRvalueHttpOperationPayloadBorrow =
    requires(T&& value) { std::move(value).request(); } ||
    requires(T&& value) { std::move(value).response(); } ||
    requires(const T&& value) { std::move(value).response(); } ||
    requires(T&& value) { std::move(value).bodyPlan(); } ||
    requires(T&& value) { std::move(value).plan(); } ||
    requires(T&& value) { std::move(value).contentPlan(); } ||
    requires(T&& value) { std::move(value).responseHeadPlan(); } ||
    requires(T&& value) { std::move(value).commitPlan(); } ||
    requires(T&& value) { std::move(value).negotiation(); };

static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<
    ruvia::Http1ParsedRequest>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<
    ruvia::Http1ParsedClientResponseHead>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<
    ruvia::PreparedHttp1ClientRequest>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<
    ruvia::detail::ResponseStreamHead>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<
    ruvia::detail::PreparedHttp1ResponseStream>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<
    ruvia::detail::Http2SubmittedWebSocketHandshake>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<
    ruvia::detail::HttpWebSocketServerHandshake>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<
    ruvia::detail::Http2SubmittedBufferedResponseHead>);
static_assert(!ExposesAnyRvalueHttpOperationPayloadBorrow<
    ruvia::detail::Http2SubmittedStreamingResponseHead>);

template <typename T>
concept HasStaleHttp2StreamRemoteContentForwarders = requires(
    const T& stream) {
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
    { connection.releaseReceivedData(std::uint32_t{1}) } ->
        std::same_as<void>;
};

template <typename T>
concept HasStaleHttp2ReceiveDeferral = requires(T& connection) {
    connection.deferStreamWindowRelease(std::uint32_t{1});
    connection.releaseStreamWindow(std::uint32_t{1});
};

template <typename T>
concept HasHttp2TunnelAlternatives = requires(const T& state) {
    { state.notConnect() } ->
        std::same_as<const ruvia::detail::Http2NotConnect*>;
    { state.pending() } ->
        std::same_as<const ruvia::detail::Http2ConnectPending*>;
    { state.open() } ->
        std::same_as<const ruvia::detail::Http2TunnelOpen*>;
    { state.rejected() } ->
        std::same_as<const ruvia::detail::Http2ConnectRejected*>;
};

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
    { state.headPending() } ->
        std::same_as<const ruvia::detail::Http2LocalHeadPending*>;
    { state.requestContentOpen() } ->
        std::same_as<const ruvia::detail::Http2LocalRequestContentOpen*>;
    { state.responseContentOpen() } ->
        std::same_as<const ruvia::detail::Http2LocalResponseContentOpen*>;
    { state.responseTrailersOnly() } ->
        std::same_as<const ruvia::detail::Http2LocalResponseTrailersOnly*>;
    { state.connectPending() } ->
        std::same_as<const ruvia::detail::Http2LocalConnectPending*>;
    { state.tunnelOpen() } ->
        std::same_as<const ruvia::detail::Http2LocalTunnelOpen*>;
    { state.endStreamQueued() } ->
        std::same_as<const ruvia::detail::Http2LocalEndStreamQueued*>;
    { state.endStreamCommitted() } ->
        std::same_as<const ruvia::detail::Http2LocalEndStreamCommitted*>;
    { state.aborted() } ->
        std::same_as<const ruvia::detail::Http2StreamAborted*>;
};

template <typename T>
concept HasHttp2RemoteReceiveAlternatives = requires(const T& state) {
    { state.headPending() } ->
        std::same_as<const ruvia::detail::Http2RemoteHeadPending*>;
    { state.headEndStreamPending() } ->
        std::same_as<const ruvia::detail::Http2RemoteHeadEndStreamPending*>;
    { state.contentOpen() } ->
        std::same_as<const ruvia::detail::Http2RemoteContentOpen*>;
    { state.connectPending() } ->
        std::same_as<const ruvia::detail::Http2RemoteConnectPending*>;
    { state.connectPendingEndStream() } ->
        std::same_as<const ruvia::detail::Http2RemoteConnectPendingEndStream*>;
    { state.connectRejectedAwaitingEndStream() } -> std::same_as<
        const ruvia::detail::Http2RemoteConnectRejectedAwaitingEndStream*>;
    { state.tunnelOpen() } ->
        std::same_as<const ruvia::detail::Http2RemoteTunnelOpen*>;
    { state.endStream() } ->
        std::same_as<const ruvia::detail::Http2RemoteEndStream*>;
    { state.aborted() } ->
        std::same_as<const ruvia::detail::Http2RemoteAborted*>;
};

template <typename T>
concept HasStaleHttp2BodyEnded = requires(const T& stream) {
    stream.bodyEnded();
};

template <typename T>
concept HasStaleHttp2PeerEndStream = requires(const T& stream) {
    stream.peerEndStream();
};

template <typename T>
concept HasStaleHttp2HeadersDecoded = requires(const T& stream) {
    stream.headersDecoded();
};

template <typename T>
concept HasHttp2LocalCloseSource = requires(const T& state) {
    { state.source() } ->
        std::same_as<ruvia::detail::Http2StreamCloseSource>;
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
    { stream.abort(ruvia::detail::Http2StreamCloseSource::kLocal) } ->
        std::same_as<bool>;
};

template <typename T>
concept HasStaleHttp2IsReset = requires(const T& stream) {
    stream.isReset();
};

template <typename T>
concept HasStaleHttp2MarkReset = requires(T& stream) {
    stream.markReset(ruvia::detail::Http2StreamCloseSource::kLocal);
};

template <typename T>
concept HasStaleHttp2MarkClosed = requires(T& stream) {
    stream.markClosed(ruvia::detail::Http2StreamCloseSource::kLocal);
};

template <typename T>
concept HasHttp2RemoveAborted = requires(T& table) {
    table.removeAborted(
        [](const ruvia::detail::Http2StreamState&) noexcept {});
};

template <typename T>
concept HasStaleHttp2RemoveReset = requires(T& table) {
    table.removeReset(
        [](const ruvia::detail::Http2StreamState&) noexcept {});
};

using HttpResponseBodySetter = void (ruvia::HttpResponse::*)(std::string_view);
using HttpResponseHeadersGetter = const ruvia::HttpResponseHeaders& (
    ruvia::HttpResponse::*)() const & noexcept;

template <typename T>
concept ExposesAnyRvalueResponseView =
    requires(T&& value) { std::move(value).headers(); } ||
    requires(T&& value) { std::move(value).header(std::string_view{}); } ||
    requires(T&& value) { std::move(value).begin(); } ||
    requires(T&& value) { std::move(value).end(); } ||
    requires(T&& value) { std::move(value).cbegin(); } ||
    requires(T&& value) { std::move(value).cend(); };

template <typename T>
concept ExposesAnyRvalueResponseBodyBorrow =
    requires(T&& value) { std::move(value).empty(); } ||
    requires(T&& value) { std::move(value).borrowedBytes(); } ||
    requires(T&& value) { std::move(value).staticBytes(); } ||
    requires(T&& value) { std::move(value).ownedBytes(); } ||
    requires(T&& value) { std::move(value).ownedFile(); } ||
    requires(T&& value) { std::move(value).borrowedFile(); } ||
    requires(T&& value) { std::move(value).bytes(); } ||
    requires(T&& value) { std::move(value).file(); } ||
    requires(T&& value) { std::move(value).nativePathCStr(); };

template <typename T>
concept ExposesRvalueResponseBodyAccess =
    requires(T&& response) {
        ruvia::detail::responseBody(std::move(response));
    } ||
    requires(T&& response) {
        ruvia::detail::HttpResponseBodyAccess::body(std::move(response));
    };

static_assert(!HasLegacyResponseBodyCopy<ruvia::HttpResponse>);
static_assert(!HasLegacyResponseBodyView<ruvia::HttpResponse>);
static_assert(!std::is_copy_constructible_v<ruvia::HttpResponse>);
static_assert(!std::is_copy_assignable_v<ruvia::HttpResponse>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::HttpResponse>);
static_assert(std::is_nothrow_move_assignable_v<ruvia::HttpResponse>);
static_assert(std::same_as<
    decltype(static_cast<HttpResponseBodySetter>(&ruvia::HttpResponse::body)),
    HttpResponseBodySetter>);
static_assert(std::same_as<
    decltype(static_cast<HttpResponseHeadersGetter>(
        &ruvia::HttpResponse::headers)),
    HttpResponseHeadersGetter>);
static_assert(!std::default_initializable<ruvia::HttpResponseHeaders>);
static_assert(!std::constructible_from<
    ruvia::HttpResponseHeaders,
    std::pmr::memory_resource*>);
static_assert(!ExposesAnyRvalueResponseView<ruvia::HttpResponse>);
static_assert(!ExposesAnyRvalueResponseView<ruvia::HttpResponseHeaders>);
static_assert(!ExposesAnyRvalueResponseBodyBorrow<
    ruvia::detail::HttpResponseBody>);
static_assert(!ExposesAnyRvalueResponseBodyBorrow<
    ruvia::detail::HttpOwnedResponseBytes>);
static_assert(!ExposesAnyRvalueResponseBodyBorrow<
    ruvia::detail::HttpOwnedResponseFile>);
static_assert(!ExposesRvalueResponseBodyAccess<ruvia::HttpResponse>);
static_assert(!HasSharedCacheFreshnessPolicy<ruvia::CacheControl>);
static_assert(std::same_as<
    decltype(ruvia::CacheControl::sMaxAge),
    std::optional<std::uint64_t>>);
static_assert(std::same_as<
    decltype(ruvia::CookieOptions{}.sameSite),
    std::optional<ruvia::CookieSameSite>>);
static_assert(std::same_as<
    decltype(ruvia::CookieOptions{}.priority),
    std::optional<ruvia::CookiePriority>>);
static_assert(std::same_as<
    decltype(ruvia::CookieOptions{}.prefix),
    std::optional<ruvia::CookiePrefix>>);
static_assert(std::same_as<
    decltype(ruvia::CookieOptions{}.maxAge),
    std::optional<std::chrono::seconds>>);
static_assert(HasHttpClientRequestContentAlternatives<
    ruvia::HttpClientRequestContent>);
static_assert(!HasStaleHttpClientContentMode<
    ruvia::HttpClientRequestContent>);
static_assert(!HasHttpClientRequestContentValue<
    ruvia::HttpClientRequestContent>);
static_assert(HasHttpClientRequestContentValue<
    ruvia::HttpClientRequestBytes>);
static_assert(!HasHttpClientRequestContentValue<
    ruvia::HttpClientRequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestContent>);
static_assert(!std::default_initializable<
    ruvia::HttpClientRequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestBytes>);

static_assert(HasHttp1PreparedContentAlternatives<
    ruvia::Http1ClientRequestContentPlan>);
static_assert(HasHttp1ClientExpectationAlternatives<
    ruvia::Http1ClientRequestWirePolicy>);
static_assert(!HasHttp1PreparedContentDisposition<
    ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasHttp1PreparedContentBytes<
    ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasHttp1PreparedContentBytes<
    ruvia::Http1ClientRequestWithoutContent>);
static_assert(HasHttp1PreparedContentBytes<
    ruvia::Http1ClientImmediateRequestContent>);
static_assert(HasHttp1PreparedContentBytes<
    ruvia::Http1ClientContinueGatedRequestContent>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientRequestWithoutContent>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientImmediateRequestContent>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientContinueGatedRequestContent>);

static_assert(HasHttp1ResponseHeadAlternatives<
    ruvia::detail::Http1ResponseHeadPlan>);
static_assert(HasHttp1ProtocolVersion<
    ruvia::detail::Http1ServerConnectionPlan>);
static_assert(HasHttp1ProtocolVersion<
    ruvia::detail::Http1ResponseHeadPlan>);
static_assert(HasHttp1BufferedContentLength<
    ruvia::detail::Http1BufferedResponseHead>);
static_assert(HasHttp1BufferedPlanComposition<
    ruvia::detail::Http1BufferedResponsePlan>);
static_assert(!HasStaleHttp1ResponseSignal<
    ruvia::detail::Http1ServerConnectionPlan>);
static_assert(!HasStaleHttp1ResponseHeadScalar<
    ruvia::detail::Http1ResponseHeadPlan>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1ServerConnectionPlan>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1ResponseHeadPlan>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponsePlan>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponseHead>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1ChunkedResponseStreamHead>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1CloseDelimitedResponseStreamHead>);
static_assert(!HasStalePreparedStreamPolicy<
    ruvia::detail::PreparedHttp1ResponseStream>);

static_assert(HasFinalResponseControlResultAlternatives<
    ruvia::detail::HttpFinalResponseControlPlanResult>);
static_assert(HasFinalResponseControlProtocolAlternatives<
    ruvia::detail::HttpFinalResponseControlPlan>);
static_assert(HasHttp1FinalResponseControlFields<
    ruvia::detail::Http1FinalResponseControl>);
static_assert(!HasHttp1FinalResponseControlFields<
    ruvia::detail::Http2FinalResponseControl>);
static_assert(!HasStaleFinalResponseControlStatus<
    ruvia::detail::HttpFinalResponseControlPlanResult>);
static_assert(!HasStaleTopLevelUpgradeProtocols<
    ruvia::detail::HttpFinalResponseControlPlan>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1FinalResponseControl>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2FinalResponseControl>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpFinalResponseControlPlan>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpFinalResponseControlPlanFailure>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpFinalResponseControlPlanResult>);
static_assert(HasHttp1FinalCommitAlternatives<
    ruvia::detail::Http1FinalResponseCommitResult>);
static_assert(HasPreparedHttp1StreamAlternatives<
    ruvia::detail::PreparedHttp1ResponseStreamResult>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1FinalResponseCommit>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1FinalResponseCommitFailure>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1FinalResponseCommitResult>);
static_assert(!std::default_initializable<
    ruvia::detail::PreparedHttp1ResponseStreamResult>);

static_assert(HasHttp2ResponseHeadContentLengthAlternatives<
    ruvia::detail::Http2ResponseHeadPlan>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2ResponseHeadPlan>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2ResponseHeadPlanResult>);
static_assert(HasHttp2ResponseContentLengthValue<
    ruvia::detail::Http2CanonicalResponseContentLength>);
static_assert(HasHttp2ResponseContentLengthValue<
    ruvia::detail::Http2ExplicitResponseContentLength>);
static_assert(!HasHttp2ResponseContentLengthValue<
    ruvia::detail::Http2AbsentResponseContentLength>);
static_assert(!HasHttp2ResponseContentLengthValue<
    ruvia::detail::Http2ForbiddenResponseContentLength>);

static_assert(HasHttp2RequestContentAlternatives<
    ruvia::detail::Http2RequestContent>);
static_assert(!HasStaleHttp2ContentMode<
    ruvia::detail::Http2RequestContent>);
static_assert(!HasHttp2RequestContentLength<
    ruvia::detail::Http2RequestContent>);
static_assert(!HasHttp2RequestContentLength<
    ruvia::detail::Http2RequestWithoutContent>);
static_assert(HasHttp2RequestContentLength<
    ruvia::detail::Http2KnownLengthRequestContent>);
static_assert(!HasHttp2RequestContentLength<
    ruvia::detail::Http2StreamingRequestContent>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RequestContent>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RequestWithoutContent>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2KnownLengthRequestContent>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2StreamingRequestContent>);
static_assert(HasHttp2LocalContentAlternatives<
    ruvia::detail::Http2LocalContentState>);
static_assert(!HasStaleHttp2LocalModeAccessor<
    ruvia::detail::Http2LocalContentState>);
static_assert(!HasHttp2LocalDeclaredLength<
    ruvia::detail::Http2LocalContentState>);
static_assert(!HasHttp2LocalDeclaredLength<
    ruvia::detail::Http2LocalContentUnset>);
static_assert(!HasHttp2LocalDeclaredLength<
    ruvia::detail::Http2LocalContentForbidden>);
static_assert(!HasHttp2LocalDeclaredLength<
    ruvia::detail::Http2LocalContentUnbounded>);
static_assert(HasHttp2LocalDeclaredLength<
    ruvia::detail::Http2LocalContentKnownLength>);
static_assert(std::default_initializable<
    ruvia::detail::Http2LocalContentState>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalContentUnset>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalContentForbidden>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalContentUnbounded>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalContentKnownLength>);
static_assert(!HasStaleHttp2StreamLocalContentForwarders<
    ruvia::detail::Http2StreamState>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http2StreamState&>()
        .localContent()),
    const ruvia::detail::Http2LocalContentState&>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http2StreamState&>()
        .responseStatus()),
    const std::uint16_t*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http2ClosedStreamHistory&>()
        .source(std::uint32_t{})),
    std::optional<ruvia::detail::Http2StreamCloseSource>>);
static_assert(!std::default_initializable<
    ruvia::detail::HpackDecodeResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::HpackDecodeResult&>()
        .decoded()),
    const ruvia::detail::HpackDecoded*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::HpackDecodeResult&>()
        .failure()),
    const ruvia::detail::HpackDecodeFailure*>);
static_assert(!ExposesRvalueDecodedContent<
    ruvia::detail::HpackDecodeResult>);
static_assert(!ExposesRvalueDecodeFailure<
    ruvia::detail::HpackDecodeResult>);
static_assert(!std::default_initializable<
    ruvia::Http1RequestParseFailure>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::Http1RequestParseFailure&>().error()),
    ruvia::HttpParseError>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1ServerRequestParseState&>().failure()),
    const ruvia::detail::Http1ServerRequestParseFailure*>);
static_assert(HasHttp1ServerParseAlternatives<
    ruvia::detail::Http1ServerRequestParseState>);
static_assert(!HasStaleHttp1ServerParseScalars<
    ruvia::detail::Http1ServerRequestParseState>);
static_assert(HasHttp2RemoteContentAlternatives<
    ruvia::detail::Http2RemoteContentState>);
static_assert(!HasStaleHttp2RemoteContentTuple<
    ruvia::detail::Http2RemoteContentState>);
static_assert(!HasHttp2RemoteDeclaredLength<
    ruvia::detail::Http2RemoteContentState>);
static_assert(!HasHttp2RemoteReceivedBytes<
    ruvia::detail::Http2RemoteContentState>);
static_assert(HasHttp2RemoteReceivedBytes<
    ruvia::detail::Http2RemoteContentAllowedWithoutLength>);
static_assert(HasHttp2RemoteReceivedBytes<
    ruvia::detail::Http2RemoteContentAllowedKnownLength>);
static_assert(!HasHttp2RemoteReceivedBytes<
    ruvia::detail::Http2RemoteContentMetadataOnlyWithoutLength>);
static_assert(!HasHttp2RemoteReceivedBytes<
    ruvia::detail::Http2RemoteContentMetadataOnlyKnownLength>);
static_assert(!HasHttp2RemoteDeclaredLength<
    ruvia::detail::Http2RemoteContentAllowedWithoutLength>);
static_assert(HasHttp2RemoteDeclaredLength<
    ruvia::detail::Http2RemoteContentAllowedKnownLength>);
static_assert(!HasHttp2RemoteDeclaredLength<
    ruvia::detail::Http2RemoteContentMetadataOnlyWithoutLength>);
static_assert(HasHttp2RemoteDeclaredLength<
    ruvia::detail::Http2RemoteContentMetadataOnlyKnownLength>);
static_assert(std::default_initializable<
    ruvia::detail::Http2RemoteContentState>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteContentAllowedWithoutLength>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteContentAllowedKnownLength>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteContentMetadataOnlyWithoutLength>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteContentMetadataOnlyKnownLength>);
static_assert(!HasStaleHttp2RemoteCheckAcceptSplit<
    ruvia::detail::Http2RemoteContentState>);
static_assert(!HasStaleHttp2StreamRemoteContentForwarders<
    ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2WebRuntimeState<
    ruvia::detail::Http2StreamState>);
static_assert(HasHttp2ReceiveDataRelease<
    ruvia::detail::Http2Connection>);
static_assert(!HasStaleHttp2ReceiveDeferral<
    ruvia::detail::Http2Connection>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http2StreamState&>()
        .remoteContent()),
    const ruvia::detail::Http2RemoteContentState&>);
static_assert(HasHttpResponseContentAlternatives<
    ruvia::detail::HttpResponseContentSemantics>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpResponseContentSemantics>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpInformationalResponseContent>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpProtocolSwitchResponseContent>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpConnectTunnelResponseContent>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpResponseWithoutContent>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpResponseWithContent>);
static_assert(HasHttp2TunnelAlternatives<
    ruvia::detail::Http2TunnelState>);
static_assert(!HasStaleHttp2TunnelKindPhase<
    ruvia::detail::Http2TunnelState>);
static_assert(!HasHttp2ConnectForm<
    ruvia::detail::Http2TunnelState>);
static_assert(!HasHttp2ConnectForm<
    ruvia::detail::Http2NotConnect>);
static_assert(HasHttp2ConnectForm<
    ruvia::detail::Http2ConnectPending>);
static_assert(!HasHttp2ConnectForm<
    ruvia::detail::Http2TunnelOpen>);
static_assert(!HasHttp2ConnectForm<
    ruvia::detail::Http2ConnectRejected>);
static_assert(std::default_initializable<
    ruvia::detail::Http2TunnelState>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2NotConnect>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2ConnectPending>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2TunnelOpen>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2ConnectRejected>);
static_assert(!HasStaleHttp2StreamTunnelForwarders<
    ruvia::detail::Http2StreamState>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http2StreamState&>()
        .tunnel()),
    const ruvia::detail::Http2TunnelState&>);
static_assert(HasHttp2LocalSendAlternatives<
    ruvia::detail::Http2LocalSendState>);
static_assert(!HasStaleHttp2LocalSendProduct<
    ruvia::detail::Http2LocalSendState>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalSendState>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalHeadPending>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalRequestContentOpen>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalResponseContentOpen>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalResponseTrailersOnly>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalConnectPending>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalTunnelOpen>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalEndStreamQueued>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2LocalEndStreamCommitted>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2StreamAborted>);
static_assert(!std::constructible_from<
    ruvia::detail::Http2StreamAborted,
    ruvia::detail::Http2StreamCloseSource>);
static_assert(!HasHttp2LocalCloseSource<
    ruvia::detail::Http2LocalSendState>);
static_assert(!HasHttp2LocalCloseSource<
    ruvia::detail::Http2LocalEndStreamCommitted>);
static_assert(HasHttp2LocalCloseSource<
    ruvia::detail::Http2StreamAborted>);
static_assert(!HasStaleHttp2StreamLocalSendForwarders<
    ruvia::detail::Http2StreamState>);
static_assert(HasHttp2AbortLifecycle<
    ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2IsReset<
    ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2MarkReset<
    ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2MarkClosed<
    ruvia::detail::Http2StreamState>);
static_assert(HasHttp2RemoveAborted<
    ruvia::detail::Http2StreamTable>);
static_assert(!HasStaleHttp2RemoveReset<
    ruvia::detail::Http2StreamTable>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http2StreamState&>()
        .localSend()),
    const ruvia::detail::Http2LocalSendState&>);
static_assert(HasHttp2RemoteReceiveAlternatives<
    ruvia::detail::Http2RemoteReceiveState>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteReceiveState>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteHeadPending>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteHeadEndStreamPending>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteContentOpen>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteConnectPending>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteConnectPendingEndStream>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteConnectRejectedAwaitingEndStream>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteTunnelOpen>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteEndStream>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RemoteAborted>);
static_assert(!HasStaleHttp2BodyEnded<
    ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2PeerEndStream<
    ruvia::detail::Http2StreamState>);
static_assert(!HasStaleHttp2HeadersDecoded<
    ruvia::detail::Http2StreamState>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http2StreamState&>()
        .remoteReceive()),
    const ruvia::detail::Http2RemoteReceiveState&>);

template <typename T>
concept HasHttp1RequestBodyPlanAlternatives = requires(const T& plan) {
    { plan.withoutBody() } ->
        std::same_as<const ruvia::detail::Http1RequestWithoutBody*>;
    { plan.knownLength() } ->
        std::same_as<const ruvia::detail::Http1KnownLengthRequestBody*>;
    { plan.chunked() } ->
        std::same_as<const ruvia::detail::Http1ChunkedRequestBody*>;
};

template <typename T>
concept HasHttp1RequestFramingAccessor = requires(const T& plan) {
    plan.mode();
};

template <typename T>
concept HasHttp1RequestPlanContentLength = requires(const T& framing) {
    { framing.contentLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasHttp1RequestPlanTransferCodings = requires(const T& framing) {
    framing.transferCodings();
};

template <typename T>
concept HasOptionalHttpServerExpectationAction = requires(const T& state) {
    { state.expectationAction() } -> std::same_as<std::optional<
        ruvia::detail::HttpServerExpectationAction>>;
};

template <typename T>
concept HasPublicHttp1RequestBodyPlanFactories = requires {
    T::makeWithoutBody();
    T::makeKnownLength(std::size_t{});
    T::makeChunked(ruvia::detail::HttpTransferCodings{});
};

static_assert(HasHttp1RequestBodyPlanAlternatives<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(!HasHttp1RequestFramingAccessor<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(!HasHttp1RequestPlanContentLength<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(!HasHttp1RequestPlanTransferCodings<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(HasHttp1RequestPlanContentLength<
    ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!HasHttp1RequestPlanContentLength<
    ruvia::detail::Http1ChunkedRequestBody>);
static_assert(HasHttp1RequestPlanTransferCodings<
    ruvia::detail::Http1ChunkedRequestBody>);
static_assert(!HasHttp1RequestPlanTransferCodings<
    ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!HasPublicHttp1RequestBodyPlanFactories<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(!std::default_initializable<ruvia::detail::Http1RequestBodyPlan>);
static_assert(!std::constructible_from<
    ruvia::detail::Http1RequestBodyPlan,
    ruvia::detail::HttpRequestExpectations>);
static_assert(!std::default_initializable<ruvia::detail::Http1RequestWithoutBody>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!std::default_initializable<ruvia::detail::Http1ChunkedRequestBody>);
static_assert(!std::constructible_from<
    ruvia::detail::Http1KnownLengthRequestBody,
    std::size_t>);
static_assert(!std::constructible_from<
    ruvia::detail::Http1ChunkedRequestBody,
    ruvia::detail::HttpTransferCodings>);
static_assert(HasOptionalHttpServerExpectationAction<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(HasOptionalHttpServerExpectationAction<
    ruvia::detail::Http2StreamState>);

template <typename T>
concept HasHttp1ClientResponsePlanAlternatives = requires(const T& plan) {
    { plan.informational() } ->
        std::same_as<const ruvia::Http1ClientInformationalResponse*>;
    { plan.withoutContent() } ->
        std::same_as<const ruvia::Http1ClientResponseWithoutContent*>;
    { plan.knownLength() } ->
        std::same_as<const ruvia::Http1ClientKnownLengthResponse*>;
    { plan.chunked() } ->
        std::same_as<const ruvia::Http1ClientChunkedResponse*>;
    { plan.closeDelimited() } ->
        std::same_as<const ruvia::Http1ClientCloseDelimitedResponse*>;
    { plan.connectTunnel() } ->
        std::same_as<const ruvia::Http1ClientConnectTunnel*>;
    { plan.protocolUpgrade() } ->
        std::same_as<const ruvia::Http1ClientProtocolUpgrade*>;
    { plan.requestContentSignal() } ->
        std::same_as<std::optional<
            ruvia::Http1ClientRequestContentSignal>>;
};

template <typename T>
concept HasAnyRvalueHttp1ClientResponsePlanAccessor =
    requires(T&& plan) { std::move(plan).informational(); } ||
    requires(T&& plan) { std::move(plan).withoutContent(); } ||
    requires(T&& plan) { std::move(plan).knownLength(); } ||
    requires(T&& plan) { std::move(plan).chunked(); } ||
    requires(T&& plan) { std::move(plan).closeDelimited(); } ||
    requires(T&& plan) { std::move(plan).connectTunnel(); } ||
    requires(T&& plan) { std::move(plan).protocolUpgrade(); };

template <typename T>
concept HasAnyRvalueHttpClientHeaderLookupAccessor =
    requires(T&& result) { std::move(result).absent(); } ||
    requires(T&& result) { std::move(result).found(); } ||
    requires(T&& result) { std::move(result).repeated(); };

template <typename T>
concept HasAnyRvalueHttpClientRedirectTargetAccessor =
    requires(T&& result) { std::move(result).target(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept ExposesAnyRvalueHttpClientOwnedView =
    requires(T&& value) { std::move(value).name(); } ||
    requires(T&& value) { std::move(value).value(); } ||
    requires(T&& value) { std::move(value).headers(); } ||
    requires(T&& value) { std::move(value).body(); } ||
    requires(T&& value) { std::move(value).transferCodings(); };

template <typename T>
concept AcceptsTemporaryHttpClientResponseHeaderLookup =
    requires(T&& response) {
        ruvia::lookupUniqueHttpClientResponseHeader(
            std::move(response), std::string_view{});
    };

template <typename T>
concept HasHttp1ClientResponseMode = requires(const T& plan) {
    plan.mode();
};

template <typename T>
concept HasHttp1ClientResponseConnectionAccessor = requires(const T& plan) {
    plan.connectionDisposition();
};

template <typename T>
concept HasHttp1ClientResponseContentLength = requires(const T& framing) {
    { framing.contentLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasHttp1ClientResponseTransferCodings = requires(const T& framing) {
    framing.transferCodings();
};

template <typename T>
concept HasHttp1ClientResponsePersistence = requires(const T& framing) {
    { framing.persistence() } ->
        std::same_as<ruvia::Http1ClientResponsePersistence>;
};

static_assert(HasHttp1ClientResponsePlanAlternatives<
    ruvia::Http1ClientResponsePlan>);
static_assert(!HasHttp1ClientResponseMode<ruvia::Http1ClientResponsePlan>);
static_assert(!HasHttp1ClientResponseConnectionAccessor<
    ruvia::Http1ClientResponsePlan>);
static_assert(!HasHttp1ClientResponseContentLength<
    ruvia::Http1ClientResponsePlan>);
static_assert(HasHttp1ClientResponseContentLength<
    ruvia::Http1ClientKnownLengthResponse>);
static_assert(!HasHttp1ClientResponseContentLength<
    ruvia::Http1ClientChunkedResponse>);
static_assert(HasHttp1ClientResponseTransferCodings<
    ruvia::Http1ClientChunkedResponse>);
static_assert(HasHttp1ClientResponseTransferCodings<
    ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!HasHttp1ClientResponseTransferCodings<
    ruvia::Http1ClientKnownLengthResponse>);
static_assert(HasHttp1ClientResponsePersistence<
    ruvia::Http1ClientResponseWithoutContent>);
static_assert(HasHttp1ClientResponsePersistence<
    ruvia::Http1ClientKnownLengthResponse>);
static_assert(HasHttp1ClientResponsePersistence<
    ruvia::Http1ClientChunkedResponse>);
static_assert(!HasHttp1ClientResponsePersistence<
    ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!std::default_initializable<ruvia::Http1ClientResponsePlan>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientInformationalResponse>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientResponseWithoutContent>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientKnownLengthResponse>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientChunkedResponse>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!std::default_initializable<ruvia::Http1ClientConnectTunnel>);
static_assert(!std::default_initializable<ruvia::Http1ClientProtocolUpgrade>);

static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http2Connection&>().nextEvent()),
    std::optional<ruvia::detail::Http2Event>>);
static_assert(!std::default_initializable<ruvia::detail::Http2Event>);
static_assert(!ExposesAnyRvalueSansIoEventBorrow<ruvia::detail::Http2Event>);
static_assert(!ExposesAnyRvalueSansIoEventBorrow<
    ruvia::detail::Http2GoawayEvent>);
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
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http2Connection&>().consumeOutput(
        std::size_t{})),
    ruvia::detail::Http2OutputConsumeStatus>);
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
static_assert(std::same_as<
    decltype(ruvia::detail::resolveHttpByteRange(
        std::string_view{}, std::uint64_t{})),
    ruvia::detail::HttpByteRangeResolution>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpByteRangeResolution>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpByteRangeResolution&>().ignored()),
    const ruvia::detail::HttpByteRangeIgnored*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpByteRangeResolution&>().unsatisfiable()),
    const ruvia::detail::HttpByteRangeUnsatisfiable*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpByteRangeResolution&>().resolved()),
    const ruvia::detail::HttpResolvedByteRange*>);
static_assert(!HasByteRangeOutcomeField<
    ruvia::detail::HttpByteRangeResolution>);
static_assert(!HasByteRangePayloadField<
    ruvia::detail::HttpByteRangeResolution>);
static_assert(!HasByteRangeOffsetAccessor<
    ruvia::detail::HttpByteRangeResolution>);
static_assert(!HasByteRangeLengthAccessor<
    ruvia::detail::HttpByteRangeResolution>);
static_assert(!HasByteRangeOffsetAccessor<
    ruvia::detail::HttpByteRangeIgnored>);
static_assert(!HasByteRangeLengthAccessor<
    ruvia::detail::HttpByteRangeUnsatisfiable>);
static_assert(HasByteRangeOffsetAccessor<
    ruvia::detail::HttpResolvedByteRange>);
static_assert(HasByteRangeLengthAccessor<
    ruvia::detail::HttpResolvedByteRange>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpByteRangeIgnored>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpByteRangeUnsatisfiable>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpResolvedByteRange>);
static_assert(!std::constructible_from<
    ruvia::detail::HttpResolvedByteRange,
    std::uint64_t,
    std::uint64_t>);
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
            std::declval<const ruvia::HttpResponse&>(),
            std::declval<ruvia::detail::HttpBufferedResponseWritePlan>())),
    ruvia::detail::Http2BufferedResponseHeadSubmitResult>);
static_assert(!AcceptsUnpreparedBufferedResponseHead<
    ruvia::detail::Http2Connection>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http2Connection&>()
        .finishResponse(
            std::uint32_t{},
            std::declval<const ruvia::detail::HttpResponseTrailerSection&>())),
    ruvia::detail::Http2FinishSubmitStatus>);
static_assert(!AcceptsStagedResponseTrailerSection<
    ruvia::detail::Http2Connection>);
static_assert(!AcceptsImplicitResponseFinish<
    ruvia::detail::Http2Connection>);
static_assert(!AcceptsRawResponseTrailerFinish<
    ruvia::detail::Http2Connection>);
static_assert(HasResponseTrailerSectionAlternatives<
    ruvia::detail::HttpResponseTrailerSectionResult>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpResponseTrailerSection>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpResponseTrailerSectionFailure>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpResponseTrailerSectionResult>);
static_assert(!HasStagedResponseTrailerBlock<
    ruvia::detail::Http2StreamState>);
static_assert(!HasStagedResponseTrailers<
    ruvia::detail::Http2StreamHeaderBlocks>);
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
using ResponseStreamCommitPlanner =
    ruvia::detail::ResponseStreamCommitPlan (*)(
        ruvia::detail::ResponseStreamFraming,
        ruvia::HttpKnownMethod,
        std::uint16_t,
        ruvia::detail::ResponseTrailerIntent) noexcept;
using ResponseStreamHeadPreparer =
    ruvia::detail::ResponseStreamHead (*)(
        ruvia::HttpResponse,
        ruvia::detail::ResponseStreamKind,
        ruvia::detail::ResponseStreamCommitPlan);
using BufferedResponseWritePlanner =
    ruvia::detail::HttpBufferedResponseWritePlan (*)(
        ruvia::HttpKnownMethod,
        const ruvia::HttpResponse&) noexcept;
static_assert(std::same_as<
    decltype(&ruvia::detail::httpBufferedResponseWritePlan),
    BufferedResponseWritePlanner>);
static_assert(std::same_as<
    decltype(&ruvia::detail::httpResponseStreamCommitPlan),
    ResponseStreamCommitPlanner>);
static_assert(std::same_as<
    decltype(&ruvia::detail::prepareResponseStreamHead),
    ResponseStreamHeadPreparer>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::ResponseStreamCommitPlan&>()
                 .responseStatus()),
    std::uint16_t>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::ResponseStreamCommitPlan&>()
                 .framing()),
    ruvia::detail::ResponseStreamFraming>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::HttpResponseBodyPlan&>()
                 .responseStatus()),
    std::uint16_t>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::HttpBufferedResponseWritePlan&>()
                 .responseStatus()),
    std::uint16_t>);
static_assert(!AcceptsLooseResponseStreamBodyPlan<
    ruvia::detail::HttpResponseBodyPlan>);
static_assert(!AcceptsLooseBufferedResponseBodyPlan<
    ruvia::detail::HttpResponseBodyPlan>);
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
using WebSocketServerNegotiator =
    ruvia::detail::WebSocketServerNegotiation (*)(
        const ruvia::HttpRequest&,
        std::string_view) noexcept;
using HttpWebSocketServerHandshakeFactory =
    ruvia::detail::HttpWebSocketServerHandshake (*)(
        const ruvia::HttpRequest&,
        std::string_view) noexcept;
static_assert(std::same_as<
    decltype(&ruvia::detail::makeWebSocketServerNegotiation),
    WebSocketServerNegotiator>);
static_assert(std::same_as<
    decltype(&ruvia::detail::makeHttpWebSocketServerHandshake),
    HttpWebSocketServerHandshakeFactory>);
static_assert(std::is_enum_v<
    ruvia::detail::WebSocketDeflateNegotiation>);
static_assert(!std::constructible_from<
    ruvia::detail::WebSocketDeflateNegotiation,
    bool>);
static_assert(!HasLooseWebSocketDeflateFields<
    ruvia::detail::WebSocketDeflateNegotiation>);
static_assert(!std::default_initializable<
    ruvia::detail::WebSocketServerNegotiation>);
static_assert(!std::constructible_from<
    ruvia::detail::WebSocketServerNegotiation,
    std::string_view,
    ruvia::detail::WebSocketDeflateNegotiation>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        WebSocketServerNegotiation&>().subprotocol()),
    std::string_view>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        WebSocketServerNegotiation&>().deflate()),
    ruvia::detail::WebSocketDeflateNegotiation>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        WebSocketServerNegotiation&>().extensions()),
    std::string_view>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpWebSocketServerHandshake>);
static_assert(HasWebSocketNegotiationAccessor<
    ruvia::detail::HttpWebSocketServerHandshake>);
static_assert(!HasLooseWebSocketNegotiationFields<
    ruvia::detail::HttpWebSocketServerHandshake>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2WebSocketHandshakeSubmitResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2WebSocketHandshakeSubmitResult&>().submitted()),
    const ruvia::detail::Http2SubmittedWebSocketHandshake*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2WebSocketHandshakeSubmitResult&>().failure()),
    const ruvia::detail::Http2WebSocketHandshakeSubmitFailure*>);
static_assert(HasWebSocketNegotiationAccessor<
    ruvia::detail::Http2SubmittedWebSocketHandshake>);
static_assert(!HasWebSocketHandshakeErrorAccessor<
    ruvia::detail::Http2SubmittedWebSocketHandshake>);
static_assert(HasWebSocketHandshakeErrorAccessor<
    ruvia::detail::Http2WebSocketHandshakeSubmitFailure>);
static_assert(!HasWebSocketNegotiationAccessor<
    ruvia::detail::Http2WebSocketHandshakeSubmitFailure>);
static_assert(!std::constructible_from<
    ruvia::detail::Http2SubmittedWebSocketHandshake,
    ruvia::detail::WebSocketServerNegotiation>);
static_assert(!std::constructible_from<
    ruvia::detail::Http2WebSocketHandshakeSubmitFailure,
    ruvia::detail::Http2WebSocketHandshakeSubmitError>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http2Connection&>()
        .submitWebSocketHandshake(
            std::uint32_t{},
            std::declval<ruvia::detail::WebSocketServerNegotiation>())),
    ruvia::detail::Http2WebSocketHandshakeSubmitResult>);
static_assert(!AcceptsLooseWebSocketHandshakeSubmit<
    ruvia::detail::Http2Connection>);
static_assert(!std::constructible_from<
    ruvia::detail::WsConnection,
    std::pmr::string&,
    std::size_t,
    bool>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::WsConnection&>().poll()),
    std::optional<ruvia::detail::WsEvent>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::WsConnection&>().submitFrame(
        ruvia::WebSocketOpcode::kText,
        std::string_view{})),
    ruvia::detail::WsFrameSubmitStatus>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::WsConnection&>().submitClose(
        std::uint16_t{},
        std::string_view{})),
    ruvia::detail::WsCloseSubmitStatus>);
static_assert(!HasWsSubmitMessageAlias<ruvia::detail::WsConnection>);
static_assert(!HasWsSubmitPingAlias<ruvia::detail::WsConnection>);
static_assert(!HasWsSubmitPongAlias<ruvia::detail::WsConnection>);
static_assert(!HasWsApplicationFrameStateSideChannel<
    ruvia::detail::WsConnection>);
static_assert(!HasWsEndsTransportAlias<ruvia::detail::WsOutputPlan>);
static_assert(!HasWsTransportEndPendingSideChannel<
    ruvia::detail::WsConnection>);
static_assert(!HasWsClosedStateSideChannel<ruvia::detail::WsConnection>);
static_assert(!HasWsClosePhaseSideChannel<ruvia::detail::WsConnection>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::WsConnection&>()
        .livenessMode()),
    ruvia::detail::WsLivenessMode>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::WsConnection&>().abort()),
    ruvia::detail::WsAbortDisposition>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::WsOutputPlan&>()
        .disposition()),
    ruvia::detail::WsTransportDisposition>);
static_assert(std::same_as<
    decltype(ruvia::detail::encodeWebSocketClosePayload(
        std::uint16_t{},
        std::string_view{})),
    ruvia::detail::WebSocketClosePayloadEncodeResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        WebSocketClosePayloadEncodeResult&>().encoded()),
    const ruvia::detail::WebSocketEncodedClosePayload*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        WebSocketClosePayloadEncodeResult&>().failure()),
    const ruvia::detail::WebSocketClosePayloadEncodeFailure*>);
static_assert(!ExposesRvalueEncodedContent<
    ruvia::detail::WebSocketClosePayloadEncodeResult>);
static_assert(!ExposesRvalueEncodeFailure<
    ruvia::detail::WebSocketClosePayloadEncodeResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        WebSocketEncodedClosePayload&>().bytes()),
    std::string_view>);
static_assert(!std::default_initializable<
    ruvia::detail::WebSocketClosePayloadEncodeResult>);
static_assert(!std::default_initializable<ruvia::detail::WsEvent>);
static_assert(!ExposesAnyRvalueSansIoEventBorrow<ruvia::detail::WsEvent>);
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
static_assert(!ExposesAnyRvalueWebSocketFrameReadAccessor<
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
static_assert(!ExposesAnyRvalueWebSocketInboundAccessor<
    ruvia::detail::WebSocketInboundResult>);
static_assert(HasWsProtocolFailure<
    ruvia::detail::WebSocketInboundFailure>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http1ChunkedBodyDecoder&>().decode(
        std::string_view{})),
    ruvia::detail::Http1ChunkDecodeResult>);
static_assert(std::default_initializable<ruvia::ProtocolByteLimit>);
static_assert(!std::constructible_from<ruvia::ProtocolByteLimit, std::size_t>);
static_assert(std::same_as<
    decltype(ruvia::ProtocolByteLimit::limited(std::size_t{1})),
    ruvia::ProtocolByteLimit>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ProtocolByteLimit&>().maximum()),
    std::optional<std::size_t>>);
static_assert(!std::default_initializable<ruvia::detail::Http1ChunkDecodeResult>);
static_assert(!HasAnyRvalueHttp1ChunkDecodeAccessor<
    ruvia::detail::Http1ChunkDecodeResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http1ChunkDecodeResult&>()
        .bodyChunk()),
    const ruvia::detail::Http1ChunkDecodeBodyChunk*>);
static_assert(HasConsumedBytes<ruvia::detail::Http1ChunkDecodeNeedMore>);
static_assert(HasConsumedBytes<ruvia::detail::Http1ChunkDecodeBodyChunk>);
static_assert(HasConsumedBytes<ruvia::detail::Http1ChunkDecodeComplete>);
static_assert(HasConsumedBytes<ruvia::detail::Http1ChunkDecodeFailure>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http1ChunkDecodeResult&>()
        .failure()),
    const ruvia::detail::Http1ChunkDecodeFailure*>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::TransferCodingDecoder&>().decode(
        std::string_view{}, std::span<char>{})),
    ruvia::detail::TransferCodingDecodeResult>);
static_assert(!std::default_initializable<
    ruvia::detail::TransferCodingDecodeResult>);
static_assert(!HasAnyRvalueTransferCodingDecodeAccessor<
    ruvia::detail::TransferCodingDecodeResult>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::TransferCodingDecoder&>()
        .finishInput()),
    ruvia::detail::TransferCodingDecodeResult>);
static_assert(HasConsumedBytes<
    ruvia::detail::TransferCodingDecodeNeedInput>);
static_assert(HasConsumedBytes<
    ruvia::detail::TransferCodingDecodeOutput>);
static_assert(HasConsumedBytes<
    ruvia::detail::TransferCodingDecodeComplete>);
static_assert(HasConsumedBytes<
    ruvia::detail::TransferCodingDecodeFailure>);
static_assert(HasTransferOutputBytes<
    ruvia::detail::TransferCodingDecodeOutput>);
static_assert(!HasTransferOutputBytes<
    ruvia::detail::TransferCodingDecodeFailure>);
static_assert(HasTransferDecodeError<
    ruvia::detail::TransferCodingDecodeFailure>);
static_assert(std::same_as<
    decltype(ruvia::detail::scanHttpChunkedBody(std::string_view{})),
    ruvia::detail::HttpChunkScanResult>);
static_assert(!std::default_initializable<ruvia::detail::HttpChunkScanResult>);
static_assert(!HasAnyRvalueHttpChunkScanAccessor<
    ruvia::detail::HttpChunkScanResult>);
static_assert(!HasConsumedBytes<ruvia::detail::HttpChunkScanNeedMore>);
static_assert(HasConsumedBytes<ruvia::detail::HttpChunkScanComplete>);
static_assert(!HasConsumedBytes<ruvia::detail::HttpChunkScanFailure>);
static_assert(!HasChunkScanError<ruvia::detail::HttpChunkScanComplete>);
static_assert(HasChunkScanError<ruvia::detail::HttpChunkScanFailure>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::MultipartParser&>().poll()),
    ruvia::MultipartPollResult>);
static_assert(!std::is_copy_constructible_v<ruvia::MultipartParser>);
static_assert(!std::is_copy_assignable_v<ruvia::MultipartParser>);
static_assert(!std::is_move_constructible_v<ruvia::MultipartParser>);
static_assert(!std::is_move_assignable_v<ruvia::MultipartParser>);
static_assert(!std::default_initializable<ruvia::MultipartPollResult>);
static_assert(!HasMultipartStatus<ruvia::MultipartPollResult>);
static_assert(!HasAnyRvalueMultipartPollAccessor<ruvia::MultipartPollResult>);
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
static_assert(std::same_as<
    decltype(ruvia::parseMultipartBody(
        std::string_view{}, ruvia::MultipartBoundary("x"))),
    ruvia::MultipartBodyParseResult>);
static_assert(!std::default_initializable<ruvia::MultipartBodyParseResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::MultipartBodyParseResult&>().body()),
    const ruvia::MultipartBody*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::MultipartBodyParseResult&>().failure()),
    const ruvia::MultipartBodyParseFailure*>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpMultipartDelimiterResult>);
static_assert(!HasMultipartStatus<
    ruvia::detail::HttpMultipartDelimiterResult>);
static_assert(!HasAnyRvalueMultipartDelimiterAccessor<
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
static_assert(!HasAnyRvalueMultipartBoundaryAccessor<
    ruvia::detail::HttpMultipartBoundaryParseResult>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpMultipartPartHeaderParseResult>);
static_assert(!HasAnyRvalueMultipartPartHeaderAccessor<
    ruvia::detail::HttpMultipartPartHeaderParseResult>);
static_assert(std::same_as<
    decltype(ruvia::lookupUniqueHttpClientResponseHeader(
        std::declval<const ruvia::HttpClientResponse&>(),
        std::string_view{})),
    ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!std::default_initializable<
    ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(std::move_constructible<ruvia::HttpClientResponse>);
static_assert(!std::assignable_from<
    ruvia::HttpClientResponse&, ruvia::HttpClientResponse&&>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::HttpClientResponseHeader>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::HttpClientResponse>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::Http1ClientChunkedResponse>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!AcceptsTemporaryHttpClientResponseHeaderLookup<
    ruvia::HttpClientResponse>);
static_assert(std::move_constructible<ruvia::Http1ParsedClientResponseHead>);
static_assert(!std::assignable_from<
    ruvia::Http1ParsedClientResponseHead&,
    ruvia::Http1ParsedClientResponseHead&&>);
static_assert(std::move_constructible<ruvia::Http1ClientResponseParseResult>);
static_assert(!std::assignable_from<
    ruvia::Http1ClientResponseParseResult&,
    ruvia::Http1ClientResponseParseResult&&>);
static_assert(!HasAnyRvalueHttp1RequestParseAccessor<
    ruvia::Http1RequestParseResult>);
static_assert(!HasAnyRvalueHttp1ClientResponseParseAccessor<
    ruvia::Http1ClientResponseParseResult>);
static_assert(!HasAnyRvalueHttp1ClientRequestPrepareAccessor<
    ruvia::Http1ClientRequestPrepareResult>);
static_assert(!HasAnyRvalueHttp1InterimResponsePrepareAccessor<
    ruvia::Http1InterimResponsePrepareResult>);
static_assert(!HasAnyRvalueHttpClientRequestContentAccessor<
    ruvia::HttpClientRequestContent>);
static_assert(!HasAnyRvalueHttp1ClientRequestContentPlanAccessor<
    ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasAnyRvalueHttp1ClientRequestWirePolicyAccessor<
    ruvia::Http1ClientRequestWirePolicy>);
static_assert(!HasAnyRvalueHttp1ClientResponsePlanAccessor<
    ruvia::Http1ClientResponsePlan>);
static_assert(!HasAnyRvalueHttpClientHeaderLookupAccessor<
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
static_assert(std::move_constructible<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!std::assignable_from<
    ruvia::HttpClientRedirectTargetResult&,
    ruvia::HttpClientRedirectTargetResult&&>);
static_assert(!HasAnyRvalueHttpClientRedirectTargetAccessor<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::HttpClientRedirectTarget>);
static_assert(std::move_constructible<ruvia::HttpClientRedirectTarget>);
static_assert(!std::assignable_from<
    ruvia::HttpClientRedirectTarget&,
    ruvia::HttpClientRedirectTarget&&>);
static_assert(!HasHttpClientRedirectStatus<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!HasHttpClientRedirectError<
    ruvia::HttpClientRedirectTarget>);
static_assert(HasHttpClientRedirectError<
    ruvia::HttpClientRedirectTargetFailure>);
static_assert(std::same_as<
    decltype(ruvia::detail::responseBody(
        std::declval<const ruvia::HttpResponse&>())),
    const ruvia::detail::HttpResponseBody&>);
static_assert(!std::copy_constructible<
    ruvia::detail::HttpResponseBody>);
static_assert(std::is_nothrow_move_constructible_v<
    ruvia::detail::HttpResponseBody>);
static_assert(!std::is_move_assignable_v<
    ruvia::detail::HttpResponseBody>);
static_assert(std::is_nothrow_move_constructible_v<
    ruvia::HttpResponse>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpBorrowedResponseBytes>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpStaticResponseBytes>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpOwnedResponseBytes>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpOwnedResponseFile>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpBorrowedResponseFile>);
static_assert(!std::default_initializable<
    ruvia::detail::ResponseFileBody>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpContentCodingFieldResult>);
static_assert(
    ruvia::detail::HttpUnsupportedContentCoding::status() == 415);
static_assert(
    ruvia::detail::httpSupportedRequestContentCodings() ==
    std::string_view("gzip, br, zstd"));
static_assert(!ExposesRvalueContentCoding<
    ruvia::detail::HttpContentCodingFieldResult>);
static_assert(!ExposesRvalueUnsupportedContentCoding<
    ruvia::detail::HttpContentCodingFieldResult>);
static_assert(std::same_as<
    decltype(ruvia::detail::httpContentCodingFromFieldValue(
        std::string_view{})),
    ruvia::detail::HttpContentCodingFieldResult>);
static_assert(std::same_as<
    decltype(ruvia::detail::httpClientResponseContentCoding(
        std::declval<const ruvia::HttpClientResponse&>())),
    ruvia::detail::HttpContentCodingFieldResult>);
static_assert(!HasContentLengthPresent<
    ruvia::detail::HttpContentLengthState>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::HttpContentLengthState&>()
        .value()),
    std::optional<std::size_t>>);
static_assert(!HasStaleTransferEncodingAccessors<
    ruvia::detail::HttpTransferEncodingState>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpTransferEncodingValue>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpNonChunkedTransferEncoding>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpFinalChunkedTransferEncoding>);
static_assert(!ExposesRvalueFinalChunked<
    ruvia::detail::HttpTransferEncodingValue>);
static_assert(!ExposesRvalueNonChunked<
    ruvia::detail::HttpTransferEncodingValue>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::HttpTransferEncodingState&>()
        .value()),
    std::optional<ruvia::detail::HttpTransferEncodingValue>>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpContentDecodeResult>);
static_assert(!std::copy_constructible<
    ruvia::detail::HttpContentDecodeResult>);
static_assert(std::move_constructible<
    ruvia::detail::HttpContentDecodeResult>);
static_assert(!std::is_move_assignable_v<
    ruvia::detail::HttpContentDecodeResult>);
static_assert(!ExposesRvalueDecodedContent<
    ruvia::detail::HttpContentDecodeResult>);
static_assert(!ExposesRvalueDecodeFailure<
    ruvia::detail::HttpContentDecodeResult>);
static_assert(std::same_as<
    decltype(std::declval<
        ruvia::detail::HttpContentDecodeResult&>().decoded()),
    ruvia::detail::HttpDecodedContent*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpContentDecodeResult&>().failure()),
    const ruvia::detail::HttpContentDecodeFailure*>);
static_assert(std::same_as<
    decltype(ruvia::detail::decodeHttpContent(
        ruvia::detail::HttpContentCoding::kGzip,
        std::string_view{},
        std::size_t{},
        std::declval<std::pmr::memory_resource*>())),
    ruvia::detail::HttpContentDecodeResult>);
static_assert(std::same_as<
    decltype(ruvia::detail::decodeHttpClientResponseContentEncoding(
        std::declval<const ruvia::HttpClientResponse&>(),
        std::size_t{},
        std::declval<std::pmr::memory_resource*>())),
    ruvia::detail::HttpContentDecodeResult>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpContentEncodeResult>);
static_assert(!std::copy_constructible<
    ruvia::detail::HttpContentEncodeResult>);
static_assert(std::move_constructible<
    ruvia::detail::HttpContentEncodeResult>);
static_assert(!std::is_move_assignable_v<
    ruvia::detail::HttpContentEncodeResult>);
static_assert(!ExposesRvalueEncodedContent<
    ruvia::detail::HttpContentEncodeResult>);
static_assert(!ExposesRvalueEncodeFailure<
    ruvia::detail::HttpContentEncodeResult>);
static_assert(std::same_as<
    decltype(std::declval<
        ruvia::detail::HttpContentEncodeResult&>().encoded()),
    ruvia::detail::HttpEncodedContent*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpContentEncodeResult&>().failure()),
    const ruvia::detail::HttpContentEncodeFailure*>);
static_assert(std::same_as<
    decltype(ruvia::detail::encodeHttpContent(
        ruvia::detail::HttpContentCoding::kGzip,
        std::string_view{},
        std::size_t{},
        std::declval<std::pmr::memory_resource*>())),
    ruvia::detail::HttpContentEncodeResult>);
static_assert(!AcceptsUrlDecodeOutputParameter<std::pmr::string>);
static_assert(std::same_as<
    decltype(ruvia::detail::decodeUrlComponent(
        std::string_view{},
        ruvia::detail::UrlDecodeMode::kPercent,
        std::declval<std::pmr::memory_resource*>())),
    std::optional<std::pmr::string>>);

int main() {
    ruvia::CookieOptions cookieOptions;
    cookieOptions.sameSite = ruvia::CookieSameSite::kLax;
    cookieOptions.maxAge = std::chrono::seconds(60);
    const ruvia::detail::SetCookiePlan cookiePlan(
        "sid", "value", cookieOptions);
    std::array<char, 128> cookieBuffer{};
    cookiePlan.write(cookieBuffer.data());
    if (std::string_view(cookieBuffer.data(), cookiePlan.size()) !=
        "sid=value; Path=/; Max-Age=60; SameSite=Lax") {
        return 52;
    }

    const auto decodedUrl = ruvia::detail::decodeUrlComponent(
        "installed%20decoder",
        ruvia::detail::UrlDecodeMode::kPercent,
        std::pmr::get_default_resource());
    const auto malformedUrl = ruvia::detail::decodeUrlComponent(
        "prefix%2",
        ruvia::detail::UrlDecodeMode::kPercent,
        std::pmr::get_default_resource());
    if (!decodedUrl.has_value() ||
        std::string_view(*decodedUrl) != "installed decoder" ||
        malformedUrl.has_value()) {
        return 51;
    }
    auto encodedContent = ruvia::detail::encodeHttpContent(
        ruvia::detail::HttpContentCoding::kGzip,
        "installed content encoder",
        1024,
        std::pmr::get_default_resource());
    if (encodedContent.encoded() == nullptr ||
        encodedContent.failure() != nullptr ||
        encodedContent.encoded()->bytes().empty()) {
        return 50;
    }
    const auto identityDecode = ruvia::detail::decodeHttpContent(
        ruvia::detail::HttpContentCoding::kIdentity,
        "identity",
        8,
        std::pmr::get_default_resource());
    if (identityDecode.decoded() == nullptr ||
        identityDecode.failure() != nullptr ||
        identityDecode.decoded()->bytes() != "identity") {
        return 49;
    }
    const ruvia::HttpResponse emptyResponse;
    const auto& emptyBody = ruvia::detail::responseBody(emptyResponse);
    if (emptyBody.empty() == nullptr ||
        emptyBody.borrowedBytes() != nullptr ||
        emptyBody.staticBytes() != nullptr ||
        emptyBody.ownedBytes() != nullptr ||
        emptyBody.ownedFile() != nullptr ||
        emptyBody.borrowedFile() != nullptr ||
        emptyBody.file().has_value() ||
        !emptyBody.bytes().empty()) {
        return 48;
    }
    std::pmr::monotonic_buffer_resource remoteReceiveResource;
    ruvia::detail::Http2StreamState remoteReceiveStream(
        3, &remoteReceiveResource);
    const auto& remoteReceive = remoteReceiveStream.remoteReceive();
    if (remoteReceive.headPending() == nullptr ||
        !remoteReceiveStream.beginStandardConnect() ||
        !remoteReceiveStream.finalizeRemoteConnectHead() ||
        remoteReceive.connectPending() == nullptr ||
        !remoteReceiveStream.rejectConnect() ||
        remoteReceive.connectRejectedAwaitingEndStream() == nullptr ||
        !remoteReceiveStream.finishRemoteRejectedConnect() ||
        remoteReceive.endStream() == nullptr) {
        return 39;
    }
    ruvia::detail::Http2StreamState remotePendingEndStream(
        5, &remoteReceiveResource);
    const auto& pendingEnd = remotePendingEndStream.remoteReceive();
    if (!remotePendingEndStream.beginStandardConnect() ||
        !remotePendingEndStream.finalizeRemoteConnectHead() ||
        !remotePendingEndStream.finishRemotePendingConnect() ||
        pendingEnd.connectPendingEndStream() == nullptr ||
        !remotePendingEndStream.acceptConnect() ||
        pendingEnd.endStream() == nullptr) {
        return 40;
    }

    std::pmr::monotonic_buffer_resource localSendResource;
    ruvia::detail::Http2StreamState localSendStream(1, &localSendResource);
    const auto& localSend = localSendStream.localSend();
    if (localSend.headPending() == nullptr ||
        localSend.responseTrailersOnly() != nullptr ||
        !localSendStream.beginLocalResponseTrailersOnly() ||
        localSend.headPending() != nullptr ||
        localSend.responseTrailersOnly() == nullptr ||
        !localSendStream.queueLocalEndStream() ||
        localSend.endStreamQueued() == nullptr ||
        !localSendStream.commitLocalEndStream() ||
        localSend.endStreamCommitted() == nullptr ||
        !localSendStream.abort(ruvia::detail::Http2StreamCloseSource::kPeer) ||
        localSend.aborted() == nullptr ||
        localSend.aborted()->source() !=
            ruvia::detail::Http2StreamCloseSource::kPeer ||
        localSendStream.abort(static_cast<
            ruvia::detail::Http2StreamCloseSource>(0xFF))) {
        return 38;
    }

    ruvia::detail::Http2TunnelState tunnel;
    if (tunnel.notConnect() == nullptr || tunnel.pending() != nullptr ||
        !tunnel.begin(ruvia::detail::Http2ConnectForm::kExtended) ||
        tunnel.notConnect() != nullptr || tunnel.pending() == nullptr ||
        tunnel.pending()->form() !=
            ruvia::detail::Http2ConnectForm::kExtended ||
        !tunnel.accept() || tunnel.pending() != nullptr ||
        tunnel.open() == nullptr || tunnel.rejected() != nullptr) {
        return 37;
    }

    ruvia::detail::Http2RemoteContentState remoteContent;
    if (remoteContent.allowedWithoutLength() == nullptr ||
        remoteContent.allowedKnownLength() != nullptr ||
        remoteContent.allowedWithoutLength()->receivedBytes() != 0 ||
        !remoteContent.declareKnownLength(3) ||
        remoteContent.allowedKnownLength() == nullptr ||
        remoteContent.allowedKnownLength()->declaredLength() != 3 ||
        remoteContent.account(2) !=
            ruvia::detail::Http2RemoteContentAccountingResult::kAccepted) {
        return 36;
    }
    if (remoteContent.terminalLengthValid() ||
        remoteContent.account(2) !=
            ruvia::detail::Http2RemoteContentAccountingResult::
                kDeclaredLengthExceeded ||
        remoteContent.allowedKnownLength()->receivedBytes() != 2) {
        return 36;
    }
    ruvia::detail::Http2RemoteContentState metadataOnly;
    if (!metadataOnly.declareKnownLength(9) ||
        !metadataOnly.selectMetadataOnly() ||
        metadataOnly.metadataOnlyKnownLength() == nullptr ||
        metadataOnly.metadataOnlyKnownLength()->declaredLength() != 9 ||
        metadataOnly.account(1) !=
            ruvia::detail::Http2RemoteContentAccountingResult::
                kContentForbidden) {
        return 36;
    }

    const auto headSemantics = ruvia::detail::httpResponseContentSemantics(
        ruvia::HttpKnownMethod::kHead, 200);
    const auto tunnelSemantics = ruvia::detail::httpResponseContentSemantics(
        ruvia::HttpKnownMethod::kConnect, 200);
    if (headSemantics.withoutContent() == nullptr ||
        headSemantics.withContent() != nullptr ||
        tunnelSemantics.connectTunnel() == nullptr) {
        return 36;
    }

    const auto outboundOrigin = ruvia::HttpOrigin::https("example.test");
    ruvia::HttpClientRequest outboundRequest;
    outboundRequest.method = "POST";
    outboundRequest.target = "/submit";
    outboundRequest.content = ruvia::HttpClientRequestContent::bytes("payload");
    const auto* outboundBytes = outboundRequest.content.borrowedBytes();
    if (outboundRequest.content.withoutContent() != nullptr ||
        outboundBytes == nullptr || outboundBytes->value() != "payload") {
        return 34;
    }
    std::array<char, 512> outboundHeadBuffer;
    const auto outboundPrepared = ruvia::Http1ClientRequestWriter().prepare(
        outboundOrigin, outboundRequest, outboundHeadBuffer);
    const auto* outboundWire = outboundPrepared.prepared();
    const auto* outboundImmediate = outboundWire == nullptr
        ? nullptr
        : outboundWire->contentPlan().immediate();
    if (outboundOrigin.scheme() != ruvia::HttpScheme::kHttps ||
        outboundOrigin.host() != "example.test" || outboundOrigin.port() != 443 ||
        outboundRequest.target != "/submit" || outboundWire == nullptr ||
        outboundWire->head().find("Content-Length: 7\r\n") ==
            std::string_view::npos ||
        outboundImmediate == nullptr ||
        outboundImmediate->bytes() != "payload") {
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
        expectWire->contentPlan().continueGated() == nullptr ||
        expectWire->head().find("Expect: 100-continue\r\n") ==
            std::string_view::npos) {
        return 25;
    }
    ruvia::Http1ClientResponseParser expectParser(*expectWire);
    const auto continueResult = expectParser.parse(
        "HTTP/1.1 100 Continue\r\n\r\n");
    const auto* continueHead = continueResult.parsed();
    if (continueHead == nullptr ||
        continueHead->plan().informational() == nullptr ||
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
        expectFinalHead->plan().withoutContent() == nullptr ||
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
        wsInput,
        wsOffset,
        wsPendingCompactUntil,
        ruvia::ProtocolByteLimit::limited(1024),
        false);
    if (wsNeedInput.needInput() == nullptr ||
        wsNeedInput.frame() != nullptr ||
        wsNeedInput.failure() != nullptr) {
        return 31;
    }
    const auto multipart = ruvia::parseMultipartBody(
        "--x--\r\n", ruvia::MultipartBoundary("x"));
    if (multipart.failure() != nullptr || multipart.body() == nullptr ||
        !multipart.body()->parts().empty()) {
        return 3;
    }
    ruvia::MultipartParser multipartParser(
        ruvia::MultipartBoundary("x"),
        std::pmr::get_default_resource());
    multipartParser.feed("--x--");
    const auto multipartNeedInput = multipartParser.poll();
    if (multipartNeedInput.needInput() == nullptr) {
        return 18;
    }
    multipartParser.finishInput();
    const auto multipartDone = multipartParser.poll();
    if (multipartDone.done() == nullptr) {
        return 19;
    }
    ruvia::MultipartParser failedMultipartParser(
        ruvia::MultipartBoundary("x"),
        std::pmr::get_default_resource());
    failedMultipartParser.feed(std::string(64 * 1024 + 1, 'p'));
    const auto multipartFailure = failedMultipartParser.poll();
    const auto repeatedMultipartFailure = failedMultipartParser.poll();
    if (multipartFailure.failure() == nullptr ||
        repeatedMultipartFailure.failure() == nullptr ||
        multipartFailure.failure()->error() !=
            ruvia::MultipartParseError::kPreambleTooLarge ||
        repeatedMultipartFailure.failure()->error() !=
            multipartFailure.failure()->error()) {
        return 72;
    }
    try {
        failedMultipartParser.feed("--x--");
        return 73;
    } catch (const std::logic_error&) {
    }
    constexpr std::string_view chunkedRequest =
        "POST / HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "1\r\nx\r\n0\r\n\r\n";
    const auto parseResult = ruvia::Http1RequestParser().parse(chunkedRequest);
    const auto* parsed = parseResult.parsed();
    if (parsed == nullptr || parsed->bodyPlan().chunked() == nullptr ||
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
    const auto* transferCodedBody = transferCoded.bodyPlan.chunked();
    if (!transferCoded.messageReady() ||
        transferCodedBody == nullptr ||
        transferCodedBody->transferCodings().count != 1 ||
        transferCodedBody->transferCodings().values[0] !=
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
    ruvia::Http1ClientResponseParser knownLengthParser(*getWire);
    const auto knownLengthResult = knownLengthParser.parse(
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
    const auto* knownLengthHead = knownLengthResult.parsed();
    const auto* knownLength = knownLengthHead == nullptr
        ? nullptr
        : knownLengthHead->plan().knownLength();
    if (knownLength == nullptr || knownLength->contentLength() != 3 ||
        knownLength->persistence() !=
            ruvia::Http1ClientResponsePersistence::kReuse) {
        return 15;
    }
    ruvia::Http1ClientResponseParser chunkedParser(*getWire);
    const auto chunkedResult = chunkedParser.parse(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n");
    const auto* chunkedHead = chunkedResult.parsed();
    const auto* chunked = chunkedHead == nullptr
        ? nullptr
        : chunkedHead->plan().chunked();
    if (chunked == nullptr || chunked->transferCodings().count != 1 ||
        chunked->transferCodings().values[0] !=
            ruvia::detail::HttpTransferCoding::kGzip ||
        chunked->persistence() !=
            ruvia::Http1ClientResponsePersistence::kReuse) {
        return 15;
    }
    ruvia::Http1ClientResponseParser clientParser(*getWire);
    const auto clientResult = clientParser.parse(closeDelimitedHead);
    const auto* clientHead = clientResult.parsed();
    if (clientHead == nullptr ||
        clientHead->plan().closeDelimited() == nullptr ||
        clientHead->consumedBytes() != closeDelimitedHead.size()) {
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
    if (tunnelHead == nullptr ||
        tunnelHead->plan().connectTunnel() == nullptr) {
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
    if (upgradeHead == nullptr ||
        upgradeHead->plan().protocolUpgrade() == nullptr) {
        return 24;
    }

    const auto streamPlan = ruvia::detail::http1PlanResponseStream(
        serverParser.parseMessage("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n"),
        ruvia::detail::Http1ServerClosePolicy::kAllowReuse);
    if (streamPlan.framing() != ruvia::detail::ResponseStreamFraming::kHttp1Chunked ||
        streamPlan.requestConnectionPlan().disposition() !=
            ruvia::detail::Http1ConnectionDisposition::kReuse ||
        streamPlan.requestConnectionPlan().protocolVersion() !=
            ruvia::HttpProtocolVersion::kHttp11 ||
        streamPlan.closePolicy() != ruvia::detail::Http1ServerClosePolicy::kAllowReuse) {
        return 4;
    }

    ruvia::HttpResponse streamResponse;
    streamResponse.header("Connection", "close");
    const auto preparedStreamResult =
        ruvia::detail::prepareHttp1ResponseStreamHead(
        std::move(streamResponse),
        ruvia::detail::ResponseStreamKind::kGeneric,
        streamPlan,
        ruvia::detail::ResponseTrailerIntent::kNone);
    const auto* preparedStream = preparedStreamResult.prepared();
    if (preparedStream == nullptr || preparedStreamResult.failure() != nullptr ||
        preparedStream->connectionPlan().disposition() !=
            ruvia::detail::Http1ConnectionDisposition::kClose ||
        preparedStream->response().header("Connection") != "close" ||
        preparedStream->responseHeadPlan().chunkedStream() == nullptr ||
        preparedStream->responseHeadPlan().buffered() != nullptr ||
        preparedStream->responseHeadPlan().closeDelimitedStream() != nullptr) {
        return 5;
    }

    const auto http10Plan = ruvia::detail::http1PlanResponseStream(
        serverParser.parseMessage(
            "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"),
        ruvia::detail::Http1ServerClosePolicy::kAllowReuse);
    ruvia::HttpResponse resetContentStream;
    resetContentStream.status(205);
    const auto preparedHttp10Result =
        ruvia::detail::prepareHttp1ResponseStreamHead(
        std::move(resetContentStream),
        ruvia::detail::ResponseStreamKind::kGeneric,
        http10Plan,
        ruvia::detail::ResponseTrailerIntent::kNone);
    const auto* preparedHttp10 = preparedHttp10Result.prepared();
    if (preparedHttp10 == nullptr || preparedHttp10Result.failure() != nullptr ||
        !preparedHttp10->commitPlan().bodyPlan().bodySuppressed() ||
        preparedHttp10->commitPlan().headDisposition() !=
            ruvia::detail::ResponseStreamHeadDisposition::kMessageEnded ||
        preparedHttp10->responseHeadPlan().closeDelimitedStream() == nullptr ||
        preparedHttp10->responseHeadPlan().protocolVersion() !=
            ruvia::HttpProtocolVersion::kHttp10 ||
        preparedHttp10->connectionPlan().disposition() !=
            ruvia::detail::Http1ConnectionDisposition::kReuse ||
        preparedHttp10->response().header("Connection") != "keep-alive") {
        return 13;
    }

    ruvia::HttpResponse response;
    response.body("body");
    const auto writePlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpKnownMethod::kHead, response);
    if (writePlan.requestMethod() != ruvia::HttpKnownMethod::kHead ||
        writePlan.bodyPlan().requestMethod() !=
            ruvia::HttpKnownMethod::kHead ||
        !writePlan.matchesResponse(response) ||
        !writePlan.bodySuppressed() || writePlan.sendBody() ||
        writePlan.contentLength() != 4) {
        return 6;
    }
    const auto bufferedResponsePlan =
        ruvia::detail::http1BufferedResponsePlan(
            writePlan,
            streamPlan.requestConnectionPlan());
    const auto& bufferedHeadPlan = bufferedResponsePlan.headPlan();
    if (bufferedHeadPlan.buffered() == nullptr ||
        bufferedHeadPlan.buffered()->contentLength() != 4 ||
        bufferedResponsePlan.writePlan().contentLength() != 4 ||
        bufferedHeadPlan.protocolVersion() !=
            ruvia::HttpProtocolVersion::kHttp11 ||
        bufferedHeadPlan.chunkedStream() != nullptr ||
        bufferedHeadPlan.closeDelimitedStream() != nullptr) {
        return 6;
    }

    ruvia::HttpResponse http1ControlResponse;
    http1ControlResponse.header("Connection", "Upgrade");
    http1ControlResponse.header("Upgrade", "websocket");
    const auto http1ControlResult =
        ruvia::detail::httpFinalResponseControlPlan(
            http1ControlResponse,
            ruvia::HttpProtocolVersion::kHttp11);
    const auto* http1ControlPlan = http1ControlResult.plan();
    const auto* http1Control = http1ControlPlan == nullptr
        ? nullptr
        : http1ControlPlan->http1();
    if (http1Control == nullptr || http1ControlResult.failure() != nullptr ||
        !http1Control->connectionOptions().upgrade() ||
        !http1Control->upgradeProtocols().hasProtocol() ||
        http1ControlPlan->http2() != nullptr) {
        return 45;
    }

    const auto http2ControlResult =
        ruvia::detail::httpFinalResponseControlPlan(
            response,
            ruvia::HttpProtocolVersion::kHttp2);
    if (http2ControlResult.plan() == nullptr ||
        http2ControlResult.plan()->http1() != nullptr ||
        http2ControlResult.plan()->http2() == nullptr ||
        http2ControlResult.failure() != nullptr) {
        return 46;
    }

    ruvia::HttpResponse forbiddenHttp2Control;
    forbiddenHttp2Control.header("Connection", "close");
    const auto forbiddenHttp2ControlResult =
        ruvia::detail::httpFinalResponseControlPlan(
            forbiddenHttp2Control,
            ruvia::HttpProtocolVersion::kHttp2);
    if (forbiddenHttp2ControlResult.plan() != nullptr ||
        forbiddenHttp2ControlResult.failure() == nullptr ||
        forbiddenHttp2ControlResult.failure()->error() !=
            ruvia::detail::HttpFinalResponseControlPlanError::
                kConnectionSpecificFieldForbidden) {
        return 47;
    }

    const auto h2BufferedHeadResult =
        ruvia::detail::http2BufferedResponseHeadPlan(writePlan, response);
    const auto* h2BufferedHead = h2BufferedHeadResult.plan();
    if (h2BufferedHead == nullptr ||
        h2BufferedHeadResult.failure() != nullptr ||
        h2BufferedHead->canonicalContentLength() == nullptr ||
        h2BufferedHead->canonicalContentLength()->value() != 4 ||
        h2BufferedHead->explicitContentLength() != nullptr ||
        h2BufferedHead->absentContentLength() != nullptr ||
        h2BufferedHead->forbiddenContentLength() != nullptr) {
        return 42;
    }

    ruvia::HttpResponse h2StreamingResponse;
    h2StreamingResponse.header("Content-Length", "0004");
    const auto h2StreamingBodyPlan = ruvia::detail::httpResponseBodyPlan(
        ruvia::HttpKnownMethod::kGet,
        h2StreamingResponse.status());
    const auto h2StreamingHeadResult =
        ruvia::detail::http2StreamingResponseHeadPlan(
            h2StreamingBodyPlan,
            h2StreamingResponse);
    const auto* h2StreamingHead = h2StreamingHeadResult.plan();
    if (h2StreamingHead == nullptr ||
        h2StreamingHeadResult.failure() != nullptr ||
        h2StreamingHead->canonicalContentLength() != nullptr ||
        h2StreamingHead->explicitContentLength() == nullptr ||
        h2StreamingHead->explicitContentLength()->value() != 4 ||
        h2StreamingHead->absentContentLength() != nullptr ||
        h2StreamingHead->forbiddenContentLength() != nullptr) {
        return 43;
    }

    h2StreamingResponse.header("Content-Length", "invalid");
    const auto invalidH2StreamingHead =
        ruvia::detail::http2StreamingResponseHeadPlan(
            h2StreamingBodyPlan,
            h2StreamingResponse);
    if (invalidH2StreamingHead.plan() != nullptr ||
        invalidH2StreamingHead.failure() == nullptr ||
        invalidH2StreamingHead.failure()->error() !=
            ruvia::detail::Http2ResponseHeadPlanError::kInvalidContentLength) {
        return 44;
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

    const auto installedResolvedRange = ruvia::detail::resolveHttpByteRange(
        "Bytes=10-19", 100);
    const auto installedIgnoredRange = ruvia::detail::resolveHttpByteRange(
        "items=10-19", 100);
    const auto installedUnsatisfiableRange =
        ruvia::detail::resolveHttpByteRange("bytes=100-", 100);
    if (installedResolvedRange.resolved() == nullptr ||
        installedResolvedRange.resolved()->offset() != 10 ||
        installedResolvedRange.resolved()->length() != 10 ||
        installedResolvedRange.ignored() != nullptr ||
        installedResolvedRange.unsatisfiable() != nullptr ||
        installedIgnoredRange.ignored() == nullptr ||
        installedIgnoredRange.resolved() != nullptr ||
        installedUnsatisfiableRange.unsatisfiable() == nullptr ||
        installedUnsatisfiableRange.resolved() != nullptr) {
        return 33;
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
    const auto h2WithoutContent = ruvia::detail::Http2RequestContent::none();
    const auto h2ZeroLength =
        ruvia::detail::Http2RequestContent::knownLength(0);
    const auto h2Streaming = ruvia::detail::Http2RequestContent::streaming();
    if (h2WithoutContent.withoutContent() == nullptr ||
        h2WithoutContent.knownLengthContent() != nullptr ||
        h2ZeroLength.knownLengthContent() == nullptr ||
        h2ZeroLength.knownLengthContent()->length() != 0 ||
        h2Streaming.streamingContent() == nullptr) {
        return 35;
    }
    const auto request = h2.submitRegularRequestHead(
        "PROPFIND",
        "https",
        "example.test",
        "/",
        {},
        h2WithoutContent);
    const auto* submittedRequest = request.submitted();
    if (submittedRequest == nullptr || request.failure() != nullptr) {
        return 7;
    }
    const auto streamId = submittedRequest->streamId();
    const auto* extensionStream = h2.stream(streamId);
    if (extensionStream == nullptr ||
        extensionStream->requestMethod() != "PROPFIND" ||
        extensionStream->requestKnownMethod() != ruvia::HttpKnownMethod::kUnknown ||
        extensionStream->localContent().forbidden() == nullptr ||
        extensionStream->localContent().knownLength() != nullptr ||
        extensionStream->localContent().acceptedBytes() != 0) {
        return 23;
    }
    if (h2.submitData(
            streamId, "forbidden", ruvia::detail::Http2EndStream::kEndStream) !=
        ruvia::detail::Http2DataSubmitStatus::kInvalidState) {
        return 8;
    }

    const auto connect = h2.submitConnectRequestHead("example.test:443");
    const auto* submittedConnect = connect.submitted();
    const auto* connectStream = submittedConnect == nullptr
        ? nullptr
        : h2.stream(submittedConnect->streamId());
    const auto* pendingConnect = connectStream == nullptr
        ? nullptr
        : connectStream->tunnel().pending();
    if (submittedConnect == nullptr || connect.failure() != nullptr ||
        pendingConnect == nullptr ||
        connectStream->localSend().connectPending() == nullptr ||
        connectStream->localSend().tunnelOpen() != nullptr ||
        pendingConnect->form() !=
            ruvia::detail::Http2ConnectForm::kStandard ||
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
