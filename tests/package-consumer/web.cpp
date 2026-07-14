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

#include <ruvia/http/ProtocolByteLimit.h>
#include <ruvia/web/App.h>
#include <ruvia/web/AppHook.h>
#include <ruvia/web/ConnInfo.h>
#include <ruvia/web/ContextRequest.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/Error.h>
#include <ruvia/web/ServerConfig.h>
#include <ruvia/web/detail/server/HttpServerOptions.h>
#include <ruvia/web/Middleware.h>
#include <ruvia/web/Model.h>
#include <ruvia/web/RequestFields.h>
#include <ruvia/web/detail/router/RouteModes.h>
#include <ruvia/web/Streaming.h>
#include <ruvia/web/ValidationTypes.h>
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
#include <ruvia/web/detail/server/Http2BufferedResponseWrite.h>
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
#ifdef RUVIA_ENABLE_DATABASE
#include <ruvia/web/db/Db.h>
#endif
#ifdef RUVIA_ENABLE_REDIS
#include <ruvia/web/redis/Redis.h>
#endif

static_assert(!std::is_copy_constructible_v<ruvia::MultipartReader>);
static_assert(!std::is_copy_assignable_v<ruvia::MultipartReader>);
static_assert(!std::is_move_constructible_v<ruvia::MultipartReader>);
static_assert(!std::is_move_assignable_v<ruvia::MultipartReader>);
static_assert(std::is_move_constructible_v<ruvia::RequestNameValueList>);
static_assert(!std::is_move_assignable_v<ruvia::RequestNameValueList>);

#ifdef RUVIA_ENABLE_JWT
static_assert(std::same_as<
    decltype(ruvia::JwtSignOptions{}.expiresIn),
    std::optional<std::chrono::seconds>>);
static_assert(std::same_as<
    decltype(ruvia::JwtSignOptions{}.notBeforeDelay),
    std::optional<std::chrono::seconds>>);
#endif

#ifdef RUVIA_ENABLE_DATABASE
template <typename T>
concept ExposesAnyRvalueDbOwnedView =
    requires(T&& value) { std::move(value).text(); } ||
    requires(T&& value) { std::move(value)[std::size_t{}]; } ||
    requires(T&& value) { std::move(value).begin(); } ||
    requires(T&& value) { std::move(value).end(); } ||
    requires(T&& value) { std::move(value).rows(); } ||
    requires(T&& value) { std::move(value).applied(); } ||
    requires(T&& value) { std::move(value).skipped(); };

static_assert(std::is_move_assignable_v<ruvia::DbField>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::DbField>);
static_assert(std::is_move_assignable_v<ruvia::DbRow>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::DbRow>);
static_assert(std::is_move_constructible_v<ruvia::DbMigrationReport>);
static_assert(!std::is_move_assignable_v<ruvia::DbMigrationReport>);
static_assert(std::is_move_constructible_v<ruvia::QueryResult>);
static_assert(!std::is_move_assignable_v<ruvia::QueryResult>);
static_assert(std::is_move_constructible_v<ruvia::DbStreamResult>);
static_assert(!std::is_move_assignable_v<ruvia::DbStreamResult>);
static_assert(std::is_move_constructible_v<ruvia::DbTransaction>);
static_assert(!std::is_move_assignable_v<ruvia::DbTransaction>);
static_assert(std::same_as<
    decltype(ruvia::DbConfig{}.connectTimeout),
    std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
    decltype(ruvia::DbConfig{}.queryTimeout),
    std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
    decltype(ruvia::DbConfig{}.acquireTimeout),
    std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<decltype(ruvia::DbConfig::mariaDb()), ruvia::DbConfig>);
static_assert(std::same_as<decltype(ruvia::DbConfig::postgreSql()), ruvia::DbConfig>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbValue>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbField>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbRow>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::QueryResult>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbMigrationReport>);
#endif

#ifdef RUVIA_ENABLE_REDIS
template <typename T>
concept ExposesAnyRvalueRedisOwnedView =
    requires(T&& value) { std::move(value).duration(); } ||
    requires(T&& value) { std::move(value).key(); } ||
    requires(T&& value) { std::move(value).value(); } ||
    requires(T&& value) { std::move(value).values(); } ||
    requires(T&& value) { std::move(value).entries(); } ||
    requires(T&& value) { std::move(value).message(); } ||
    requires(T&& value) { std::move(value).string(); } ||
    requires(T&& value) { std::move(value).array(); };

static_assert(std::is_move_assignable_v<ruvia::RedisKeyValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisKeyValue>);
static_assert(std::is_move_assignable_v<ruvia::RedisScoredValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisScoredValue>);
static_assert(std::is_move_assignable_v<ruvia::RedisValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisValue>);
static_assert(std::same_as<
    decltype(ruvia::RedisConfig{}.connectTimeout),
    std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
    decltype(ruvia::RedisConfig{}.commandTimeout),
    std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
    decltype(ruvia::RedisConfig{}.acquireTimeout),
    std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
    decltype(ruvia::RedisConfig{}.maxReplyBytes),
    std::optional<std::size_t>>);
static_assert(std::same_as<
    decltype(ruvia::RedisScanOptions{}.count),
    std::optional<std::uint64_t>>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisSetExpiration>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisKeyValue>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisScoredValue>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisHashScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisZScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisError>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisValue>);
#endif

template <typename T>
concept HasWebWorkerCorePostEscape = requires(const T& worker) {
    worker.core();
};

template <typename T>
concept HasAppInstanceAlias = requires {
    T::instance();
};

static_assert(!HasAppInstanceAlias<ruvia::App>);
static_assert(!HasWebWorkerCorePostEscape<ruvia::WebWorkerHandle>);

template <typename Runtime, typename Executor>
concept HasDirectHttp2BeginDispatch = requires(
    Runtime& runtime,
    Executor executor) {
    runtime.beginDispatch(executor);
};

#ifdef RUVIA_ENABLE_REDIS
template <typename T>
concept HasLegacyRedisSetOptionBooleans = requires(T& options) {
    options.ttl;
    options.nx;
    options.xx;
    options.get;
    options.keepTtl;
};

template <typename T>
concept HasRedisTransactionDiscard = requires(T& transaction) {
    transaction.discard();
};

template <typename T>
concept HasLvalueRedisExec = requires(T& batch) {
    batch.exec();
};

template <typename T>
concept HasRvalueRedisExec = requires(T& batch) {
    std::move(batch).exec();
};

static_assert(!HasLegacyRedisSetOptionBooleans<ruvia::RedisSetOptions>);
static_assert(!HasRedisTransactionDiscard<ruvia::RedisTransaction>);
static_assert(!HasLvalueRedisExec<ruvia::RedisPipeline>);
static_assert(HasRvalueRedisExec<ruvia::RedisPipeline>);
static_assert(!HasLvalueRedisExec<ruvia::RedisTransaction>);
static_assert(HasRvalueRedisExec<ruvia::RedisTransaction>);
static_assert(std::move_constructible<ruvia::RedisPipeline>);
static_assert(!std::assignable_from<ruvia::RedisPipeline&, ruvia::RedisPipeline&&>);
static_assert(std::move_constructible<ruvia::RedisTransaction>);
static_assert(!std::assignable_from<ruvia::RedisTransaction&, ruvia::RedisTransaction&&>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::RedisSetOptions>().condition),
    std::optional<ruvia::RedisSetCondition>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::RedisSetOptions>().expiration),
    std::optional<ruvia::RedisSetExpiration>>);
static_assert(!std::default_initializable<ruvia::RedisSetExpiration>);
static_assert(std::same_as<
    decltype(ruvia::RedisSetExpiration::expiresAfter(
        std::chrono::milliseconds(1))),
    ruvia::RedisSetExpiration>);
#endif

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

template <typename T>
concept ExposesAnyRvalueWebExecutionBorrow =
    requires(T&& value) { std::move(value).values(); } ||
    requires(T&& value) { std::move(value).match(); } ||
    requires(T&& value) { std::move(value).resolved(); } ||
    requires(T&& value) { std::move(value).methodNotAllowed(); } ||
    requires(T&& value) { std::move(value).notFound(); } ||
    requires(T&& value) { std::move(value).handled(); } ||
    requires(T&& value) { std::move(value).buffered(); } ||
    requires(T&& value) { std::move(value).bufferedResponse(); } ||
    requires(T&& value) { std::move(value).sessionFinished(); } ||
    requires(T&& value) { std::move(value).completion(); } ||
    requires(T&& value) { std::move(value).completed(); } ||
    requires(T&& value) { std::move(value).peerAbortedBeforeCommit(); } ||
    requires(T&& value) { std::move(value).peerAbortedAfterCommit(); } ||
    requires(T&& value) { std::move(value).failedBeforeCommit(); } ||
    requires(T&& value) { std::move(value).failedAfterCommit(); } ||
    requires(T&& value) { std::move(value).unavailable(); } ||
    requires(T&& value) { std::move(value).failed(); } ||
    requires(T&& value) { std::move(value).discarded(); } ||
    requires(T&& value) { std::move(value).compaction(); } ||
    requires(T&& value) { std::move(value).restored(); } ||
    requires(T&& value) { std::move(value).committedStream(); } ||
    requires(T&& value) { std::move(value).bufferCompletion(); };

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

template <typename Request>
concept HasTypeOnlyValidation = requires(const Request& request) {
    {
        request.template valid<int>()
    } -> std::same_as<const int&>;
};

template <typename Request>
concept HasPublicValidatedDataInjection = requires(const Request& request) {
    request.addValidatedData(int{});
};

template <typename Request>
concept HasExplicitValidationTarget = requires(const Request& request) {
    request.template valid<int>(ruvia::ValidationTarget::kJson);
    request.addValidatedData(ruvia::ValidationTarget::kQuery, int{});
};

template <typename Request>
concept HasRequestArrayBufferAlias = requires(const Request& request) {
    request.arrayBuffer();
};

template <typename Blob>
concept HasRequestBlobTypeAlias = requires(const Blob& blob) {
    blob.type();
};

template <typename Options>
concept HasLegacyParseBodyFlags = requires(Options& options) {
    options.all;
    options.dot;
};

template <typename Request>
concept HasRawRequestEscape = requires(const Request& request) {
    request.raw();
};

template <typename Request>
concept HasRequestFormDataAlias = requires(const Request& request) {
    request.formData();
};

template <typename Form>
concept HasFormPathValueType = requires {
    typename Form::PathValue;
};

template <typename Form>
concept HasFormDataEntryLookup = requires(const Form& form) {
    form.entry(std::string_view{});
};

template <typename Form>
concept HasFormAtLookup = requires(const Form& form) {
    form.at(std::string_view{});
};

template <typename Request>
concept HasRequestCloneMethod = requires(const Request& request) {
    request.clone();
};

template <typename Request>
concept HasRawRequestCloneType = requires {
    typename Request::RawRequestClone;
};

template <typename Request>
concept HasIndexedRoutePath = requires(const Request& request) {
    request.routePath(std::ptrdiff_t{0});
};

template <typename ContextT>
concept HasFreeRoutePath = requires(const ContextT& context) {
    routePath(context);
};

template <typename ContextT>
concept HasFreeMatchedRoutes = requires(const ContextT& context) {
    matchedRoutes(context);
};

template <typename Request>
concept HasRequestMatchedRoutes = requires(const Request& request) {
    request.matchedRoutes();
};

template <typename Request>
concept HasRequestUrl = requires(const Request& request) {
    request.url();
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
concept HasGeneratedModelCompileTimeGetAlias = requires(const T& model) {
    model.template get<"name">();
};

template <typename T>
concept HasGeneratedModelPublicBodyParseHooks =
    requires {
        T::ruviaParseJsonBody(
            std::string_view{},
            static_cast<std::pmr::memory_resource*>(nullptr));
    } || requires {
        T::ruviaParseFormBody(
            std::string_view{},
            static_cast<std::pmr::memory_resource*>(nullptr));
    };

template <typename T>
concept HasGeneratedModelInputAccessor = requires(const T& model) {
    model.body();
};

template <typename T>
concept HasGeneratedModelPublicJsonDepthHook = requires {
    T::ruviaParseJsonBodyDepth(
        std::string_view{},
        static_cast<std::pmr::memory_resource*>(nullptr),
        std::size_t{});
};

template <typename T>
concept HasGeneratedModelPublicFormFieldsHook = requires {
    T::ruviaParseFormFields(
        std::declval<const ruvia::RequestNameValueList&>(),
        static_cast<std::pmr::memory_resource*>(nullptr));
};

template <typename T>
concept HasGeneratedModelNonConstNameGetter = requires {
    static_cast<const std::optional<ruvia::String>& (T::*)()>(&T::name);
};

template <typename T>
concept HasGeneratedModelPublicJsonWriterHooks =
    requires(const T& model, std::pmr::string& output) {
        model.ruviaAppendJson(output);
    } || requires(const T& model) {
        model.ruviaJsonSizeHint();
    };

template <typename T>
concept HasGeneratedModelPublicFieldStateHook = requires(const T& model) {
    model.template ruviaFieldState<"name">();
};

struct InstalledModelBodyDuckProbe final {
    static int ruviaParseJsonBody(std::string_view, std::pmr::memory_resource*);
    static int ruviaParseFormBody(std::string_view, std::pmr::memory_resource*);
};

RUVIA_REQUEST_MODEL(InstalledPackageRequest,
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(count, ruvia::Int32)
);

RUVIA_RESPONSE_MODEL(InstalledPackageResponse,
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(count, ruvia::Int32)
);

static_assert(!std::copy_constructible<InstalledPackageRequest>);
static_assert(std::movable<InstalledPackageRequest>);
static_assert(!HasGeneratedModelDynamicGet<InstalledPackageRequest>);
static_assert(!HasGeneratedModelTypedDynamicGet<InstalledPackageRequest>);
static_assert(!HasGeneratedModelCompileTimeGetAlias<InstalledPackageRequest>);
static_assert(!HasGeneratedModelPublicBodyParseHooks<InstalledPackageRequest>);
static_assert(!HasGeneratedModelInputAccessor<InstalledPackageRequest>);
static_assert(!HasGeneratedModelPublicJsonDepthHook<InstalledPackageRequest>);
static_assert(!HasGeneratedModelPublicFormFieldsHook<InstalledPackageRequest>);
static_assert(!HasGeneratedModelNonConstNameGetter<InstalledPackageRequest>);
static_assert(!HasGeneratedModelPublicJsonWriterHooks<InstalledPackageRequest>);
static_assert(!HasGeneratedModelPublicFieldStateHook<InstalledPackageRequest>);
static_assert(std::is_base_of_v<
    ruvia::detail::RequestModelSchemaTag,
    InstalledPackageRequest>);
static_assert(!ruvia::JsonBody<InstalledPackageResponse>::value);
static_assert(!ruvia::FormBody<InstalledPackageResponse>::value);
static_assert(ruvia::detail::isResponseModel<InstalledPackageResponse>);
static_assert(!ruvia::JsonBody<InstalledModelBodyDuckProbe>::value);
static_assert(!ruvia::FormBody<InstalledModelBodyDuckProbe>::value);
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

template <typename NextT>
concept HasPublicNextRuntimeState = requires {
    typename NextT::State;
};

template <typename Result>
concept HasLegacyStreamHandledPredicate = requires(const Result& result) {
    result.streamHandled();
};

template <typename Policy>
concept HasEmbeddedPolicyEnabledFlag = requires(Policy& policy) {
    policy.enabled;
};

template <typename ContextT>
concept HasResponseInit = requires {
    typename ContextT::ResponseInit;
};

template <typename ContextT>
concept HasContextVarFacade = requires(ContextT& context) {
    context.var();
};

template <typename ContextT>
concept HasContextFinalized = requires(const ContextT& context) {
    context.finalized();
};

template <typename ContextT>
concept HasLegacyStreamSSE = requires(ContextT& context) {
    context.streamSSE();
};

template <typename Writer>
concept HasLegacyWriteSSE = requires(Writer& writer, const ruvia::SseMessage& message) {
    writer.writeSSE(message);
};

template <typename ContextT>
concept HasBuilderMetadataArguments = requires(const ContextT& context) {
    context.body(std::string_view{}, std::uint16_t{201});
    context.text(std::string_view{}, std::uint16_t{201});
    context.html(std::string_view{}, std::uint16_t{201});
    context.json(std::uint32_t{1}, std::uint16_t{201});
};

template <typename ContextT>
concept HasPmrStringBuilderLvalues = requires(
    const ContextT& context,
    std::pmr::string& body,
    const std::pmr::string& constBody) {
    context.body(body);
    context.text(constBody);
    context.html(body);
};

template <typename ContextT>
concept HasContextCookieGenerator = requires(const ContextT& context) {
    context.generateCookie(std::string_view{}, std::string_view{});
};

template <typename ContextT>
concept HasContextSignedCookieGenerator = requires(const ContextT& context) {
    context.generateSignedCookie(
        std::string_view{},
        std::string_view{},
        std::string_view{});
};

template <typename ContextT>
concept HasContextRenderPipeline = requires(ContextT& context) {
    typename ContextT::RenderOptions;
    typename ContextT::Renderer;
    typename ContextT::Layout;
    context.render(std::string_view{});
};

using RecordHttpAccessFunction = void (*)(
    const ruvia::detail::AccessLogSink&,
    const ruvia::HttpRequest&,
    std::string_view,
    std::uint16_t,
    std::chrono::steady_clock::time_point) noexcept;
using AppOnAccessFunction = ruvia::App& (ruvia::App::*)(
    ruvia::AccessLogCallback);
using AppSetCompressionFunction = ruvia::App& (ruvia::App::*)(
    std::optional<ruvia::CompressionConfig>);
using AppSetCorsFunction = ruvia::App& (ruvia::App::*)(
    std::optional<ruvia::CorsConfig>);
using AppSetConnectionTimeoutFunction = ruvia::App& (ruvia::App::*)(
    std::optional<std::chrono::milliseconds>);
using AppSetOptionalSizeFunction = ruvia::App& (ruvia::App::*)(
    std::optional<std::size_t>);
using ContextTextFunction = ruvia::HttpResponse (ruvia::Context::*)(
    std::string_view) const;
using ContextPmrStringFunction = ruvia::HttpResponse (ruvia::Context::*)(
    std::pmr::string&&) const;
using ContextDeleteCookieFunction = void (ruvia::Context::*)(
    std::string_view,
    ruvia::CookieOptions);

static_assert(std::same_as<
    decltype(static_cast<AppOnAccessFunction>(&ruvia::App::onAccess)),
    AppOnAccessFunction>);
static_assert(std::same_as<
    decltype(static_cast<AppSetCompressionFunction>(&ruvia::App::setCompression)),
    AppSetCompressionFunction>);
static_assert(std::same_as<
    decltype(static_cast<AppSetCorsFunction>(&ruvia::App::setCors)),
    AppSetCorsFunction>);
static_assert(std::same_as<
    decltype(static_cast<AppSetConnectionTimeoutFunction>(
        &ruvia::App::setKeepaliveTimeout)),
    AppSetConnectionTimeoutFunction>);
static_assert(std::same_as<
    decltype(static_cast<AppSetConnectionTimeoutFunction>(
        &ruvia::App::setClientHeaderTimeout)),
    AppSetConnectionTimeoutFunction>);
static_assert(std::same_as<
    decltype(static_cast<AppSetConnectionTimeoutFunction>(
        &ruvia::App::setClientBodyTimeout)),
    AppSetConnectionTimeoutFunction>);
static_assert(std::same_as<
    decltype(static_cast<AppSetConnectionTimeoutFunction>(
        &ruvia::App::setSendTimeout)),
    AppSetConnectionTimeoutFunction>);
static_assert(std::same_as<
    decltype(static_cast<AppSetOptionalSizeFunction>(
        &ruvia::App::setMaxConnectionsPerWorker)),
    AppSetOptionalSizeFunction>);
static_assert(std::same_as<
    decltype(static_cast<AppSetOptionalSizeFunction>(
        &ruvia::App::setKeepaliveRequests)),
    AppSetOptionalSizeFunction>);
static_assert(std::same_as<
    decltype(static_cast<AppSetOptionalSizeFunction>(
        &ruvia::App::setMaxStreamBodyBytes)),
    AppSetOptionalSizeFunction>);
static_assert(std::same_as<
    decltype(static_cast<ContextTextFunction>(&ruvia::Context::text)),
    ContextTextFunction>);
static_assert(std::same_as<
    decltype(static_cast<ContextPmrStringFunction>(&ruvia::Context::body)),
    ContextPmrStringFunction>);
static_assert(std::same_as<
    decltype(static_cast<ContextPmrStringFunction>(&ruvia::Context::text)),
    ContextPmrStringFunction>);
static_assert(std::same_as<
    decltype(static_cast<ContextPmrStringFunction>(&ruvia::Context::html)),
    ContextPmrStringFunction>);
static_assert(HasPmrStringBuilderLvalues<ruvia::Context>);
static_assert(std::same_as<
    decltype(static_cast<ContextDeleteCookieFunction>(&ruvia::Context::deleteCookie)),
    ContextDeleteCookieFunction>);
static_assert(!HasContextCookieGenerator<ruvia::Context>);
static_assert(!HasContextSignedCookieGenerator<ruvia::Context>);
static_assert(!HasContextRenderPipeline<ruvia::Context>);
static_assert(!HasResponseInit<ruvia::Context>);
static_assert(!HasContextVarFacade<ruvia::Context>);
static_assert(!HasContextFinalized<ruvia::Context>);
static_assert(!HasLegacyStreamSSE<ruvia::Context>);
static_assert(!HasLegacyWriteSSE<ruvia::SseWriter>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::Context&>().streamSse()),
    ruvia::SseWriter>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::SseWriter&>().write(
        std::declval<const ruvia::SseMessage&>())),
    ruvia::Task<void>>);
static_assert(!HasBuilderMetadataArguments<ruvia::Context>);
static_assert(std::same_as<
    ruvia::Context::HeaderOptions,
    ruvia::HttpResponse::HeaderOptions>);
static_assert(!HasEmbeddedPolicyEnabledFlag<ruvia::CompressionConfig>);
static_assert(!HasEmbeddedPolicyEnabledFlag<ruvia::CorsConfig>);
static_assert(std::same_as<
    decltype(ruvia::CorsConfig{}.maxAge),
    std::optional<std::chrono::seconds>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::HttpServerOptions>().compression),
    std::optional<ruvia::CompressionConfig>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::HttpServerOptions>().cors),
    std::optional<ruvia::CorsConfig>>);
static_assert(std::same_as<
    decltype(ruvia::detail::HttpServerOptions{}.keepaliveTimeout),
    std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
    decltype(ruvia::detail::HttpServerOptions{}.maxConnections),
    std::optional<std::size_t>>);
static_assert(std::same_as<
    decltype(ruvia::detail::HttpServerOptions{}.keepaliveRequests),
    std::optional<std::size_t>>);
static_assert(std::same_as<
    decltype(ruvia::detail::HttpServerOptions{}.maxStreamBodyBytes),
    std::optional<std::size_t>>);
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
    decltype(std::declval<const ruvia::ContextRequest&>().queries(std::string_view{})),
    std::span<const std::string_view>>);
static_assert(!HasRequestCloneMethod<ruvia::ContextRequest>);
static_assert(!HasRawRequestCloneType<ruvia::ContextRequest>);
static_assert(!HasRequestUrl<ruvia::ContextRequest>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest&>().routePath()),
    std::string_view>);
static_assert(!HasRequestMatchedRoutes<ruvia::ContextRequest>);
static_assert(!HasIndexedRoutePath<ruvia::ContextRequest>);
static_assert(!HasFreeRoutePath<ruvia::Context>);
static_assert(!HasFreeMatchedRoutes<ruvia::Context>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest&>().bytes()),
    ruvia::Task<std::span<const std::byte>>>);
static_assert(!HasRawRequestEscape<ruvia::ContextRequest>);
static_assert(!HasRequestArrayBufferAlias<ruvia::ContextRequest>);
static_assert(!HasRequestBlobTypeAlias<ruvia::ContextRequest::RequestBlob>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest::RequestBlob&>().contentType()),
    std::string_view>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestBlob,
    std::span<const std::byte>,
    std::string_view>);
static_assert(!std::is_copy_constructible_v<ruvia::ContextRequest::RequestFormField>);
static_assert(std::is_move_constructible_v<ruvia::ContextRequest::RequestFormField>);
static_assert(!std::is_move_assignable_v<ruvia::ContextRequest::RequestFormField>);
static_assert(!std::is_copy_constructible_v<
    ruvia::ContextRequest::RequestFormData::Entry>);
static_assert(!std::is_copy_assignable_v<
    ruvia::ContextRequest::RequestFormData::Entry>);
static_assert(std::is_move_constructible_v<
    ruvia::ContextRequest::RequestFormData::Entry>);
static_assert(!std::is_move_assignable_v<
    ruvia::ContextRequest::RequestFormData::Entry>);
static_assert(!std::is_copy_constructible_v<ruvia::ContextRequest::RequestFormData>);
static_assert(!std::is_copy_assignable_v<ruvia::ContextRequest::RequestFormData>);
static_assert(std::is_move_constructible_v<ruvia::ContextRequest::RequestFormData>);
static_assert(!std::is_move_assignable_v<ruvia::ContextRequest::RequestFormData>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestFormData,
    std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestFormData,
    std::pmr::vector<ruvia::ContextRequest::RequestFormField>&&>);
static_assert(!std::is_copy_constructible_v<
    ruvia::ContextRequest::RequestFormData::Object>);
static_assert(std::is_move_constructible_v<
    ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!std::is_move_assignable_v<
    ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestFormData::Object,
    const ruvia::ContextRequest::RequestFormData*,
    std::string_view>);
static_assert(!HasLegacyParseBodyFlags<ruvia::ContextRequest::ParseBodyOptions>);
static_assert(
    ruvia::ContextRequest::ParseBodyOptions{}.repeatedScalars ==
    ruvia::ContextRequest::RepeatedScalarPolicy::kLastValue);
static_assert(
    ruvia::ContextRequest::ParseBodyOptions{}.dottedNames ==
    ruvia::ContextRequest::DottedNamePolicy::kLiteral);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest&>().parseBody()),
    ruvia::Task<ruvia::ContextRequest::RequestFormData>>);
static_assert(!HasRequestFormDataAlias<ruvia::ContextRequest>);
static_assert(!HasFormPathValueType<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataEntryLookup<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormAtLookup<ruvia::ContextRequest::RequestFormData>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest::RequestFormData&>().get(
        std::string_view{})),
    ruvia::ContextRequest::RequestFormData::Value>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest::RequestFormData::Object&>().get(
        std::string_view{})),
    ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormAtLookup<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(HasTypeOnlyValidation<ruvia::ContextRequest>);
static_assert(!HasPublicValidatedDataInjection<ruvia::ContextRequest>);
static_assert(!HasExplicitValidationTarget<ruvia::ContextRequest>);
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
static_assert(!HasPublicNextRuntimeState<ruvia::Next>);
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
    ruvia::detail::Http2BufferedResponseWriteResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2BufferedResponseWriteResult&>().completed()),
    const ruvia::detail::Http2BufferedResponseWriteCompleted*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2BufferedResponseWriteResult&>()
        .peerAbortedBeforeCommit()),
    const ruvia::detail::Http2BufferedResponseWritePeerAbortedBeforeCommit*>);
static_assert(HasResponseStatus<
    ruvia::detail::Http2BufferedResponseWriteCompleted>);
static_assert(HasResponseStatus<
    ruvia::detail::Http2BufferedResponseWritePeerAbortedAfterCommit>);
static_assert(HasResponseStatus<
    ruvia::detail::Http2BufferedResponseWriteFailedAfterCommit>);
static_assert(!HasResponseStatus<
    ruvia::detail::Http2BufferedResponseWritePeerAbortedBeforeCommit>);
static_assert(!HasResponseStatus<
    ruvia::detail::Http2BufferedResponseWriteFailedBeforeCommit>);
static_assert(HasResponseSubmitError<
    ruvia::detail::Http2BufferedResponseWriteFailedBeforeCommit>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1SessionRequestCompletion>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1RequestSequence>);
static_assert(std::constructible_from<
    ruvia::detail::Http1RequestSequence,
    std::optional<std::size_t>>);
static_assert(!std::constructible_from<
    ruvia::detail::Http1RequestSequence,
    std::size_t>);
static_assert(!std::copy_constructible<
    ruvia::detail::Http1RequestSequence>);
static_assert(!std::move_constructible<
    ruvia::detail::Http1RequestSequence>);
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
        ruvia::ProtocolByteLimit::unlimited(),
        std::size_t{})),
    ruvia::detail::Http2RequestBodyStoreResult>);
static_assert(!HasDirectHttp2BeginDispatch<
    ruvia::detail::Http2SansIoStreamRuntime,
    asio::io_context::executor_type>);
static_assert(!HasDirectHttp2BodyModeSelection<
    ruvia::detail::Http2RequestBodyRuntime>);
static_assert(!HasLooseRouteResolutionAccessors<
    ruvia::detail::RouteResolution>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<ruvia::detail::RouteMatch>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<ruvia::detail::ResolvedRoute>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::RouteResolution>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::StreamDispatchResult>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::ResponseStreamDispatchResult>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::Http1BufferedResponseWriteResult>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::Http2BufferedResponseWriteResult>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::HttpFileZeroCopyResult>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::Http1RequestBufferCompletion>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::Http1SessionRequestCompletion>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::HttpWebSocketBufferedResponse>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::HttpWebSocketRouteResult>);
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
static_assert(std::is_move_constructible_v<
    ruvia::detail::RouteEndpoint>);
static_assert(!std::is_move_assignable_v<
    ruvia::detail::RouteEndpoint>);
static_assert(std::is_move_constructible_v<
    ruvia::detail::RouteEntry>);
static_assert(!std::is_move_assignable_v<
    ruvia::detail::RouteEntry>);
static_assert(!std::is_polymorphic_v<ruvia::detail::RouteTable>);
static_assert(!std::is_move_constructible_v<
    ruvia::detail::RouteTable>);
static_assert(!std::is_move_assignable_v<
    ruvia::detail::RouteTable>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::Http2SansIoSessionContext>);
static_assert(std::is_nothrow_constructible_v<
    ruvia::detail::Http2SansIoSessionContext,
    ruvia::detail::ContextServices,
    const ruvia::detail::HttpServerOptions&,
    ruvia::detail::ConnectionScanner::Entry&,
    const bool&>);
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
static_assert(!std::default_initializable<ruvia::RateLimitRule>);
static_assert(std::same_as<
    decltype(ruvia::RateLimitRule::fixedWindow(
        std::size_t{1}, std::chrono::seconds(1))),
    ruvia::RateLimitRule>);
static_assert(!std::default_initializable<ruvia::WebSocketHeartbeatPolicy>);
static_assert(std::same_as<
    decltype(ruvia::WebSocketLifecycleOptions{}.heartbeat),
    std::optional<ruvia::WebSocketHeartbeatPolicy>>);
static_assert(std::same_as<
    decltype(ruvia::WebSocketLifecycleOptions{}.closeHandshakeTimeout),
    std::optional<std::chrono::milliseconds>>);
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
    const ruvia::detail::HttpServerOptions defaultOptions;
    if (defaultOptions.maxStreamBodyBytes.has_value()) {
        return 3;
    }
    const ruvia::WebSocketRouteOptions webSocketOptions;
    if (webSocketOptions.lifecycle.closeHandshakeTimeout !=
        std::optional<std::chrono::milliseconds>(std::chrono::seconds(5))) {
        return 4;
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
        body.store(
            "web-owned", ruvia::ProtocolByteLimit::unlimited(), 1024) !=
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
    compressed.body(std::string(2048, 'a'));
    ruvia::detail::applyResponseCompression(
        ruvia::detail::HttpContentCoding::kGzip,
        ruvia::HttpKnownMethod::kGet,
        compressed,
        ruvia::CompressionConfig{.minBytes = 16});
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
    const auto installedModel = ruvia::JsonBody<InstalledPackageRequest>::parse(
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
    InstalledPackageResponse installedResponse(&installedModelResource);
    installedResponse
        .name(installedModel->name()->view())
        .count(*installedModel->count());
    const auto installedModelJson = ruvia::toJson(installedResponse, &installedModelResource);
    if (installedModelJson != R"({"name":"installed model","count":7})") {
        return 18;
    }
    const auto invalidFieldModel = ruvia::JsonBody<InstalledPackageRequest>::parse(
        R"({"name":42,"count":7})",
        std::pmr::get_default_resource());
    if (!invalidFieldModel.has_value() ||
        invalidFieldModel->name().has_value() ||
        ruvia::detail::ModelValidationAccess::fieldState<"name">(
            *invalidFieldModel) !=
            ruvia::detail::ModelFieldState::kInvalidType) {
        return 19;
    }
    const auto malformedModel = ruvia::JsonBody<InstalledPackageRequest>::parse(
        R"({"name":"incomplete")",
        std::pmr::get_default_resource());
    if (malformedModel.has_value()) {
        return 20;
    }
    const auto unavailableWorker = ruvia::app().workerFor("package-consumer");
    (void)unavailableWorker.stats();
    if (unavailableWorker.valid()) {
        return 21;
    }
    const auto postResult = unavailableWorker.post(
        [](ruvia::WebWorkerContext& context) -> ruvia::Task<void> {
            (void)context.worker();
            (void)context.resource();
            (void)context.stopToken();
#ifdef RUVIA_ENABLE_DATABASE
            if (false) {
                (void)context.db();
            }
#endif
#ifdef RUVIA_ENABLE_REDIS
            if (false) {
                (void)context.redis();
            }
#endif
            co_return;
        });
    if (postResult != ruvia::PostResult::kWorkerStopping) {
        return 22;
    }
    ruvia::app().setWorkerMailboxCapacity(1024).setHttpListenPort(8080);
    return 0;
}
