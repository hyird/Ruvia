#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <ruvia/http/ProtocolByteLimit.h>
#include <ruvia/core/EventLoopPool.h>
#include <ruvia/web/App.h>
#include <ruvia/web/AppHook.h>
#include <ruvia/web/ConnInfo.h>
#include <ruvia/web/ContextRequest.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/Dotenv.h>
#include <ruvia/web/Error.h>
#include <ruvia/web/ServerConfig.h>
#include <ruvia/web/detail/server/HttpServerOptions.h>
#include <ruvia/web/detail/server/Http1ClosingRejection.h>
#include <ruvia/web/detail/server/HttpServerWorkerCompletion.h>
#include <ruvia/web/Middleware.h>
#include <ruvia/web/Model.h>
#include <ruvia/web/RequestFields.h>
#include <ruvia/web/SecurityHeaders.h>
#include <ruvia/web/detail/router/RouteModes.h>
#include <ruvia/web/Streaming.h>
#include <ruvia/web/Validation.h>
#include <ruvia/web/ValidationTypes.h>
#include <ruvia/web/WebSocket.h>
#include <ruvia/web/detail/app/AppLifecycle.h>
#include <ruvia/web/detail/StaticFilesInternal.h>
#include <ruvia/web/detail/ValidatedValues.h>
#include <ruvia/web/detail/http/ContextCapabilities.h>
#include <ruvia/web/detail/body/HttpRequestBodyFacade.h>
#include <ruvia/web/detail/http/ContextServices.h>
#include <ruvia/web/detail/http/ContextSessionState.h>
#include <ruvia/web/detail/http/CsrfInternal.h>
#include <ruvia/web/detail/http2/Http2SansIoStreamRuntime.h>
#include <ruvia/web/detail/http2/Http2SansIoSendWindow.h>
#include <ruvia/web/detail/websocket/WsTransportReadResult.h>
#include <ruvia/web/detail/json/JsonString.h>
#include <ruvia/web/detail/model/Parser.h>
#include <ruvia/web/detail/router/RouteTable.h>
#include <ruvia/web/detail/router/RouteStreamState.h>
#include <ruvia/web/detail/server/Http2SansIoSession.h>
#include <ruvia/web/detail/server/Http2BufferedResponseWrite.h>
#include <ruvia/web/detail/server/Http1BufferedResponseWrite.h>
#include <ruvia/web/detail/server/HttpFileFallback.h>
#include <ruvia/web/detail/server/HttpFileWrite.h>
#include <ruvia/web/detail/server/Http1RequestSequence.h>
#include <ruvia/web/detail/server/Http1SessionRequestCompletion.h>
#include <ruvia/web/detail/server/HttpServerAccessLog.h>
#include <ruvia/web/detail/server/HttpResponseStreamDispatch.h>
#include <ruvia/web/detail/server/HttpResponseStreamState.h>
#include <ruvia/web/detail/server/HttpResponseCompression.h>
#include <ruvia/web/detail/server/HttpBufferedResponse.h>
#include <ruvia/web/detail/server/RateLimitDecision.h>
#include <ruvia/web/detail/server/RateLimiter.h>
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
static_assert(std::same_as<
    decltype(ruvia::detail::generateSecureToken(std::declval<std::span<char>>())),
    ruvia::detail::SecureTokenResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::ContextSessionState&>().persistNew()),
    const ruvia::detail::SessionPersistNew*>);
static_assert(std::default_initializable<
    ruvia::detail::HttpServerWorkerCompletion>);
static_assert(!std::copy_constructible<
    ruvia::detail::HttpServerWorkerCompletion>);
static_assert(!std::move_constructible<
    ruvia::detail::HttpServerWorkerCompletion>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::HttpServerWorkerCompletion&>()
                 .markStartupReady()),
    bool>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::HttpServerWorkerCompletion&>()
                 .markStartupFailed(std::declval<std::exception_ptr>())),
    bool>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        HttpServerWorkerCompletion&>().workerFailure()),
    std::exception_ptr>);
static_assert(!std::is_copy_assignable_v<ruvia::MultipartReader>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::AppLifecycle&>().requestStop()),
    ruvia::detail::AppStopRequest>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::AppLifecycle&>()
                 .completeStartHooks()),
    ruvia::detail::AppStartHooksCompletion>);

template <typename Result>
concept ExposesRvalueSendWindowAlternative = requires(Result result) {
    std::move(result).ready();
    std::move(result).aborted();
};

static_assert(!std::default_initializable<
    ruvia::detail::Http2SendWindowWaitResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http2SendWindowWaitResult&>()
                 .ready()),
    const ruvia::detail::Http2SendWindowReady*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http2SendWindowWaitResult&>()
                 .aborted()),
    const ruvia::detail::Http2SendWindowAborted*>);
static_assert(!ExposesRvalueSendWindowAlternative<
    ruvia::detail::Http2SendWindowWaitResult>);

template <typename Result>
concept ExposesRvalueWsTransportReadAlternative = requires(Result result) {
    std::move(result).data();
    std::move(result).end();
    std::move(result).failure();
};

static_assert(!std::default_initializable<
    ruvia::detail::WsTransportReadResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::WsTransportReadResult&>()
                 .failure()),
    const ruvia::detail::WsTransportReadFailure*>);
static_assert(!ExposesRvalueWsTransportReadAlternative<
    ruvia::detail::WsTransportReadResult>);

template <typename Decision>
concept ExposesRvalueRateLimitAlternative = requires(Decision decision) {
    std::move(decision).allowed();
    std::move(decision).rejection();
};

static_assert(!std::default_initializable<ruvia::detail::RateLimitDecision>);
static_assert(!std::default_initializable<ruvia::detail::RateLimitAllowed>);
static_assert(!std::default_initializable<ruvia::detail::RateLimitRejection>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::RateLimitDecision&>().allowed()),
    const ruvia::detail::RateLimitAllowed*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::RateLimitDecision&>().rejection()),
    const ruvia::detail::RateLimitRejection*>);
static_assert(!ExposesRvalueRateLimitAlternative<
    ruvia::detail::RateLimitDecision>);
struct PackageBodyReaderTarget final {
    ruvia::Task<std::optional<std::string_view>> read();
};
struct PackageBodyLoaderTarget final {
    ruvia::Task<std::string_view> readAll();
    ruvia::Task<void> discard();
};
static_assert(!std::is_move_constructible_v<
    ruvia::detail::BodyReaderBinding<PackageBodyReaderTarget>>);
static_assert(!std::is_move_constructible_v<
    ruvia::detail::RequestBodyLoaderBinding<PackageBodyLoaderTarget>>);
template <typename Rejection>
concept ExposesRvalueHttp1ClosingAlternative = requires(Rejection rejection) {
    std::move(rejection).error();
    std::move(rejection).rateLimit();
};
static_assert(std::default_initializable<
    ruvia::detail::Http1ClosingRejection>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http1ClosingRejection&>()
                 .error()),
    const ruvia::HttpErrorInfo*>);
static_assert(!ExposesRvalueHttp1ClosingAlternative<
    ruvia::detail::Http1ClosingRejection>);
static_assert(!std::is_move_constructible_v<ruvia::MultipartReader>);
static_assert(!std::is_move_assignable_v<ruvia::MultipartReader>);
static_assert(std::is_move_constructible_v<ruvia::RequestNameValueList>);
static_assert(!std::is_move_assignable_v<ruvia::RequestNameValueList>);
static_assert(std::same_as<
    decltype(ruvia::detail::prepareBufferedHttpResponse(
        std::declval<const ruvia::HttpRequest&>(),
        std::declval<ruvia::HttpResponse&>(),
        std::declval<const ruvia::detail::HttpServerOptions&>())),
    ruvia::detail::HttpBufferedResponseWritePlan>);

template <typename T>
concept ExposesAnyRvalueEnvBorrow =
    requires { std::declval<const T&&>().get("NAME"); } ||
    requires { std::declval<const T&&>().template get<std::string_view>("NAME"); };

static_assert(!ExposesAnyRvalueEnvBorrow<ruvia::Env>);

template <typename T>
concept ExposesAnyRvalueRequestNameValueListBorrow =
    requires { std::declval<const T&&>().begin(); } ||
    requires { std::declval<const T&&>().cbegin(); } ||
    requires { std::declval<const T&&>().end(); } ||
    requires { std::declval<const T&&>().cend(); } ||
    requires { std::declval<const T&&>().data(); } ||
    requires { std::declval<const T&&>()[std::size_t{}]; } ||
    requires { std::declval<const T&&>().entries(); };

template <typename T>
concept ExposesAnyRvalueValidationIssueBorrow =
    requires { std::declval<const T&&>().field(); } ||
    requires { std::declval<const T&&>().code(); } ||
    requires { std::declval<const T&&>().message(); };

template <typename T>
concept ExposesAnyRvalueValidationErrorBorrow =
    requires { std::declval<const T&&>().issues(); } ||
    requires { std::declval<const T&&>().info(); };

template <typename T>
concept ExposesRvalueValidatorIssues = requires {
    std::declval<const T&&>().issues();
};

template <typename T>
concept AcceptsAnyRvalueValidatorMutation =
    requires { std::declval<T&&>().add("field", "code", "message"); } ||
    requires(const std::optional<std::string>& value) {
        std::declval<T&&>().required(value, "field");
    } ||
    requires(const std::optional<std::string>& value) {
        std::declval<T&&>().minLength(value, "field", std::size_t{1});
    } ||
    requires(const std::optional<std::string>& value) {
        std::declval<T&&>().maxLength(value, "field", std::size_t{1});
    } ||
    requires(const std::optional<int>& value) {
        std::declval<T&&>().range(value, "field", 0, 1);
    } ||
    requires(const std::optional<std::string>& value) {
        std::declval<T&&>().oneOf(value, "field", {"value"});
    };

template <typename T>
concept ExposesRvalueHttpErrorInfo = requires {
    std::declval<const T&&>().info();
};

static_assert(!ExposesAnyRvalueRequestNameValueListBorrow<
    ruvia::RequestNameValueList>);
static_assert(!ExposesAnyRvalueValidationIssueBorrow<ruvia::ValidationIssue>);
static_assert(!ExposesAnyRvalueValidationErrorBorrow<ruvia::ValidationError>);
static_assert(!ExposesRvalueValidatorIssues<ruvia::Validator>);
static_assert(!AcceptsAnyRvalueValidatorMutation<ruvia::Validator>);
static_assert(!ExposesRvalueHttpErrorInfo<ruvia::HttpError>);

#ifdef RUVIA_ENABLE_JWT
template <typename T>
concept ExposesAnyRvalueJwtOwnedView =
    requires(T&& value) { std::move(value).name(); } ||
    requires(T&& value) { std::move(value).value(); } ||
    requires(T&& value) { std::move(value).issuer(); } ||
    requires(T&& value) { std::move(value).subject(); } ||
    requires(T&& value) { std::move(value).audience(); } ||
    requires(T&& value) { std::move(value).id(); } ||
    requires(T&& value) { std::move(value).claims(); } ||
    requires(T&& value) { std::move(value).claim(std::string_view{}); };

static_assert(std::same_as<
    decltype(ruvia::JwtSignOptions{}.expiresIn),
    std::optional<std::chrono::seconds>>);
static_assert(std::same_as<
    decltype(ruvia::JwtSignOptions{}.notBeforeDelay),
    std::optional<std::chrono::seconds>>);
static_assert(!ExposesAnyRvalueJwtOwnedView<ruvia::JwtClaim>);
static_assert(!ExposesAnyRvalueJwtOwnedView<ruvia::JwtPayload>);
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
static_assert(std::is_copy_constructible_v<ruvia::DbValue>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::DbValue>);
static_assert(!std::is_copy_assignable_v<ruvia::DbValue>);
static_assert(!std::is_move_assignable_v<ruvia::DbValue>);
template <typename T>
concept ExposesDbValueInspection = requires(const T& value) {
    value.type();
    value.text();
    value.signedValue();
    value.unsignedValue();
    value.doubleValue();
    value.boolValue();
};
static_assert(!ExposesDbValueInspection<ruvia::DbValue>);
static_assert(std::is_move_constructible_v<ruvia::DbMigrationReport>);
static_assert(!std::is_move_assignable_v<ruvia::DbMigrationReport>);
static_assert(std::is_move_constructible_v<ruvia::QueryResult>);
static_assert(!std::is_move_assignable_v<ruvia::QueryResult>);
static_assert(std::is_move_constructible_v<ruvia::DbStreamResult>);
static_assert(!std::is_move_assignable_v<ruvia::DbStreamResult>);
static_assert(std::is_move_constructible_v<ruvia::DbTransaction>);
static_assert(!std::is_move_assignable_v<ruvia::DbTransaction>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::DbHandle&>().query(std::string_view{})),
    ruvia::ScopedOperation<ruvia::QueryResult>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::DbStreamResult&>().read()),
    ruvia::ScopedOperation<std::optional<ruvia::DbRow>>>);
static_assert(std::same_as<
    decltype(ruvia::DbConfig{}.connectTimeout),
    std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
    decltype(ruvia::DbConfig{}.queryTimeout),
    std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
    decltype(ruvia::DbConfig{}.acquireTimeout),
    std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
    decltype(ruvia::DbConfig{}.poolSizePerWorker),
    std::size_t>);
template <typename T>
concept HasLegacyDbPoolSize = requires(T& config) {
    config.poolSize;
};
static_assert(!HasLegacyDbPoolSize<ruvia::DbConfig>);
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
concept HasNativeEventLoopAccess = requires(const T& worker) {
    worker.ioContext();
    worker.executor();
};

template <typename T>
concept HasAppInstanceAlias = requires {
    T::instance();
};

template <typename T>
concept HasLegacyAppThreadNumSetter = requires(T& app) {
    app.setThreadNum(std::size_t{1});
};

template <typename T>
concept HasLegacyAppGlobalRateLimitSetter = requires(T& app) {
    app.setGlobalRateLimit(std::nullopt);
};

static_assert(!HasAppInstanceAlias<ruvia::App>);
static_assert(!HasLegacyAppThreadNumSetter<ruvia::App>);
static_assert(!HasLegacyAppGlobalRateLimitSetter<ruvia::App>);
static_assert(ruvia::kDefaultRateLimitSlotsPerWorker == 8192);
static_assert(!HasWebWorkerCorePostEscape<ruvia::WebWorkerHandle>);
static_assert(!HasNativeEventLoopAccess<ruvia::WebWorkerHandle>);
static_assert(!HasNativeEventLoopAccess<ruvia::WebWorkerContext>);
static_assert(std::same_as<
              decltype(std::declval<const ruvia::WebWorkerContext&>().worker()),
              const ruvia::WorkerHandle&>);

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
static_assert(std::same_as<
    decltype(std::declval<const ruvia::RedisHandle&>().ping()),
    ruvia::ScopedOperation<void>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::RedisPipeline&&>().exec()),
    ruvia::ScopedOperation<std::pmr::vector<ruvia::RedisValue>>>);
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
    requires(T&& value) { std::move(value).completion(); } ||
    requires(T&& value) {
        requires std::is_pointer_v<decltype(std::move(value).completed())>;
    } ||
    requires(T&& value) { std::move(value).committed(); } ||
    requires(T&& value) { std::move(value).peerAbortedBeforeCommit(); } ||
    requires(T&& value) { std::move(value).peerAbortedAfterCommit(); } ||
    requires(T&& value) { std::move(value).failedBeforeCommit(); } ||
    requires(T&& value) { std::move(value).failedAfterCommit(); } ||
    requires(T&& value) { std::move(value).routeResponse(); } ||
    requires(T&& value) { std::move(value).recoveredFailure(); } ||
    requires(T&& value) { std::move(value).unavailable(); } ||
    requires(T&& value) { std::move(value).failed(); } ||
    requires(T&& value) { std::move(value).discarded(); } ||
    requires(T&& value) { std::move(value).compaction(); } ||
    requires(T&& value) { std::move(value).restored(); } ||
    requires(T&& value) { std::move(value).committedStream(); } ||
    requires(T&& value) { std::move(value).bufferCompletion(); } ||
    requires(T&& value) { std::move(value).selectedRoute(); } ||
    requires(T&& value) { std::move(value).resolution(); } ||
    requires(T&& value) { std::move(value).body(); };

template <typename T>
concept ExposesRvalueResponseStreamCommitPlan = requires(T&& value) {
    std::move(value).commitPlan();
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

template <typename Request>
concept HasTypeOnlyValidation = requires(const Request& request) {
    {
        request.template valid<int>()
    } -> std::same_as<const int&>;
};

static_assert(
    sizeof(ruvia::detail::ValidatedModelBindings) == sizeof(void*));

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

template <typename Field>
concept ExposesAnyRvalueRequestFormFieldBorrow =
    requires { std::declval<const Field&&>().name(); } ||
    requires { std::declval<const Field&&>().value(); } ||
    requires { std::declval<const Field&&>().filename(); } ||
    requires { std::declval<const Field&&>().contentType(); } ||
    requires { std::declval<const Field&&>().path(); } ||
    requires { std::declval<const Field&&>().blob(); };

template <typename Entry>
concept ExposesRvalueRequestFormEntryFields = requires {
    std::declval<const Entry&&>().fields();
};

template <typename Form>
concept ExposesAnyRvalueRequestFormDataBorrow =
    requires { std::declval<const Form&&>().fields(); } ||
    requires { std::declval<const Form&&>().groups(); } ||
    requires { std::declval<const Form&&>().get(std::string_view{}); } ||
    requires { std::declval<const Form&&>().object(std::string_view{}); };

template <typename Object>
concept ExposesRvalueRequestFormObjectGroups = requires {
    std::declval<const Object&&>().groups();
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

template <typename T>
concept ExposesAnyRvalueModelStringBorrow =
    requires { std::declval<const T&&>().view(); } ||
    requires { std::declval<const T&&>().data(); } ||
    requires {
        static_cast<std::string_view>(std::declval<const T&&>());
    };

template <typename T>
concept ExposesRvalueFixedStringView = requires {
    std::declval<const T&&>().view();
};

template <typename T>
concept ExposesAnyRvalueModelListBorrow =
    requires { std::declval<const T&&>()[std::size_t{}]; } ||
    requires { std::declval<const T&&>().front(); } ||
    requires { std::declval<const T&&>().begin(); } ||
    requires { std::declval<const T&&>().end(); } ||
    requires { std::declval<T&&>().emplace(1); } ||
    requires {
        std::declval<T&&>().emplaceMove(typename T::value_type{});
    };

template <typename T>
concept ExposesAnyRvalueGeneratedNameMember =
    requires { std::declval<const T&&>().name(); } ||
    requires { std::declval<T&&>().nameEnsure(); } ||
    requires { std::declval<T&&>().name(std::string_view{}); };

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
static_assert(!ExposesAnyRvalueModelStringBorrow<ruvia::String>);
static_assert(!ExposesRvalueFixedStringView<ruvia::FixedString<6>>);
static_assert(!ExposesAnyRvalueModelListBorrow<ruvia::List<ruvia::Int32>>);
static_assert(!ExposesAnyRvalueGeneratedNameMember<InstalledPackageRequest>);
static_assert(!ExposesAnyRvalueGeneratedNameMember<InstalledPackageResponse>);
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

template <typename Config>
concept HasLegacyCorsFields = requires(Config& config) {
    config.allowOrigin;
    config.allowHeaders;
    config.allowCredentials;
};

template <typename Options>
concept HasLegacyStaticAllowAll = requires(Options& options) {
    options.allowAll;
};

template <typename Options>
concept HasLegacyStaticFileTypesVector = requires(Options& options) {
    options.fileTypes.push_back(std::pmr::string{});
};

template <typename ContextT>
concept HasResponseInit = requires {
    typename ContextT::ResponseInit;
};

template <typename ContextT>
concept HasContextVarFacade = requires(ContextT& context) {
    context.var();
};

template <typename Options>
concept HasMisleadingXssProtectionOption = requires(Options& options) {
    options.xssProtection;
};

template <typename Response>
concept HasContextlessSecurityHeaders = requires(
    Response& response,
    const ruvia::SecurityHeadersOptions& options) {
    ruvia::applySecurityHeaders(response, options);
};

template <typename ContextT>
concept HasArbitraryContextValueSet = requires(ContextT& context) {
    context.set(std::string_view{}, std::uint32_t{});
};

template <typename ContextT>
concept HasArbitraryContextValueGet = requires(ContextT& context) {
    context.template get<std::uint32_t>(std::string_view{});
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
using AppSetSizeFunction = ruvia::App& (ruvia::App::*)(std::size_t);
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
    decltype(static_cast<AppSetSizeFunction>(
        &ruvia::App::setWorkersPerListener)),
    AppSetSizeFunction>);
static_assert(std::same_as<
    decltype(static_cast<AppSetSizeFunction>(
        &ruvia::App::setRateLimitSlotsPerWorker)),
    AppSetSizeFunction>);
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
static_assert(!HasMisleadingXssProtectionOption<ruvia::SecurityHeadersOptions>);
static_assert(!HasContextlessSecurityHeaders<ruvia::HttpResponse>);
static_assert(std::same_as<
    decltype(ruvia::SecurityHeadersOptions{}.legacyXssFilter),
    ruvia::LegacyXssFilterPolicy>);
static_assert(
    ruvia::SecurityHeadersOptions{}.legacyXssFilter ==
    ruvia::LegacyXssFilterPolicy::kDisable);
static_assert(!HasArbitraryContextValueSet<ruvia::Context>);
static_assert(!HasArbitraryContextValueGet<ruvia::Context>);
static_assert(!HasContextFinalized<ruvia::Context>);
static_assert(!HasLegacyStreamSSE<ruvia::Context>);
static_assert(!HasLegacyWriteSSE<ruvia::SseWriter>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::Context&>().streamSse()),
    ruvia::SseWriter>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::SseWriter&>().write(
        std::declval<const ruvia::SseMessage&>())),
    ruvia::ScopedOperation<void>>);
static_assert(!HasBuilderMetadataArguments<ruvia::Context>);
static_assert(std::same_as<
    ruvia::Context::HeaderOptions,
    ruvia::HttpResponse::HeaderOptions>);
static_assert(!HasEmbeddedPolicyEnabledFlag<ruvia::CompressionConfig>);
static_assert(!HasEmbeddedPolicyEnabledFlag<ruvia::CorsConfig>);
static_assert(!HasLegacyCorsFields<ruvia::CorsConfig>);
static_assert(!HasLegacyStaticAllowAll<ruvia::StaticRootOptions>);
static_assert(!HasLegacyStaticFileTypesVector<ruvia::StaticRootOptions>);
static_assert(std::same_as<
    decltype(ruvia::StaticRootOptions{}.fileTypes),
    ruvia::StaticFileTypePolicy>);
static_assert(std::same_as<
    decltype(ruvia::CorsConfig{}.maxAge),
    std::optional<ruvia::CorsMaxAge>>);
static_assert(!std::default_initializable<ruvia::CorsOriginPolicy>);
static_assert(!std::default_initializable<ruvia::CorsRequestHeadersPolicy>);

template <typename Headers>
concept SupportsRawCorsRequestHeaders = requires(Headers headers) {
    ruvia::CorsRequestHeadersPolicy::fixed(headers);
};

template <typename Origin>
concept SupportsRawCorsOrigin = requires(Origin origin) {
    ruvia::CorsOriginPolicy::exact(origin);
};

template <typename Headers>
concept SupportsRawCorsExposeHeaders = requires(
    ruvia::CorsConfig config,
    Headers headers) {
    config.exposeHeaders = headers;
};

static_assert(std::same_as<
    decltype(ruvia::CorsConfig{}.exposeHeaders),
    ruvia::CorsHeaderNames>);
static_assert(std::same_as<
    decltype(ruvia::CorsOriginPolicy::credentialed(
        ruvia::CorsOrigin::serialized("https://app.example"))),
    ruvia::CorsOriginPolicy>);
static_assert(!SupportsRawCorsOrigin<std::string_view>);
static_assert(std::same_as<
    decltype(ruvia::CorsRequestHeadersPolicy::fixed({"authorization"})),
    ruvia::CorsRequestHeadersPolicy>);
static_assert(!SupportsRawCorsRequestHeaders<std::string_view>);
static_assert(!SupportsRawCorsExposeHeaders<std::string_view>);
static_assert(!std::default_initializable<ruvia::StaticFileTypePolicy>);
static_assert(std::same_as<
    decltype(ruvia::StaticFileTypePolicy::only({"html"})),
    ruvia::StaticFileTypePolicy>);
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
template <typename T>
concept ExposesRvalueHttp2RequestBodyStoreAlternative =
    requires(T&& result) { std::move(result).stored(); } ||
    requires(T&& result) { std::move(result).protocolFailure(); } ||
    requires(T&& result) { std::move(result).backlogOverflow(); };

static_assert(std::is_same_v<
    decltype(std::declval<ruvia::ResponseStreamWriter&>().end(
        std::declval<std::span<const ruvia::HttpHeaderView>>())),
    ruvia::ScopedOperation<void>>);
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
    ruvia::ScopedOperation<std::span<const std::byte>>>);
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
static_assert(!ExposesAnyRvalueRequestFormFieldBorrow<
    ruvia::ContextRequest::RequestFormField>);
static_assert(!std::is_copy_constructible_v<
    ruvia::ContextRequest::RequestFormData::Entry>);
static_assert(!std::is_copy_assignable_v<
    ruvia::ContextRequest::RequestFormData::Entry>);
static_assert(std::is_move_constructible_v<
    ruvia::ContextRequest::RequestFormData::Entry>);
static_assert(!std::is_move_assignable_v<
    ruvia::ContextRequest::RequestFormData::Entry>);
static_assert(!ExposesRvalueRequestFormEntryFields<
    ruvia::ContextRequest::RequestFormData::Entry>);
static_assert(!std::is_copy_constructible_v<ruvia::ContextRequest::RequestFormData>);
static_assert(!std::is_copy_assignable_v<ruvia::ContextRequest::RequestFormData>);
static_assert(std::is_move_constructible_v<ruvia::ContextRequest::RequestFormData>);
static_assert(!std::is_move_assignable_v<ruvia::ContextRequest::RequestFormData>);
static_assert(!ExposesAnyRvalueRequestFormDataBorrow<
    ruvia::ContextRequest::RequestFormData>);
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
static_assert(!ExposesRvalueRequestFormObjectGroups<
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
    ruvia::ScopedOperation<ruvia::ContextRequest::RequestFormData>>);
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
    decltype(std::declval<const ruvia::detail::
        ResponseStreamDispatchResult&>().peerAbortedAfterCommit()),
    const ruvia::detail::ResponseStreamPeerAbortedAfterCommit*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        ResponseStreamDispatchResult&>().failedAfterCommit()),
    const ruvia::detail::ResponseStreamFailedAfterCommit*>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::
        ResponseStreamDispatchResult&>().routeResponse()),
    ruvia::detail::ResponseStreamRouteResponse*>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::
        ResponseStreamDispatchResult&>().recoveredFailure()),
    ruvia::detail::ResponseStreamRecoveredFailure*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        ResponseStreamDispatchResult&>().committedStatus()),
    std::optional<std::uint16_t>>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::ResponseStreamState&>()
                 .commitPlan()),
    const ruvia::detail::ResponseStreamCommitPlan*>);
static_assert(!ExposesRvalueResponseStreamCommitPlan<
    ruvia::detail::ResponseStreamState>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponseWriteResult>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponseWriteCompleted>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponseWriteFailedBeforeCommit>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponseWriteFailedAfterCommit>);
static_assert(!HasResponseStatus<
    ruvia::detail::Http1BufferedResponseWriteResult>);
static_assert(!HasResponseWriteError<
    ruvia::detail::Http1BufferedResponseWriteResult>);
template <typename Result>
concept HasLegacyHttp1BufferedWriteOutcome = requires(const Result& result) {
    result.outcome();
};
template <typename Result>
concept ExposesAnyRvalueHttp1BufferedWriteAlternative =
    requires(const Result&& result) { std::move(result).completed(); } ||
    requires(const Result&& result) {
        std::move(result).failedBeforeCommit();
    } ||
    requires(const Result&& result) {
        std::move(result).failedAfterCommit();
    };
static_assert(!HasLegacyHttp1BufferedWriteOutcome<
    ruvia::detail::Http1BufferedResponseWriteResult>);
static_assert(!ExposesAnyRvalueHttp1BufferedWriteAlternative<
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
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http1BufferedResponseWriteResult&>().committedStatus()),
    std::optional<std::uint16_t>>);
static_assert(std::is_trivially_copyable_v<
    ruvia::detail::Http1BufferedResponseWriteResult>);
static_assert(sizeof(
    ruvia::detail::Http1BufferedResponseWriteResult) <= 4);
using ClassifyHttp1BufferedResponseWriteFunction =
    ruvia::detail::Http1BufferedResponseWriteResult (*)(
        const ruvia::detail::Http1BufferedResponsePlan&,
        std::size_t,
        std::error_code,
        std::size_t) noexcept;
static_assert(std::same_as<
    decltype(&ruvia::detail::classifyHttp1BufferedResponseWrite),
    ClassifyHttp1BufferedResponseWriteFunction>);
static_assert(std::same_as<
    decltype(ruvia::detail::writeHttpResponseFile(
        std::declval<asio::ip::tcp::socket&>(),
        std::declval<ruvia::WorkerMemory&>(),
        std::declval<std::pmr::string*>(),
        std::declval<ruvia::detail::ResponseFileBody>())),
    ruvia::Task<std::error_code>>);
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
        Http2BufferedResponseWriteResult&>().peerAbortedBeforeCommit()),
    const ruvia::detail::
        Http2BufferedResponseWritePeerAbortedBeforeCommit*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2BufferedResponseWriteResult&>().peerAbortedAfterCommit()),
    const ruvia::detail::
        Http2BufferedResponseWritePeerAbortedAfterCommit*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2BufferedResponseWriteResult&>().failedBeforeCommit()),
    const ruvia::detail::Http2BufferedResponseWriteFailedBeforeCommit*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2BufferedResponseWriteResult&>().failedAfterCommit()),
    const ruvia::detail::Http2BufferedResponseWriteFailedAfterCommit*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        Http2BufferedResponseWriteResult&>().committedStatus()),
    std::optional<std::uint16_t>>);
static_assert(std::is_trivially_copyable_v<
    ruvia::detail::Http2BufferedResponseWriteResult>);
static_assert(sizeof(
    ruvia::detail::Http2BufferedResponseWriteResult) <= 4);
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
static_assert(std::same_as<
    decltype(ruvia::detail::dispatchHttpWebSocketRoute(
        std::declval<asio::ip::tcp::socket&>(),
        std::declval<ruvia::WorkerMemory&>(),
        std::declval<ruvia::detail::ConnectionScanner::Entry&>(),
        std::declval<const ruvia::detail::Http1ServerRequestParseState&>(),
        std::declval<const ruvia::detail::ResolvedRoute&>(),
        std::declval<const ruvia::detail::RouteTable&>(),
        std::declval<ruvia::RequestMemory&>(),
        std::declval<ruvia::detail::ContextServices>(),
        std::declval<const ruvia::detail::HttpServerOptions&>(),
        std::declval<std::string_view>(),
        std::declval<ruvia::HttpResponse&>())),
    ruvia::Task<std::optional<
        ruvia::detail::Http1SessionRequestCompletion>>>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::detail::Http2RequestBodyRuntime&>().store(
        std::declval<std::string_view>(),
        ruvia::ProtocolByteLimit::unlimited(),
        std::size_t{})),
    ruvia::detail::Http2RequestBodyStoreResult>);
static_assert(std::same_as<
    decltype(std::declval<const
        ruvia::detail::Http2RequestBodyStoreResult&>().stored()),
    const ruvia::detail::Http2RequestBodyStored*>);
static_assert(std::same_as<
    decltype(std::declval<const
        ruvia::detail::Http2RequestBodyStoreResult&>().protocolFailure()),
    const ruvia::detail::HttpRequestBodyFailure*>);
static_assert(std::same_as<
    decltype(std::declval<const
        ruvia::detail::Http2RequestBodyStoreResult&>().backlogOverflow()),
    const ruvia::detail::Http2RequestBodyBacklogOverflow*>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RequestBodyStoreResult>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RequestBodyStored>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RequestBodyBacklogOverflow>);
static_assert(!ExposesRvalueHttp2RequestBodyStoreAlternative<
    ruvia::detail::Http2RequestBodyStoreResult>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RequestBodyRuntime>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http2SansIoStreamRuntime&>()
                 .selectedRoute()),
    ruvia::detail::Http2SansIoSelectedRoute*>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::Http2SansIoSelectedRoute&>()
                 .signal()),
    ruvia::detail::Http2SansIoStreamSignal*>);
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
    ruvia::detail::Http2SansIoSelectedRoute>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::Http2SansIoStreamRuntime>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::ResponseStreamDispatchResult>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::Http1BufferedResponseWriteResult>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::Http2BufferedResponseWriteResult>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::Http1RequestBufferCompletion>);
static_assert(!ExposesAnyRvalueWebExecutionBorrow<
    ruvia::detail::Http1SessionRequestCompletion>);
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
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::ContextServices&>().worker()),
    const ruvia::WorkerHandle&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::Context&>().worker()),
    const ruvia::WorkerHandle&>);
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
static_assert(!std::is_default_constructible_v<ruvia::TlsIdentity>);
static_assert(!std::is_default_constructible_v<
    ruvia::TlsClientCertificatePolicy>);
static_assert(!std::is_default_constructible_v<ruvia::TlsConfig>);
static_assert(std::same_as<
    decltype(ruvia::TlsIdentity::fromFiles("cert.pem", "key.pem")),
    ruvia::TlsIdentity>);
static_assert(std::same_as<
    decltype(ruvia::TlsClientCertificatePolicy::required("ca.pem")),
    ruvia::TlsClientCertificatePolicy>);
static_assert(std::same_as<
    decltype(ruvia::ServerTopology::http(8080)),
    ruvia::ServerTopology>);
static_assert(!std::is_aggregate_v<ruvia::ServerTopology>);
static_assert(std::is_move_constructible_v<ruvia::ServerTopology>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::App&>().setServerTopology(
        ruvia::ServerTopology::http(8080))),
    ruvia::App&>);
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
    const ruvia::detail::HttpServerWorkerState&>);
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
    auto* selectedRoute = standaloneRuntime.selectedRoute();
    if (selectedRoute == nullptr) {
        return 4;
    }
    auto& body = selectedRoute->body();
    auto* streamingBody = body.streaming();
    const auto bodyStore = body.store(
        "web-owned", ruvia::ProtocolByteLimit::unlimited(), 1024);
    if (streamingBody == nullptr || body.buffered() != nullptr ||
        body.mode() != ruvia::detail::RequestBodyMode::kStream ||
        bodyStore.stored() == nullptr ||
        streamingBody->queue().pop() != "web-owned") {
        return 4;
    }
    asio::io_context io;
    auto attachment = ruvia::attachEventLoop(io);
    const auto workerHandle = attachment.loop().handle();
    ruvia::detail::Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamRuntimeTable runtimes(
        std::pmr::get_default_resource(), termination);
    ruvia::detail::Http2StreamState acceptedStream(
        1,
        std::pmr::get_default_resource());
    auto& runtime = runtimes.ensureAccepted(acceptedStream);
    if (!runtime.selectRoute(
            ruvia::detail::RouteResolution{},
            ruvia::detail::RequestBodyMode::kBuffered)) {
        return 5;
    }
    auto* signal = runtimes.beginDispatch(1, workerHandle);
    if (signal == nullptr || runtimes.dispatchedCount() != 1) {
        return 6;
    }
    asio::post(io, [&] {
        (void)termination.terminate(
            std::make_error_code(std::errc::connection_aborted));
        attachment.stop();
    });
    io.run();
    if (!signal->terminated() || !runtimes.remove(1) ||
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
        contextServices.worker().valid() ||
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
    ruvia::app()
        .setWorkerMailboxCapacity(1024)
        .setWorkersPerListener(1)
        .setRateLimitSlotsPerWorker(ruvia::kDefaultRateLimitSlotsPerWorker)
        .setDefaultRateLimitPerWorker(std::nullopt)
        .setServerTopology(ruvia::ServerTopology::http(8080));
    return 0;
}
