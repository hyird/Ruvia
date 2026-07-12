#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>

#include <ruvia/web/App.h>
#include <ruvia/web/AppHook.h>
#include <ruvia/web/ConnInfo.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/Error.h>
#include <ruvia/web/HttpServerOptions.h>
#include <ruvia/web/MiddlewareRuntime.h>
#include <ruvia/web/Model.h>
#include <ruvia/web/RouteModes.h>
#include <ruvia/web/Streaming.h>
#include <ruvia/web/WebSocket.h>
#include <ruvia/web/detail/ContextValues.h>
#include <ruvia/web/detail/StaticFilesInternal.h>
#include <ruvia/web/detail/ValidatedValues.h>
#include <ruvia/web/detail/http/ContextCapabilities.h>
#include <ruvia/web/detail/http/ContextServices.h>
#include <ruvia/web/detail/http2/Http2SansIoStreamRuntime.h>
#include <ruvia/web/detail/json/JsonString.h>
#include <ruvia/web/detail/model/Parser.h>
#include <ruvia/web/detail/router/RouteTable.h>
#include <ruvia/web/detail/router/RouteStreamResult.h>
#include <ruvia/web/detail/server/Http2SansIoSession.h>
#include <ruvia/web/detail/server/Http2BufferedResponseDispatch.h>
#include <ruvia/web/detail/server/Http1BufferedResponseWrite.h>
#include <ruvia/web/detail/server/HttpFileFallback.h>
#include <ruvia/web/detail/server/HttpFileZeroCopy.h>
#include <ruvia/web/detail/server/Http1RequestSequence.h>
#include <ruvia/web/detail/server/Http1SessionRequestCompletion.h>
#include <ruvia/web/detail/server/HttpServerAccessLog.h>
#include <ruvia/web/detail/server/HttpResponseStreamDispatch.h>
#include <ruvia/web/detail/server/HttpResponseStreamState.h>
#include <ruvia/web/detail/server/HttpResponseCompression.h>
#include <ruvia/web/detail/server/HttpServerResponseStreamRoute.h>
#include <ruvia/web/detail/server/HttpServerWebSocketRoute.h>
#include <ruvia/http/detail/HttpResponseBodyAccess.h>

#ifdef RUVIA_ENABLE_JWT
#include <ruvia/web/auth/Jwt.h>
#endif
#ifdef RUVIA_ENABLE_MARIADB
#include <ruvia/web/db/Db.h>
#endif
#ifdef RUVIA_ENABLE_REDIS
#include <ruvia/web/redis/Redis.h>
#endif

template <typename Runtime, typename Executor>
concept HasDirectHttp2BeginDispatch = requires(
    Runtime& runtime,
    Executor executor) {
    runtime.beginDispatch(executor);
};

template <typename Body>
concept HasDirectHttp2BodyModeSelection = requires(Body& body) {
    body.selectMode(ruvia::detail::RequestBodyMode::kBuffered);
};

template <typename Resolution>
concept HasLooseRouteResolutionAccessors = requires(
    const Resolution& resolution) {
    resolution.found();
    resolution.route();
    resolution.match();
    resolution.allowedMethods();
};

template <typename Entry>
concept HasStaticRootEntryFoundFlag = requires(const Entry& entry) {
    entry.found();
};

template <typename Output>
concept AcceptsJsonDecodeOutputParameter = requires(Output& output) {
    ruvia::detail::decodeJsonString(std::string_view{}, output);
};

template <typename Value>
concept AcceptsJsonStringScanOutputParameters = requires(
    std::string_view& input,
    Value& value,
    bool& escaped) {
    ruvia::detail::parseJsonString(input, value, escaped);
};

template <typename Value>
concept AcceptsJsonValueOutputParameter = requires(
    std::string_view& input,
    Value& value,
    std::pmr::memory_resource* resource) {
    ruvia::detail::parseJsonValue(input, value, resource, std::size_t{});
};

template <typename Sequence>
concept AcceptsJsonSequenceOutputParameter = requires(
    std::string_view& input,
    Sequence& value,
    std::pmr::memory_resource* resource) {
    ruvia::detail::parseJsonSequenceValue(
        input,
        value,
        resource,
        std::size_t{});
};

template <typename Value>
concept AcceptsFormValueOutputParameter = requires(
    Value& value,
    std::pmr::memory_resource* resource) {
    ruvia::detail::parseFormValue(std::string_view{}, value, resource);
};

template <typename Value>
concept AcceptsFormScalarOutputParameter = requires(Value& value) {
    ruvia::detail::parseFormBool(std::string_view{}, value);
};

template <typename Services>
concept HasSplitContextCapabilityAccessors = requires(
    const Services& services) {
    services.bodyReader();
    services.bodyLoader();
    services.webSocket();
    services.responseStream();
};

template <typename Services>
concept HasLegacyContextBodyRefinement = requires(
    const Services& services,
    ruvia::BodyReader& reader,
    ruvia::detail::RequestBodyLoader& loader) {
    services.withBodyReader(reader);
    services.withBodyLoader(loader);
};

template <typename T>
concept HasGeneratedModelDynamicGet = requires(const T& model) {
    model.get(std::string_view{});
};

template <typename T>
concept HasGeneratedModelTypedDynamicGet = requires(const T& model) {
    model.template get<ruvia::String>(std::string_view{});
};

template <typename T>
concept HasGeneratedModelInputAccessor = requires(const T& model) {
    model.body();
};

RUVIA_MODEL(InstalledPackageModel,
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(count, ruvia::Int32)
);

static_assert(!std::copy_constructible<InstalledPackageModel>);
static_assert(std::movable<InstalledPackageModel>);
static_assert(!HasGeneratedModelDynamicGet<InstalledPackageModel>);
static_assert(!HasGeneratedModelTypedDynamicGet<InstalledPackageModel>);
static_assert(!HasGeneratedModelInputAccessor<InstalledPackageModel>);
static_assert(!std::default_initializable<ruvia::detail::ModelInput>);
static_assert(!std::constructible_from<
    ruvia::detail::ModelInput,
    ruvia::detail::ModelInputKind,
    std::string_view,
    std::pmr::memory_resource*>);

template <typename Info>
concept HasLegacyConnInfoScalarAccessors = requires(const Info& info) {
    info.secure();
    info.clientCertificateSubject();
};

template <typename Services>
concept HasBooleanTransportRefinement = requires(
    const Services& services,
    std::string_view remoteAddress,
    std::string_view certificate,
    bool secure) {
    services.withTransport(remoteAddress, certificate, secure);
};

template <typename Services>
concept AcceptsRvaluePlainTransport = requires(const Services& services) {
    services.withPlainTransport(std::string("temporary"));
};

template <typename Services>
concept AcceptsRvalueTlsAddress = requires(const Services& services) {
    services.withTlsTransport(std::string("temporary"));
};

template <typename Services>
concept AcceptsRvalueTlsCertificate = requires(const Services& services) {
    services.withTlsTransport(
        std::string_view("stable"),
        std::string("temporary"));
};

template <typename Info>
concept ExposesRvalueTransportPointer = requires {
    std::declval<const Info&&>().plain();
    std::declval<const Info&&>().tls();
};

template <typename Record>
concept HasLegacyAccessLogHttp2Flag = requires(const Record& record) {
    record.http2();
};

template <typename Alternative>
concept HasResponseStatus = requires(const Alternative& value) {
    { value.status() } -> std::same_as<std::uint16_t>;
};

template <typename Alternative>
concept HasConsumedBytes = requires(const Alternative& value) {
    { value.consumedBytes() } -> std::same_as<std::size_t>;
};

template <typename Alternative>
concept HasResponseSubmitError = requires(const Alternative& value) {
    { value.error() } ->
        std::same_as<ruvia::detail::Http2ResponseHeadSubmitError>;
};

template <typename Alternative>
concept HasResponseWriteError = requires(const Alternative& value) {
    { value.error() } ->
        std::same_as<const std::error_code&>;
};

template <typename Result>
concept HasLegacyStreamedPredicate = requires(const Result& result) {
    result.streamed();
};

template <typename Result>
concept HasLegacySharedStreamResponse = requires(Result& result) {
    result.takeResponse();
};

template <typename State>
concept HasUntypedNextOutcome = requires(State& state) {
    state.outcome;
};

template <typename Result>
concept HasLegacyStreamHandledPredicate = requires(const Result& result) {
    result.streamHandled();
};

using RecordHttpAccessFunction = void (*)(
    const ruvia::HttpServerOptions::AccessLog&,
    const ruvia::HttpRequest&,
    std::string_view,
    std::uint16_t,
    std::chrono::steady_clock::time_point) noexcept;

static_assert(std::is_same_v<
    decltype(std::declval<ruvia::ResponseStreamWriter&>().end(
        std::declval<std::span<const ruvia::HttpHeaderView>>())),
    ruvia::Task<void>>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ContextRequest&>().method()),
    std::string_view>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ContextRequest&>().knownMethod()),
    ruvia::HttpKnownMethod>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::AccessLogRecord&>().method()),
    std::string_view>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::AccessLogRecord&>()
                 .protocolVersion()),
    ruvia::HttpProtocolVersion>);
static_assert(!HasLegacyAccessLogHttp2Flag<ruvia::AccessLogRecord>);
static_assert(std::is_same_v<
    decltype(&ruvia::detail::recordHttpAccess),
    RecordHttpAccessFunction>);
static_assert(std::is_nothrow_copy_constructible_v<
    ruvia::AccessLogRecord>);
static_assert(!std::is_copy_assignable_v<ruvia::AccessLogRecord>);
static_assert(!HasUntypedNextOutcome<ruvia::Next::State>);
static_assert(std::same_as<
    decltype(ruvia::Next::State{}.table),
    const ruvia::detail::RouteTable*>);
static_assert(std::same_as<
    decltype(ruvia::Next::State{}.route),
    const ruvia::detail::RouteEntry*>);
static_assert(std::same_as<
    decltype(ruvia::Next::State{}.streamChain),
    ruvia::detail::StreamMiddlewareChainState*>);
static_assert(!std::default_initializable<
    ruvia::detail::StreamMiddlewareChainState>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        StreamMiddlewareChainState&>().handlerInvoked()),
    bool>);
static_assert(!std::default_initializable<
    ruvia::detail::StreamDispatchResult>);
static_assert(!HasLegacyStreamHandledPredicate<
    ruvia::detail::StreamDispatchResult>);
static_assert(!HasLegacySharedStreamResponse<
    ruvia::detail::StreamDispatchResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::StreamDispatchResult&>()
                 .handled()),
    const ruvia::detail::StreamRouteHandled*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::StreamDispatchResult&>()
                 .buffered()),
    const ruvia::detail::StreamRouteBufferedResponse*>);
static_assert(!std::default_initializable<
    ruvia::detail::ResponseStreamDispatchResult>);
static_assert(!HasLegacyStreamedPredicate<
    ruvia::detail::ResponseStreamDispatchResult>);
static_assert(!HasLegacySharedStreamResponse<
    ruvia::detail::ResponseStreamDispatchResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        ResponseStreamDispatchResult&>().completed()),
    const ruvia::detail::ResponseStreamCompleted*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        ResponseStreamDispatchResult&>().peerAbortedBeforeCommit()),
    const ruvia::detail::ResponseStreamPeerAbortedBeforeCommit*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::ResponseStreamState&>()
                 .commitPlan()),
    const ruvia::detail::ResponseStreamCommitPlan*>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponseWriteResult>);
static_assert(!HasResponseStatus<
    ruvia::detail::Http1BufferedResponseWriteResult>);
static_assert(!HasResponseWriteError<
    ruvia::detail::Http1BufferedResponseWriteResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1BufferedResponseWriteResult&>().completed()),
    const ruvia::detail::Http1BufferedResponseWriteCompleted*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1BufferedResponseWriteResult&>().failedBeforeCommit()),
    const ruvia::detail::Http1BufferedResponseWriteFailedBeforeCommit*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1BufferedResponseWriteResult&>().failedAfterCommit()),
    const ruvia::detail::Http1BufferedResponseWriteFailedAfterCommit*>);
static_assert(HasResponseStatus<
    ruvia::detail::Http1BufferedResponseWriteCompleted>);
static_assert(!HasResponseStatus<
    ruvia::detail::Http1BufferedResponseWriteFailedBeforeCommit>);
static_assert(HasResponseStatus<
    ruvia::detail::Http1BufferedResponseWriteFailedAfterCommit>);
static_assert(!HasResponseWriteError<
    ruvia::detail::Http1BufferedResponseWriteCompleted>);
static_assert(HasResponseWriteError<
    ruvia::detail::Http1BufferedResponseWriteFailedBeforeCommit>);
static_assert(HasResponseWriteError<
    ruvia::detail::Http1BufferedResponseWriteFailedAfterCommit>);
using ClassifyHttp1BufferedResponseWriteFunction =
    ruvia::detail::Http1BufferedResponseWriteResult (*)(
        const ruvia::detail::Http1BufferedResponsePlan&,
        std::size_t,
        std::error_code,
        std::size_t) noexcept;
static_assert(std::same_as<
    decltype(&ruvia::detail::classifyHttp1BufferedResponseWrite),
    ClassifyHttp1BufferedResponseWriteFunction>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpFileZeroCopyResult>);
static_assert(!HasResponseWriteError<
    ruvia::detail::HttpFileZeroCopyResult>);
static_assert(HasResponseWriteError<
    ruvia::detail::HttpFileZeroCopyFailed>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpFileZeroCopyResult&>().completed()),
    const ruvia::detail::HttpFileZeroCopyCompleted*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpFileZeroCopyResult&>().unavailable()),
    const ruvia::detail::HttpFileZeroCopyUnavailable*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpFileZeroCopyResult&>().failed()),
    const ruvia::detail::HttpFileZeroCopyFailed*>);
using WriteFileZeroCopyFunction =
    ruvia::Task<ruvia::detail::HttpFileZeroCopyResult> (*)(
        asio::ip::tcp::socket&,
        ruvia::detail::ResponseFileBody);
static_assert(std::same_as<
    decltype(&ruvia::detail::writeFileZeroCopy),
    WriteFileZeroCopyFunction>);
static_assert(std::same_as<
    decltype(ruvia::detail::writeFileFallback(
        std::declval<asio::ip::tcp::socket&>(),
        std::declval<ruvia::WorkerMemory&>(),
        std::declval<std::pmr::string*>(),
        std::declval<ruvia::detail::ResponseFileBody>())),
    ruvia::Task<std::error_code>>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2BufferedResponseDispatchResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2BufferedResponseDispatchResult&>().completed()),
    const ruvia::detail::Http2BufferedResponseCompleted*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2BufferedResponseDispatchResult&>()
        .peerAbortedBeforeCommit()),
    const ruvia::detail::Http2BufferedResponsePeerAbortedBeforeCommit*>);
static_assert(HasResponseStatus<
    ruvia::detail::Http2BufferedResponseCompleted>);
static_assert(HasResponseStatus<
    ruvia::detail::Http2BufferedResponsePeerAbortedAfterCommit>);
static_assert(HasResponseStatus<
    ruvia::detail::Http2BufferedResponseFailedAfterCommit>);
static_assert(!HasResponseStatus<
    ruvia::detail::Http2BufferedResponsePeerAbortedBeforeCommit>);
static_assert(!HasResponseStatus<
    ruvia::detail::Http2BufferedResponseFailedBeforeCommit>);
static_assert(HasResponseSubmitError<
    ruvia::detail::Http2BufferedResponseFailedBeforeCommit>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1SessionRequestCompletion>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1RequestSequence>);
static_assert(std::is_nothrow_constructible_v<
    ruvia::detail::Http1RequestSequence,
    std::size_t>);
static_assert(!std::copy_constructible<
    ruvia::detail::Http1RequestSequence>);
static_assert(!std::move_constructible<
    ruvia::detail::Http1RequestSequence>);
static_assert(sizeof(ruvia::detail::Http1RequestSequence) ==
              sizeof(std::size_t));
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1RequestSequence&>().nextResponseClosePolicy()),
    ruvia::detail::Http1ServerClosePolicy>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http1RequestSequence&>()
        .completeUncommittedResponse(
            std::declval<ruvia::detail::Http1ServerConnectionPlan>())),
    ruvia::detail::Http1ServerConnectionPlan>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http1RequestSequence&>()
        .completeCommittedResponse(
            std::declval<ruvia::detail::Http1ServerConnectionPlan>())),
    void>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1RequestBufferCompletion>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1SessionRequestCompletion&>().bufferedResponse()),
    const ruvia::detail::Http1BufferedResponseReady*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1SessionRequestCompletion&>().committedStream()),
    const ruvia::detail::Http1CommittedStreamResponse*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1SessionRequestCompletion&>().bufferCompletion()),
    const ruvia::detail::Http1RequestBufferCompletion&>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1SessionRequestCompletion&>().connectionPlan()),
    ruvia::detail::Http1ServerConnectionPlan>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1RequestBufferCompletion&>().discarded()),
    const ruvia::detail::Http1RequestBufferDiscarded*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1RequestBufferCompletion&>().compaction()),
    const ruvia::detail::Http1RequestBufferCompaction*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1RequestBufferCompletion&>().restored()),
    const ruvia::detail::Http1RequestBufferRestored*>);
static_assert(HasResponseStatus<
    ruvia::detail::Http1CommittedStreamResponse>);
static_assert(!HasResponseStatus<
    ruvia::detail::Http1BufferedResponseReady>);
static_assert(!HasResponseStatus<
    ruvia::detail::Http1SessionRequestCompletion>);
static_assert(HasConsumedBytes<
    ruvia::detail::Http1RequestBufferCompaction>);
static_assert(!HasConsumedBytes<
    ruvia::detail::Http1RequestBufferDiscarded>);
static_assert(!HasConsumedBytes<
    ruvia::detail::Http1RequestBufferRestored>);
static_assert(!std::default_initializable<
    ruvia::detail::HttpWebSocketRouteResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpWebSocketRouteResult&>().bufferedResponse()),
    const ruvia::detail::HttpWebSocketBufferedResponse*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpWebSocketRouteResult&>().sessionFinished()),
    const ruvia::detail::HttpWebSocketSessionFinished*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpWebSocketBufferedResponse&>().completion()),
    const ruvia::detail::Http1SessionRequestCompletion&>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::detail::Http2RequestBodyRuntime&>().store(
        std::declval<std::string_view>(),
        std::size_t{},
        std::size_t{})),
    ruvia::detail::Http2RequestBodyStoreResult>);
static_assert(!HasDirectHttp2BeginDispatch<
    ruvia::detail::Http2SansIoStreamRuntime,
    asio::io_context::executor_type>);
static_assert(!HasDirectHttp2BodyModeSelection<
    ruvia::detail::Http2RequestBodyRuntime>);
static_assert(!HasLooseRouteResolutionAccessors<
    ruvia::detail::RouteResolution>);
static_assert(!HasSplitContextCapabilityAccessors<
    ruvia::detail::ContextServices>);
static_assert(!HasLegacyContextBodyRefinement<
    ruvia::detail::ContextServices>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::ContextServices&>()
                 .requestBodySource()),
    const ruvia::detail::ContextRequestBodySource&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::ContextServices&>()
                 .responseOutput()),
    const ruvia::detail::ContextResponseOutput&>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::ContextServices&>()
                 .maxDecodedBodyBytes()),
    std::size_t>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextLazyRequestBodySource>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextStreamingRequestBodySource>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextResponseStreamOutput>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextWebSocketOutput>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::RouteEndpoint>);
static_assert(!std::is_polymorphic_v<ruvia::detail::RouteTable>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::Http2SansIoSessionContext>);
static_assert(std::is_nothrow_constructible_v<
    ruvia::detail::Http2SansIoSessionContext,
    ruvia::detail::ContextServices,
    const ruvia::HttpServerOptions&,
    ruvia::detail::ConnectionScanner::Entry&,
    const std::atomic_bool&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ConnInfo&>().plain()),
    const ruvia::PlainConnectionTransport*>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ConnInfo&>().tls()),
    const ruvia::TlsConnectionTransport*>);
static_assert(!HasLegacyConnInfoScalarAccessors<ruvia::ConnInfo>);
static_assert(!HasBooleanTransportRefinement<
    ruvia::detail::ContextServices>);
static_assert(!AcceptsRvaluePlainTransport<
    ruvia::detail::ContextServices>);
static_assert(!AcceptsRvalueTlsAddress<
    ruvia::detail::ContextServices>);
static_assert(!AcceptsRvalueTlsCertificate<
    ruvia::detail::ContextServices>);
static_assert(!ExposesRvalueTransportPointer<ruvia::ConnInfo>);
static_assert(!std::is_default_constructible_v<
    ruvia::PlainConnectionTransport>);
static_assert(!std::is_default_constructible_v<
    ruvia::TlsConnectionTransport>);
static_assert(!std::is_default_constructible_v<ruvia::ConnInfo>);
static_assert(std::is_nothrow_copy_constructible_v<ruvia::ConnInfo>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::ConnInfo>);
static_assert(std::is_nothrow_copy_assignable_v<ruvia::ConnInfo>);
static_assert(std::is_nothrow_move_assignable_v<ruvia::ConnInfo>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::RouteResolution&>().resolved()),
    const ruvia::detail::ResolvedRoute*>);
static_assert(!std::default_initializable<
    ruvia::detail::StaticRootEntryView>);
static_assert(!HasStaticRootEntryFoundFlag<
    ruvia::detail::StaticRootEntryView>);
static_assert(std::same_as<
    decltype(ruvia::detail::StaticRootAccess::find(
        std::declval<const ruvia::StaticRoot&>(),
        std::string_view{})),
    std::optional<ruvia::detail::StaticRootEntryView>>);
static_assert(!AcceptsJsonDecodeOutputParameter<std::pmr::string>);
static_assert(std::same_as<
    decltype(ruvia::detail::decodeJsonString(
        std::string_view{},
        std::declval<std::pmr::memory_resource*>())),
    std::optional<std::pmr::string>>);
static_assert(!std::default_initializable<ruvia::detail::JsonStringToken>);
static_assert(!AcceptsJsonStringScanOutputParameters<std::string_view>);
static_assert(std::same_as<
    decltype(ruvia::detail::parseJsonString(
        std::declval<std::string_view&>())),
    std::optional<ruvia::detail::JsonStringToken>>);
static_assert(!AcceptsJsonValueOutputParameter<ruvia::Array<ruvia::Int32>>);
static_assert(!AcceptsJsonSequenceOutputParameter<ruvia::Array<ruvia::Int32>>);
static_assert(std::same_as<
    decltype(ruvia::detail::parseJsonValue<ruvia::Array<ruvia::Int32>>(
        std::declval<std::string_view&>(),
        std::declval<std::pmr::memory_resource*>())),
    std::optional<ruvia::Array<ruvia::Int32>>>);
static_assert(!std::copy_constructible<ruvia::List<ruvia::Int32>>);
static_assert(!std::is_copy_assignable_v<ruvia::List<ruvia::Int32>>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::List<ruvia::Int32>>);
static_assert(std::is_nothrow_move_assignable_v<ruvia::List<ruvia::Int32>>);
static_assert(!std::copy_constructible<ruvia::String>);
static_assert(!std::is_copy_assignable_v<ruvia::String>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::String>);
static_assert(std::is_nothrow_move_assignable_v<ruvia::String>);
static_assert(!AcceptsFormValueOutputParameter<ruvia::String>);
static_assert(!AcceptsFormScalarOutputParameter<bool>);
static_assert(std::same_as<
    decltype(ruvia::detail::parseFormBool(std::string_view{})),
    std::optional<bool>>);
static_assert(std::same_as<
    decltype(ruvia::detail::parseFormValue<ruvia::String>(
        std::string_view{},
        ruvia::detail::FormValueEncoding::kUrlEncoded,
        std::declval<std::pmr::memory_resource*>())),
    std::optional<ruvia::String>>);

std::string_view peerAddress(const ruvia::Context& context) {
    return ruvia::getConnInfo(context).remote().address();
}

int main() {
    const ruvia::HttpErrorInfo error(500);
    if (error.status() != 500) {
        return 2;
    }
    const ruvia::WebSocketRouteOptions webSocketOptions;
    if (webSocketOptions.lifecycle.closeHandshakeTimeout != std::chrono::seconds(5)) {
        return 3;
    }
    ruvia::detail::Http2SansIoStreamRuntime standaloneRuntime(
        3, std::pmr::get_default_resource());
    if (!standaloneRuntime.selectRoute(
            ruvia::detail::RouteResolution{},
            ruvia::detail::RequestBodyMode::kStream)) {
        return 4;
    }
    auto& body = standaloneRuntime.body();
    if (body.selectedMode() == nullptr ||
        *body.selectedMode() != ruvia::detail::RequestBodyMode::kStream ||
        body.store("web-owned", 0, 1024) !=
            ruvia::detail::Http2RequestBodyStoreResult::kAccepted ||
        body.queue().pop() != "web-owned") {
        return 4;
    }
    asio::io_context io;
    ruvia::detail::Http2SansIoStreamRuntimeTable runtimes(
        std::pmr::get_default_resource());
    auto* runtime = runtimes.ensure(1);
    if (runtime == nullptr ||
        !runtime->selectRoute(
            ruvia::detail::RouteResolution{},
            ruvia::detail::RequestBodyMode::kBuffered)) {
        return 5;
    }
    auto* signal = runtimes.beginDispatch(1, io.get_executor());
    if (signal == nullptr || runtimes.dispatchedCount() != 1) {
        return 6;
    }
    signal->end();
    if (!signal->ended() || !runtimes.remove(1) ||
        runtimes.dispatchedCount() != 0) {
        return 7;
    }
    const ruvia::detail::ContextServices contextServices;
    if (contextServices.requestBodySource().buffered() == nullptr ||
        contextServices.requestBodySource().lazy() != nullptr ||
        contextServices.requestBodySource().streaming() != nullptr ||
        contextServices.responseOutput().buffered() == nullptr ||
        contextServices.responseOutput().responseStream() != nullptr ||
        contextServices.responseOutput().webSocket() != nullptr ||
        contextServices.connInfo().plain() == nullptr ||
        contextServices.connInfo().tls() != nullptr) {
        return 8;
    }
    const auto tlsServices = contextServices.withTlsTransport(
        "198.51.100.9",
        "CN=package-client");
    const auto* tls = tlsServices.connInfo().tls();
    if (tlsServices.connInfo().plain() != nullptr ||
        tls == nullptr ||
        tlsServices.connInfo().remote().address() != "198.51.100.9" ||
        tls->clientCertificateSubject() != "CN=package-client") {
        return 9;
    }
    ruvia::HttpResponse compressed;
    compressed.setBodyCopy(std::string(2048, 'a'));
    ruvia::detail::applyResponseCompression(
        ruvia::detail::HttpContentCoding::kGzip,
        ruvia::HttpKnownMethod::kGet,
        compressed,
        ruvia::HttpServerOptions::Compression{true, 16});
    if (compressed.header("Content-Encoding") != "gzip" ||
        ruvia::detail::responseBody(compressed).ownedBytes() == nullptr ||
        ruvia::detail::responseBody(compressed).bytes().empty()) {
        return 10;
    }
    const auto decodedJson = ruvia::detail::decodeJsonString(
        "installed\\u0020decoder",
        std::pmr::get_default_resource());
    const auto malformedJson = ruvia::detail::decodeJsonString(
        "prefix\\ud83d",
        std::pmr::get_default_resource());
    if (!decodedJson.has_value() ||
        std::string_view(*decodedJson) != "installed decoder" ||
        malformedJson.has_value()) {
        return 11;
    }
    std::string_view jsonTokenInput = "  \"installed\\u0020token\" tail";
    const auto jsonToken = ruvia::detail::parseJsonString(jsonTokenInput);
    if (!jsonToken.has_value() ||
        jsonToken->raw() != "installed\\u0020token" ||
        jsonToken->encoding() != ruvia::detail::JsonStringEncoding::kEscaped ||
        jsonTokenInput != " tail") {
        return 12;
    }
    std::string_view jsonArrayInput = "[1,2,3] tail";
    const auto jsonArray = ruvia::detail::parseJsonValue<
        ruvia::Array<ruvia::Int32>>(
        jsonArrayInput,
        std::pmr::get_default_resource());
    std::string_view malformedJsonArray = R"([1,"bad"] tail)";
    const auto originalMalformedJsonArray = malformedJsonArray;
    const auto rejectedJsonArray = ruvia::detail::parseJsonValue<
        ruvia::Array<ruvia::Int32>>(
        malformedJsonArray,
        std::pmr::get_default_resource());
    if (!jsonArray.has_value() || jsonArray->size() != 3 ||
        static_cast<std::int32_t>((*jsonArray)[2]) != 3 ||
        jsonArrayInput != " tail" || rejectedJsonArray.has_value() ||
        malformedJsonArray != originalMalformedJsonArray) {
        return 13;
    }
    ruvia::List<ruvia::Int32> installedList;
    installedList.emplace(7);
    ruvia::List<ruvia::Int32> movedList;
    movedList.emplace(8);
    movedList = std::move(installedList);
    if (movedList.size() != 1 ||
        static_cast<std::int32_t>(movedList.front()) != 7 ||
        !installedList.empty()) {
        return 14;
    }
    movedList.clear();
    if (!movedList.empty()) {
        return 15;
    }
    const auto installedFormValue = ruvia::detail::parseFormValue<
        ruvia::String>(
        "installed%20form",
        ruvia::detail::FormValueEncoding::kUrlEncoded,
        std::pmr::get_default_resource());
    const auto installedDecodedValue = ruvia::detail::parseFormValue<
        ruvia::String>(
        "literal%20form",
        ruvia::detail::FormValueEncoding::kDecoded,
        std::pmr::get_default_resource());
    if (!installedFormValue.has_value() ||
        installedFormValue->view() != "installed form" ||
        !installedDecodedValue.has_value() ||
        installedDecodedValue->view() != "literal%20form") {
        return 16;
    }
    std::string modelStringInput(128, 'o');
    ruvia::String modelString(modelStringInput, std::pmr::get_default_resource());
    modelStringInput.assign(modelStringInput.size(), 'x');
    const std::string expectedModelString(128, 'o');
    if (modelString.view() != expectedModelString ||
        modelString.resource() != std::pmr::get_default_resource()) {
        return 17;
    }
    std::pmr::monotonic_buffer_resource installedModelResource;
    const auto installedModel = InstalledPackageModel::ruviaParseJsonBody(
        R"({"name":"installed model","count":7})",
        &installedModelResource);
    if (!installedModel.has_value() ||
        !installedModel->name().has_value() ||
        installedModel->name()->view() != "installed model" ||
        installedModel->name()->resource() != &installedModelResource ||
        !installedModel->count().has_value() ||
        static_cast<std::int32_t>(*installedModel->count()) != 7) {
        return 18;
    }
    const auto invalidFieldModel = InstalledPackageModel::ruviaParseJsonBody(
        R"({"name":42,"count":7})",
        std::pmr::get_default_resource());
    if (!invalidFieldModel.has_value() ||
        invalidFieldModel->name().has_value() ||
        invalidFieldModel->ruviaFieldState<"name">() !=
            ruvia::detail::ModelFieldState::kInvalidType) {
        return 19;
    }
    const auto malformedModel = InstalledPackageModel::ruviaParseJsonBody(
        R"({"name":"incomplete")",
        std::pmr::get_default_resource());
    if (malformedModel.has_value()) {
        return 20;
    }
    ruvia::app().setHttpListenPort(8080);
    return 0;
}
