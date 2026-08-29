// HTTP context/request/response API surface: discriminated HTTP/1 parse
// outcomes, route metadata, decoded paths, Accept checks, buffered multipart,
// explicit body discard, response cookies, manual HttpResponse body ownership
// and PUT/PATCH streaming.

#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/App.h"
#ifdef RUVIA_ENABLE_JWT
#include "ruvia/web/auth/Jwt.h"
#endif
#include "ruvia/web/db/DbMigration.h"
#include "ruvia/web/db/DbRows.h"
#include "ruvia/web/db/DbTransaction.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/Csrf.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpClientRedirect.h"
#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/Http1RequestBodyPlan.h"
#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpExpectations.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/HttpTransferCoding.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/Health.h"
#include "ruvia/web/HttpClient.h"
#include "ruvia/web/HttpClientHandle.h"
#include "ruvia/web/HttpClientResponse.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/Model.h"
#include "ruvia/web/RateLimit.h"
#include "ruvia/web/SecurityHeaders.h"
#include "ruvia/web/Session.h"
#include "ruvia/web/Testing.h"
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/RedisTransaction.h"
#endif
#include "ruvia/web/WebSocket.h"
#include "ruvia/web/WebWorker.h"
#include "ruvia/web/detail/server/DocumentRootBinding.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/response/HttpBufferedResponse.h"
#include "ruvia/web/detail/server/response/HttpStreamingResponseCompression.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/redis/RedisTypes.h"

#if __has_include("ruvia/web/Json.h")
#error "ruvia/web/Json.h must not expose dynamic JSON writers; declare a RUVIA_RESPONSE_MODEL"
#endif

namespace ruvia::detail {
class RouteRateLimitResult;
}  // namespace ruvia::detail

#ifdef RUVIA_GET_DYNAMIC
#error "RUVIA_GET_DYNAMIC must not be public; use RUVIA_GET_STREAM or RUVIA_GET_SSE for explicit response streaming"
#endif

#ifdef RUVIA_POST_DYNAMIC
#error "RUVIA_POST_DYNAMIC must not be public; ordinary routes must not enter response streaming dynamically"
#endif

// The web API surface, asserted at compile time: every concept here names a
// shape the public API deliberately does NOT have, so a well-meant convenience
// alias or a second way to spell the same thing cannot be added unnoticed.

namespace {

struct CurrentUser final {
    std::uint32_t id{0};
    std::string_view name;
};

RUVIA_RESPONSE_MODEL(ContextJsonResponseProbe, RUVIA_REQUIRED_FIELD(value, ruvia::UInt32));

struct DynamicJsonBuilderProbe final {
    template <typename Writer>
    void operator()(Writer&) const;
};

struct AppUseProbeMiddleware;

using DetailRequestBodyMode = ruvia::detail::RequestBodyMode;
static_assert(std::is_enum_v<DetailRequestBodyMode>);
static_assert(std::same_as<decltype(ruvia::SecurityHeadersConfig{}.contentTypeOptionsHeader), ruvia::DefaultSecurityHeaderPolicy>);
static_assert(ruvia::SecurityHeadersConfig{}.contentTypeOptionsHeader == ruvia::DefaultSecurityHeaderPolicy::kEmitDefault);
static_assert(std::same_as<decltype(ruvia::SecurityHeadersConfig{}.frameOptionsHeader), ruvia::DefaultSecurityHeaderPolicy>);
static_assert(ruvia::SecurityHeadersConfig{}.frameOptionsHeader == ruvia::DefaultSecurityHeaderPolicy::kEmitDefault);
static_assert(std::same_as<decltype(ruvia::SecurityHeadersConfig{}.strictTransportSecurityHeader), ruvia::DefaultSecurityHeaderPolicy>);
static_assert(ruvia::SecurityHeadersConfig{}.strictTransportSecurityHeader == ruvia::DefaultSecurityHeaderPolicy::kEmitDefault);
static_assert(std::same_as<decltype(ruvia::SecurityHeadersConfig{}.xssProtectionHeader), ruvia::XssProtectionHeaderPolicy>);
static_assert(ruvia::SecurityHeadersConfig{}.xssProtectionHeader == ruvia::XssProtectionHeaderPolicy::kEmitDisabled);
static_assert(std::same_as<decltype(ruvia::SecurityHeadersConfig{}.existingHeaders), ruvia::SecurityHeaderConflictPolicy>);
static_assert(ruvia::SecurityHeadersConfig{}.existingHeaders == ruvia::SecurityHeaderConflictPolicy::kPreserveExisting);
template <typename T>
concept HasConfigurableDbPoolSize = requires(T& config) { config.poolSizePerWorker; };

static_assert(ruvia::BorrowedText(nullptr).empty());

// Startup configuration owns its text. Request- and operation-scoped views
// continue to use BorrowedText where the caller controls the full lifetime.
static_assert(std::is_same_v<decltype(ruvia::SecurityHeader::name), std::string>);
static_assert(std::is_same_v<decltype(ruvia::SecurityHeadersConfig::contentSecurityPolicy), std::string>);
static_assert(std::is_same_v<decltype(ruvia::WebSocketRouteConfig::subprotocols), std::vector<std::string>>);
static_assert(std::is_same_v<decltype(ruvia::RedisScanOptions::match), ruvia::BorrowedText>);
static_assert(std::is_same_v<decltype(ruvia::CookieOptions::path), ruvia::BorrowedText>);
static_assert(std::is_same_v<decltype(ruvia::HttpClientRequestView::method), ruvia::BorrowedText>);
static_assert(std::is_same_v<decltype(ruvia::SseMessage::event), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::ScopedErrorHandlerOptions{}.prefix), std::string>);
static_assert(std::same_as<decltype(ruvia::ScopedNotFoundHandlerOptions{}.prefix), std::string>);
static_assert(std::same_as<decltype(ruvia::DbMigrationOptions{}.id), std::string>);
static_assert(std::same_as<decltype(ruvia::DbMigrationOptions{}.sql), std::string>);
static_assert(std::same_as<decltype(ruvia::DbMigratorOptions{}.table), std::string>);
static_assert(std::same_as<decltype(ruvia::DbMigratorOptions{}.resource), std::pmr::memory_resource*>);
static_assert(std::constructible_from<ruvia::DbValue, ruvia::BorrowedText>);
static_assert(std::constructible_from<ruvia::DbMigration, ruvia::DbMigrationOptions>);
#ifdef RUVIA_ENABLE_REDIS
static_assert(std::is_aggregate_v<ruvia::SessionConfig>);
static_assert(std::same_as<decltype(ruvia::SessionConfig{}.redisAlias), std::string>);
static_assert(std::default_initializable<ruvia::SessionMiddleware>);
static_assert(std::constructible_from<ruvia::SessionMiddleware, ruvia::SessionConfig>);
static_assert(!std::copyable<ruvia::SessionMiddleware>);
static_assert(!std::movable<ruvia::SessionMiddleware>);
static_assert(!std::constructible_from<ruvia::SessionMiddleware, ruvia::BorrowedText>);
static_assert(!std::constructible_from<ruvia::SessionMiddleware, const char*>);
static_assert(std::same_as<decltype(std::declval<ruvia::Context&>().session()), ruvia::Session>);
static_assert(std::same_as<decltype(std::declval<ruvia::Context&>().trySession()), std::optional<ruvia::Session>>);
#endif

static_assert(ruvia::detail::httpBorrowedView(nullptr).empty());
constexpr auto kNullRoutePath = [] {
    const char* path = nullptr;
    ruvia::detail::RuviaPathList paths(path);
    return *paths.begin();
}();
static_assert(kNullRoutePath.empty());

static_assert(!HasConfigurableDbPoolSize<ruvia::DbConfig>);

template <typename T>
concept HasMisleadingXssProtectionOption = requires(T& options) { options.xssProtection; };

static_assert(!HasMisleadingXssProtectionOption<ruvia::SecurityHeadersConfig>);

template <typename T>
concept HasLegacyXssFilterOption = requires(T& options) { options.legacyXssFilter; };

static_assert(!HasLegacyXssFilterOption<ruvia::SecurityHeadersConfig>);

template <typename T>
concept HasSecurityContentTypeOptionsBoolean = requires(T& options) { options.contentTypeOptions; };

template <typename T>
concept HasSecurityFrameOptionsBoolean = requires(T& options) { options.frameOptions; };

template <typename T>
concept HasSecurityStrictTransportSecurityBoolean = requires(T& options) { options.strictTransportSecurity; };

template <typename T>
concept HasSecurityHeadersOverwriteExistingBoolean = requires(T& options) { options.overwriteExisting = true; };

static_assert(!HasSecurityContentTypeOptionsBoolean<ruvia::SecurityHeadersConfig>);
static_assert(!HasSecurityFrameOptionsBoolean<ruvia::SecurityHeadersConfig>);
static_assert(!HasSecurityStrictTransportSecurityBoolean<ruvia::SecurityHeadersConfig>);
static_assert(!HasSecurityHeadersOverwriteExistingBoolean<ruvia::SecurityHeadersConfig>);

template <typename T>
concept HasPlainAddressOf = requires(T& value) { &value; };

template <typename T>
concept HasLvalueAwait = requires(T& value) { value.operator co_await(); };

template <typename T>
concept HasTypeOnlyValid = requires(const ruvia::ContextRequest& request) { request.template validated<T>(); };

template <typename Request, typename T>
concept HasPublicValidatedDataInjection = requires(const Request& request) { request.addValidatedData(T{}); };

template <typename T>
concept HasUnaryContextHeader = requires(const T& context) { context.header(std::string_view{}); };

template <typename T>
concept HasUnaryContextQuery = requires(const T& context) { context.query(std::string_view{}); };

template <typename T>
concept HasUnaryContextCookie = requires(const T& context) { context.cookie(std::string_view{}); };

template <typename T>
concept HasContextCookieGenerator = requires(const T& context) { context.generateCookie(std::string_view{}, std::string_view{}); };

template <typename T>
concept HasContextSignedCookieGenerator = requires(const T& context) { context.generateSignedCookie(std::string_view{}, std::string_view{}, std::string_view{}); };

template <typename Context>
concept HasContextSetCookiePositional = requires(Context& context) { context.setCookie(std::string_view{}, std::string_view{}); } || requires(Context& context, const ruvia::CookieOptions& options) { context.setCookie(std::string_view{}, std::string_view{}, options); };

template <typename Context>
concept HasContextSetSignedCookiePositional = requires(Context& context) { context.setSignedCookie(std::string_view{}, std::string_view{}, std::string_view{}); } || requires(Context& context, const ruvia::CookieOptions& options) { context.setSignedCookie(std::string_view{}, std::string_view{}, std::string_view{}, options); };

template <typename Context>
concept HasContextDeleteCookiePositional = requires(Context& context) { context.deleteCookie(std::string_view{}); } || requires(Context& context, ruvia::CookieOptions options) { context.deleteCookie(std::string_view{}, options); };

template <typename Request>
concept HasContextRequestSignedCookiePositional = requires(const Request& request) { request.signedCookie(std::string_view{}, std::string_view{}); };

template <typename Context>
concept HasContextStaticFilePositional = requires(const Context& context, const ruvia::StaticRoot& root) { context.staticFile(root, std::string_view{}); } || requires(const Context& context, const ruvia::StaticRoot& root) { context.staticFile(root, std::string_view{}, std::string_view{}); };

template <typename Context>
concept HasContextFilePositional = requires(const Context& context) { context.file(std::filesystem::path{}); } || requires(const Context& context) { context.file(std::filesystem::path{}, std::string_view{}); };

template <typename String>
concept AcceptsAnyRvalueSetCookieOptionText = requires(String&& value) { ruvia::SetCookieOptions{.name = std::forward<String>(value), .value = "value"}; } || requires(String&& value) { ruvia::SetCookieOptions{.name = "name", .value = std::forward<String>(value)}; };

template <typename String>
concept AcceptsAnyRvalueSetSignedCookieOptionText = requires(String&& value) { ruvia::SetSignedCookieOptions{.name = std::forward<String>(value), .value = "value", .secret = "secret"}; } || requires(String&& value) { ruvia::SetSignedCookieOptions{.name = "name", .value = std::forward<String>(value), .secret = "secret"}; } || requires(String&& value) { ruvia::SetSignedCookieOptions{.name = "name", .value = "value", .secret = std::forward<String>(value)}; };

template <typename String>
concept AcceptsAnyRvalueDeleteCookieOptionText = requires(String&& value) { ruvia::DeleteCookieOptions{.name = std::forward<String>(value)}; };

template <typename String>
concept AcceptsAnyRvalueSignedCookieLookupOptionText = requires(String&& value) { ruvia::SignedCookieLookupOptions{.name = std::forward<String>(value), .secret = "secret"}; } || requires(String&& value) { ruvia::SignedCookieLookupOptions{.name = "name", .secret = std::forward<String>(value)}; };

template <typename String>
concept AcceptsAnyRvalueStaticFileOptionText = requires(String&& value) { ruvia::StaticFileResponseOptions{.relativePath = std::forward<String>(value)}; } || requires(String&& value) { ruvia::StaticFileResponseOptions{.relativePath = "index.html", .contentType = std::forward<String>(value)}; };

template <typename String>
concept AcceptsAnyRvalueFileOptionText = requires(String&& value) { ruvia::FileResponseOptions{.path = std::filesystem::path{}, .contentType = std::forward<String>(value)}; };

template <typename Context, typename Ready>
concept HasLegacyMakeReadyResponse = requires(Context& context, Ready ready) { makeReadyResponse(context, ready, std::string_view{}); };

template <typename T>
concept AcceptsTemporaryReadinessUnavailableReason = requires(T&& value) { ruvia::ReadinessResponseOptions{.unavailableReason = std::forward<T>(value)}; };

template <typename T>
concept AcceptsLvalueReadinessUnavailableReason = requires(T& value) { ruvia::ReadinessResponseOptions{.unavailableReason = value}; };

template <typename T>
concept HasUnaryContextParam = requires(const T& context) { context.param(std::string_view{}); };

template <typename T>
concept HasContextStatusTextSetter = requires(T& context) { context.status(ruvia::http_status::kOk, std::string_view{}); };

template <typename T>
concept HasResponseHeadersAlias = requires(T& response) { response.responseHeaders(); };

template <typename T>
concept HasContextRequestBytes = requires(const T& request) {
    { request.bytes() } -> std::same_as<ruvia::ScopedOperation<std::span<const std::byte>>>;
};

template <typename T>
concept HasRequestArrayBufferAlias = requires(const T& request) { request.arrayBuffer(); };

template <typename T>
concept HasContextRequestCanonicalMethodAccessors = requires(const T& request) {
    { request.method() } -> std::same_as<std::string_view>;
    { request.knownMethod() } -> std::same_as<ruvia::HttpKnownMethod>;
};

template <typename T>
concept HasRequestBlobArrayBufferAlias = requires(const T& blob) { blob.arrayBuffer(); };

template <typename T>
concept HasRequestBlobTypeAlias = requires(const T& blob) { blob.type(); };

template <typename T>
concept HasRequestBlobCanonicalAccessors = requires(const T& blob) {
    { blob.bytes() } -> std::same_as<std::span<const std::byte>>;
    { blob.text() } -> std::same_as<std::string_view>;
    { blob.contentType() } -> std::same_as<std::string_view>;
    { blob.size() } -> std::same_as<std::size_t>;
    { blob.empty() } -> std::same_as<bool>;
};

template <typename T>
concept HasLegacyParseBodyFlags = requires(T& options) {
    options.all;
    options.dot;
};

template <typename T>
concept HasRequestJsonValueAccessors = requires(const T& request) {
    { request.jsonValue() } -> std::same_as<ruvia::ScopedOperation<ruvia::JsonValue>>;
    { request.jsonValueIf() } -> std::same_as<ruvia::ScopedOperation<std::optional<ruvia::JsonValue>>>;
};

template <typename T>
concept HasUntypedRequestJsonAlias = requires(const T& request) {
    { request.json() } -> std::same_as<ruvia::ScopedOperation<ruvia::JsonValue>>;
};

template <typename T>
concept HasUntypedRequestJsonIfAlias = requires(const T& request) {
    { request.jsonIf() } -> std::same_as<ruvia::ScopedOperation<std::optional<ruvia::JsonValue>>>;
};

template <typename T>
concept HasRequestCloneMethod = requires(const T& request) { request.clone(); };

template <typename T>
concept HasRawRequestCloneType = requires { typename T::RawRequestClone; };

template <typename T>
concept HasRequestUrl = requires(const T& request) { request.url(); };

template <typename T>
concept HasRequestFormDataAlias = requires(const T& request) { request.formData(); };

template <typename T>
concept HasRequestMethodEnumAlias = requires(const T& request) { request.methodEnum(); };

template <typename T>
concept HasRequestTargetAlias = requires(const T& request) { request.target(); };

template <typename T>
concept HasRequestHeadersAlias = requires(const T& request) { request.headers(); };

template <typename T>
concept HasContextRequestHeaderListAccessor = requires(const T& request) { request.header(); };

template <typename T>
concept HasRequestQueryStringAlias = requires(const T& request) { request.queryString(); };

template <typename T>
concept HasRequestRoutePath = requires(const T& request) { request.routePath(); };

template <typename T>
concept HasRequestMatchedRoutes = requires(const T& request) { request.matchedRoutes(); };

template <typename T>
concept HasRequestRouteIndexAlias = requires(const T& request) { request.routeIndex(); };

template <typename T>
concept HasRequestHttpVersionAlias = requires(const T& request) { request.protocolVersion(); };

template <typename T>
concept HasRequestDecodedPathAlias = requires(const T& request) { request.decodedPath(); };

template <typename T>
concept HasRequestRemoteAddressAlias = requires(const T& request) { request.remoteAddress(); };

template <typename T>
concept HasRequestClientCertificateAlias = requires(const T& request) { request.clientCertificate(); };

template <typename T>
concept HasRequestIsSecureAlias = requires(const T& request) { request.isSecure(); };

template <typename T>
concept HasConnInfoCanonicalReadAccessors = requires(const T& info) {
    { info.remote().address() } -> std::same_as<std::string_view>;
    { info.scheme() } -> std::same_as<ruvia::HttpScheme>;
    { info.plain() } -> std::same_as<const ruvia::PlainConnectionTransport*>;
    { info.tls() } -> std::same_as<const ruvia::TlsConnectionTransport*>;
};

template <typename T>
concept HasConnInfoListenerId = requires(const T& info) { info.listener(); };

template <typename T>
concept HasLegacyConnInfoScalarAccessors = requires(const T& info) { info.secure(); } || requires(const T& info) { info.clientCertificateSubject(); };

template <typename T>
concept HasRvalueConnInfoTransportAccess = requires {
    std::declval<const T&&>().plain();
    std::declval<const T&&>().tls();
};

template <typename T>
concept HasGetConnInfo = requires(const T& context) {
    { ruvia::getConnInfo(context) } -> std::same_as<ruvia::ConnInfo>;
};

template <typename T>
concept HasFormValueToStringView = requires(const T& value) { value.toStringView(); };

template <typename T>
concept HasFormValueTextAlias = requires(const T& value) { value.text(); };

template <typename T>
concept HasFormValueGetter = requires(const T& value) {
    { value.value() } -> std::same_as<std::optional<std::string_view>>;
};

template <typename T>
concept HasFormValueValueOrAlias = requires(const T& value) { value.value_or(std::string_view{}); };

template <typename T>
concept HasFormValueTextsAlias = requires(const T& value) { value.texts(); };

template <typename T>
concept HasFormValueValuesGetter = requires(const T& value) { value.values(); };

template <typename T>
concept HasFormValueArrowOperator = requires(const T& value) { value.operator->(); };

template <typename T>
concept HasFormValueExistsAlias = requires(const T& value) { value.exists(); };

template <typename T>
concept HasFormValueIsArrayAlias = requires(const T& value) { value.isArray(); };

template <typename T>
concept HasFormValueIsFileAlias = requires(const T& value) { value.isFile(); };

template <typename T>
concept HasFormFieldIsArrayAlias = requires(const T& field) { field.isArray(); };

template <typename T>
concept HasFormFieldBooleanAccessors = requires(const T& field) {
    field.isFile();
    field.hasArrayName();
};

template <typename T>
concept HasFormFieldPublicFields = requires(const T& field) {
    field.name;
    field.value;
    field.filename;
    field.contentType;
    field.path;
    field.file;
    field.array;
};

template <typename T>
concept HasFormFieldCanonicalAccessors = requires(const T& field) {
    { field.name() } -> std::same_as<std::string_view>;
    { field.value() } -> std::same_as<std::string_view>;
    { field.filename() } -> std::same_as<std::string_view>;
    { field.contentType() } -> std::same_as<std::string_view>;
    { field.path() } -> std::same_as<std::span<const std::pmr::string>>;
    { field.isFile() } -> std::same_as<bool>;
    { field.hasArrayName() } -> std::same_as<bool>;
};

template <typename T>
concept ExposesAnyRvalueRequestFormFieldBorrow = requires { std::declval<const T&&>().name(); } || requires { std::declval<const T&&>().value(); } || requires { std::declval<const T&&>().filename(); } || requires { std::declval<const T&&>().contentType(); } || requires { std::declval<const T&&>().path(); } || requires { std::declval<const T&&>().blob(); };

template <typename T>
concept ExposesRvalueRequestFormGroupFields = requires { std::declval<const T&&>().fields(); };

template <typename T>
concept HasFormFieldTextAlias = requires(const T& field) { field.text(); };

template <typename T>
concept HasFormFieldFileNameAlias = requires(const T& field) { field.fileName(); };

template <typename T>
concept HasFormFieldMediaTypeAlias = requires(const T& field) { field.mediaType(); };

template <typename T>
concept HasFormFieldArrayBufferAlias = requires(const T& field) { field.arrayBuffer(); };

template <typename T>
concept HasFormValueFileNameAlias = requires(const T& value) { value.fileName(); };

template <typename T>
concept HasFormValueMediaTypeAlias = requires(const T& value) { value.mediaType(); };

template <typename T>
concept HasFormGroupIsArrayAlias = requires(const T& group) { group.isArray(); };

template <typename T>
concept HasFormGroupCanonicalAccessors = requires(const T& group) {
    { group.name() } -> std::same_as<std::string_view>;
    { group.field() } -> std::same_as<const ruvia::ContextRequest::RequestFormField*>;
    { group.fields() } -> std::same_as<std::span<const ruvia::ContextRequest::RequestFormField* const>>;
    { group.size() } -> std::same_as<std::size_t>;
    { group.empty() } -> std::same_as<bool>;
    { group.multiple() } -> std::same_as<bool>;
    { group.hasArrayName() } -> std::same_as<bool>;
    { group.value() } -> std::same_as<std::optional<std::string_view>>;
};

template <typename T>
concept HasFormValueZeroAllocationAccessors = requires(const T& value) {
    { value.size() } -> std::same_as<std::size_t>;
    { value.fields() } -> std::same_as<std::span<const ruvia::ContextRequest::RequestFormField* const>>;
    { value.field() } -> std::same_as<const ruvia::ContextRequest::RequestFormField*>;
    { value.hasArrayName() } -> std::same_as<bool>;
};

template <typename T>
concept HasLegacyHttpRequestQueryGetter = requires(const T& request) {
    { request.query(std::string_view{}) } -> std::same_as<std::optional<std::string_view>>;
};

template <typename T>
concept HasHttpRequestWireMetadata = requires(const T& request) {
    { request.scheme() } -> std::same_as<std::string_view>;
    { request.authority() } -> std::same_as<std::string_view>;
    { request.targetForm() } -> std::same_as<ruvia::HttpRequestTargetForm>;
    { request.lastRawQueryValue(std::string_view{}) } -> std::same_as<std::optional<std::string_view>>;
};

template <typename T>
concept HasHttpRequestDecodedPathAlias = requires(const T& request) { request.decodedPath(); };

template <typename T>
concept ExposesRvalueHttpRequestHeaders = requires(T&& request) { std::move(request).headers(); };

template <typename T>
concept ExposesRvalueRequestMemoryBorrow = requires(T&& memory) { std::move(memory).resource(); } || requires(T&& memory) { std::move(memory).template allocator<>(); };

template <typename T>
concept ExposesRvalueRouteListIterator = requires(T&& list) { std::move(list).begin(); } || requires(T&& list) { std::move(list).end(); };

template <typename String>
concept AcceptsTemporaryRoutePath = requires(String&& path) { ruvia::detail::RuviaPathList(std::forward<String>(path)); };

template <typename T>
concept HasFormDataGetAllAlias = requires(const T& form) { form.getAll(std::string_view{}); };

template <typename T>
concept HasFormDataValuesAllAlias = requires(const T& form) { form.values(); };

template <typename T>
concept HasFormDataNamedValuesAllocator = requires(const T& form) { form.values(std::string_view{}); };

template <typename T>
concept HasFormDataNamedFieldsAllocator = requires(const T& form) { form.fields(std::string_view{}); };

template <typename T>
concept HasFormDataNamedFieldAlias = requires(const T& form) { form.field(std::string_view{}); };

template <typename T>
concept HasFormDataIsArrayAlias = requires(const T& form) { form.isArray(std::string_view{}); };

template <typename T>
concept HasFormDataValueAlias = requires(const T& form) { form.value(std::string_view{}); };

template <typename T>
concept HasFormDataHasAlias = requires(const T& form) { form.has(std::string_view{}); };

template <typename T>
concept HasFormDataKeysAllocator = requires(const T& form) { form.keys(); };

template <typename T>
concept HasFormDataEntriesAlias = requires(const T& form) { form.entries(); };

template <typename T>
concept HasFormDataIndexAlias = requires(const T& form) { form[std::string_view{}]; };

template <typename T>
concept HasFormDataEntryLookup = requires(const T& form) { form.entry(std::string_view{}); };

template <typename T>
concept HasFormDataPathAliases = requires(const T& form) {
    form.getAt(std::string_view{});
    form.hasAt(std::string_view{});
    form.countAt(std::string_view{});
    form.isArrayAt(std::string_view{});
    form.valueAt(std::string_view{});
    form.valuesAt(std::string_view{});
    form.getAllAt(std::string_view{});
};

template <typename T>
concept HasFormDataCanonicalAccessors = requires(const T& form) {
    { form.fields() } -> std::same_as<std::span<const ruvia::ContextRequest::RequestFormField>>;
    { form.groups() } -> std::same_as<std::span<const ruvia::ContextRequest::RequestFormData::Group>>;
    { form.get(std::string_view{}) } -> std::same_as<ruvia::ContextRequest::RequestFormData::Value>;
    { form.count(std::string_view{}) } -> std::same_as<std::size_t>;
};

template <typename T>
concept ExposesAnyRvalueRequestFormDataBorrow = requires { std::declval<const T&&>().fields(); } || requires { std::declval<const T&&>().groups(); } || requires { std::declval<const T&&>().get(std::string_view{}); } || requires { std::declval<const T&&>().object(std::string_view{}); };

template <typename T>
concept HasFormAtLookup = requires(const T& form) { form.at(std::string_view{}); };

template <typename T>
concept HasFormPathValueType = requires { typename T::PathValue; };

template <typename T>
concept HasFormDataEntryType = requires { typename T::Entry; };

template <typename T>
concept HasFormObjectGetAllAlias = requires(const T& object) { object.getAll(std::string_view{}); };

template <typename T>
concept HasFormObjectKeysAllocator = requires(const T& object) { object.keys(); };

template <typename T>
concept HasFormObjectEntriesAlias = requires(const T& object) { object.entries(); };

template <typename T>
concept HasFormObjectIndexAlias = requires(const T& object) { object[std::string_view{}]; };

template <typename T>
concept HasFormObjectValueAlias = requires(const T& object) { object.value(std::string_view{}); };

template <typename T>
concept HasFormObjectHasAlias = requires(const T& object) { object.has(std::string_view{}); };

template <typename T>
concept HasFormObjectNamedValuesAllocator = requires(const T& object) { object.values(std::string_view{}); };

template <typename T>
concept HasFormObjectCanonicalAccessors = requires(const T& object) {
    { object.groups() } -> std::same_as<std::span<const ruvia::ContextRequest::RequestFormData::Group>>;
    { object.get(std::string_view{}) } -> std::same_as<ruvia::ContextRequest::RequestFormData::Value>;
    { object.count(std::string_view{}) } -> std::same_as<std::size_t>;
};

template <typename T>
concept ExposesRvalueRequestFormObjectGroups = requires { std::declval<const T&&>().groups(); };

template <typename T>
concept HasByteSpanResponseBody = requires(const T& context, std::span<const std::byte> body) {
    { context.body(body) } -> std::same_as<ruvia::HttpResponse>;
};

template <typename T>
concept HasStdStringResponseBody = requires(const T& context, std::string body) { context.body(body); };

template <typename T>
concept HasPmrStringResponseBuilders = requires(const T& context, std::pmr::string& lvalue, const std::pmr::string& constLvalue) {
    { context.body(lvalue) } -> std::same_as<ruvia::HttpResponse>;
    { context.text(constLvalue) } -> std::same_as<ruvia::HttpResponse>;
    { context.html(std::move(lvalue)) } -> std::same_as<ruvia::HttpResponse>;
};

template <typename T>
concept HasContextNewResponseAlias = requires(const T& context) { context.newResponse(std::string_view{}); };

template <typename T>
concept HasContextSetHeaderAlias = requires(T& context) { context.setHeader(std::string_view{}, std::string_view{}); };

template <typename T>
concept HasContextResponseSlotAlias = requires(T& context, ruvia::HttpResponse response) { context.res(std::move(response)); };

template <typename T>
concept HasResponseSetHeaderAlias = requires(T& response) { response.setHeader(std::string_view{}, std::string_view{}); };

template <typename T>
concept HasResponseHeaderSetter = requires(T& response) {
    { response.header(std::string_view{}, std::string_view{}) } -> std::same_as<void>;
};

template <typename T>
concept HasResponseAppendHeaderAlias = requires(T& response) { response.appendHeader(std::string_view{}, std::string_view{}); };

template <typename T>
concept HasResponseHasHeaderAlias = requires(T& response) { response.hasHeader(std::string_view{}); };

template <typename T>
concept HasResponseHeaderOptionsSetter = requires(T& response) {
    { response.header(std::string_view{}, std::string_view{}, ruvia::HttpResponse::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend}) } -> std::same_as<void>;
};

template <typename T>
concept HasResponseHeaderAppendBoolean = requires(T& options) { options.append = true; };

template <typename T>
concept CanBoolInitializeResponseHeaderOptions = requires { T{true}; };

template <typename T>
concept HasResponseHeaderRemoveSetter = requires(T& response) {
    { response.removeHeader(std::string_view{}) } -> std::same_as<void>;
};

template <typename T>
concept HasContextBuilderMetadataArguments = requires(const T& context) {
    context.body(std::string_view{}, std::uint16_t{});
    context.text(std::string_view{}, std::uint16_t{});
    context.html(std::string_view{}, std::uint16_t{});
    context.json(std::uint32_t{1}, std::uint16_t{});
};

template <typename T, typename Value>
concept AcceptsContextJson = requires(const T& context, const Value& value) {
    { context.json(value) } -> std::same_as<ruvia::HttpResponse>;
};

template <typename T>
concept HasContextJsonObjectBuilder = requires(const T& context) { context.jsonObject(DynamicJsonBuilderProbe{}); };

template <typename T>
concept HasContextJsonArrayBuilder = requires(const T& context) { context.jsonArray(DynamicJsonBuilderProbe{}); };

template <typename T>
concept HasResponseHeadersEraseAlias = requires(T& response) { response.headers().erase(std::string_view{}); };

template <typename T>
concept HasResponseHeadersGetAlias = requires(T& response) { response.headers().get(std::string_view{}); };

template <typename T>
concept HasResponseHeadersEntriesAlias = requires(T& response) { response.headers().entries(); };

template <typename T>
concept HasResponseHeadersHasAlias = requires(T& response) { response.headers().has(std::string_view{}); };

template <typename T>
concept HasResponseHeadersSetAlias = requires(T& response) { response.headers().set(std::string_view{}, std::string_view{}); };

template <typename T>
concept HasResponseHeadersAppendAlias = requires(T& response) { response.headers().append(std::string_view{}, std::string_view{}); };

template <typename T>
concept HasResponseHeadersRemoveAlias = requires(T& response) { response.headers().remove(std::string_view{}); };

template <typename T>
concept HasResponseSetStatusAlias = requires(T& response) { response.setStatus(std::uint16_t{}, std::string_view{}); };

template <typename T>
concept HasResponseStatusSetter = requires(T& response) {
    { response.status(ruvia::http_status::kOk) } -> std::same_as<void>;
};

template <typename T>
concept HasResponseReasonPhraseSetter = requires(T& response) { response.status(std::uint16_t{}, std::string_view{}); };

template <typename T>
concept HasResponseStatusCodeAlias = requires(const T& response) { response.statusCode(); };

template <typename T>
concept HasDynamicHttpClientFactory = requires { T::newHttpClient(std::string_view{}); };

template <typename T>
concept HasHttpClientRequestBuilder = requires(const T& client) { client.newRequest(ruvia::HttpKnownMethod::kGet, std::string_view{}); };

template <typename T>
concept HasHttpClientWithOptions = requires(const T& client) { client.withOptions(ruvia::OperationOptions{}); };

template <typename T>
concept HasAliasedUseHttpClient = requires(T& app, ruvia::HttpClientConfig config) { app.useHttpClient(std::string_view{}, std::move(config)); };

template <typename T>
concept HasHttpClientOriginCacheCapacitySetter = requires(T& app) {
    { app.setHttpClientOriginCacheCapacityPerWorker(std::size_t{64}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasMaxHttpClientOriginsSetter = requires(T& app) {
    { app.setMaxHttpClientOriginsPerWorker(std::size_t{64}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasHttpClientVerifyCertificateBoolean = requires(T& config) { config.verifyCertificate; };

template <typename T>
concept HasHttpClientCookiesEnabledBoolean = requires(T& config) { config.cookiesEnabled; };

template <typename T>
concept HasHttpClientTcpNoDelayBoolean = requires(T& config) { config.tcpNoDelay = true; };

template <typename T>
concept HasHttpClientKeepAliveBoolean = requires(T& config) { config.keepAlive = true; };

template <typename T>
concept HasHttpClientTcpSocketPolicies = requires(T& config) {
    { config.tcpNoDelay } -> std::same_as<ruvia::TcpNoDelayPolicy&>;
    { config.tcpKeepAlive } -> std::same_as<ruvia::TcpKeepAlivePolicy&>;
};

template <typename T>
concept HasHttpClientRuntimeConfiguration = requires(T& client) { client.setUserAgent(std::string_view{}); } || requires(T& client) { client.enableCookies(true); } || requires(T& client) { client.addCookie(std::string_view{}, std::string_view{}); };

template <typename T>
concept HasHttpClientStatsAliases = requires(const T& client) { client.bufferedRequests(); } || requires(const T& client) { client.outstandingRequests(); } || requires(const T& client) { client.bytesSent(); } || requires(const T& client) { client.bytesReceived(); };

template <typename T>
concept HasHttpClientSchemeAliases = requires(const T& client) { client.secure(); } || requires(const T& client) { client.onDefaultPort(); };

template <typename T>
concept HasHttpClientResponseGetHeader = requires(const T& response) { response.getHeader(std::string_view{}); };

template <typename T>
concept HasHttpClientResponseGetTrailer = requires(const T& response) { response.getTrailer(std::string_view{}); };

template <typename T>
concept HasResponseStatusGetter = requires(const T& response) {
    { response.status() } -> std::same_as<ruvia::HttpStatusCode>;
};

using ResponseHeadersGetter = const ruvia::HttpResponseHeaders& (ruvia::HttpResponse::*)() const& noexcept;

template <typename T>
concept HasResponseSetBodyOwnedAlias = requires(T& response, std::pmr::string body) { response.setBodyOwned(std::move(body)); };

template <typename T>
concept HasResponseSetBodyCopyAlias = requires(T& response) { response.setBodyCopy(std::string_view{}); };

template <typename T>
concept HasResponseSetBodyViewAlias = requires(T& response) { response.setBodyView(std::string_view{}); };

template <typename T>
concept HasResponseStreamWriteOwnedAlias = requires(T& writer, std::pmr::string chunk) { writer.writeOwned(std::move(chunk)); };

template <typename T>
concept HasResponseStreamOwningWrite = requires { static_cast<ruvia::ScopedOperation<void> (T::*)(std::pmr::string&&)&>(&T::write); };

template <typename T>
concept HasWebSocketOwnedWriteAlias = requires(T& socket, std::pmr::string payload) { socket.textOwned(std::move(payload)); } || requires(T& socket, std::pmr::string payload) { socket.binaryOwned(std::move(payload)); } || requires(T& socket, std::pmr::string payload) { socket.pongOwned(std::move(payload)); } || requires(T& socket, std::pmr::string payload) { socket.pingOwned(std::move(payload)); };

template <typename T>
concept HasWebSocketOwningWrites = requires {
    static_cast<ruvia::ScopedOperation<void> (T::*)(std::pmr::string&&)&>(&T::text);
    static_cast<ruvia::ScopedOperation<void> (T::*)(std::pmr::string&&)&>(&T::binary);
    static_cast<ruvia::ScopedOperation<void> (T::*)(std::pmr::string&&)&>(&T::pong);
    static_cast<ruvia::ScopedOperation<void> (T::*)(std::pmr::string&&)&>(&T::ping);
};

template <typename T>
concept HasResponseBodySetter = requires(T& response) {
    { response.body(std::string_view{}) } -> std::same_as<void>;
};
template <typename T>
concept HasHttpClientResponseHeadStatusCodeField = requires { requires std::is_member_object_pointer_v<decltype(&T::statusCode)>; };

template <typename T>
concept HasHttpClientResponseHeadStatusGetter = requires(const T& head) {
    { head.status() } -> std::same_as<ruvia::HttpStatusCode>;
};

template <typename T>
concept HasHttpClientRequestViewHeaderViews = requires(T& options, std::span<const ruvia::HttpHeaderView> headers) {
    options.headers = headers;
    { std::span<const ruvia::HttpHeaderView>(options.headers) } -> std::same_as<std::span<const ruvia::HttpHeaderView>>;
};

template <typename T>
concept HasHttpClientRequestViewHeaderArray = requires(T& options, const ruvia::HttpHeaderView (&headers)[1]) { options.headers = headers; };

template <typename T>
concept HasHttpClientRequestViewHeaderVector = requires(T& options, const std::vector<ruvia::HttpHeaderView>& headers) { options.headers = headers; };

template <typename T>
concept HasHttpClientRequestViewInitializerListHeaders = requires(T& options, std::initializer_list<ruvia::HttpHeaderView> headers) { options.headers = headers; };

template <typename T>
concept HasHttpClientRequestViewBorrowedText = requires(T& request) {
    { request.method.view() } -> std::same_as<std::string_view>;
    { request.target.view() } -> std::same_as<std::string_view>;
};

template <typename String>
concept AcceptsAnyTemporaryHttpClientRequestViewText = requires(String&& value) { ruvia::HttpClientRequestView{.method = std::forward<String>(value)}; } || requires(String&& value) { ruvia::HttpClientRequestView{.target = std::forward<String>(value)}; } || requires(ruvia::HttpClientRequestView& request, String&& value) { request.method = std::forward<String>(value); } || requires(ruvia::HttpClientRequestView& request, String&& value) { request.target = std::forward<String>(value); };

template <typename String>
concept AcceptsLvalueHttpClientRequestViewText = requires(ruvia::HttpClientRequestView& request, String& value) {
    ruvia::HttpClientRequestView{.method = value, .target = value};
    request.method = value;
    request.target = value;
};

template <typename T>
concept HasRawHttpClientRequestViewBody = requires(T& request) {
    { request.body } -> std::same_as<std::string_view&>;
};

template <typename T>
concept HasDiscriminatedHttpClientRequestContentView = requires(T& request) {
    { request.content.withoutContent() } -> std::same_as<const ruvia::HttpClientRequestWithoutContent*>;
    { request.content.borrowedBytes() } -> std::same_as<const ruvia::HttpClientRequestBytesView*>;
};

template <typename T>
concept HasAnyRvalueHttpClientRequestContentViewAccessor = requires(T&& content) { std::move(content).withoutContent(); } || requires(T&& content) { std::move(content).borrowedBytes(); };

template <typename T>
concept HasStaleHttpClientRequestContentViewTuple = requires(T& request) {
    request.content.mode();
    request.content.value();
};

template <typename T>
concept HasHttpClientRequestContentViewBytesFactory = requires(T&& value) {
    { ruvia::HttpClientRequestContentView::bytes(std::forward<T>(value)) } -> std::same_as<ruvia::HttpClientRequestContentView>;
};

template <typename T>
concept HasTypedHttpOriginViewAccessors = requires(const T& origin) {
    { origin.scheme() } -> std::same_as<ruvia::HttpScheme>;
    { origin.host() } -> std::same_as<std::string_view>;
    { origin.port() } -> std::same_as<std::uint16_t>;
};

template <typename T>
concept HasHttpOriginRvalueHostFactory = requires(T&& host) { ruvia::HttpOriginView::http({.host = std::forward<T>(host)}); };

template <typename T>
concept HasHttpOriginLvalueHostFactory = requires(T& host) { ruvia::HttpOriginView::http({.host = host}); };

template <typename T>
concept HasHttpOriginPositionalFactories = requires(T host) { ruvia::HttpOriginView::http(host); } || requires(T host) { ruvia::HttpOriginView::http(host, std::uint16_t{80}); } || requires(T host) { ruvia::HttpOriginView::https(host); } || requires(T host) { ruvia::HttpOriginView::https(host, std::uint16_t{443}); };

template <typename T>
concept HasOutboundClientFacet = requires(T& value) { value.client(); };

template <typename T>
concept HasUseHttpClient = requires(T& value, ruvia::HttpOriginView origin) { value.useHttpClient(std::move(origin)); };

template <typename T>
concept HasRuntimeHttpClientMutation = requires(T& value, ruvia::HttpOriginView origin) {
    value.addHttpClient("upstream", std::move(origin));
    value.removeHttpClient("upstream");
};

#ifdef RUVIA_ENABLE_DATABASE
template <typename T>
concept HasDbHandleDefaultParams = requires(const T& handle) {
    handle.query(std::string_view{});
    handle.execute(std::string_view{});
    handle.queryStream(std::string_view{});
};

template <typename T>
concept HasDbHandleInitializerListParams = requires(const T& handle, std::initializer_list<ruvia::DbValue> params) {
    handle.query(std::string_view{}, params);
    handle.execute(std::string_view{}, params);
    handle.queryStream(std::string_view{}, params);
};

template <typename T>
concept HasDbTransactionDefaultParams = requires(T& transaction) {
    transaction.query(std::string_view{});
    transaction.execute(std::string_view{});
};

template <typename T>
concept HasDbTransactionInitializerListParams = requires(T& transaction, std::initializer_list<ruvia::DbValue> params) {
    transaction.query(std::string_view{}, params);
    transaction.execute(std::string_view{}, params);
};
#endif
template <typename T>
concept HasHttpClientResponseHeadHeadersField = requires { requires std::is_member_object_pointer_v<decltype(&T::headers)>; };

template <typename T>
concept HasHttpClientResponseHeadHeadersGetter = requires(const T& head) {
    { head.headers() } -> std::same_as<std::span<const ruvia::HttpClientResponseHeader>>;
};

template <typename T>
concept HasHttpClientResponseHeadBodyField = requires { requires std::is_member_object_pointer_v<decltype(&T::body)>; };

template <typename T>
concept HasHttpClientResponseHeadBodyGetter = requires(const T& head) {
    { head.body() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttpClientResponseHeaderNameField = requires { requires std::is_member_object_pointer_v<decltype(&T::name)>; };

template <typename T>
concept HasHttpClientResponseHeaderNameGetter = requires(const T& header) {
    { header.name() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttpClientResponseHeaderValueField = requires { requires std::is_member_object_pointer_v<decltype(&T::value)>; };

template <typename T>
concept HasHttpClientResponseHeaderValueGetter = requires(const T& header) {
    { header.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept ExposesAnyRvalueHttpClientOwnedView = requires(T&& value) { std::move(value).name(); } || requires(T&& value) { std::move(value).value(); } || requires(T&& value) { std::move(value).headers(); } || requires(T&& value) { std::move(value).body(); } || requires(T&& value) { std::move(value).method(); };

template <typename T>
concept HasCompleteType = requires { sizeof(T); };

template <typename T>
concept HasHttpHeaderViewPublicFields = requires(T& header) {
    header.name;
    header.value;
};

template <typename T>
concept HasHttpHeaderViewCanonicalReadAccessors = requires(const T& header) {
    { header.name() } -> std::same_as<std::string_view>;
    { header.value() } -> std::same_as<std::string_view>;
};
template <typename T>
concept HasMultipartPartPublicFields = requires(T& part) {
    part.name;
    part.filename;
    part.contentType;
    part.body;
};

template <typename T>
concept HasMultipartPartCanonicalReadAccessors = requires(const T& part) {
    { part.name() } -> std::same_as<std::string_view>;
    { part.filename() } -> std::same_as<std::string_view>;
    { part.contentType() } -> std::same_as<std::string_view>;
    { part.body() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasMultipartStreamPartPublicFields = requires(T& part) {
    part.name;
    part.filename;
    part.contentType;
    part.body;
    part.phase;
};

template <typename T>
concept HasMultipartStreamPartCanonicalReadAccessors = requires(const T& part) {
    { part.name() } -> std::same_as<std::string_view>;
    { part.filename() } -> std::same_as<std::string_view>;
    { part.contentType() } -> std::same_as<std::string_view>;
    { part.body() } -> std::same_as<std::string_view>;
    { part.phase() } -> std::same_as<ruvia::MultipartChunkPhase>;
};

template <typename T>
concept HasMultipartPollResultAccessors = requires(const T& result) {
    { result.needInput() } -> std::same_as<const ruvia::MultipartPollNeedInput*>;
    { result.part() } -> std::same_as<const ruvia::MultipartStreamPart*>;
    { result.done() } -> std::same_as<const ruvia::MultipartPollDone*>;
    { result.failure() } -> std::same_as<const ruvia::MultipartPollFailure*>;
};

template <typename T>
concept HasAnyRvalueMultipartPollAccessor = requires(T&& result) { std::move(result).needInput(); } || requires(T&& result) { std::move(result).part(); } || requires(T&& result) { std::move(result).done(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasWebSocketMessagePublicFields = requires(T& message) {
    message.opcode;
    message.payload;
};

template <typename T>
concept HasWebSocketMessageCanonicalReadAccessors = requires(const T& message) {
    { message.opcode() } -> std::same_as<ruvia::WebSocketOpcode>;
    { message.payload() } -> std::same_as<std::string_view>;
    { message.text() } -> std::same_as<bool>;
    { message.binary() } -> std::same_as<bool>;
};

template <typename T>
concept HasPositionalWebSocketHeartbeatFactory = requires { T::periodic(std::chrono::milliseconds{1}); } || requires { T::periodic(std::chrono::milliseconds{1}, std::chrono::milliseconds{1}); };

template <typename T>
concept HasWebSocketPublicCallbackConstructor = requires(void* target, typename T::Read read, typename T::Write write, typename T::Close close) { T(target, read, write, close); };

template <typename T>
concept HasWebSocketRuntimeCallbacks = requires {
    typename T::Read;
    typename T::Write;
    typename T::Close;
};

template <typename T>
concept HasWebSocketCloseOptions = requires(T& socket) {
    { socket.close(ruvia::WebSocketCloseOptions{}) } -> std::same_as<ruvia::ScopedOperation<void>>;
};

template <typename T>
concept HasPositionalWebSocketClose = requires(T& socket) { socket.close(std::uint16_t{1000}, std::string_view{}); };

template <typename T>
concept HasContextGetIfAlias = requires(T& context) { context.template getIf<std::string_view>(std::string_view{}); };

template <typename T>
concept HasArbitraryContextValueSet = requires(T& context) { context.set(std::string_view{}, CurrentUser{}); };

template <typename T>
concept HasArbitraryContextValueGet = requires(T& context) { context.template get<CurrentUser>(std::string_view{}); };

template <typename T>
concept HasContextVarIfAlias = requires(T& context) { context.template varIf<std::string_view>(std::string_view{}); };

template <typename T>
concept HasContextVarFacade = requires(T& context) { context.var(); };

template <typename T>
concept HasContextVarHasAlias = requires(T& context) { context.var().template has<CurrentUser>(std::string_view{}); };

template <typename T>
concept HasConstContextVarHasAlias = requires(const T& context) { context.var().template has<CurrentUser>(std::string_view{}); };

template <typename T>
concept HasContextJsonErrorAlias = requires(const T& context) { context.jsonError(std::uint16_t{}, std::string_view{}, std::string_view{}); };

template <typename T>
concept HasRequestCookiesAlias = requires(const T& request) { request.cookies(); };

template <typename T>
concept HasContextRequestQueryListAccessor = requires(const T& request) { request.query(); };

template <typename T>
concept HasContextRequestQueriesListAccessor = requires(const T& request) { request.queries(); };

template <typename T>
concept HasContextRequestCookieListAccessor = requires(const T& request) { request.cookie(); };

template <typename T>
concept HasContextRequestParamListAccessor = requires(const T& request) { request.param(); };

template <typename T>
concept HasRequestNameValueListGetAllAlias = requires(const T& list) { list.getAll(std::string_view{}); };

template <typename T>
concept HasRequestNameValueListSpanAlias = requires(const T& list) { list.span(); };

template <typename T>
concept HasRequestNameValueListKeysAllocator = requires(const T& list) { list.keys(); };

template <typename T>
concept HasRequestNameValueListValuesAllocator = requires(const T& list) { list.values(); };

template <typename T>
concept HasRequestNameValueListNamedValuesAllocator = requires(const T& list) { list.values(std::string_view{}); };

template <typename T>
concept HasRequestNameValueListNameIndexAlias = requires(const T& list) { list[std::string_view{}]; };

template <typename T>
concept HasRequestNameValueListHasAlias = requires(const T& list) { list.has(std::string_view{}); };

template <typename T>
concept HasRequestNameValueViewPublicFields = requires(T& entry) {
    entry.name;
    entry.value;
};

template <typename T>
concept HasRequestNameValueViewCanonicalAccessors = requires(const T& entry) {
    { entry.name() } -> std::same_as<std::string_view>;
    { entry.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasRequestNameValueListPublicMutators = requires(T& list) {
    list.reserve(std::size_t{1});
    list.push_back(typename T::value_type{});
    list.emplace_back(typename T::value_type{});
};

template <typename T>
concept HasRequestNameValueListMutableAccess = requires(T& list) {
    { list.begin() } -> std::same_as<typename T::iterator>;
    { list.end() } -> std::same_as<typename T::iterator>;
    { list.data() } -> std::same_as<ruvia::RequestNameValueView*>;
    { list[std::size_t{}] } -> std::same_as<ruvia::RequestNameValueView&>;
};

template <typename T>
concept HasRequestNameValueListMutableIteratorAlias = requires { typename T::iterator; };

template <typename T>
concept HasRequestNameValueListCanonicalAccessors = requires(const T& list) {
    { list.get(std::string_view{}) } -> std::same_as<std::optional<std::string_view>>;
    { list.count(std::string_view{}) } -> std::same_as<std::size_t>;
    { list.entries() } -> std::same_as<std::span<const ruvia::RequestNameValueView>>;
};

template <typename T>
concept ExposesAnyRvalueRequestNameValueListBorrow = requires { std::declval<const T&&>().begin(); } || requires { std::declval<const T&&>().cbegin(); } || requires { std::declval<const T&&>().end(); } || requires { std::declval<const T&&>().cend(); } || requires { std::declval<const T&&>().data(); } || requires { std::declval<const T&&>()[std::size_t{}]; } || requires { std::declval<const T&&>().entries(); };

template <typename T>
concept ExposesAnyRvalueValidationIssueBorrow = requires { std::declval<const T&&>().field(); } || requires { std::declval<const T&&>().code(); } || requires { std::declval<const T&&>().message(); };

template <typename T>
concept ExposesAnyRvalueValidationErrorBorrow = requires { std::declval<const T&&>().issues(); } || requires { std::declval<const T&&>().info(); };

template <typename T>
concept ExposesRvalueValidatorIssues = requires { std::declval<const T&&>().issues(); };

template <typename T>
concept AcceptsAnyRvalueValidatorMutation = requires { std::declval<T&&>().add("field", "code", "message"); } || requires(const std::optional<std::string>& value) { std::declval<T&&>().required(value, "field"); } || requires(const std::optional<std::string>& value) { std::declval<T&&>().minLength(value, "field", std::size_t{1}); } || requires(const std::optional<std::string>& value) { std::declval<T&&>().maxLength(value, "field", std::size_t{1}); } || requires(const std::optional<int>& value) { std::declval<T&&>().range(value, "field", 0, 1); } || requires(const std::optional<std::string>& value) { std::declval<T&&>().oneOf(value, "field", {"value"}); };

template <typename Validator>
concept HasValidatorThrowIfInvalidPositional = requires(Validator& validator) { validator.throwIfInvalid(ruvia::http_status::kBadRequest, std::string_view{}, std::string_view{}); };

template <typename Issues>
concept HasValidationErrorPositionalConstructor = requires(Issues&& issues) { ruvia::ValidationError(std::forward<Issues>(issues), ruvia::http_status::kBadRequest, std::string_view{}, std::string_view{}); };

template <typename String>
concept AcceptsAnyRvalueValidationErrorOptionText = requires(String&& value) { ruvia::ValidationErrorOptions{.code = std::forward<String>(value)}; } || requires(String&& value) { ruvia::ValidationErrorOptions{.message = std::forward<String>(value)}; };

template <typename String>
concept AcceptsLvalueValidationErrorOptionText = requires(String& value) { ruvia::ValidationErrorOptions{.code = value, .message = value}; };

template <typename T>
concept ExposesRvalueHttpErrorInfo = requires { std::declval<const T&&>().info(); };

template <typename T>
concept HasAppErrorHandlerSetterAlias = requires(T& app) { app.setErrorHandler(static_cast<ruvia::HttpErrorHandler>(nullptr)); };

template <typename T>
concept HasAppNotFoundHandlerSetterAlias = requires(T& app) { app.setNotFoundHandler(static_cast<ruvia::HttpNotFoundHandler>(nullptr)); };

template <typename T>
concept HasScopedAppErrorHandlerPositional = requires(T& app) { app.onError(std::string_view{}, static_cast<ruvia::HttpErrorHandler>(nullptr)); };

template <typename T>
concept HasScopedAppNotFoundHandlerPositional = requires(T& app) { app.onNotFound(std::string_view{}, static_cast<ruvia::HttpNotFoundHandler>(nullptr)); };

template <typename T>
concept HasScopedAppErrorHandlerOptions = requires(T& app) {
    { app.onError(ruvia::ScopedErrorHandlerOptions{.prefix = "/api", .handler = static_cast<ruvia::HttpErrorHandler>(nullptr)}) } -> std::same_as<T&>;
};

template <typename T>
concept HasScopedAppNotFoundHandlerOptions = requires(T& app) {
    { app.onNotFound(ruvia::ScopedNotFoundHandlerOptions{.prefix = "/api", .handler = static_cast<ruvia::HttpNotFoundHandler>(nullptr)}) } -> std::same_as<T&>;
};

template <typename String>
concept AcceptsAnyRvalueScopedErrorHandlerPrefix = requires(String&& value) { ruvia::ScopedErrorHandlerOptions{.prefix = std::forward<String>(value), .handler = static_cast<ruvia::HttpErrorHandler>(nullptr)}; };

template <typename String>
concept AcceptsAnyRvalueScopedNotFoundHandlerPrefix = requires(String&& value) { ruvia::ScopedNotFoundHandlerOptions{.prefix = std::forward<String>(value), .handler = static_cast<ruvia::HttpNotFoundHandler>(nullptr)}; };

template <typename String>
concept AcceptsLvalueScopedFallbackPrefix = requires(String& value) {
    ruvia::ScopedErrorHandlerOptions{.prefix = value, .handler = static_cast<ruvia::HttpErrorHandler>(nullptr)};
    ruvia::ScopedNotFoundHandlerOptions{.prefix = value, .handler = static_cast<ruvia::HttpNotFoundHandler>(nullptr)};
};

template <typename T>
concept HasAppSetRateLimitAlias = requires(T& app) { app.setRateLimit(std::size_t{1}, std::chrono::milliseconds{1000}); };

template <typename T>
concept HasAppInstanceAlias = requires { T::instance(); };

template <typename T>
concept HasWebWorkerCorePostEscape = requires(const T& worker) { worker.core(); };

template <typename T>
concept HasRateLimitSlotCount = requires(T& rule) { rule.slotCount; };

template <typename T>
concept HasPositionalFixedWindowRateLimitFactory = requires { T::fixedWindow(std::size_t{1}, std::chrono::milliseconds{1000}); } || requires { T::fixedWindow(std::size_t{1}, std::chrono::milliseconds{1000}, ruvia::RateLimitOverflowPolicy::kDeny); };

template <typename T>
concept HasAppUseMiddlewareTemplate = requires { &T::template use<AppUseProbeMiddleware>; };

template <typename T>
concept HasAppUseAtScopeOptions = requires(T& app) {
    { app.template useAt<AppUseProbeMiddleware>(ruvia::MiddlewareScopeOptions{.prefix = "/api"}) } -> std::same_as<T&>;
};

template <typename T>
concept HasAppUseAtPositional = requires(T& app) {
    { app.template useAt<AppUseProbeMiddleware>(std::string_view{}) } -> std::same_as<T&>;
};

template <typename String>
concept AcceptsAnyRvalueMiddlewareScopePrefix = requires(String&& value) { ruvia::MiddlewareScopeOptions{.prefix = std::forward<String>(value)}; };

template <typename String>
concept AcceptsLvalueMiddlewareScopePrefix = requires(String& value) { ruvia::MiddlewareScopeOptions{.prefix = value}; };

template <typename T>
concept HasControllerRouteBuilderPublicRegisterRoute = requires(const T& builder, ruvia::detail::ControllerRouteHandler handler) { builder.registerRoute(ruvia::HttpKnownMethod::kGet, std::string_view{"/"}, handler, ruvia::detail::RequestBodyMode::kBuffered, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{}); };

template <typename T>
concept HasControllerRouteBuilderPublicRegisterResponseStreamRoute = requires(const T& builder, ruvia::detail::ControllerRouteStreamHandler handler) { builder.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet, std::string_view{"/"}, handler, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{}); };

template <typename T>
concept HasControllerRouteBuilderPublicCreateScope = requires(const T& builder) { builder.createScope(std::string_view{"/"}); };

template <typename T>
concept HasControllerMiddlewareDescriptorPublicCallbackConstructor = requires(typename T::Invoke invoke, typename T::Create create, typename T::Destroy destroy) { T(invoke, create, destroy); };

template <typename T>
concept HasControllerStorePublicMutators = requires(T& store) {
    store.reserve(std::size_t{1});
    store.template emplace<int>();
};

template <typename T>
concept HasControllerStorePublicSize = requires(const T& store) {
    { store.size() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasControllerPublicGroupPrefix = requires { T::ruviaControllerGroupPrefix(); };

template <typename T>
concept HasControllerPublicGroupMiddlewares = requires { T::ruviaControllerGroupMiddlewares(); };

template <typename T>
concept HasControllerPublicRegisterRoutes = requires(T& controller, ruvia::detail::Router& router) { controller.registerRoutes(router); };

template <typename T>
concept HasControllerPublicRegistrationState = requires { T::ruviaControllerRegistered_; };

template <typename T>
concept HasControllerRegistrationAccessPublicHooks = requires {
    T::groupPrefix();
    T::groupMiddlewares();
};

template <typename T>
concept HasDbRowPublicMutators = requires(T& row, ruvia::DbField field) {
    row.reserve(std::size_t{1});
    row.push_back(std::move(field));
    row.emplace_back(nullptr, std::pmr::get_default_resource());
    row.emplace_back(std::string_view{}, std::pmr::get_default_resource());
};

template <typename T>
concept HasDbValuePmrStringConstructor = requires(std::pmr::string value) { T(std::move(value)); };

template <typename String>
concept AcceptsTemporaryDbValueText = requires(String&& value) { ruvia::DbValue(std::forward<String>(value)); };

template <typename String>
concept AcceptsLvalueDbValueText = requires(String& value) { ruvia::DbValue(value); };

template <typename String, typename Migration = ruvia::DbMigration>
concept AcceptsAnyTemporaryDbMigrationText = requires(String&& value) { ruvia::DbMigrationOptions{.id = std::forward<String>(value), .sql = "SELECT 1"}; } || requires(String&& value) { ruvia::DbMigrationOptions{.id = "migration", .sql = std::forward<String>(value)}; };

template <typename String>
concept AcceptsLvalueDbMigrationText = requires(String& value) { ruvia::DbMigration{{.id = value, .sql = value}}; };

template <typename Migration = ruvia::DbMigration>
concept HasPositionalDbMigrationConstructor = requires { Migration(ruvia::BorrowedText{"migration"}, ruvia::BorrowedText{"SELECT 1"}); };

template <typename T>
concept HasDbMigrationTextAccessors = requires(const T& migration) {
    { migration.id() } -> std::same_as<std::string_view>;
    { migration.sql() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasDbRowCanonicalReadAccessors = requires(const T& row) {
    { row.empty() } -> std::same_as<bool>;
    { row.size() } -> std::same_as<std::size_t>;
    { row[std::size_t{}] } -> std::same_as<const ruvia::DbField&>;
    { row[std::string_view{}] } -> std::same_as<const ruvia::DbField&>;
    { row.begin() } -> std::same_as<const ruvia::DbField*>;
    { row.end() } -> std::same_as<const ruvia::DbField*>;
};

template <typename T>
concept HasDbFieldCanonicalReadAccessors = requires(const T& field) {
    { field.value() } -> std::same_as<std::optional<std::string_view>>;
    { field.template as<std::int64_t>() } -> std::same_as<std::optional<std::int64_t>>;
    { field.template as<std::string>() } -> std::same_as<std::optional<std::string>>;
};

template <typename T>
concept HasRowsCanonicalReadAccessors = requires(const T& result) {
    { result.empty() } -> std::same_as<bool>;
    { result.size() } -> std::same_as<std::size_t>;
    { result[std::size_t{}] } -> std::same_as<const ruvia::DbRow&>;
    { result.begin() } -> std::same_as<const ruvia::DbRow*>;
    { result.end() } -> std::same_as<const ruvia::DbRow*>;
};

template <typename T>
concept HasLegacyDbFieldText = requires(const T& field) { field.text(); };

template <typename T>
concept HasLegacyDbRowsSpan = requires(const T& result) { result.rows(); };

template <typename T>
concept HasExecResultCanonicalReadAccessors = requires(const T& result) {
    { result.affectedRows() } -> std::same_as<std::uint64_t>;
    { result.lastInsertId() } -> std::same_as<std::optional<std::uint64_t>>;
};

template <typename T>
concept HasDbMigrationReportCanonicalReadAccessors = requires(const T& report) {
    { report.applied() } -> std::same_as<std::span<const std::pmr::string>>;
    { report.skipped() } -> std::same_as<std::span<const std::pmr::string>>;
    { report.changed() } -> std::same_as<bool>;
};

#ifdef RUVIA_ENABLE_JWT
template <typename T>
concept HasJwtClaimPublicFields = requires(T& claim) {
    claim.name;
    claim.value;
};

template <typename T>
concept HasJwtClaimPositionalConstructor = requires { T(std::string_view{}, std::string_view{}); };

template <typename T>
concept HasJwtClaimResourceConstructor = requires(std::pmr::memory_resource* resource) { T(std::string_view{}, std::string_view{}, resource); };

template <typename T>
concept HasJwtSignResourceArgument = requires(const T& options, std::pmr::memory_resource* resource) { ruvia::jwtSign(options, resource); };

template <typename T>
concept HasJwtVerifyPositionalToken = requires(std::string_view token, const T& options) { ruvia::jwtVerify(token, options); } || requires(std::string_view token, const T& options, std::pmr::memory_resource* resource) { ruvia::jwtVerify(token, options, resource); };

template <typename Token>
concept HasJwtDecodeUnverifiedPositionalToken = requires(Token&& token) { ruvia::jwtDecodeUnverified(std::forward<Token>(token)); };

template <typename String>
concept AcceptsAnyRvalueJwtClaimOptionText = requires(String&& value) { ruvia::JwtClaimOptions{.name = std::forward<String>(value), .value = "value"}; } || requires(String&& value) { ruvia::JwtClaimOptions{.name = "name", .value = std::forward<String>(value)}; };

template <typename String>
concept AcceptsAnyRvalueJwtTokenOptionText = requires(String&& value) { ruvia::JwtVerifyOptions{.token = std::forward<String>(value)}; } || requires(String&& value) { ruvia::JwtDecodeUnverifiedOptions{.token = std::forward<String>(value)}; };

template <typename String>
concept AcceptsAnyRvalueJwtSecretOptionText = requires(String&& value) { ruvia::JwtSignOptions{.secret = std::forward<String>(value)}; } || requires(String&& value) { ruvia::JwtVerifyOptions{.secret = std::forward<String>(value)}; };

template <typename String>
concept AcceptsLvalueJwtSecretOptionText = requires(String& value) {
    ruvia::JwtSignOptions{.secret = value};
    ruvia::JwtVerifyOptions{.secret = value};
};

template <typename String>
concept AcceptsLvalueJwtClaimOptionText = requires(String& value) { ruvia::JwtClaimOptions{.name = value, .value = value}; };

template <typename T>
concept HasJwtClaimCanonicalReadAccessors = requires(const T& claim) {
    { claim.name() } -> std::same_as<std::string_view>;
    { claim.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasJwtPayloadCanonicalReadAccessors = requires(const T& payload) {
    { payload.issuer() } -> std::same_as<std::string_view>;
    { payload.subject() } -> std::same_as<std::string_view>;
    { payload.audience() } -> std::same_as<std::string_view>;
    { payload.id() } -> std::same_as<std::string_view>;
    { payload.expiresAt() } -> std::same_as<std::optional<std::chrono::system_clock::time_point>>;
    { payload.notBefore() } -> std::same_as<std::optional<std::chrono::system_clock::time_point>>;
    { payload.issuedAt() } -> std::same_as<std::optional<std::chrono::system_clock::time_point>>;
    { payload.claims() } -> std::same_as<std::span<const ruvia::JwtClaim>>;
    { payload.claim(std::string_view{}) } -> std::same_as<std::optional<std::string_view>>;
};
#endif

template <typename T>
concept HasRedisValuePublicFactories = requires(std::pmr::memory_resource* resource, std::pmr::vector<ruvia::RedisValue> values) {
    T::nullValue(resource);
    T::stringValue(std::string_view{}, resource);
    T::errorValue(std::string_view{}, resource);
    T::integerValue(std::int64_t{0}, resource);
    T::arrayValue(std::move(values), resource);
};

template <typename T>
concept HasRedisValueCanonicalReadAccessors = requires(const T& value) {
    { value.kind() } -> std::same_as<ruvia::RedisValue::Kind>;
    { value.null() } -> std::same_as<bool>;
    { value.string() } -> std::same_as<std::string_view>;
    { value.error() } -> std::same_as<std::string_view>;
    { value.integer() } -> std::same_as<std::int64_t>;
    { value.array() } -> std::same_as<std::span<const ruvia::RedisValue>>;
};

template <typename T>
concept HasRedisErrorMessageAlias = requires(const T& error) { error.message(); };

template <typename T>
concept HasRvalueRedisValueBorrow = requires(T&& value) { std::move(value).string(); } || requires(T&& value) { std::move(value).error(); } || requires(T&& value) { std::move(value).array(); };

template <typename T>
concept HasRedisKeyValuePublicFields = requires(T& value) {
    value.key;
    value.value;
};

template <typename T>
concept HasRedisKeyValueCanonicalReadAccessors = requires(const T& value) {
    { value.key() } -> std::same_as<std::string_view>;
    { value.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasRedisScoredValuePublicFields = requires(T& value) {
    value.value;
    value.score;
};

template <typename T>
concept HasRedisScoredValueCanonicalReadAccessors = requires(const T& value) {
    { value.value() } -> std::same_as<std::string_view>;
    { value.score() } -> std::same_as<double>;
};

template <typename T>
concept HasRedisScanResultPublicFields = requires(T& result) {
    result.cursor;
    result.values;
};

template <typename T>
concept HasRedisScanResultCanonicalReadAccessors = requires(const T& result) {
    { result.done() } -> std::same_as<bool>;
    { result.nextCursor() } -> std::same_as<std::optional<ruvia::RedisScanCursor>>;
    { result.values() } -> std::same_as<std::span<const std::pmr::string>>;
};

template <typename T>
concept HasRedisHashScanResultPublicFields = requires(T& result) {
    result.cursor;
    result.entries;
};

template <typename T>
concept HasRedisHashScanResultCanonicalReadAccessors = requires(const T& result) {
    { result.done() } -> std::same_as<bool>;
    { result.nextCursor() } -> std::same_as<std::optional<ruvia::RedisScanCursor>>;
    { result.entries() } -> std::same_as<std::span<const ruvia::RedisKeyValue>>;
};

template <typename T>
concept HasRedisZScanResultPublicFields = requires(T& result) {
    result.cursor;
    result.entries;
};

template <typename T>
concept HasRedisZScanResultCanonicalReadAccessors = requires(const T& result) {
    { result.done() } -> std::same_as<bool>;
    { result.nextCursor() } -> std::same_as<std::optional<ruvia::RedisScanCursor>>;
    { result.entries() } -> std::same_as<std::span<const ruvia::RedisScoredValue>>;
};

template <typename T>
concept HasAppRateLimitConfig = requires(T& app) {
    { app.rateLimit({.rule = {.maxRequests = std::size_t{1}, .window = std::chrono::seconds(1)}, .capacityPerWorker = 1024}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppDefaultRateLimitPerWorkerTupleSetter = requires(T& app) {
    { app.setRateLimit(std::size_t{1}, std::chrono::milliseconds{1000}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppRateLimitDisable = requires(T& app) {
    { app.rateLimit(nullptr) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppRateLimitCapacityPerWorkerSetter = requires(T& app) {
    { app.setRateLimitCapacityPerWorker(std::size_t{1024}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppRateLimitSlotsPerWorkerSetter = requires(T& app) {
    { app.setRateLimitSlotsPerWorker(std::size_t{1024}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppWorkerCountSetter = requires(T& app) {
    { app.setWorkerCount(std::size_t{2}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppProcessSignalHandlersSetter = requires(T& app) {
    { app.setProcessSignalHandlers(ruvia::ProcessSignalHandlerPolicy::kInstall) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppProcessSignalHandlersBooleanSetter = requires(T& app) {
    { app.setProcessSignalHandlers(true) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppSignalShutdownSetter = requires(T& app) {
    { app.setSignalShutdown(true) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasLegacyAppThreadNumSetter = requires(T& app) { app.setThreadNum(std::size_t{2}); };

template <typename T>
concept HasLegacyAppGlobalRateLimitSetter = requires(T& app) { app.setGlobalRateLimit(std::nullopt); };

template <typename T>
concept HasAppServerConfig = requires(T& app) {
    { app.server(ruvia::ServerConfig{}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppDocumentRootConfig = requires(T& app) {
    { app.documentRoot(ruvia::DocumentRootConfig{}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasCanonicalOptionalAppConfigDisable = requires(T& app) {
    { app.deadline(nullptr) } -> std::same_as<ruvia::App&>;
    { app.compression(nullptr) } -> std::same_as<ruvia::App&>;
    { app.cors(nullptr) } -> std::same_as<ruvia::App&>;
    { app.documentRoot(nullptr) } -> std::same_as<ruvia::App&>;
    { app.blockingPool(nullptr) } -> std::same_as<ruvia::App&>;
    { app.rateLimit(nullptr) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasDocumentRootRefreshOptions = requires(T& options) { options.refreshInterval; };

template <typename T>
concept HasDocumentRootRefreshMode = requires(T& options) { options.refreshMode; };

template <typename T>
concept HasDocumentRootLiveReload = requires(T& options) { options.liveReload; };

template <typename T>
concept HasDocumentRootPrecompressionFields = requires(T& config) {
    { config.precompressGzip } -> std::same_as<bool&>;
    { config.precompressBrotli } -> std::same_as<bool&>;
    { config.precompressZstd } -> std::same_as<bool&>;
    { config.precompressMinBytes } -> std::same_as<std::size_t&>;
    { config.precompressMaxBytes } -> std::same_as<std::size_t&>;
};

template <typename T>
concept HasNestedDocumentRootPrecompression = requires(T& config) { config.runtime.precompression; };

template <typename T>
concept HasDocumentRootRuntimePolicy = requires(T& config) {
    { config.runtime } -> std::same_as<ruvia::DocumentRootRuntimeConfig&>;
};

template <typename T>
concept HasStaticRootRangeRequestPolicy = requires(T& options) {
    { options.rangeRequests } -> std::same_as<ruvia::StaticRangeRequestPolicy&>;
};

template <typename T>
concept HasStaticRootResponseValidatorPolicy = requires(T& options) {
    { options.responseValidators } -> std::same_as<ruvia::StaticResponseValidatorPolicy&>;
};

template <typename T>
concept HasStaticRootDotfilePolicy = requires(T& options) {
    { options.dotfiles } -> std::same_as<ruvia::StaticDotfilePolicy&>;
};

template <typename T>
concept HasStaticRootEnableRanges = requires(T& options) { options.enableRanges; };

template <typename T>
concept HasStaticRootEnableValidators = requires(T& options) { options.enableValidators; };

template <typename T>
concept HasStaticRootServeDotfilesBoolean = requires(T& options) { options.serveDotfiles = true; };

template <typename T>
concept HasDocumentRootBindingAccessor = requires(const T& documentRoot) {
    { documentRoot.binding() } -> std::same_as<ruvia::detail::DocumentRootBinding>;
};

template <typename T>
concept HasSplitDocumentRootDispatch = requires(T& routes, const ruvia::HttpRequest& request, const ruvia::detail::RouteResolution& resolution, ruvia::RequestMemory& memory, const ruvia::StaticRoot* root, const ruvia::DocumentRootRuntimeConfig* runtimeOptions, ruvia::detail::ContextServices& services) { routes.dispatchBufferedResponse(request, resolution, memory, root, runtimeOptions, services); };

template <typename T>
concept HasRawDocumentRootDispatch = requires(T& routes, const ruvia::HttpRequest& request, const ruvia::detail::RouteResolution& resolution, ruvia::RequestMemory& memory, const ruvia::StaticRoot* root) { routes.dispatchBufferedResponse(request, resolution, memory, root); };

template <typename T>
concept HasCanonicalBufferedDispatch = requires(T& routes, const ruvia::HttpRequest& request, const ruvia::detail::RouteResolution& resolution, ruvia::RequestMemory& memory, ruvia::detail::DocumentRootBinding documentRoot, ruvia::detail::ContextServices& services) {
    { routes.dispatchBufferedResponse(request, resolution, memory, std::move(documentRoot), services) } -> std::same_as<ruvia::Task<ruvia::HttpResponse>>;
};

template <typename T>
concept HasContextlessDispatch = requires(T& routes, const ruvia::HttpRequest& request, ruvia::RequestMemory& memory) { routes.dispatch(request, memory); };

template <typename T>
concept HasContextlessBufferedDispatch = requires(T& routes, const ruvia::HttpRequest& request, const ruvia::detail::RouteResolution& resolution, ruvia::RequestMemory& memory, ruvia::detail::DocumentRootBinding documentRoot) { routes.dispatchBufferedResponse(request, resolution, memory, std::move(documentRoot)); };

template <typename T>
concept HasRawResponseCompressionCoding = requires(const ruvia::HttpRequest& request, T coding, ruvia::HttpResponse& response, const ruvia::detail::HttpServerOptions& options) { ruvia::detail::prepareBufferedHttpResponse(request, coding, response, options); };

template <typename T>
concept HasTypedResponseCompressionSelection = requires(const ruvia::HttpRequest& request, const T& policy, ruvia::HttpResponse& response, const ruvia::detail::HttpServerOptions& options) {
    { ruvia::detail::prepareBufferedHttpResponse(request, policy, response, options) } -> std::same_as<ruvia::detail::HttpBufferedResponsePreparation>;
};

template <typename T>
concept HasRawApplyResponseCompressionCoding = requires(T coding, ruvia::HttpKnownMethod method, ruvia::HttpResponse& response, const ruvia::CompressionConfig& options) { ruvia::detail::applyResponseCompression(coding, method, response, options); };

template <typename T>
concept HasTypedApplyResponseCompressionSelection = requires(const T& selection, ruvia::HttpKnownMethod method, ruvia::HttpResponse& response, const ruvia::CompressionConfig& options) {
    { ruvia::detail::applyResponseCompression(selection, method, response, options) } -> std::same_as<ruvia::detail::HttpResponseCompressionResult>;
};

template <typename T>
concept HasTypedResponseCompressionPreflight = requires(const T& selection, ruvia::HttpKnownMethod method, const ruvia::HttpResponse& response) {
    { ruvia::detail::httpResponseCompressionEligibility(selection, method, response, ruvia::detail::ResponseStreamKind::kGeneric) } -> std::same_as<ruvia::detail::HttpResponseCompressionEligibility>;
};

template <typename T>
concept HasAppDocumentRootPathSetter = requires(T& app, const std::filesystem::path& root) {
    { app.setDocumentRoot(root) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppListenAddressSetter = requires(T& app) {
    { app.setListenAddress(std::string_view{}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppListenAddressPortSetter = requires(T& app) {
    { app.setListenAddress(std::string_view{}, std::uint16_t{8080}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppListenerSetter = requires(T& app) {
    { app.listen({.address = "127.0.0.1", .http = 8080}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAccessLogRecordPublicFields = requires(T& record) {
    record.method;
    record.path;
    record.remoteAddress;
    record.status;
    record.durationMicros;
    record.http2;
};

template <typename T>
concept HasCanonicalAccessLogCallback = requires(T& app, ruvia::AccessLogCallback callback) {
    { app.onAccess(callback) } -> std::same_as<ruvia::App&>;
};

struct AccessLogListener final {
    void operator()(const ruvia::AccessLogRecord&) noexcept;
};

template <typename T>
concept HasBoundAccessLogCallback = requires(AccessLogListener& listener) {
    { T::bind(listener) } -> std::same_as<T>;
};

template <typename T>
concept HasPublicCallbackBorrow = requires(const T& callback) { callback.borrow(); };

template <typename T>
concept HasAccessLogRecordCanonicalReadAccessors = requires(const T& record) {
    { record.method() } -> std::same_as<std::string_view>;
    { record.knownMethod() } -> std::same_as<ruvia::HttpKnownMethod>;
    { record.path() } -> std::same_as<std::string_view>;
    { record.remoteAddress() } -> std::same_as<std::string_view>;
    { record.status() } -> std::same_as<ruvia::HttpStatusCode>;
    { record.durationMicros() } -> std::same_as<std::uint64_t>;
    { record.protocolVersion() } -> std::same_as<ruvia::HttpProtocolVersion>;
};

template <typename T>
concept HasLegacyAccessLogHttp2Flag = requires(const T& record) {
    { record.http2() } -> std::same_as<bool>;
};

template <typename T>
concept HasValidationIssuePublicFields = requires(T& issue) {
    issue.field;
    issue.code;
    issue.message;
};

template <typename T>
concept HasValidationIssueCanonicalReadAccessors = requires(const T& issue) {
    { issue.field() } -> std::same_as<std::string_view>;
    { issue.code() } -> std::same_as<std::string_view>;
    { issue.message() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttpErrorInfoPublicFields = requires(T& info) {
    info.statusCode;
    info.statusText;
    info.code;
    info.message;
    info.validationIssues;
};

template <typename T>
concept HasHttpErrorInfoCanonicalReadAccessors = requires(const T& info) {
    { info.status() } -> std::same_as<ruvia::HttpStatusCode>;
    { info.statusText() } -> std::same_as<std::string_view>;
    { info.code() } -> std::same_as<std::string_view>;
    { info.message() } -> std::same_as<std::string_view>;
    { info.validationIssues() } -> std::same_as<std::span<const ruvia::ValidationIssue>>;
};

template <typename Status>
concept HasHttpErrorInfoPositionalConstructor = requires(Status status) { ruvia::HttpErrorInfo(status, std::string_view{}, std::string_view{}); };

template <typename Status>
concept HasHttpErrorPositionalConstructor = requires(Status status) { ruvia::HttpError(status, std::string_view{}, std::string_view{}); };

template <typename Context>
concept HasContextErrorPositional = requires(Context& context) { context.error(ruvia::http_status::kBadRequest, std::string_view{}, std::string_view{}); };

template <typename Context>
concept HasContextRedirectPositional = requires(Context& context) { context.redirect(std::string_view{}, ruvia::http_status::kFound); };

template <typename String>
concept AcceptsTemporaryRedirectLocation = requires(String&& value) { ruvia::RedirectResponseOptions{.location = std::forward<String>(value)}; };

template <typename String>
concept AcceptsLvalueRedirectLocation = requires(String& value) { ruvia::RedirectResponseOptions{.location = value}; };

template <typename T>
concept HasCsrfProtectionPositionalConstructor = requires { T(ruvia::BorrowedText{"XSRF-TOKEN"}, ruvia::BorrowedText{"X-XSRF-TOKEN"}); };

template <typename String>
concept AcceptsAnyRvalueCsrfProtectionOptionText = requires(String&& value) { ruvia::CsrfProtectionConfig{.cookieName = std::forward<String>(value)}; } || requires(String&& value) { ruvia::CsrfProtectionConfig{.headerName = std::forward<String>(value)}; };

template <typename String>
concept AcceptsLvalueCsrfProtectionOptionText = requires(String& value) { ruvia::CsrfProtectionConfig{.cookieName = value, .headerName = value}; };

template <typename String>
concept AcceptsAnyRvalueHttpErrorInfoOptionText = requires(String&& value) { ruvia::HttpErrorInfo({.code = std::forward<String>(value)}); } || requires(String&& value) { ruvia::HttpErrorInfo({.message = std::forward<String>(value)}); } || requires(String&& value) { ruvia::HttpErrorInfo({.statusText = std::forward<String>(value)}); };

template <typename String>
concept AcceptsLvalueHttpErrorInfoOptionText = requires(String& value) { ruvia::HttpErrorInfo({.status = ruvia::http_status::kBadRequest, .code = value, .message = value, .statusText = value}); };

template <typename Issues>
concept AcceptsRvalueHttpErrorInfoOptionIssues = requires(Issues&& issues) { ruvia::HttpErrorInfo({.status = ruvia::http_status::kBadRequest, .validationIssues = std::forward<Issues>(issues)}); };

template <typename Issues>
concept AcceptsLvalueHttpErrorInfoOptionIssues = requires(Issues& issues) { ruvia::HttpErrorInfo({.status = ruvia::http_status::kBadRequest, .validationIssues = issues}); };

template <typename T>
concept HasRawHttpErrorDetailsJson = requires(const T& info) {
    { info.detailsJson() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasStaleHttpParseTupleAccessors = requires(const T& result) {
    result.status();
    result.error();
    result.request();
    result.consumedBytes();
};

template <typename T>
concept HasHttp1DiscriminatedParseAccessors = requires(const T& result) {
    { result.needMore() } -> std::same_as<const ruvia::Http1RequestNeedMore*>;
    { result.parsed() } -> std::same_as<const ruvia::Http1ParsedRequest*>;
    { result.failure() } -> std::same_as<const ruvia::Http1RequestParseFailure*>;
};

template <typename T>
concept HasAnyRvalueHttp1RequestParseAccessor = requires(T&& result) { std::move(result).needMore(); } || requires(T&& result) { std::move(result).parsed(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasHttp1RequestBodyPlanAlternatives = requires(const T& plan) {
    { plan.withoutBody() } -> std::same_as<const ruvia::Http1RequestWithoutBody*>;
    { plan.knownLength() } -> std::same_as<const ruvia::Http1KnownLengthRequestBody*>;
    { plan.chunked() } -> std::same_as<const ruvia::Http1ChunkedRequestBody*>;
};

template <typename T>
concept HasStaleHttp1RequestFramingAccessor = requires(const T& plan) { plan.mode(); };

template <typename T>
concept HasHttp1RequestBodyContentLength = requires(const T& framing) {
    { framing.contentLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasHttp1RequestBodyTransferCodings = requires(const T& framing) {
    { framing.transferCodings() } -> std::same_as<ruvia::HttpTransferCodings>;
} && requires(const T&& framing) {
    { std::move(framing).transferCodings() } -> std::same_as<ruvia::HttpTransferCodings>;
};

template <typename T>
concept HasPublicHttp1RequestBodyPlanFactories = requires {
    T::makeWithoutBody();
    T::makeKnownLength(std::size_t{});
    T::makeChunked(ruvia::HttpTransferCodings{});
};

template <typename T>
concept HasHttp1ClientDiscriminatedParseAccessors = requires(const T& result) {
    { result.needMore() } -> std::same_as<const ruvia::Http1ClientResponseNeedMore*>;
    { result.parsed() } -> std::same_as<const ruvia::Http1ParsedClientResponseHead*>;
    { result.failure() } -> std::same_as<const ruvia::Http1ClientResponseParseFailure*>;
    { result.terminal() } -> std::same_as<const ruvia::Http1ClientResponseParseTerminal*>;
};

template <typename T>
concept HasAnyRvalueHttp1ClientResponseParseAccessor = requires(T&& result) { std::move(result).needMore(); } || requires(T&& result) { std::move(result).parsed(); } || requires(const T&& result) { std::move(result).parsed(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasHttp1ClientRequestPrepareAccessors = requires(const T& result) {
    { result.bufferTooSmall() } -> std::same_as<const ruvia::Http1ClientRequestBufferTooSmall*>;
    { result.prepared() } -> std::same_as<const ruvia::PreparedHttp1ClientRequest*>;
    { result.failure() } -> std::same_as<const ruvia::Http1ClientRequestPrepareFailure*>;
};

template <typename T>
concept HasResultKindDiscriminator = requires(const T& result) { result.kind(); };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestPrepareAccessor = requires(T&& result) { std::move(result).bufferTooSmall(); } || requires(T&& result) { std::move(result).prepared(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasHttp1ClientPreparedContentPlan = requires(const T& prepared) {
    { prepared.contentPlan().withoutContent() } -> std::same_as<const ruvia::Http1ClientRequestWithoutContent*>;
    { prepared.contentPlan().immediate() } -> std::same_as<const ruvia::Http1ClientImmediateRequestContent*>;
    { prepared.contentPlan().continueGated() } -> std::same_as<const ruvia::Http1ClientContinueGatedRequestContent*>;
};

template <typename T>
concept HasAnyRvalueHttp1ClientRequestContentPlanAccessor = requires(T&& plan) { std::move(plan).withoutContent(); } || requires(T&& plan) { std::move(plan).immediate(); } || requires(T&& plan) { std::move(plan).continueGated(); };

template <typename T>
concept HasLegacyHttp1ClientExpectationAlternatives = requires(const T& policy) { policy.noExpectation(); } || requires(const T& policy) { policy.continueExpectation(); };

template <typename T>
concept HasHttp1ClientExpectation = requires(const T& policy) { requires std::same_as<std::remove_cvref_t<decltype(policy.expectation)>, ruvia::HttpClientRequestExpectation>; };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestWirePolicyAccessor = requires(T&& policy) { std::move(policy).noExpectation(); } || requires(T&& policy) { std::move(policy).continueExpectation(); };

template <typename T>
concept HasStaleHttp1ClientPreparedContentTuple = requires(const T& prepared) {
    prepared.contentPlan().disposition();
    prepared.contentPlan().bytes();
};

template <typename T>
concept HasStaleHttp1ClientResponseContext = requires(const T& prepared) { prepared.responseContext(); };

template <typename T>
concept HasHttp1ClientRequestWriterContract = requires(const T& writer, const ruvia::HttpOriginView& origin, const ruvia::HttpClientRequestView& request, std::span<char> buffer, std::span<const ruvia::HttpHeaderView> headers) {
    { writer.prepare(origin, request, buffer) } -> std::same_as<ruvia::Http1ClientRequestPrepareResult>;
    { writer.prepareConnect(origin, headers, buffer) } -> std::same_as<ruvia::Http1ClientRequestPrepareResult>;
    { writer.prepare(origin, request, buffer, ruvia::Http1ClientRequestWirePolicy{.expectation = ruvia::HttpClientRequestExpectation::kContinue}) } -> std::same_as<ruvia::Http1ClientRequestPrepareResult>;
};

template <typename T>
concept HasPositionalHttp1ClientRequestWriterResource = requires(std::pmr::memory_resource* resource) { T(resource); };

template <typename Headers>
concept AcceptsHttp1ConnectHeaders = requires(const ruvia::Http1ClientRequestWriter& writer, const ruvia::HttpOriginView& origin, std::array<char, 512>& buffer, Headers&& headers) { writer.prepareConnect(origin, std::forward<Headers>(headers), buffer); };

template <typename T>
concept HasHttp1ClientResponseParserContract = requires(T& parser, std::string_view buffer) {
    { parser.parse(buffer) } -> std::same_as<ruvia::Http1ClientResponseParseResult>;
    { parser.completeRequestContent() } -> std::same_as<ruvia::Http1ClientRequestContentCompletionStatus>;
};

template <typename T>
concept HasHttp1ClientRequestContentSignal = requires(const T& plan) {
    { plan.requestContentSignal() } -> std::same_as<std::optional<ruvia::HttpClientRequestContentSignal>>;
};

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
concept HasStaleHttp1ClientResponseMode = requires(const T& plan) { plan.mode(); };

template <typename T>
concept HasStaleHttp1ClientResponseConnectionAccessor = requires(const T& plan) { plan.connectionDisposition(); };

template <typename T>
concept HasHttp1ClientResponseContentLength = requires(const T& framing) {
    { framing.contentLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasHttp1ClientResponseTransferCodings = requires(const T& framing) {
    { framing.transferCodings() } -> std::same_as<ruvia::HttpTransferCodings>;
} && requires(const T&& framing) {
    { std::move(framing).transferCodings() } -> std::same_as<ruvia::HttpTransferCodings>;
};

template <typename T>
concept HasHttp1ClosePolicy = requires(const T& framing) {
    { framing.persistence() } -> std::same_as<ruvia::Http1ClosePolicy>;
};

template <typename T>
concept HasHttpClientHeaderLookupAccessors = requires(const T& result) {
    { result.absent() } -> std::same_as<const ruvia::HttpClientResponseHeaderAbsent*>;
    { result.found() } -> std::same_as<const ruvia::HttpClientResponseHeaderFound*>;
    { result.repeated() } -> std::same_as<const ruvia::HttpClientResponseHeaderRepeated*>;
};

template <typename T>
concept HasAnyRvalueHttpClientHeaderLookupAccessor = requires(T&& result) { std::move(result).absent(); } || requires(T&& result) { std::move(result).found(); } || requires(T&& result) { std::move(result).repeated(); };

template <typename T>
concept HasHttpClientHeaderValue = requires(const T& result) {
    { result.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept AcceptsTemporaryHttpClientResponseHeaderLookup = requires(T&& response) { ruvia::lookupUniqueHttpClientResponseHeader(std::move(response), std::string_view{}); };

template <typename T>
concept HasHttpClientRedirectStatus = requires(const T& result) { result.status(); };

template <typename Status>
concept HasPositionalHttpClientRedirectRequestPlan = requires(const ruvia::HttpClientRequestView& request, Status status) { ruvia::planHttpClientRedirectRequest(request, status); } || requires(const ruvia::HttpClientRequestView& request, Status status, std::pmr::memory_resource* resource) { ruvia::planHttpClientRedirectRequest(request, status, resource); };

template <typename Target>
concept HasPositionalHttpClientRedirectTargetResolution = requires(const ruvia::HttpOriginView& origin, Target currentTarget, Target location) { ruvia::resolveHttpClientRedirectTarget(origin, currentTarget, location); } || requires(const ruvia::HttpOriginView& origin, Target currentTarget, Target location, std::pmr::memory_resource* resource) { ruvia::resolveHttpClientRedirectTarget(origin, currentTarget, location, resource); };

template <typename T>
concept HasStaleHttp1ClientHeadOffset = requires(const T& head) { head.bodyOffset(); };

template <typename T>
concept HasDotenvResultPublicFields = requires(T& result) {
    result.loaded;
    result.variablesSet;
    result.variablesSkipped;
};

template <typename T>
concept HasDotenvResultCanonicalReadAccessors = requires(const T& result) {
    { result.loaded() } -> std::same_as<bool>;
    { result.variablesSet() } -> std::same_as<std::size_t>;
    { result.variablesSkipped() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasDotenvOverrideExistingBoolean = requires(T& options) { options.overrideExisting; };

template <typename T>
concept HasDotenvRequiredBoolean = requires(T& options) { options.required; };

template <typename T>
concept ExposesAnyRvalueEnvBorrow = requires { std::declval<const T&&>().get("NAME"); } || requires { std::declval<const T&&>().template get<std::string_view>("NAME"); };

template <typename T>
concept HasContextRenderPipeline = requires(T& context) {
    typename T::RenderOptions;
    typename T::Renderer;
    typename T::Layout;
    context.render(std::string_view{});
};

static_assert(!std::is_copy_constructible_v<ruvia::Next>);
static_assert(!std::is_copy_assignable_v<ruvia::Next>);
static_assert(!std::is_move_constructible_v<ruvia::Next>);
static_assert(!std::is_move_assignable_v<ruvia::Next>);
static_assert(!std::is_copy_constructible_v<ruvia::Next::Awaitable>);
static_assert(!std::is_copy_assignable_v<ruvia::Next::Awaitable>);
static_assert(!std::is_move_constructible_v<ruvia::Next::Awaitable>);
static_assert(!std::is_move_assignable_v<ruvia::Next::Awaitable>);
static_assert(!std::is_default_constructible_v<ruvia::detail::HttpStreamingResponseCompression>);
static_assert(!std::is_copy_constructible_v<ruvia::detail::HttpStreamingResponseCompression>);
static_assert(!std::is_move_constructible_v<ruvia::detail::HttpStreamingResponseCompression>);
static_assert(std::constructible_from<ruvia::detail::HttpStreamingResponseCompression, std::pmr::memory_resource*, ruvia::detail::HttpResponseCodingSelection, ruvia::detail::HttpResponseCodingAvailability>);
static_assert(!std::constructible_from<ruvia::detail::HttpStreamingResponseCompression, std::pmr::memory_resource*, ruvia::detail::HttpResponseCodingSelection>);
static_assert(!HasPlainAddressOf<const ruvia::Next>);
static_assert(!HasPlainAddressOf<ruvia::Next::Awaitable>);
static_assert(!HasLvalueAwait<ruvia::Next::Awaitable>);
static_assert(!std::is_copy_constructible_v<ruvia::RequestNameValueList>);
static_assert(!std::is_copy_assignable_v<ruvia::RequestNameValueList>);
static_assert(!std::is_default_constructible_v<ruvia::RequestNameValueList>);
static_assert(std::is_move_constructible_v<ruvia::RequestNameValueList>);
static_assert(!std::is_move_assignable_v<ruvia::RequestNameValueList>);
static_assert(!std::is_constructible_v<ruvia::RequestNameValueList, std::pmr::memory_resource*>);
static_assert(!noexcept(std::declval<const ruvia::ContextRequest&>().query(std::string_view{})));
static_assert(!noexcept(std::declval<const ruvia::ContextRequest&>().header(std::string_view{})));
static_assert(!noexcept(std::declval<const ruvia::ContextRequest&>().param(std::string_view{})));
static_assert(HasTypeOnlyValid<CurrentUser>);
static_assert(!HasPublicValidatedDataInjection<ruvia::ContextRequest, CurrentUser>);
static_assert(!HasUnaryContextHeader<ruvia::Context>);
static_assert(!HasUnaryContextQuery<ruvia::Context>);
static_assert(!HasUnaryContextCookie<ruvia::Context>);
static_assert(!HasUnaryContextParam<ruvia::Context>);
static_assert(!HasContextStatusTextSetter<ruvia::Context>);
static_assert(!HasResponseHeadersAlias<ruvia::HttpResponse>);
static_assert(!std::is_copy_constructible_v<ruvia::HttpResponse>);
static_assert(!std::is_copy_assignable_v<ruvia::HttpResponse>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::HttpResponse>);
static_assert(std::is_nothrow_move_assignable_v<ruvia::HttpResponse>);
static_assert(HasContextRequestBytes<ruvia::ContextRequest>);
static_assert(!HasRequestArrayBufferAlias<ruvia::ContextRequest>);
static_assert(HasContextRequestCanonicalMethodAccessors<ruvia::ContextRequest>);
static_assert(!HasRequestBlobArrayBufferAlias<ruvia::ContextRequest::RequestBlob>);
static_assert(!HasRequestBlobTypeAlias<ruvia::ContextRequest::RequestBlob>);
static_assert(HasRequestBlobCanonicalAccessors<ruvia::ContextRequest::RequestBlob>);
static_assert(!std::is_constructible_v<ruvia::ContextRequest::RequestBlob, std::span<const std::byte>, std::string_view>);
static_assert(!std::is_copy_constructible_v<ruvia::ContextRequest::RequestFormField>);
static_assert(std::is_move_constructible_v<ruvia::ContextRequest::RequestFormField>);
static_assert(!std::is_move_assignable_v<ruvia::ContextRequest::RequestFormField>);
static_assert(!std::is_copy_constructible_v<ruvia::ContextRequest::RequestFormData::Group>);
static_assert(!std::is_copy_assignable_v<ruvia::ContextRequest::RequestFormData::Group>);
static_assert(std::is_move_constructible_v<ruvia::ContextRequest::RequestFormData::Group>);
static_assert(!std::is_move_assignable_v<ruvia::ContextRequest::RequestFormData::Group>);
static_assert(!std::is_copy_constructible_v<ruvia::ContextRequest::RequestFormData>);
static_assert(!std::is_copy_assignable_v<ruvia::ContextRequest::RequestFormData>);
static_assert(std::is_move_constructible_v<ruvia::ContextRequest::RequestFormData>);
static_assert(!std::is_move_assignable_v<ruvia::ContextRequest::RequestFormData>);
static_assert(!std::is_constructible_v<ruvia::ContextRequest::RequestFormData, std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<ruvia::ContextRequest::RequestFormData, std::pmr::vector<ruvia::ContextRequest::RequestFormField>&&>);
static_assert(!std::is_copy_constructible_v<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(std::is_move_constructible_v<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!std::is_move_assignable_v<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!std::is_constructible_v<ruvia::ContextRequest::RequestFormData::Object, const ruvia::ContextRequest::RequestFormData*, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::ContextRequest::RequestFormData::Object, const ruvia::ContextRequest::RequestFormData&, std::string_view>);
static_assert(!HasLegacyParseBodyFlags<ruvia::ContextRequest::ParseBodyOptions>);
static_assert(ruvia::ContextRequest::ParseBodyOptions{}.repeatedScalars == ruvia::ContextRequest::RepeatedScalarPolicy::kLastValue);
static_assert(ruvia::ContextRequest::ParseBodyOptions{}.dottedNames == ruvia::ContextRequest::DottedNamePolicy::kLiteral);
static_assert(HasRequestJsonValueAccessors<ruvia::ContextRequest>);
static_assert(!HasUntypedRequestJsonAlias<ruvia::ContextRequest>);
static_assert(!HasUntypedRequestJsonIfAlias<ruvia::ContextRequest>);
static_assert(!HasRequestCloneMethod<ruvia::ContextRequest>);
static_assert(!HasRawRequestCloneType<ruvia::ContextRequest>);
static_assert(!HasRequestUrl<ruvia::ContextRequest>);
static_assert(!HasRequestFormDataAlias<ruvia::ContextRequest>);
static_assert(!HasRequestMethodEnumAlias<ruvia::ContextRequest>);
static_assert(!HasRequestTargetAlias<ruvia::ContextRequest>);
static_assert(!HasRequestHeadersAlias<ruvia::ContextRequest>);
static_assert(!HasContextRequestHeaderListAccessor<ruvia::ContextRequest>);
static_assert(!HasRequestQueryStringAlias<ruvia::ContextRequest>);
static_assert(HasRequestRoutePath<ruvia::ContextRequest>);
static_assert(!HasRequestMatchedRoutes<ruvia::ContextRequest>);
static_assert(!HasRequestRouteIndexAlias<ruvia::ContextRequest>);
static_assert(!HasRequestHttpVersionAlias<ruvia::ContextRequest>);
static_assert(!HasRequestDecodedPathAlias<ruvia::ContextRequest>);
static_assert(!HasRequestRemoteAddressAlias<ruvia::ContextRequest>);
static_assert(!HasRequestClientCertificateAlias<ruvia::ContextRequest>);
static_assert(!HasRequestIsSecureAlias<ruvia::ContextRequest>);
static_assert(HasConnInfoCanonicalReadAccessors<ruvia::ConnInfo>);
static_assert(!HasConnInfoListenerId<ruvia::ConnInfo>);
static_assert(!HasLegacyConnInfoScalarAccessors<ruvia::ConnInfo>);
static_assert(!HasRvalueConnInfoTransportAccess<ruvia::ConnInfo>);
static_assert(HasGetConnInfo<ruvia::Context>);
static_assert(!std::is_default_constructible_v<ruvia::ConnInfo>);
static_assert(!std::is_default_constructible_v<ruvia::PlainConnectionTransport>);
static_assert(!std::is_default_constructible_v<ruvia::TlsConnectionTransport>);
static_assert(!HasFormValueToStringView<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueTextAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(HasFormValueGetter<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueValueOrAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueTextsAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueValuesGetter<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueArrowOperator<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueExistsAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueIsArrayAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueIsFileAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormPathValueType<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataEntryType<ruvia::ContextRequest::RequestFormData>);
static_assert(!std::is_constructible_v<ruvia::ContextRequest::RequestFormData::Group, std::pmr::memory_resource*, std::string_view, bool>);
static_assert(!std::is_constructible_v<ruvia::ContextRequest::RequestFormData::Value, const ruvia::ContextRequest::RequestFormData::Group*>);
static_assert(!HasFormGroupIsArrayAlias<ruvia::ContextRequest::RequestFormData::Group>);
static_assert(HasFormGroupCanonicalAccessors<ruvia::ContextRequest::RequestFormData::Group>);
static_assert(HasFormFieldBooleanAccessors<ruvia::ContextRequest::RequestFormField>);
static_assert(!HasFormFieldIsArrayAlias<ruvia::ContextRequest::RequestFormField>);
#ifndef _MSC_VER
static_assert(!HasFormFieldPublicFields<ruvia::ContextRequest::RequestFormField>);
#endif
static_assert(HasFormFieldCanonicalAccessors<ruvia::ContextRequest::RequestFormField>);
static_assert(!ExposesAnyRvalueRequestFormFieldBorrow<ruvia::ContextRequest::RequestFormField>);
static_assert(!std::is_constructible_v<ruvia::ContextRequest::RequestFormField, std::pmr::memory_resource*, std::pmr::string&&, std::pmr::string&&, std::pmr::string&&, std::pmr::string&&, bool, bool>);
static_assert(!HasFormFieldTextAlias<ruvia::ContextRequest::RequestFormField>);
static_assert(!HasFormFieldFileNameAlias<ruvia::ContextRequest::RequestFormField>);
static_assert(!HasFormFieldMediaTypeAlias<ruvia::ContextRequest::RequestFormField>);
static_assert(!HasFormFieldArrayBufferAlias<ruvia::ContextRequest::RequestFormField>);
static_assert(!HasFormValueFileNameAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueMediaTypeAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(std::is_trivially_copyable_v<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(HasFormValueZeroAllocationAccessors<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!std::is_default_constructible_v<ruvia::HttpRequest>);
static_assert(std::derived_from<ruvia::HttpProtocolError, std::exception>);
static_assert(std::is_nothrow_constructible_v<ruvia::HttpProtocolError, ruvia::HttpStatusCode, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::HttpProtocolError, std::uint16_t, std::string_view>);
static_assert(!HasLegacyHttpRequestQueryGetter<ruvia::HttpRequest>);
static_assert(HasHttpRequestWireMetadata<ruvia::HttpRequest>);
static_assert(!HasHttpRequestDecodedPathAlias<ruvia::HttpRequest>);
static_assert(!ExposesRvalueHttpRequestHeaders<ruvia::HttpRequest>);
static_assert(!ExposesRvalueRequestMemoryBorrow<ruvia::RequestMemory>);
static_assert(!ExposesRvalueRouteListIterator<ruvia::detail::RuviaMethodList>);
static_assert(!ExposesRvalueRouteListIterator<ruvia::detail::RuviaPathList>);
static_assert(!AcceptsTemporaryRoutePath<std::string>);
static_assert(!AcceptsTemporaryRoutePath<const std::string>);
static_assert(!AcceptsTemporaryRoutePath<std::pmr::string>);
static_assert(!HasRequestRemoteAddressAlias<ruvia::HttpRequest>);
static_assert(!HasRequestClientCertificateAlias<ruvia::HttpRequest>);
static_assert(!HasRequestIsSecureAlias<ruvia::HttpRequest>);
static_assert(!HasFormDataGetAllAlias<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataValuesAllAlias<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataNamedValuesAllocator<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataNamedFieldsAllocator<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataNamedFieldAlias<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataIsArrayAlias<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataValueAlias<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataHasAlias<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataKeysAllocator<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataEntriesAlias<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataIndexAlias<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataEntryLookup<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormDataPathAliases<ruvia::ContextRequest::RequestFormData>);
static_assert(!HasFormAtLookup<ruvia::ContextRequest::RequestFormData>);
static_assert(HasFormDataCanonicalAccessors<ruvia::ContextRequest::RequestFormData>);
static_assert(!ExposesRvalueRequestFormGroupFields<ruvia::ContextRequest::RequestFormData::Group>);
static_assert(!ExposesAnyRvalueRequestFormDataBorrow<ruvia::ContextRequest::RequestFormData>);
static_assert(noexcept(std::declval<const ruvia::ContextRequest::RequestFormData&>().get(std::string_view{})));
static_assert(!HasFormObjectGetAllAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectKeysAllocator<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectEntriesAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectIndexAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectValueAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectHasAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectNamedValuesAllocator<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(HasFormObjectCanonicalAccessors<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!ExposesRvalueRequestFormObjectGroups<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormAtLookup<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(noexcept(std::declval<const ruvia::ContextRequest::RequestFormData::Object&>().get(std::string_view{})));
static_assert(HasByteSpanResponseBody<ruvia::Context>);
static_assert(!HasStdStringResponseBody<ruvia::Context>);
static_assert(HasPmrStringResponseBuilders<ruvia::Context>);
static_assert(!HasContextNewResponseAlias<ruvia::Context>);
static_assert(!HasContextSetHeaderAlias<ruvia::Context>);
static_assert(!HasContextResponseSlotAlias<ruvia::Context>);
static_assert(!HasResponseSetHeaderAlias<ruvia::HttpResponse>);
static_assert(HasResponseHeaderSetter<ruvia::HttpResponse>);
static_assert(!HasResponseAppendHeaderAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHasHeaderAlias<ruvia::HttpResponse>);
static_assert(HasResponseHeaderOptionsSetter<ruvia::HttpResponse>);
static_assert(std::same_as<decltype(ruvia::HttpResponse::HeaderOptions{}.mode), ruvia::HttpResponseHeaderMode>);
static_assert(ruvia::HttpResponse::HeaderOptions{}.mode == ruvia::HttpResponseHeaderMode::kReplace);
static_assert(std::same_as<std::underlying_type_t<ruvia::HttpResponseHeaderMode>, std::uint8_t>);
static_assert(!HasResponseHeaderAppendBoolean<ruvia::HttpResponse::HeaderOptions>);
static_assert(!CanBoolInitializeResponseHeaderOptions<ruvia::HttpResponse::HeaderOptions>);
static_assert(!HasContextBuilderMetadataArguments<ruvia::Context>);
static_assert(AcceptsContextJson<ruvia::Context, ContextJsonResponseProbe>);
static_assert(!AcceptsContextJson<ruvia::Context, std::uint32_t>);
static_assert(!HasContextJsonObjectBuilder<ruvia::Context>);
static_assert(!HasContextJsonArrayBuilder<ruvia::Context>);
static_assert(HasResponseHeaderRemoveSetter<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersEraseAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersGetAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersEntriesAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersHasAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersSetAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersAppendAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersRemoveAlias<ruvia::HttpResponse>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::HttpResponse&>().headers()), const ruvia::HttpResponseHeaders&>);
static_assert(!HasResponseSetStatusAlias<ruvia::HttpResponse>);
static_assert(HasResponseStatusSetter<ruvia::HttpResponse>);
static_assert(!HasResponseReasonPhraseSetter<ruvia::HttpResponse>);
static_assert(!HasResponseStatusCodeAlias<ruvia::HttpResponse>);
static_assert(HasResponseStatusGetter<ruvia::HttpResponse>);
static_assert(std::same_as<decltype(static_cast<ResponseHeadersGetter>(&ruvia::HttpResponse::headers)), ResponseHeadersGetter>);
static_assert(!std::default_initializable<ruvia::HttpResponseHeaders>);
static_assert(!std::constructible_from<ruvia::HttpResponseHeaders, std::pmr::memory_resource*>);
static_assert(!HasResponseSetBodyOwnedAlias<ruvia::HttpResponse>);
static_assert(!HasResponseSetBodyCopyAlias<ruvia::HttpResponse>);
static_assert(!HasResponseSetBodyViewAlias<ruvia::HttpResponse>);
static_assert(HasResponseBodySetter<ruvia::HttpResponse>);
static_assert(!HasHttpClientResponseHeadStatusCodeField<ruvia::HttpClientResponseHead>);
static_assert(HasHttpClientResponseHeadStatusGetter<ruvia::HttpClientResponseHead>);
static_assert(std::is_move_constructible_v<ruvia::HttpClientResponseHead>);
static_assert(!std::is_move_assignable_v<ruvia::HttpClientResponseHead>);
static_assert(HasHttpClientRequestViewHeaderViews<ruvia::HttpClientRequestView>);
static_assert(HasHttpClientRequestViewHeaderArray<ruvia::HttpClientRequestView>);
static_assert(HasHttpClientRequestViewHeaderVector<ruvia::HttpClientRequestView>);
static_assert(HasHttpClientRequestViewInitializerListHeaders<ruvia::HttpClientRequestView>);
static_assert(HasHttpClientRequestViewBorrowedText<ruvia::HttpClientRequestView>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestViewText<std::string>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestViewText<const std::string>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestViewText<std::pmr::string>);
static_assert(AcceptsLvalueHttpClientRequestViewText<std::string>);
constexpr ruvia::HttpClientRequestView kLiteralHttpClientRequestView{.method = "POST", .target = "/items"};
static_assert(kLiteralHttpClientRequestView.method.view() == "POST");
static_assert(kLiteralHttpClientRequestView.target.view() == "/items");
static_assert(!HasRawHttpClientRequestViewBody<ruvia::HttpClientRequestView>);
static_assert(HasDiscriminatedHttpClientRequestContentView<ruvia::HttpClientRequestView>);
static_assert(!HasStaleHttpClientRequestContentViewTuple<ruvia::HttpClientRequestView>);
static_assert(!HasAnyRvalueHttpClientRequestContentViewAccessor<ruvia::HttpClientRequestContentView>);
static_assert(!std::is_default_constructible_v<ruvia::HttpClientRequestContentView>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestBytesView>);
static_assert(HasHttpClientRequestContentViewBytesFactory<std::string&>);
static_assert(!HasHttpClientRequestContentViewBytesFactory<std::string>);
static_assert(!HasHttpClientRequestContentViewBytesFactory<std::pmr::string>);
static_assert(std::is_aggregate_v<ruvia::HttpOriginOptions>);
static_assert(!std::is_default_constructible_v<ruvia::HttpOriginView>);
static_assert(HasTypedHttpOriginViewAccessors<ruvia::HttpOriginView>);
static_assert(std::is_same_v<decltype(ruvia::HttpOriginView::https({.host = "example.test"})), ruvia::HttpOriginView>);
static_assert(std::is_same_v<decltype(ruvia::HttpOriginView::https({.host = "example.test", .port = std::uint16_t{8443}})), ruvia::HttpOriginView>);
static_assert(!noexcept(ruvia::HttpOriginView::http({.host = "example.test"})));
static_assert(!std::is_constructible_v<ruvia::HttpOriginView, ruvia::HttpScheme, std::string_view, std::uint16_t>);
static_assert(!HasHttpOriginPositionalFactories<std::string_view>);
static_assert(HasHttpOriginLvalueHostFactory<std::string>);
static_assert(!HasHttpOriginRvalueHostFactory<std::string>);
static_assert(!HasHttpOriginRvalueHostFactory<std::pmr::string>);
static_assert(!HasOutboundClientFacet<ruvia::Context>);
static_assert(!HasUseHttpClient<ruvia::App>);
static_assert(!HasRuntimeHttpClientMutation<ruvia::App>);
static_assert(!HasHttpClientResponseHeadHeadersField<ruvia::HttpClientResponseHead>);
static_assert(HasHttpClientResponseHeadHeadersGetter<ruvia::HttpClientResponseHead>);
static_assert(!HasHttpClientResponseHeadBodyField<ruvia::HttpClientResponseHead>);
static_assert(!HasHttpClientResponseHeadBodyGetter<ruvia::HttpClientResponseHead>);
#ifdef RUVIA_ENABLE_DATABASE
static_assert(HasDbHandleDefaultParams<ruvia::DbHandle>);
static_assert(!HasDbHandleInitializerListParams<ruvia::DbHandle>);
static_assert(HasDbTransactionDefaultParams<ruvia::DbTransaction>);
static_assert(!HasDbTransactionInitializerListParams<ruvia::DbTransaction>);
#endif
static_assert(!std::is_default_constructible_v<ruvia::HttpClientResponseHead>);
static_assert(!std::is_constructible_v<ruvia::HttpClientResponseHead, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::HttpClientResponseHeader>);
static_assert(!std::is_constructible_v<ruvia::HttpClientResponseHeader, std::string_view, std::string_view, std::pmr::memory_resource*>);
static_assert(!HasHttpClientResponseHeaderNameField<ruvia::HttpClientResponseHeader>);
static_assert(HasHttpClientResponseHeaderNameGetter<ruvia::HttpClientResponseHeader>);
static_assert(!HasHttpClientResponseHeaderValueField<ruvia::HttpClientResponseHeader>);
static_assert(HasHttpClientResponseHeaderValueGetter<ruvia::HttpClientResponseHeader>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::HttpClientResponseHeader>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::HttpClientResponseHead>);
static_assert(!HasCompleteType<ruvia::detail::HttpClientResponseHeaderAccess>);
static_assert(!HasCompleteType<ruvia::detail::HttpClientResponseHeadAccess>);
static_assert(!HasCompleteType<ruvia::detail::Http1RequestParseResultAccess>);
static_assert(!HasCompleteType<ruvia::detail::Http1ClientRequestPrepareResultAccess>);
static_assert(!HasCompleteType<ruvia::detail::Http1ClientResponseParseResultAccess>);
static_assert(!HasCompleteType<ruvia::detail::Http1ClientResponsePlanAccess>);
static_assert(!HasCompleteType<ruvia::detail::MultipartPartAccess>);
static_assert(!HasCompleteType<ruvia::detail::RequestNameValueViewAccess>);
static_assert(!HasCompleteType<ruvia::detail::RequestNameValueListAccess>);
static_assert(!HasCompleteType<ruvia::detail::MultipartStreamPartAccess>);
static_assert(!HasCompleteType<ruvia::detail::WebSocketMessageAccess>);
static_assert(!HasCompleteType<ruvia::detail::AccessLogRecordAccess>);
static_assert(!HasCompleteType<ruvia::detail::DotenvResultAccess>);
static_assert(!HasCompleteType<ruvia::detail::RequestFormFieldAccess>);
static_assert(!HasCompleteType<ruvia::detail::RequestFormDataAccess>);
static_assert(!HasCompleteType<ruvia::detail::StreamingAccess>);
static_assert(!HasCompleteType<ruvia::detail::SessionAccess>);
static_assert(!HasCompleteType<ruvia::detail::DbValueAccess>);
static_assert(!HasCompleteType<ruvia::detail::RedisTypesAccess>);
#ifndef _MSC_VER
static_assert(!HasHttpHeaderViewPublicFields<ruvia::HttpHeaderView>);
#endif
static_assert(HasHttpHeaderViewCanonicalReadAccessors<ruvia::HttpHeaderView>);
static_assert(std::is_constructible_v<ruvia::HttpHeaderView, std::string_view, std::string_view>);
#ifndef _MSC_VER
static_assert(!HasMultipartPartPublicFields<ruvia::MultipartPart>);
#endif
static_assert(HasMultipartPartCanonicalReadAccessors<ruvia::MultipartPart>);
#ifndef _MSC_VER
static_assert(!HasMultipartStreamPartPublicFields<ruvia::MultipartStreamPart>);
#endif
static_assert(HasMultipartStreamPartCanonicalReadAccessors<ruvia::MultipartStreamPart>);
static_assert(!std::is_default_constructible_v<ruvia::MultipartBoundary>);
static_assert(std::is_constructible_v<ruvia::MultipartBoundary, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::MultipartParser, std::string_view, std::pmr::memory_resource*>);
static_assert(std::is_constructible_v<ruvia::MultipartParser, ruvia::MultipartParseOptions>);
static_assert(std::is_aggregate_v<ruvia::MultipartParseOptions>);
static_assert(!std::is_default_constructible_v<ruvia::MultipartParseOptions>);
static_assert(!std::is_constructible_v<ruvia::MultipartParser, ruvia::MultipartBoundary, std::pmr::memory_resource*>);
static_assert(!std::is_copy_constructible_v<ruvia::MultipartParser>);
static_assert(!std::is_copy_assignable_v<ruvia::MultipartParser>);
static_assert(!std::is_move_constructible_v<ruvia::MultipartParser>);
static_assert(!std::is_move_assignable_v<ruvia::MultipartParser>);
static_assert(std::is_constructible_v<ruvia::MultipartReader, ruvia::BodyReader&, ruvia::MultipartParseOptions>);
static_assert(!std::is_constructible_v<ruvia::MultipartReader, ruvia::BodyReader&, ruvia::MultipartBoundary, std::pmr::memory_resource*>);
static_assert(!std::is_copy_constructible_v<ruvia::MultipartReader>);
static_assert(!std::is_copy_assignable_v<ruvia::MultipartReader>);
static_assert(!std::is_move_constructible_v<ruvia::MultipartReader>);
static_assert(!std::is_move_assignable_v<ruvia::MultipartReader>);
static_assert(!std::is_default_constructible_v<ruvia::MultipartPollResult>);
static_assert(HasMultipartPollResultAccessors<ruvia::MultipartPollResult>);
static_assert(!HasAnyRvalueMultipartPollAccessor<ruvia::MultipartPollResult>);
#ifndef _MSC_VER
static_assert(!HasWebSocketMessagePublicFields<ruvia::WebSocketMessage>);
#endif
static_assert(HasWebSocketMessageCanonicalReadAccessors<ruvia::WebSocketMessage>);
static_assert(!std::is_default_constructible_v<ruvia::WebSocketMessage>);
static_assert(!std::is_constructible_v<ruvia::WebSocketMessage, ruvia::WebSocketOpcode, std::string_view>);
static_assert(!HasWebSocketPublicCallbackConstructor<ruvia::WebSocket>);
static_assert(!HasWebSocketRuntimeCallbacks<ruvia::WebSocket>);
static_assert(std::is_aggregate_v<ruvia::WebSocketCloseOptions>);
static_assert(std::same_as<decltype(ruvia::WebSocketCloseOptions{}.code), std::uint16_t>);
static_assert(std::same_as<decltype(ruvia::WebSocketCloseOptions{}.reason), ruvia::BorrowedText>);
static_assert(HasWebSocketCloseOptions<ruvia::WebSocket>);
static_assert(!HasPositionalWebSocketClose<ruvia::WebSocket>);
static_assert(std::is_aggregate_v<ruvia::WebSocketHeartbeatConfig>);
static_assert(std::same_as<decltype(ruvia::WebSocketHeartbeatConfig{.pingInterval = std::chrono::milliseconds{1}, .pongTimeout = std::chrono::milliseconds{2}}), ruvia::WebSocketHeartbeatConfig>);
static_assert(std::same_as<decltype(ruvia::WebSocketRouteConfig{}.subprotocols), std::vector<std::string>>);
static_assert(!HasContextGetIfAlias<ruvia::Context>);
static_assert(!HasArbitraryContextValueSet<ruvia::Context>);
static_assert(!HasArbitraryContextValueGet<ruvia::Context>);
static_assert(!HasContextVarIfAlias<ruvia::Context>);
static_assert(!HasContextVarFacade<ruvia::Context>);
static_assert(!HasContextVarHasAlias<ruvia::Context>);
static_assert(!HasConstContextVarHasAlias<ruvia::Context>);
static_assert(!HasContextJsonErrorAlias<ruvia::Context>);
static_assert(!HasAppInstanceAlias<ruvia::App>);
static_assert(!HasWebWorkerCorePostEscape<ruvia::WebWorkerHandle>);
static_assert(!HasRequestCookiesAlias<ruvia::ContextRequest>);
static_assert(!HasContextRequestQueryListAccessor<ruvia::ContextRequest>);
static_assert(!HasContextRequestQueriesListAccessor<ruvia::ContextRequest>);
static_assert(!HasContextRequestCookieListAccessor<ruvia::ContextRequest>);
static_assert(!HasContextRequestParamListAccessor<ruvia::ContextRequest>);
static_assert(!HasRequestNameValueListGetAllAlias<ruvia::RequestNameValueList>);
static_assert(!HasRequestNameValueListSpanAlias<ruvia::RequestNameValueList>);
static_assert(!HasRequestNameValueListKeysAllocator<ruvia::RequestNameValueList>);
static_assert(!HasRequestNameValueListValuesAllocator<ruvia::RequestNameValueList>);
static_assert(!HasRequestNameValueListNamedValuesAllocator<ruvia::RequestNameValueList>);
static_assert(!HasRequestNameValueListNameIndexAlias<ruvia::RequestNameValueList>);
static_assert(!HasRequestNameValueListHasAlias<ruvia::RequestNameValueList>);
static_assert(!std::is_default_constructible_v<ruvia::RequestNameValueView>);
#ifndef _MSC_VER
static_assert(!HasRequestNameValueViewPublicFields<ruvia::RequestNameValueView>);
#endif
static_assert(HasRequestNameValueViewCanonicalAccessors<ruvia::RequestNameValueView>);
static_assert(!HasRequestNameValueListPublicMutators<ruvia::RequestNameValueList>);
static_assert(!HasRequestNameValueListMutableAccess<ruvia::RequestNameValueList>);
static_assert(!HasRequestNameValueListMutableIteratorAlias<ruvia::RequestNameValueList>);
static_assert(std::is_pointer_v<ruvia::RequestNameValueList::const_iterator>);
static_assert(HasRequestNameValueListCanonicalAccessors<ruvia::RequestNameValueList>);
static_assert(!ExposesAnyRvalueRequestNameValueListBorrow<ruvia::RequestNameValueList>);
static_assert(!HasAppErrorHandlerSetterAlias<ruvia::App>);
static_assert(!HasAppNotFoundHandlerSetterAlias<ruvia::App>);
static_assert(!HasAppSetRateLimitAlias<ruvia::App>);
static_assert(!HasRateLimitSlotCount<ruvia::RateLimitRule>);
static_assert(std::default_initializable<ruvia::RateLimitRule>);
static_assert(std::is_aggregate_v<ruvia::RateLimitRule>);
static_assert(std::same_as<decltype(ruvia::RateLimitRule{.maxRequests = std::size_t{1}, .window = std::chrono::seconds(1)}), ruvia::RateLimitRule>);
static_assert(std::is_aggregate_v<ruvia::MiddlewareScopeOptions>);
static_assert(std::same_as<decltype(ruvia::MiddlewareScopeOptions{}.prefix), std::string>);
static_assert(HasAppUseMiddlewareTemplate<ruvia::App>);
static_assert(HasAppUseAtScopeOptions<ruvia::App>);
static_assert(HasAppUseAtScopeOptions<ruvia::TestApp>);
static_assert(!HasAppUseAtPositional<ruvia::App>);
static_assert(!HasAppUseAtPositional<ruvia::TestApp>);
static_assert(AcceptsLvalueMiddlewareScopePrefix<std::string>);
static_assert(AcceptsAnyRvalueMiddlewareScopePrefix<std::string>);
static_assert(!std::is_constructible_v<ruvia::detail::ControllerRouteBuilder, ruvia::detail::Router&, std::string_view>);
#ifndef _MSC_VER
static_assert(!HasControllerRouteBuilderPublicRegisterRoute<ruvia::detail::ControllerRouteBuilder>);
static_assert(!HasControllerRouteBuilderPublicRegisterResponseStreamRoute<ruvia::detail::ControllerRouteBuilder>);
static_assert(!HasControllerRouteBuilderPublicCreateScope<ruvia::detail::ControllerRouteBuilder>);
#endif
static_assert(!HasControllerMiddlewareDescriptorPublicCallbackConstructor<ruvia::detail::ControllerMiddlewareDescriptor>);
static_assert(!HasControllerStorePublicMutators<ruvia::detail::ControllerStore>);
static_assert(!HasControllerStorePublicSize<ruvia::detail::ControllerStore>);
static_assert(!HasDbRowPublicMutators<ruvia::DbRow>);
static_assert(!HasDbValuePmrStringConstructor<ruvia::DbValue>);
static_assert(!AcceptsTemporaryDbValueText<std::string>);
static_assert(!AcceptsTemporaryDbValueText<const std::string>);
static_assert(AcceptsLvalueDbValueText<std::string>);
static_assert(AcceptsAnyTemporaryDbMigrationText<std::string>);
static_assert(AcceptsAnyTemporaryDbMigrationText<const std::string>);
static_assert(AcceptsLvalueDbMigrationText<std::string>);
static_assert(std::is_aggregate_v<ruvia::DbMigrationOptions>);
static_assert(std::same_as<decltype(ruvia::DbMigrationOptions{}.id), std::string>);
static_assert(std::same_as<decltype(ruvia::DbMigrationOptions{}.sql), std::string>);
static_assert(std::same_as<decltype(ruvia::DbMigrationOptions{}.atomicity), ruvia::DbMigrationAtomicity>);
static_assert(!HasPositionalDbMigrationConstructor<ruvia::DbMigration>);
static_assert(HasDbMigrationTextAccessors<ruvia::DbMigration>);
static_assert(!std::is_default_constructible_v<ruvia::DbRow>);
static_assert(!std::is_constructible_v<ruvia::DbRow, std::pmr::memory_resource*>);
static_assert(HasDbRowCanonicalReadAccessors<ruvia::DbRow>);
static_assert(std::is_move_assignable_v<ruvia::DbRow>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::DbRow>);
static_assert(!std::is_default_constructible_v<ruvia::DbField>);
static_assert(!std::is_constructible_v<ruvia::DbField, std::nullptr_t, std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<ruvia::DbField, std::string_view, std::pmr::memory_resource*>);
static_assert(HasDbFieldCanonicalReadAccessors<ruvia::DbField>);
static_assert(!HasLegacyDbFieldText<ruvia::DbField>);
static_assert(std::derived_from<ruvia::DbConversionError, std::runtime_error>);
static_assert(std::same_as<decltype(std::declval<const ruvia::DbConversionError&>().code()), ruvia::DbConversionError::Code>);
static_assert(std::is_move_assignable_v<ruvia::DbField>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::DbField>);
static_assert(!std::is_default_constructible_v<ruvia::DbRows>);
static_assert(!std::is_constructible_v<ruvia::DbRows, std::pmr::memory_resource*>);
static_assert(HasRowsCanonicalReadAccessors<ruvia::DbRows>);
static_assert(!HasLegacyDbRowsSpan<ruvia::DbRows>);
static_assert(!HasExecResultCanonicalReadAccessors<ruvia::DbRows>);
static_assert(std::is_move_constructible_v<ruvia::DbRows>);
static_assert(!std::is_move_assignable_v<ruvia::DbRows>);
static_assert(!std::is_constructible_v<ruvia::DbError, ruvia::DbError::Code, std::string>);
static_assert(std::same_as<decltype(std::declval<const ruvia::DbError&>().code()), ruvia::DbError::Code>);
static_assert(std::same_as<decltype(std::declval<const ruvia::DbError&>().driver()), std::optional<ruvia::DbDriver>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::DbError&>().nativeCode()), std::optional<std::int64_t>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::DbError&>().sqlState()), std::optional<std::string_view>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::DbError&>().constraintName()), std::optional<std::string_view>>);
static_assert(!HasRowsCanonicalReadAccessors<ruvia::DbExecResult>);
static_assert(HasExecResultCanonicalReadAccessors<ruvia::DbExecResult>);
static_assert(std::is_move_constructible_v<ruvia::DbStreamResult>);
static_assert(!std::is_move_assignable_v<ruvia::DbStreamResult>);
static_assert(std::is_move_constructible_v<ruvia::DbTransaction>);
static_assert(!std::is_move_assignable_v<ruvia::DbTransaction>);
static_assert(!std::is_default_constructible_v<ruvia::DbMigrationReport>);
static_assert(!std::is_constructible_v<ruvia::DbMigrationReport, std::pmr::memory_resource*>);
static_assert(HasDbMigrationReportCanonicalReadAccessors<ruvia::DbMigrationReport>);
static_assert(std::is_move_constructible_v<ruvia::DbMigrationReport>);
static_assert(!std::is_move_assignable_v<ruvia::DbMigrationReport>);
#ifdef RUVIA_ENABLE_JWT
static_assert(!std::is_default_constructible_v<ruvia::JwtClaim>);
static_assert(std::is_aggregate_v<ruvia::JwtClaimOptions>);
static_assert(std::is_aggregate_v<ruvia::JwtSignOptions>);
static_assert(std::is_aggregate_v<ruvia::JwtVerifyOptions>);
static_assert(std::is_aggregate_v<ruvia::JwtDecodeUnverifiedOptions>);
static_assert(std::same_as<decltype(ruvia::JwtClaimOptions{}.name), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::JwtClaimOptions{}.value), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::JwtSignOptions{}.secret), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::JwtSignOptions{}.resource), std::pmr::memory_resource*>);
static_assert(std::same_as<decltype(ruvia::JwtVerifyOptions{}.token), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::JwtVerifyOptions{}.secret), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::JwtVerifyOptions{}.resource), std::pmr::memory_resource*>);
static_assert(std::same_as<decltype(ruvia::JwtDecodeUnverifiedOptions{}.token), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::JwtDecodeUnverifiedOptions{}.resource), std::pmr::memory_resource*>);
static_assert(std::is_constructible_v<ruvia::JwtClaim, ruvia::JwtClaimOptions>);
static_assert(!HasJwtClaimPositionalConstructor<ruvia::JwtClaim>);
static_assert(!HasJwtClaimResourceConstructor<ruvia::JwtClaim>);
static_assert(!HasJwtSignResourceArgument<ruvia::JwtSignOptions>);
static_assert(!HasJwtVerifyPositionalToken<ruvia::JwtVerifyOptions>);
static_assert(!HasJwtDecodeUnverifiedPositionalToken<std::string_view>);
static_assert(!HasJwtDecodeUnverifiedPositionalToken<std::string&>);
static_assert(AcceptsLvalueJwtClaimOptionText<std::string>);
static_assert(!AcceptsAnyRvalueJwtClaimOptionText<std::string>);
static_assert(!AcceptsAnyRvalueJwtClaimOptionText<std::pmr::string>);
static_assert(!AcceptsAnyRvalueJwtTokenOptionText<std::string>);
static_assert(!AcceptsAnyRvalueJwtTokenOptionText<std::pmr::string>);
static_assert(AcceptsLvalueJwtSecretOptionText<std::string>);
static_assert(!AcceptsAnyRvalueJwtSecretOptionText<std::string>);
static_assert(!AcceptsAnyRvalueJwtSecretOptionText<std::pmr::string>);
#ifndef _MSC_VER
static_assert(!HasJwtClaimPublicFields<ruvia::JwtClaim>);
#endif
static_assert(HasJwtClaimCanonicalReadAccessors<ruvia::JwtClaim>);
static_assert(!std::is_default_constructible_v<ruvia::JwtPayload>);
static_assert(std::is_aggregate_v<ruvia::JwtPayloadOptions>);
static_assert(std::same_as<decltype(ruvia::JwtPayloadOptions{}.resource), std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<ruvia::JwtPayload, std::pmr::memory_resource*>);
static_assert(HasJwtPayloadCanonicalReadAccessors<ruvia::JwtPayload>);
#endif
static_assert(!std::is_default_constructible_v<ruvia::RedisValue>);
static_assert(!std::is_constructible_v<ruvia::RedisValue, std::pmr::memory_resource*>);
static_assert(!HasRedisValuePublicFactories<ruvia::RedisValue>);
static_assert(HasRedisValueCanonicalReadAccessors<ruvia::RedisValue>);
static_assert(std::is_move_assignable_v<ruvia::RedisValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisValue>);
static_assert(!HasRvalueRedisValueBorrow<ruvia::RedisValue>);
static_assert(std::is_final_v<ruvia::RedisError>);
static_assert(std::derived_from<ruvia::RedisError, std::runtime_error>);
static_assert(!HasRedisErrorMessageAlias<ruvia::RedisError>);
static_assert(!std::is_default_constructible_v<ruvia::RedisKeyValue>);
static_assert(!std::is_constructible_v<ruvia::RedisKeyValue, std::string_view, std::string_view, std::pmr::memory_resource*>);
#ifndef _MSC_VER
static_assert(!HasRedisKeyValuePublicFields<ruvia::RedisKeyValue>);
#endif
static_assert(HasRedisKeyValueCanonicalReadAccessors<ruvia::RedisKeyValue>);
static_assert(std::is_move_assignable_v<ruvia::RedisKeyValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisKeyValue>);
static_assert(!std::is_default_constructible_v<ruvia::RedisScoredValue>);
static_assert(!std::is_constructible_v<ruvia::RedisScoredValue, std::string_view, double, std::pmr::memory_resource*>);
#ifndef _MSC_VER
static_assert(!HasRedisScoredValuePublicFields<ruvia::RedisScoredValue>);
#endif
static_assert(HasRedisScoredValueCanonicalReadAccessors<ruvia::RedisScoredValue>);
static_assert(std::is_move_assignable_v<ruvia::RedisScoredValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisScoredValue>);
static_assert(!std::is_default_constructible_v<ruvia::RedisScanResult>);
static_assert(!std::is_constructible_v<ruvia::RedisScanResult, std::pmr::memory_resource*>);
#ifndef _MSC_VER
static_assert(!HasRedisScanResultPublicFields<ruvia::RedisScanResult>);
#endif
static_assert(HasRedisScanResultCanonicalReadAccessors<ruvia::RedisScanResult>);
static_assert(!std::is_default_constructible_v<ruvia::RedisHashScanResult>);
static_assert(!std::is_constructible_v<ruvia::RedisHashScanResult, std::pmr::memory_resource*>);
#ifndef _MSC_VER
static_assert(!HasRedisHashScanResultPublicFields<ruvia::RedisHashScanResult>);
#endif
static_assert(HasRedisHashScanResultCanonicalReadAccessors<ruvia::RedisHashScanResult>);
static_assert(!std::is_default_constructible_v<ruvia::RedisZScanResult>);
static_assert(!std::is_constructible_v<ruvia::RedisZScanResult, std::pmr::memory_resource*>);
#ifndef _MSC_VER
static_assert(!HasRedisZScanResultPublicFields<ruvia::RedisZScanResult>);
#endif
static_assert(HasRedisZScanResultCanonicalReadAccessors<ruvia::RedisZScanResult>);
static_assert(HasAppRateLimitConfig<ruvia::App>);
static_assert(HasAppRateLimitDisable<ruvia::App>);
static_assert(!HasAppDefaultRateLimitPerWorkerTupleSetter<ruvia::App>);
static_assert(!HasAppRateLimitCapacityPerWorkerSetter<ruvia::App>);
static_assert(!HasAppRateLimitSlotsPerWorkerSetter<ruvia::App>);
static_assert(ruvia::kDefaultRateLimitCapacityPerWorker == 8192);
static_assert(std::is_aggregate_v<ruvia::ServerConfig>);
static_assert(HasAppServerConfig<ruvia::App>);
static_assert(!HasAppWorkerCountSetter<ruvia::App>);
static_assert(std::same_as<std::underlying_type_t<ruvia::ProcessSignalHandlerPolicy>, std::uint8_t>);
static_assert(!HasAppProcessSignalHandlersSetter<ruvia::App>);
static_assert(!HasAppProcessSignalHandlersBooleanSetter<ruvia::App>);
static_assert(!HasAppSignalShutdownSetter<ruvia::App>);
static_assert(!HasLegacyAppThreadNumSetter<ruvia::App>);
static_assert(!HasLegacyAppGlobalRateLimitSetter<ruvia::App>);
static_assert(HasAppDocumentRootConfig<ruvia::App>);
static_assert(!HasDocumentRootRefreshOptions<ruvia::StaticRootOptions>);
static_assert(HasDocumentRootRefreshOptions<ruvia::DocumentRootRuntimeConfig>);
static_assert(!HasDocumentRootRefreshMode<ruvia::DocumentRootRuntimeConfig>);
static_assert(!HasDocumentRootLiveReload<ruvia::DocumentRootRuntimeConfig>);
static_assert(HasDocumentRootRuntimePolicy<ruvia::DocumentRootConfig>);
static_assert(HasDocumentRootPrecompressionFields<ruvia::DocumentRootConfig>);
static_assert(!HasNestedDocumentRootPrecompression<ruvia::DocumentRootConfig>);
static_assert(std::is_enum_v<ruvia::StaticRangeRequestPolicy>);
static_assert(std::same_as<std::underlying_type_t<ruvia::StaticRangeRequestPolicy>, std::uint8_t>);
static_assert(std::is_enum_v<ruvia::StaticResponseValidatorPolicy>);
static_assert(std::same_as<std::underlying_type_t<ruvia::StaticResponseValidatorPolicy>, std::uint8_t>);
static_assert(std::is_enum_v<ruvia::StaticDotfilePolicy>);
static_assert(std::same_as<std::underlying_type_t<ruvia::StaticDotfilePolicy>, std::uint8_t>);
static_assert(HasStaticRootRangeRequestPolicy<ruvia::StaticRootOptions>);
static_assert(HasStaticRootResponseValidatorPolicy<ruvia::StaticRootOptions>);
static_assert(HasStaticRootDotfilePolicy<ruvia::StaticRootOptions>);
static_assert(!HasStaticRootEnableRanges<ruvia::StaticRootOptions>);
static_assert(!HasStaticRootEnableValidators<ruvia::StaticRootOptions>);
static_assert(!HasStaticRootServeDotfilesBoolean<ruvia::StaticRootOptions>);
static_assert(!std::is_default_constructible_v<ruvia::detail::DocumentRootBinding>);
static_assert(!std::copy_constructible<ruvia::detail::DocumentRootBinding>);
static_assert(std::move_constructible<ruvia::detail::DocumentRootBinding>);
static_assert(!std::is_constructible_v<ruvia::detail::DocumentRootBinding, const ruvia::StaticRoot*, const ruvia::StaticRoot*>);
static_assert(HasDocumentRootBindingAccessor<ruvia::detail::HttpServerOptions::DocumentRoot>);
static_assert(std::default_initializable<ruvia::detail::HttpServerOptions::DocumentRoot>);
static_assert(!std::is_aggregate_v<ruvia::detail::HttpServerOptions::DocumentRoot>);
static_assert(std::same_as<decltype(ruvia::detail::HttpServerOptions::DocumentRoot::standalone(std::declval<const ruvia::StaticRoot&>())), ruvia::detail::HttpServerOptions::DocumentRoot>);
static_assert(std::same_as<decltype(ruvia::detail::HttpServerOptions::DocumentRoot::refreshing(std::declval<const ruvia::StaticRoot&>(), ruvia::DocumentRootRuntimeConfig{})), ruvia::detail::HttpServerOptions::DocumentRoot>);
static_assert(!HasSplitDocumentRootDispatch<ruvia::detail::RouteTable>);
static_assert(!HasRawDocumentRootDispatch<ruvia::detail::RouteTable>);
static_assert(HasCanonicalBufferedDispatch<ruvia::detail::RouteTable>);
static_assert(!HasContextlessDispatch<ruvia::detail::RouteTable>);
static_assert(!HasContextlessBufferedDispatch<ruvia::detail::RouteTable>);
static_assert(std::is_aggregate_v<ruvia::CompressionConfig>);
static_assert(std::is_aggregate_v<ruvia::CorsConfig>);
static_assert(std::is_aggregate_v<ruvia::CorsOriginConfig>);
static_assert(std::is_aggregate_v<ruvia::CorsRequestHeadersConfig>);
static_assert(std::is_aggregate_v<ruvia::DeadlineConfig>);
static_assert(std::is_aggregate_v<ruvia::DocumentRootConfig>);
static_assert(std::is_aggregate_v<ruvia::StaticRootOptions>);
static_assert(!HasRawResponseCompressionCoding<ruvia::HttpContentCoding>);
static_assert(!HasTypedResponseCompressionSelection<ruvia::detail::HttpResponseCodingSelection>);
static_assert(HasTypedResponseCompressionSelection<ruvia::detail::HttpResponseCodingPolicy>);
static_assert(!std::default_initializable<ruvia::detail::HttpResponseCompressionResult>);
static_assert(!std::default_initializable<ruvia::detail::HttpResponseCodingPolicy>);
static_assert(!HasRawApplyResponseCompressionCoding<ruvia::HttpContentCoding>);
static_assert(HasTypedApplyResponseCompressionSelection<ruvia::detail::HttpResponseCodingSelection>);
static_assert(HasTypedResponseCompressionPreflight<ruvia::detail::HttpResponseCodingSelection>);
static_assert(!HasAppDocumentRootPathSetter<ruvia::App>);
static_assert(!HasAppListenAddressSetter<ruvia::App>);
static_assert(!HasAppListenAddressPortSetter<ruvia::App>);
static_assert(HasAppListenerSetter<ruvia::App>);
static_assert(HasCanonicalOptionalAppConfigDisable<ruvia::App>);
static_assert(std::is_aggregate_v<ruvia::ListenConfig>);
static_assert(std::same_as<decltype(ruvia::ListenConfig{.address = "127.0.0.1", .http = 80, .https = 443, .tls = {.certificateChainFile = "cert.pem", .privateKeyFile = "key.pem"}, .autoHttpsRedirect = true}), ruvia::ListenConfig>);
static_assert(std::is_aggregate_v<ruvia::TlsConfig>);
static_assert(std::same_as<decltype(ruvia::TlsConfig{.certificateChainFile = "cert.pem", .privateKeyFile = "key.pem", .privateKeyPassword = std::string{}}), ruvia::TlsConfig>);
static_assert(std::is_aggregate_v<ruvia::TlsClientCertificateConfig>);
static_assert(std::is_aggregate_v<ruvia::TlsSniConfig>);
static_assert(HasCanonicalAccessLogCallback<ruvia::App>);
static_assert(std::same_as<decltype(std::declval<ruvia::App&>().onError(static_cast<ruvia::HttpErrorHandler>(nullptr))), ruvia::App&>);
static_assert(std::same_as<decltype(std::declval<ruvia::App&>().onNotFound(static_cast<ruvia::HttpNotFoundHandler>(nullptr))), ruvia::App&>);
static_assert(std::is_aggregate_v<ruvia::ScopedErrorHandlerOptions>);
static_assert(std::is_aggregate_v<ruvia::ScopedNotFoundHandlerOptions>);
static_assert(std::same_as<decltype(ruvia::ScopedErrorHandlerOptions{}.prefix), std::string>);
static_assert(std::same_as<decltype(ruvia::ScopedErrorHandlerOptions{}.handler), ruvia::HttpErrorHandler>);
static_assert(std::same_as<decltype(ruvia::ScopedNotFoundHandlerOptions{}.prefix), std::string>);
static_assert(std::same_as<decltype(ruvia::ScopedNotFoundHandlerOptions{}.handler), ruvia::HttpNotFoundHandler>);
static_assert(HasScopedAppErrorHandlerOptions<ruvia::App>);
static_assert(HasScopedAppNotFoundHandlerOptions<ruvia::App>);
static_assert(HasScopedAppErrorHandlerOptions<ruvia::TestApp>);
static_assert(HasScopedAppNotFoundHandlerOptions<ruvia::TestApp>);
static_assert(!HasScopedAppErrorHandlerPositional<ruvia::App>);
static_assert(!HasScopedAppNotFoundHandlerPositional<ruvia::App>);
static_assert(!HasScopedAppErrorHandlerPositional<ruvia::TestApp>);
static_assert(!HasScopedAppNotFoundHandlerPositional<ruvia::TestApp>);
static_assert(AcceptsAnyRvalueScopedErrorHandlerPrefix<std::string>);
static_assert(AcceptsAnyRvalueScopedNotFoundHandlerPrefix<std::string>);
static_assert(AcceptsLvalueScopedFallbackPrefix<std::string>);
static_assert(!HasBoundAccessLogCallback<ruvia::AccessLogCallback>);
static_assert(!HasPublicCallbackBorrow<ruvia::AccessLogCallback>);
static_assert(!HasPublicCallbackBorrow<ruvia::ConnectionFailureCallback>);
static_assert(!HasPublicCallbackBorrow<ruvia::HttpErrorHandler>);
static_assert(!HasPublicCallbackBorrow<ruvia::HttpNotFoundHandler>);
static_assert(!std::is_default_constructible_v<ruvia::AccessLogRecord>);
static_assert(!std::is_constructible_v<ruvia::AccessLogRecord, std::string_view, ruvia::HttpKnownMethod, std::string_view, std::string_view, std::uint16_t, std::uint64_t, bool>);
#ifndef _MSC_VER
static_assert(!HasAccessLogRecordPublicFields<ruvia::AccessLogRecord>);
#endif
static_assert(HasAccessLogRecordCanonicalReadAccessors<ruvia::AccessLogRecord>);
static_assert(!HasLegacyAccessLogHttp2Flag<ruvia::AccessLogRecord>);
static_assert(std::is_nothrow_copy_constructible_v<ruvia::AccessLogRecord>);
static_assert(!std::is_copy_assignable_v<ruvia::AccessLogRecord>);
static_assert(!std::is_default_constructible_v<ruvia::ValidationIssue>);
static_assert(!std::is_constructible_v<ruvia::ValidationIssue, std::string_view, std::string_view, std::string_view, std::pmr::memory_resource*>);
static_assert(std::is_aggregate_v<ruvia::ValidationIssueOptions>);
static_assert(std::same_as<decltype(ruvia::ValidationIssueOptions{}.field), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::ValidationIssueOptions{}.code), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::ValidationIssueOptions{}.message), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::ValidationIssueOptions{}.resource), std::pmr::memory_resource*>);
#ifndef _MSC_VER
static_assert(!HasValidationIssuePublicFields<ruvia::ValidationIssue>);
#endif
static_assert(HasValidationIssueCanonicalReadAccessors<ruvia::ValidationIssue>);
static_assert(!ExposesAnyRvalueValidationIssueBorrow<ruvia::ValidationIssue>);
static_assert(!ExposesAnyRvalueValidationErrorBorrow<ruvia::ValidationError>);
static_assert(std::is_aggregate_v<ruvia::Validator::Options>);
static_assert(std::same_as<decltype(ruvia::Validator::Options{}.resource), std::pmr::memory_resource*>);
static_assert(std::is_constructible_v<ruvia::Validator, ruvia::Validator::Options>);
static_assert(!std::is_constructible_v<ruvia::Validator, std::pmr::memory_resource*>);
static_assert(!ExposesRvalueValidatorIssues<ruvia::Validator>);
static_assert(!AcceptsAnyRvalueValidatorMutation<ruvia::Validator>);
static_assert(std::is_aggregate_v<ruvia::ValidationErrorOptions>);
static_assert(std::same_as<decltype(ruvia::ValidationErrorOptions{}.status), ruvia::HttpStatusCode>);
static_assert(std::same_as<decltype(ruvia::ValidationErrorOptions{}.code), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::ValidationErrorOptions{}.message), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::ValidationErrorOptions{}.resource), std::pmr::memory_resource*>);
static_assert(std::same_as<decltype(std::declval<ruvia::Validator&>().throwIfInvalid({.status = ruvia::http_status::kBadRequest, .code = "validation_failed", .message = "request validation failed"})), void>);
static_assert(!HasValidatorThrowIfInvalidPositional<ruvia::Validator>);
static_assert(!HasValidationErrorPositionalConstructor<ruvia::ValidationError::IssueList&>);
static_assert(!HasValidationErrorPositionalConstructor<ruvia::ValidationError::IssueList>);
static_assert(AcceptsLvalueValidationErrorOptionText<std::string>);
static_assert(!AcceptsAnyRvalueValidationErrorOptionText<std::string>);
static_assert(!AcceptsAnyRvalueValidationErrorOptionText<std::pmr::string>);
static_assert(!ExposesRvalueHttpErrorInfo<ruvia::HttpError>);
static_assert(std::is_aggregate_v<ruvia::HttpErrorInfoOptions>);
static_assert(std::same_as<decltype(ruvia::HttpErrorInfoOptions{}.status), ruvia::HttpStatusCode>);
static_assert(std::same_as<decltype(ruvia::HttpErrorInfoOptions{}.code), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::HttpErrorInfoOptions{}.message), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::HttpErrorInfoOptions{}.statusText), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::HttpErrorInfoOptions{}.validationIssues), ruvia::BorrowedValidationIssues>);
static_assert(std::same_as<decltype(ruvia::HttpErrorInfo({.status = ruvia::http_status::kBadRequest, .code = "bad", .message = "request failed"})), ruvia::HttpErrorInfo>);
static_assert(std::is_constructible_v<ruvia::HttpError, ruvia::HttpErrorInfoOptions>);
static_assert(!HasHttpErrorInfoPositionalConstructor<ruvia::HttpStatusCode>);
static_assert(!HasHttpErrorPositionalConstructor<ruvia::HttpStatusCode>);
static_assert(AcceptsLvalueHttpErrorInfoOptionText<std::string>);
static_assert(!AcceptsAnyRvalueHttpErrorInfoOptionText<std::string>);
static_assert(!AcceptsAnyRvalueHttpErrorInfoOptionText<std::pmr::string>);
static_assert(!AcceptsRvalueHttpErrorInfoOptionIssues<ruvia::ValidationError::IssueList>);
static_assert(AcceptsLvalueHttpErrorInfoOptionIssues<ruvia::ValidationError::IssueList>);
static_assert(AcceptsRvalueHttpErrorInfoOptionIssues<std::span<const ruvia::ValidationIssue>>);
static_assert(!HasHttpErrorInfoPublicFields<ruvia::HttpErrorInfo>);
static_assert(HasHttpErrorInfoCanonicalReadAccessors<ruvia::HttpErrorInfo>);
static_assert(!HasRawHttpErrorDetailsJson<ruvia::HttpErrorInfo>);
static_assert(!HasCompleteType<ruvia::detail::RouteRateLimitResult>);
static_assert(!std::is_default_constructible_v<ruvia::Http1RequestParseResult>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ParsedRequest>);
static_assert(!HasStaleHttpParseTupleAccessors<ruvia::Http1RequestParseResult>);
static_assert(HasHttp1DiscriminatedParseAccessors<ruvia::Http1RequestParseResult>);
static_assert(!HasResultKindDiscriminator<ruvia::Http1RequestParseResult>);
static_assert(!HasAnyRvalueHttp1RequestParseAccessor<ruvia::Http1RequestParseResult>);
static_assert(HasHttp1RequestBodyPlanAlternatives<ruvia::Http1RequestBodyPlan>);
static_assert(!HasStaleHttp1RequestFramingAccessor<ruvia::Http1RequestBodyPlan>);
static_assert(!HasHttp1RequestBodyContentLength<ruvia::Http1RequestBodyPlan>);
static_assert(!HasHttp1RequestBodyTransferCodings<ruvia::Http1RequestBodyPlan>);
static_assert(HasHttp1RequestBodyContentLength<ruvia::Http1KnownLengthRequestBody>);
static_assert(!HasHttp1RequestBodyContentLength<ruvia::Http1ChunkedRequestBody>);
static_assert(HasHttp1RequestBodyTransferCodings<ruvia::Http1ChunkedRequestBody>);
static_assert(!HasHttp1RequestBodyTransferCodings<ruvia::Http1KnownLengthRequestBody>);
static_assert(!HasPublicHttp1RequestBodyPlanFactories<ruvia::Http1RequestBodyPlan>);
static_assert(!std::default_initializable<ruvia::Http1RequestBodyPlan>);
static_assert(!std::constructible_from<ruvia::Http1RequestBodyPlan, ruvia::HttpRequestExpectations>);
static_assert(!std::default_initializable<ruvia::Http1RequestWithoutBody>);
static_assert(!std::default_initializable<ruvia::Http1KnownLengthRequestBody>);
static_assert(!std::default_initializable<ruvia::Http1ChunkedRequestBody>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientRequestPrepareResult>);
static_assert(HasHttp1ClientRequestPrepareAccessors<ruvia::Http1ClientRequestPrepareResult>);
static_assert(!HasResultKindDiscriminator<ruvia::Http1ClientRequestPrepareResult>);
static_assert(!HasAnyRvalueHttp1ClientRequestPrepareAccessor<ruvia::Http1ClientRequestPrepareResult>);
static_assert(HasHttp1ClientPreparedContentPlan<ruvia::PreparedHttp1ClientRequest>);
static_assert(!HasAnyRvalueHttp1ClientRequestContentPlanAccessor<ruvia::Http1ClientRequestContentPlan>);
static_assert(std::is_aggregate_v<ruvia::Http1ClientRequestWirePolicy>);
static_assert(std::same_as<decltype(ruvia::Http1ClientRequestWirePolicy{}.closePolicy), ruvia::Http1ClosePolicy>);
static_assert(std::same_as<decltype(ruvia::Http1ClientRequestWirePolicy{}.expectation), ruvia::HttpClientRequestExpectation>);
static_assert(HasHttp1ClientExpectation<ruvia::Http1ClientRequestWirePolicy>);
static_assert(!HasLegacyHttp1ClientExpectationAlternatives<ruvia::Http1ClientRequestWirePolicy>);
static_assert(!HasAnyRvalueHttp1ClientRequestWirePolicyAccessor<ruvia::Http1ClientRequestWirePolicy>);
static_assert(!HasStaleHttp1ClientPreparedContentTuple<ruvia::PreparedHttp1ClientRequest>);
static_assert(!std::default_initializable<ruvia::Http1ClientRequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::Http1ClientImmediateRequestContent>);
static_assert(!std::default_initializable<ruvia::Http1ClientContinueGatedRequestContent>);
static_assert(!HasStaleHttp1ClientResponseContext<ruvia::PreparedHttp1ClientRequest>);
static_assert(std::is_default_constructible_v<ruvia::Http1ClientRequestWirePolicy>);
static_assert(HasHttp1ClientRequestWriterContract<ruvia::Http1ClientRequestWriter>);
static_assert(std::is_aggregate_v<ruvia::Http1ClientRequestWriter::Options>);
static_assert(std::same_as<decltype(ruvia::Http1ClientRequestWriter::Options{}.resource), std::pmr::memory_resource*>);
static_assert(!HasPositionalHttp1ClientRequestWriterResource<ruvia::Http1ClientRequestWriter>);
static_assert(AcceptsHttp1ConnectHeaders<std::vector<ruvia::HttpHeaderView>&>);
static_assert(AcceptsHttp1ConnectHeaders<std::array<ruvia::HttpHeaderView, 1>&>);
static_assert(AcceptsHttp1ConnectHeaders<std::span<const ruvia::HttpHeaderView>>);
static_assert(AcceptsHttp1ConnectHeaders<std::vector<ruvia::HttpHeaderView>>);
static_assert(AcceptsHttp1ConnectHeaders<const std::vector<ruvia::HttpHeaderView>>);
static_assert(AcceptsHttp1ConnectHeaders<std::array<ruvia::HttpHeaderView, 1>>);
static_assert(AcceptsHttp1ConnectHeaders<const std::array<ruvia::HttpHeaderView, 1>>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientResponseParseResult>);
static_assert(!std::is_copy_constructible_v<ruvia::Http1ClientResponseParseResult>);
static_assert(std::is_move_constructible_v<ruvia::Http1ClientResponseParseResult>);
static_assert(!std::is_move_assignable_v<ruvia::Http1ClientResponseParseResult>);
static_assert(std::is_move_constructible_v<ruvia::Http1ParsedClientResponseHead>);
static_assert(!std::is_move_assignable_v<ruvia::Http1ParsedClientResponseHead>);
static_assert(HasHttp1ClientDiscriminatedParseAccessors<ruvia::Http1ClientResponseParseResult>);
static_assert(!HasResultKindDiscriminator<ruvia::Http1ClientResponseParseResult>);
static_assert(!HasAnyRvalueHttp1ClientResponseParseAccessor<ruvia::Http1ClientResponseParseResult>);
static_assert(HasHttp1ClientResponseParserContract<ruvia::Http1ClientResponseParser>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientResponseParser>);
static_assert(!std::is_copy_constructible_v<ruvia::Http1ClientResponseParser>);
static_assert(!std::is_move_constructible_v<ruvia::Http1ClientResponseParser>);
static_assert(std::is_constructible_v<ruvia::Http1ClientResponseParser, ruvia::Http1ClientExchangeState&&>);
static_assert(std::is_constructible_v<ruvia::Http1ClientResponseParser, ruvia::Http1ClientExchangeState&&, ruvia::Http1ClientResponseParser::Options>);
static_assert(!std::is_constructible_v<ruvia::Http1ClientResponseParser, ruvia::Http1ClientExchangeState&&, std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<ruvia::Http1ClientResponseParser, ruvia::Http1ClientExchangeState&>);
static_assert(!std::is_constructible_v<ruvia::Http1ClientResponseParser, const ruvia::PreparedHttp1ClientRequest&>);
static_assert(HasHttp1ClientRequestContentSignal<ruvia::Http1ClientResponsePlan>);
static_assert(HasHttp1ClientResponsePlanAlternatives<ruvia::Http1ClientResponsePlan>);
static_assert(HasHttp1ClientZeroContentFraming<ruvia::Http1ClientResponseWithZeroContent>);
static_assert(!HasAnyRvalueHttp1ClientResponsePlanAccessor<ruvia::Http1ClientResponsePlan>);
static_assert(!HasStaleHttp1ClientResponseMode<ruvia::Http1ClientResponsePlan>);
static_assert(!HasStaleHttp1ClientResponseConnectionAccessor<ruvia::Http1ClientResponsePlan>);
static_assert(!HasHttp1ClientResponseContentLength<ruvia::Http1ClientResponsePlan>);
static_assert(HasHttp1ClientResponseContentLength<ruvia::Http1ClientKnownLengthResponse>);
static_assert(!HasHttp1ClientResponseContentLength<ruvia::Http1ClientChunkedResponse>);
static_assert(HasHttp1ClientResponseTransferCodings<ruvia::Http1ClientChunkedResponse>);
static_assert(HasHttp1ClientResponseTransferCodings<ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!HasHttp1ClientResponseTransferCodings<ruvia::Http1ClientKnownLengthResponse>);
static_assert(HasHttp1ClosePolicy<ruvia::Http1ClientInformationalResponse>);
static_assert(HasHttp1ClosePolicy<ruvia::Http1ClientResponseWithoutContent>);
static_assert(HasHttp1ClosePolicy<ruvia::Http1ClientKnownLengthResponse>);
static_assert(HasHttp1ClosePolicy<ruvia::Http1ClientChunkedResponse>);
static_assert(!HasHttp1ClosePolicy<ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!std::default_initializable<ruvia::Http1ClientResponsePlan>);
static_assert(!std::default_initializable<ruvia::Http1ClientInformationalResponse>);
static_assert(!std::default_initializable<ruvia::Http1ClientResponseWithZeroContent>);
static_assert(!std::default_initializable<ruvia::Http1ClientKnownLengthResponse>);
static_assert(!std::default_initializable<ruvia::Http1ClientConnectTunnel>);
static_assert(!std::default_initializable<ruvia::Http1ClientProtocolUpgrade>);
static_assert(!HasStaleHttp1ClientHeadOffset<ruvia::Http1ParsedClientResponseHead>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::Http1ClientChunkedResponse>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!AcceptsTemporaryHttpClientResponseHeaderLookup<ruvia::HttpClientResponseHead>);
static_assert(std::same_as<decltype(ruvia::lookupUniqueHttpClientResponseHeader(std::declval<const ruvia::HttpClientResponseHead&>(), std::string_view{})), ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!std::is_default_constructible_v<ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(HasHttpClientHeaderLookupAccessors<ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!HasAnyRvalueHttpClientHeaderLookupAccessor<ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!HasHttpClientHeaderValue<ruvia::HttpClientResponseHeaderAbsent>);
static_assert(HasHttpClientHeaderValue<ruvia::HttpClientResponseHeaderFound>);
static_assert(!HasHttpClientHeaderValue<ruvia::HttpClientResponseHeaderRepeated>);
static_assert(!HasHttpClientRedirectStatus<ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(std::same_as<decltype(ruvia::planHttpClientRedirectRequest(std::declval<const ruvia::HttpClientRequestView&>(), ruvia::HttpClientRedirectRequestPlanOptions{.status = ruvia::http_status::kFound})), ruvia::HttpClientRedirectRequestPlan>);
static_assert(std::is_aggregate_v<ruvia::HttpClientRedirectRequestPlanOptions>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClientRedirectRequestPlanOptions&>().status), ruvia::HttpStatusCode>);
static_assert(std::same_as<decltype(ruvia::HttpClientRedirectRequestPlanOptions{.status = ruvia::http_status::kFound}.resource), std::pmr::memory_resource*>);
static_assert(!std::is_copy_constructible_v<ruvia::HttpClientRedirectRequestPlan>);
static_assert(std::is_move_constructible_v<ruvia::HttpClientRedirectRequestPlan>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::HttpClientRedirectRequestPlan>);
static_assert(std::same_as<decltype(ruvia::resolveHttpClientRedirectTarget(std::declval<const ruvia::HttpOriginView&>(), ruvia::HttpClientRedirectTargetOptions{.currentTarget = "/", .location = "/next"})), ruvia::HttpClientRedirectResolutionResult>);
static_assert(std::is_aggregate_v<ruvia::HttpClientRedirectTargetOptions>);
static_assert(std::same_as<decltype(ruvia::HttpClientRedirectTargetOptions{}.currentTarget), std::string_view>);
static_assert(std::same_as<decltype(ruvia::HttpClientRedirectTargetOptions{}.location), std::string_view>);
static_assert(std::same_as<decltype(ruvia::HttpClientRedirectTargetOptions{}.resource), std::pmr::memory_resource*>);
static_assert(!HasPositionalHttpClientRedirectRequestPlan<ruvia::HttpStatusCode>);
static_assert(!HasPositionalHttpClientRedirectTargetResolution<std::string_view>);
static_assert(!std::is_copy_constructible_v<ruvia::HttpClientRedirectResolutionResult>);
static_assert(std::is_move_constructible_v<ruvia::HttpClientRedirectResolutionResult>);
static_assert(!std::is_copy_constructible_v<ruvia::HttpClientResolvedRedirect>);
static_assert(std::is_move_constructible_v<ruvia::HttpClientResolvedRedirect>);

[[maybe_unused]] void classifyCrossOriginRedirect(const ruvia::HttpOriginView& origin, std::string_view currentTarget, std::string_view location) {
    const auto result = ruvia::resolveHttpClientRedirectTarget(origin, {.currentTarget = currentTarget, .location = location});
    if (const auto* resolved = result.resolved()) {
        [[maybe_unused]] const auto scheme = resolved->scheme();
        [[maybe_unused]] const auto host = resolved->host();
        [[maybe_unused]] const auto port = resolved->port();
        [[maybe_unused]] const auto target = resolved->target();
        if (!resolved->crossOrigin()) {
            return;  // same origin: reuse the connection and credentials
        }
        // Cross-origin: drop Authorization/Proxy-Authorization and cookie
        // material before following, then connect to resolved->origin().
        [[maybe_unused]] const auto nextOrigin = resolved->origin();
        return;
    }
    switch (result.failure()->error()) {
        case ruvia::HttpClientRedirectResolutionError::kInvalidCurrentTarget:
        case ruvia::HttpClientRedirectResolutionError::kInvalidLocation:
        case ruvia::HttpClientRedirectResolutionError::kUnsupportedScheme:
            break;
    }
}
static_assert(!std::is_default_constructible_v<ruvia::DotenvResult>);
#ifndef _MSC_VER
static_assert(!HasDotenvResultPublicFields<ruvia::DotenvResult>);
#endif
static_assert(HasDotenvResultCanonicalReadAccessors<ruvia::DotenvResult>);
static_assert(std::same_as<decltype(ruvia::DotenvOptions{}.existingVariables), ruvia::DotenvExistingVariablePolicy>);
static_assert(std::same_as<decltype(ruvia::DotenvOptions{}.missingFile), ruvia::DotenvMissingFilePolicy>);
static_assert(ruvia::DotenvOptions{}.existingVariables == ruvia::DotenvExistingVariablePolicy::kPreserve);
static_assert(ruvia::DotenvOptions{}.missingFile == ruvia::DotenvMissingFilePolicy::kIgnore);
static_assert(!HasDotenvOverrideExistingBoolean<ruvia::DotenvOptions>);
static_assert(!HasDotenvRequiredBoolean<ruvia::DotenvOptions>);
static_assert(!ExposesAnyRvalueEnvBorrow<ruvia::Env>);
static_assert(!HasContextRenderPipeline<ruvia::Context>);
static_assert(std::is_aggregate_v<ruvia::ReadinessResponseOptions>);
static_assert(std::same_as<decltype(ruvia::ReadinessResponseOptions{}.state), ruvia::ReadinessState>);
static_assert(std::same_as<decltype(ruvia::ReadinessResponseOptions{}.unavailableReason), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::makeReadinessResponse(std::declval<ruvia::Context&>())), ruvia::HttpResponse>);
static_assert(std::same_as<decltype(ruvia::makeReadinessResponse(std::declval<ruvia::Context&>(), {.state = ruvia::ReadinessState::kUnavailable, .unavailableReason = "not ready"})), ruvia::HttpResponse>);
static_assert(!HasLegacyMakeReadyResponse<ruvia::Context, bool>);
static_assert(AcceptsLvalueReadinessUnavailableReason<std::string>);
static_assert(!AcceptsTemporaryReadinessUnavailableReason<std::string>);
static_assert(!AcceptsTemporaryReadinessUnavailableReason<std::pmr::string>);
static_assert(std::is_aggregate_v<ruvia::RedirectResponseOptions>);
static_assert(std::same_as<decltype(ruvia::RedirectResponseOptions{}.location), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::RedirectResponseOptions{}.status), ruvia::HttpStatusCode>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::Context&>().redirect({.location = "/next", .status = ruvia::http_status::kSeeOther})), ruvia::HttpResponse>);
static_assert(!HasContextRedirectPositional<ruvia::Context>);
static_assert(AcceptsLvalueRedirectLocation<std::string>);
static_assert(!AcceptsTemporaryRedirectLocation<std::string>);
static_assert(!AcceptsTemporaryRedirectLocation<std::pmr::string>);
static_assert(std::is_aggregate_v<ruvia::CsrfProtectionConfig>);
static_assert(std::same_as<decltype(ruvia::CsrfProtectionConfig{}.cookieName), std::string>);
static_assert(std::same_as<decltype(ruvia::CsrfProtectionConfig{}.headerName), std::string>);
static_assert(std::is_constructible_v<ruvia::CsrfProtection, ruvia::CsrfProtectionConfig>);
static_assert(!std::copyable<ruvia::CsrfProtection>);
static_assert(!std::movable<ruvia::CsrfProtection>);
static_assert(!HasCsrfProtectionPositionalConstructor<ruvia::CsrfProtection>);
static_assert(AcceptsLvalueCsrfProtectionOptionText<std::string>);
static_assert(AcceptsAnyRvalueCsrfProtectionOptionText<std::string>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::Context&>().error({.status = ruvia::http_status::kBadRequest, .code = "bad", .message = "request failed"})), ruvia::HttpResponse>);
static_assert(!HasContextErrorPositional<ruvia::Context>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::Context&>().status(ruvia::http_status::kNoContent)), void>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::Context&>().header(std::string_view{}, std::string_view{})), void>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::Context&>().header(std::string_view{}, std::string_view{}, ruvia::Context::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend})), void>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::Context&>().removeHeader(std::string_view{})), void>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::Context&>().respond(std::declval<ruvia::HttpResponse&&>())), void>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::Context&>().response()), const ruvia::HttpResponse*>);
static_assert(std::is_aggregate_v<ruvia::SetCookieOptions>);
static_assert(std::is_aggregate_v<ruvia::SetSignedCookieOptions>);
static_assert(std::is_aggregate_v<ruvia::DeleteCookieOptions>);
static_assert(std::is_aggregate_v<ruvia::SignedCookieLookupOptions>);
static_assert(std::is_aggregate_v<ruvia::StaticFileResponseOptions>);
static_assert(std::is_aggregate_v<ruvia::FileResponseOptions>);
static_assert(std::same_as<decltype(ruvia::SetCookieOptions{}.name), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::SetCookieOptions{}.value), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::SetCookieOptions{}.attributes), ruvia::CookieOptions>);
static_assert(std::same_as<decltype(ruvia::SetSignedCookieOptions{}.name), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::SetSignedCookieOptions{}.value), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::SetSignedCookieOptions{}.secret), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::DeleteCookieOptions{}.name), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::SignedCookieLookupOptions{}.name), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::SignedCookieLookupOptions{}.secret), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::StaticFileResponseOptions{}.relativePath), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::StaticFileResponseOptions{}.contentType), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::FileResponseOptions{}.path), std::filesystem::path>);
static_assert(std::same_as<decltype(ruvia::FileResponseOptions{}.contentType), ruvia::BorrowedText>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::Context&>().setCookie({.name = "name", .value = "value"})), void>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::Context&>().file(ruvia::FileResponseOptions{.path = std::filesystem::path{}})), ruvia::HttpResponse>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::Context&>().staticFile(std::declval<const ruvia::StaticRoot&>(), ruvia::StaticFileResponseOptions{.relativePath = "index.html"})), ruvia::HttpResponse>);
static_assert(!HasContextSetCookiePositional<ruvia::Context>);
static_assert(!HasContextSetSignedCookiePositional<ruvia::Context>);
static_assert(!HasContextDeleteCookiePositional<ruvia::Context>);
static_assert(!HasContextRequestSignedCookiePositional<ruvia::ContextRequest>);
static_assert(!HasContextStaticFilePositional<ruvia::Context>);
static_assert(!HasContextFilePositional<ruvia::Context>);
static_assert(!AcceptsAnyRvalueSetCookieOptionText<std::string>);
static_assert(!AcceptsAnyRvalueSetCookieOptionText<std::pmr::string>);
static_assert(!AcceptsAnyRvalueSetSignedCookieOptionText<std::string>);
static_assert(!AcceptsAnyRvalueSetSignedCookieOptionText<std::pmr::string>);
static_assert(!AcceptsAnyRvalueDeleteCookieOptionText<std::string>);
static_assert(!AcceptsAnyRvalueDeleteCookieOptionText<std::pmr::string>);
static_assert(!AcceptsAnyRvalueSignedCookieLookupOptionText<std::string>);
static_assert(!AcceptsAnyRvalueSignedCookieLookupOptionText<std::pmr::string>);
static_assert(!AcceptsAnyRvalueStaticFileOptionText<std::string>);
static_assert(!AcceptsAnyRvalueStaticFileOptionText<std::pmr::string>);
static_assert(!AcceptsAnyRvalueFileOptionText<std::string>);
static_assert(!AcceptsAnyRvalueFileOptionText<std::pmr::string>);
constexpr ruvia::CookieOptions kLiteralCookieOptions{.path = "/app", .domain = "example.com"};
static_assert(kLiteralCookieOptions.path.view() == "/app");
static_assert(kLiteralCookieOptions.domain.view() == "example.com");
static_assert(std::same_as<decltype(ruvia::CookieOptions{}.httpOnly), ruvia::CookieAttributePolicy>);
static_assert(std::same_as<decltype(ruvia::CookieOptions{}.secure), ruvia::CookieAttributePolicy>);
static_assert(std::same_as<decltype(ruvia::CookieOptions{}.partitioned), ruvia::CookieAttributePolicy>);
template <typename T>
concept HasCookieHttpOnlyBoolean = requires(T& options) { options.httpOnly = true; };
template <typename T>
concept HasCookieSecureBoolean = requires(T& options) { options.secure = true; };
template <typename T>
concept HasCookiePartitionedBoolean = requires(T& options) { options.partitioned = true; };
static_assert(!HasCookieHttpOnlyBoolean<ruvia::CookieOptions>);
static_assert(!HasCookieSecureBoolean<ruvia::CookieOptions>);
static_assert(!HasCookiePartitionedBoolean<ruvia::CookieOptions>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::Context&>().deleteCookie({.name = "name"})), void>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::Context&>().setSignedCookie({.name = "name", .value = "value", .secret = "secret"})), void>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::ContextRequest&>().signedCookie({.name = "name", .secret = "secret"})), std::optional<std::string_view>>);
static_assert(!HasContextCookieGenerator<ruvia::Context>);
static_assert(!HasContextSignedCookieGenerator<ruvia::Context>);
static_assert(HasResponseStreamOwningWrite<ruvia::ResponseStreamWriter>);
static_assert(!HasResponseStreamWriteOwnedAlias<ruvia::ResponseStreamWriter>);
template <typename Reader>
concept HasRvalueBodyReaderOperation = requires(Reader&& reader) { std::move(reader).read(); };
template <typename Writer>
concept HasRvalueResponseStreamOperation = requires(Writer&& writer) { std::move(writer).write(std::string_view{}); } || requires(Writer&& writer) { std::move(writer).write(std::pmr::string{}); } || requires(Writer&& writer) { std::move(writer).writeln(std::string_view{}); } || requires(Writer&& writer) { std::move(writer).sleep(std::chrono::milliseconds{1}); } || requires(Writer&& writer) { std::move(writer).end(); };
template <typename Socket>
concept HasRvalueWebSocketOperation = requires(Socket&& socket) { std::move(socket).read(); } || requires(Socket&& socket) { std::move(socket).text(std::string_view{}); } || requires(Socket&& socket) { std::move(socket).text(std::pmr::string{}); } || requires(Socket&& socket) { std::move(socket).binary(std::string_view{}); } || requires(Socket&& socket) { std::move(socket).binary(std::pmr::string{}); } || requires(Socket&& socket) { std::move(socket).pong(std::string_view{}); } || requires(Socket&& socket) { std::move(socket).pong(std::pmr::string{}); } || requires(Socket&& socket) { std::move(socket).ping(); } || requires(Socket&& socket) { std::move(socket).ping(std::pmr::string{}); } || requires(Socket&& socket) { std::move(socket).close(); };
template <typename Reader>
concept HasRvalueMultipartReaderOperation = requires(Reader&& reader) { std::move(reader).read(); };
template <typename Body>
concept HasRvalueHttpClientResponseBodyOperation = requires(Body&& body) { std::move(body).read(); } || requires(Body&& body) { std::move(body).readAll(); } || requires(Body&& body, ruvia::ResponseStreamWriter& output) { std::move(body).pipeTo(output); };
static_assert(!HasRvalueBodyReaderOperation<ruvia::BodyReader>);
static_assert(!HasRvalueResponseStreamOperation<ruvia::ResponseStreamWriter>);
static_assert(!HasRvalueWebSocketOperation<ruvia::WebSocket>);
static_assert(!HasRvalueMultipartReaderOperation<ruvia::MultipartReader>);
static_assert(!HasRvalueHttpClientResponseBodyOperation<ruvia::HttpClientResponseBody>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::ResponseStreamWriter&>().write(std::pmr::string{})), ruvia::ScopedOperation<void>>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::ResponseStreamWriter&>().writeln(std::string_view{})), ruvia::ScopedOperation<void>>);
static_assert(HasWebSocketOwningWrites<ruvia::WebSocket>);
static_assert(!HasWebSocketOwnedWriteAlias<ruvia::WebSocket>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::WebSocket&>().text(std::pmr::string{})), ruvia::ScopedOperation<void>>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::WebSocket&>().binary(std::pmr::string{})), ruvia::ScopedOperation<void>>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::WebSocket&>().pong(std::pmr::string{})), ruvia::ScopedOperation<void>>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::WebSocket&>().ping(std::pmr::string{})), ruvia::ScopedOperation<void>>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::ResponseStreamWriter&>().sleep(std::chrono::milliseconds{1})), ruvia::ScopedOperation<ruvia::TimerSleepResult>>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::ResponseStreamWriter&>().end(std::declval<std::span<const ruvia::HttpHeaderView>>())), ruvia::ScopedOperation<void>>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::ResponseStreamWriter&>().aborted()), bool>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::Context&>().streamSse()), ruvia::SseWriter>);
static_assert(std::is_same_v<decltype(std::declval<ruvia::SseWriter&>().sleep(std::chrono::milliseconds{1})), ruvia::ScopedOperation<ruvia::TimerSleepResult>>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::SseWriter&>().aborted()), bool>);
static_assert(!std::is_constructible_v<ruvia::SseWriter, ruvia::ResponseStreamWriter&>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::ContextRequest&>().queries(std::string_view{})), std::span<const std::string_view>>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::ContextRequest&>().header(std::string_view{})), std::optional<std::string_view>>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::ContextRequest&>().query(std::string_view{})), std::optional<std::string_view>>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::ContextRequest&>().param(std::string_view{})), std::optional<std::string_view>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::Context&>().httpClient()), ruvia::HttpClientHandle>);
static_assert(std::same_as<decltype(std::declval<const ruvia::Context&>().httpClient(std::string_view{})), ruvia::HttpClientHandle>);
static_assert(std::same_as<decltype(std::declval<const ruvia::WebWorkerContext&>().httpClient()), ruvia::HttpClientHandle>);
static_assert(std::same_as<decltype(std::declval<const ruvia::WebWorkerContext&>().httpClient(std::string_view{})), ruvia::HttpClientHandle>);
static_assert(std::constructible_from<ruvia::HttpClient, ruvia::EventLoop, ruvia::HttpClientConfig>);
static_assert(!std::copy_constructible<ruvia::HttpClient>);
static_assert(!std::move_constructible<ruvia::HttpClient>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClient&>().send(ruvia::HttpClientRequestView{})), ruvia::ScopedOperation<ruvia::HttpClientResponse>>);
static_assert(std::same_as<decltype(std::declval<ruvia::HttpClient&>().shutdown()), ruvia::Task<void>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClient&>().worker()), const ruvia::WorkerHandle&>);
template <typename Client>
concept HasRvalueStandaloneHttpClientOperation = requires(Client&& client) { std::move(client).withOptions({}); } || requires(Client&& client) { std::move(client).send(ruvia::HttpClientRequestView{}); } || requires(Client&& client) { std::move(client).shutdown(); };
static_assert(!HasRvalueStandaloneHttpClientOperation<ruvia::HttpClient>);
static_assert(!HasDynamicHttpClientFactory<ruvia::HttpClientHandle>);
static_assert(!HasHttpClientRequestBuilder<ruvia::HttpClientHandle>);
static_assert(HasHttpClientWithOptions<ruvia::HttpClientHandle>);
static_assert(!HasHttpClientStatsAliases<ruvia::HttpClientHandle>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClientHandle&>().host()), std::string_view>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClientHandle&>().scheme()), ruvia::HttpScheme>);
static_assert(!HasHttpClientSchemeAliases<ruvia::HttpClientHandle>);
static_assert(!HasHttpClientRuntimeConfiguration<ruvia::HttpClientHandle>);
static_assert(std::is_aggregate_v<ruvia::HttpClientConfig>);
static_assert(std::same_as<decltype(ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp, .host = "localhost"}), ruvia::HttpClientConfig>);
static_assert(std::is_aggregate_v<ruvia::HttpClientRegistrationConfig>);
static_assert(std::same_as<decltype(ruvia::HttpClientRegistrationConfig{}.config), ruvia::HttpClientConfig>);
static_assert(std::same_as<decltype(std::declval<ruvia::App&>().httpClient(ruvia::HttpClientRegistrationConfig{})), ruvia::App&>);
static_assert(std::same_as<decltype(std::declval<ruvia::App&>().httpClient(nullptr)), ruvia::App&>);
#ifdef RUVIA_ENABLE_DATABASE
static_assert(std::same_as<decltype(std::declval<ruvia::App&>().database(nullptr)), ruvia::App&>);
#endif
#ifdef RUVIA_ENABLE_REDIS
static_assert(std::same_as<decltype(std::declval<ruvia::App&>().redis(nullptr)), ruvia::App&>);
#endif
static_assert(std::same_as<decltype(std::declval<ruvia::App&>().trustedProxies(nullptr)), ruvia::App&>);
static_assert(!HasHttpClientOriginCacheCapacitySetter<ruvia::App>);
static_assert(!HasMaxHttpClientOriginsSetter<ruvia::App>);
static_assert(std::is_aggregate_v<ruvia::HttpClientRequestView>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClientHandle&>().send(ruvia::HttpClientRequestView{.target = "/"})), ruvia::ScopedOperation<ruvia::HttpClientResponse>>);
static_assert(std::same_as<decltype(std::declval<ruvia::HttpClientConfig&>().writeTimeout), std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<decltype(std::declval<ruvia::HttpClientConfig&>().tlsPeerVerification), ruvia::TlsPeerVerificationPolicy>);
static_assert(ruvia::HttpClientConfig{}.tlsPeerVerification == ruvia::TlsPeerVerificationPolicy::kVerify);
static_assert(!HasHttpClientVerifyCertificateBoolean<ruvia::HttpClientConfig>);
static_assert(HasHttpClientTcpSocketPolicies<ruvia::HttpClientConfig>);
static_assert(ruvia::HttpClientConfig{}.tcpNoDelay == ruvia::TcpNoDelayPolicy::kEnable);
static_assert(ruvia::HttpClientConfig{}.tcpKeepAlive == ruvia::TcpKeepAlivePolicy::kEnable);
static_assert(!HasHttpClientTcpNoDelayBoolean<ruvia::HttpClientConfig>);
static_assert(!HasHttpClientKeepAliveBoolean<ruvia::HttpClientConfig>);
static_assert(std::same_as<decltype(std::declval<ruvia::HttpClientConfig&>().receivedCookies), ruvia::HttpClientReceivedCookiePolicy>);
static_assert(ruvia::HttpClientConfig{}.receivedCookies == ruvia::HttpClientReceivedCookiePolicy::kIgnore);
static_assert(!HasHttpClientCookiesEnabledBoolean<ruvia::HttpClientConfig>);
static_assert(std::same_as<decltype(std::declval<ruvia::HttpClientConfig&>().maxCookies), std::size_t>);
static_assert(std::same_as<decltype(std::declval<ruvia::HttpClientConfig&>().maxCookieBytes), std::size_t>);
static_assert(!HasAliasedUseHttpClient<ruvia::App>);
static_assert(!std::is_copy_constructible_v<ruvia::HttpClientResponse>);
static_assert(std::is_move_constructible_v<ruvia::HttpClientResponse>);
static_assert(!std::is_move_constructible_v<ruvia::HttpClientResponseBody>);
static_assert(!std::is_move_assignable_v<ruvia::HttpClientResponseBody>);
static_assert(!std::is_destructible_v<ruvia::HttpClientResponseBody>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClientResponse&>().headers()), std::span<const ruvia::HttpClientResponseHeader>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClientResponse&>().trailers()), std::span<const ruvia::HttpClientResponseHeader>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClientResponse&>().status()), ruvia::HttpStatusCode>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClientResponse&>().header(std::string_view{})), std::optional<std::string_view>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClientResponse&>().trailer(std::string_view{})), std::optional<std::string_view>>);
static_assert(!HasResponseStatusCodeAlias<ruvia::HttpClientResponse>);
static_assert(!HasHttpClientResponseGetHeader<ruvia::HttpClientResponse>);
static_assert(!HasHttpClientResponseGetTrailer<ruvia::HttpClientResponse>);
static_assert(std::same_as<decltype(std::declval<ruvia::HttpClientResponse&>().body()), ruvia::HttpClientResponseBody&>);
template <typename Response>
concept ConstHttpClientResponseBody = requires(const Response& response) { response.body(); };
static_assert(!ConstHttpClientResponseBody<ruvia::HttpClientResponse>);
static_assert(!std::is_default_constructible_v<ruvia::HttpClientResponse>);

template <typename Response>
concept HttpClientRvalueHeaderLookup = requires(Response&& response) {
    std::move(response).header(std::string_view{});
    std::move(response).trailer(std::string_view{});
};

static_assert(!HttpClientRvalueHeaderLookup<ruvia::HttpClientResponse>);

#ifdef RUVIA_ENABLE_REDIS
static_assert(!std::is_convertible_v<ruvia::RedisPipeline*, ruvia::detail::RedisCommandBatchMixin<ruvia::RedisPipeline>*>);
static_assert(!std::is_convertible_v<ruvia::RedisTransaction*, ruvia::detail::RedisCommandBatchMixin<ruvia::RedisTransaction>*>);

[[maybe_unused]] void instantiateRedisTransactionTypedCommands(ruvia::RedisTransaction& transaction) {
    transaction.incrBy("counter", 1).decrBy("counter", 1).lrange("items", 0, -1).zadd("scores", 1.0, "member").zrange("scores", 0, -1);
}
#endif

}  // namespace

int main() {
    const ruvia::DbMigratorOptions migrationOptions;
    if (migrationOptions.table != "ruvia_schema_migrations") {
        return 1;
    }
    return 0;
}
