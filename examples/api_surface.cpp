#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/web/App.h"
#include "ruvia/web/auth/Jwt.h"
#include "ruvia/web/db/DbMigration.h"
#include "ruvia/web/db/DbQueryResult.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/Csrf.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpParser.h"
#include "ruvia/web/RateLimit.h"
#include "ruvia/web/Session.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia::detail {
class RouteRateLimitResult;
}  // namespace ruvia::detail

#ifdef RUVIA_GET_DYNAMIC
#error "RUVIA_GET_DYNAMIC must not be public; use RUVIA_GET_STREAM or RUVIA_GET_SSE for explicit response streaming"
#endif

#ifdef RUVIA_POST_DYNAMIC
#error "RUVIA_POST_DYNAMIC must not be public; ordinary routes must not enter response streaming dynamically"
#endif

namespace {

struct CurrentUser final {
    std::uint32_t id{0};
    std::string_view name;
};

struct AppUseProbeMiddleware;

RUVIA_MODEL(ClonePayload,
    RUVIA_FIELD(message, ruvia::String)
);

RUVIA_MODEL(SurfaceJsonResponse,
    RUVIA_FIELD(message, ruvia::String)
);

inline constexpr ruvia::ContextKey<CurrentUser> kCurrentUser("currentUser");

using DetailRequestBodyMode = ruvia::detail::RequestBodyMode;
using DetailResponseBodyMode = ruvia::detail::ResponseBodyMode;
static_assert(std::is_enum_v<DetailRequestBodyMode>);
static_assert(std::is_enum_v<DetailResponseBodyMode>);

template <typename T>
concept HasPlainAddressOf = requires(T& value) {
    &value;
};

template <typename T>
concept HasLvalueAwait = requires(T& value) {
    value.operator co_await();
};

template <typename T>
concept HasDefaultValid = requires(const ruvia::ContextRequest& request) {
    request.template valid<T>();
};

template <typename T>
concept HasUnaryContextHeader = requires(const T& context) {
    context.header(std::string_view{});
};

template <typename T>
concept HasUnaryContextQuery = requires(const T& context) {
    context.query(std::string_view{});
};

template <typename T>
concept HasUnaryContextCookie = requires(const T& context) {
    context.cookie(std::string_view{});
};

template <typename T>
concept HasUnaryContextParam = requires(const T& context) {
    context.param(std::string_view{});
};

template <typename T>
concept HasContextStatusTextSetter = requires(T& context) {
    context.status(200, std::string_view{});
};

template <typename T>
concept HasResponseHeadersAlias = requires(T& response) {
    response.responseHeaders();
};

template <typename T>
concept HasRequestBytesAlias = requires(const T& request) {
    request.bytes();
};

template <typename T>
concept HasRequestBlobArrayBufferAlias = requires(const T& blob) {
    blob.arrayBuffer();
};

template <typename T>
concept HasRequestJsonValueAlias = requires(const T& request) {
    { request.json() } -> std::same_as<ruvia::Task<ruvia::JsonValue>>;
};

template <typename T>
concept HasMemberCloneRawRequestAlias = requires(const T& request) {
    request.cloneRawRequest();
};

template <typename T>
concept HasRawRequestCloneTextAlias = requires(const T& request) {
    request.text();
};

template <typename T>
concept HasRawRequestCloneBodyGetter = requires(const T& request) {
    { request.body() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasRawRequestCloneBytesGetter = requires(const T& request) {
    { request.bytes() } -> std::same_as<std::span<const std::byte>>;
};

template <typename T>
concept HasRawRequestCloneCanonicalReadAccessors = requires(const T& request) {
    { request.method() } -> std::same_as<std::string_view>;
    { request.url() } -> std::same_as<std::string_view>;
    { request.path() } -> std::same_as<std::string_view>;
    { request.header(std::string_view{}) } -> std::same_as<std::string_view>;
    { request.body() } -> std::same_as<std::string_view>;
    { request.bytes() } -> std::same_as<std::span<const std::byte>>;
    { request.blob() } -> std::same_as<ruvia::ContextRequest::RequestBlob>;
    { request.parseBody() } -> std::same_as<ruvia::ContextRequest::RequestFormData>;
    { request.formData() } -> std::same_as<ruvia::ContextRequest::RequestFormData>;
    { request.isSecure() } -> std::same_as<bool>;
};

template <typename T>
concept HasRawRequestCloneArrayBufferAlias = requires(const T& request) {
    request.arrayBuffer();
};

template <typename T>
concept HasRawRequestCloneMethodEnumAlias = requires(const T& request) {
    request.methodEnum();
};

template <typename T>
concept HasRawRequestCloneHeadersAlias = requires(const T& request) {
    request.headers();
};

template <typename T>
concept HasRawRequestCloneTargetAlias = requires(const T& request) {
    request.target();
};

template <typename T>
concept HasRawRequestCloneQueryStringAlias = requires(const T& request) {
    request.queryString();
};

template <typename T>
concept HasRawRequestCloneHttpVersionAlias = requires(const T& request) {
    request.httpVersion();
};

template <typename T>
concept HasRawRequestCloneRemoteAddressAlias = requires(const T& request) {
    request.remoteAddress();
};

template <typename T>
concept HasRawRequestCloneClientCertificateAlias = requires(const T& request) {
    request.clientCertificate();
};

template <typename T>
concept HasRequestMethodEnumAlias = requires(const T& request) {
    request.methodEnum();
};

template <typename T>
concept HasRequestTargetAlias = requires(const T& request) {
    request.target();
};

template <typename T>
concept HasRequestHeadersAlias = requires(const T& request) {
    request.headers();
};

template <typename T>
concept HasContextRequestHeaderListAccessor = requires(const T& request) {
    request.header();
};

template <typename T>
concept HasRequestQueryStringAlias = requires(const T& request) {
    request.queryString();
};

template <typename T>
concept HasRequestRoutePathAlias = requires(const T& request) {
    request.routePath();
};

template <typename T>
concept HasRequestMatchedRoutesAlias = requires(const T& request) {
    request.matchedRoutes();
};

template <typename T>
concept HasRequestRouteIndexAlias = requires(const T& request) {
    request.routeIndex();
};

template <typename T>
concept HasRequestHttpVersionAlias = requires(const T& request) {
    request.httpVersion();
};

template <typename T>
concept HasRequestDecodedPathAlias = requires(const T& request) {
    request.decodedPath();
};

template <typename T>
concept HasRequestRemoteAddressAlias = requires(const T& request) {
    request.remoteAddress();
};

template <typename T>
concept HasRequestClientCertificateAlias = requires(const T& request) {
    request.clientCertificate();
};

template <typename T>
concept HasRequestIsSecureAlias = requires(const T& request) {
    request.isSecure();
};

template <typename T>
concept HasFormValueToStringView = requires(const T& value) {
    value.toStringView();
};

template <typename T>
concept HasFormValueTextAlias = requires(const T& value) {
    value.text();
};

template <typename T>
concept HasFormValueGetter = requires(const T& value) {
    { value.value() } -> std::same_as<std::optional<std::string_view>>;
};

template <typename T>
concept HasFormValueValueOrAlias = requires(const T& value) {
    value.value_or(std::string_view{});
};

template <typename T>
concept HasFormValueTextsAlias = requires(const T& value) {
    value.texts();
};

template <typename T>
concept HasFormValueValuesGetter = requires(const T& value) {
    value.values();
};

template <typename T>
concept HasFormValueArrowOperator = requires(const T& value) {
    value.operator->();
};

template <typename T>
concept HasFormValueExistsAlias = requires(const T& value) {
    value.exists();
};

template <typename T>
concept HasFormValueIsArrayAlias = requires(const T& value) {
    value.isArray();
};

template <typename T>
concept HasFormValueIsFileAlias = requires(const T& value) {
    value.isFile();
};

template <typename T>
concept HasFormFieldBooleanMethodAliases = requires(const T& field) {
    field.isFile();
    field.isArray();
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
    { field.file() } -> std::same_as<bool>;
    { field.array() } -> std::same_as<bool>;
};

template <typename T>
concept HasFormFieldTextAlias = requires(const T& field) {
    field.text();
};

template <typename T>
concept HasFormFieldFileNameAlias = requires(const T& field) {
    field.fileName();
};

template <typename T>
concept HasFormFieldMediaTypeAlias = requires(const T& field) {
    field.mediaType();
};

template <typename T>
concept HasFormFieldArrayBufferAlias = requires(const T& field) {
    field.arrayBuffer();
};

template <typename T>
concept HasFormValueFileNameAlias = requires(const T& value) {
    value.fileName();
};

template <typename T>
concept HasFormValueMediaTypeAlias = requires(const T& value) {
    value.mediaType();
};

template <typename T>
concept HasFormValueZeroAllocationAccessors = requires(const T& value) {
    { value.size() } -> std::same_as<std::size_t>;
    { value.fields() } -> std::same_as<std::span<const ruvia::ContextRequest::RequestFormField* const>>;
    { value.field() } -> std::same_as<const ruvia::ContextRequest::RequestFormField*>;
    { value.array() } -> std::same_as<bool>;
};

template <typename T>
concept HasHttpRequestQueryGetter = requires(const T& request) {
    { request.query(std::string_view{}) } -> std::same_as<std::optional<std::string_view>>;
};

template <typename T>
concept HasHttpRequestDecodedPathAlias = requires(const T& request) {
    request.decodedPath();
};

template <typename T>
concept HasFormDataGetAllAlias = requires(const T& form) {
    form.getAll(std::string_view{});
};

template <typename T>
concept HasFormDataValuesAllAlias = requires(const T& form) {
    form.values();
};

template <typename T>
concept HasFormDataNamedValuesAllocator = requires(const T& form) {
    form.values(std::string_view{});
};

template <typename T>
concept HasFormDataNamedFieldsAllocator = requires(const T& form) {
    form.fields(std::string_view{});
};

template <typename T>
concept HasFormDataNamedFieldAlias = requires(const T& form) {
    form.field(std::string_view{});
};

template <typename T>
concept HasFormDataIsArrayAlias = requires(const T& form) {
    form.isArray(std::string_view{});
};

template <typename T>
concept HasFormDataValueAlias = requires(const T& form) {
    form.value(std::string_view{});
};

template <typename T>
concept HasFormDataHasAlias = requires(const T& form) {
    form.has(std::string_view{});
};

template <typename T>
concept HasFormDataKeysAllocator = requires(const T& form) {
    form.keys();
};

template <typename T>
concept HasFormDataEntriesAlias = requires(const T& form) {
    form.entries();
};

template <typename T>
concept HasFormDataIndexAlias = requires(const T& form) {
    form[std::string_view{}];
};

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
    { form.groups() } -> std::same_as<std::span<const ruvia::ContextRequest::RequestFormData::Entry>>;
    { form.get(std::string_view{}) } -> std::same_as<ruvia::ContextRequest::RequestFormData::Value>;
    { form.count(std::string_view{}) } -> std::same_as<std::size_t>;
    { form.at(std::string_view{}) } -> std::same_as<ruvia::ContextRequest::RequestFormData::PathValue>;
};

template <typename T>
concept HasFormObjectGetAllAlias = requires(const T& object) {
    object.getAll(std::string_view{});
};

template <typename T>
concept HasFormObjectKeysAllocator = requires(const T& object) {
    object.keys();
};

template <typename T>
concept HasFormObjectEntriesAlias = requires(const T& object) {
    object.entries();
};

template <typename T>
concept HasFormObjectIndexAlias = requires(const T& object) {
    object[std::string_view{}];
};

template <typename T>
concept HasFormObjectGetAlias = requires(const T& object) {
    object.get(std::string_view{});
};

template <typename T>
concept HasFormObjectValueAlias = requires(const T& object) {
    object.value(std::string_view{});
};

template <typename T>
concept HasFormObjectHasAlias = requires(const T& object) {
    object.has(std::string_view{});
};

template <typename T>
concept HasFormObjectNamedValuesAllocator = requires(const T& object) {
    object.values(std::string_view{});
};

template <typename T>
concept HasFormObjectCanonicalAccessors = requires(const T& object) {
    { object.groups() } -> std::same_as<std::span<const ruvia::ContextRequest::RequestFormData::Entry>>;
    { object.at(std::string_view{}) } -> std::same_as<ruvia::ContextRequest::RequestFormData::PathValue>;
    { object.count(std::string_view{}) } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasModelBodyAccessor = requires(const T& model) {
    { model.body() } -> std::same_as<const ruvia::RequestObject&>;
};

template <typename T>
concept HasByteSpanResponseBody = requires(const T& context, std::span<const std::byte> body) {
    { context.body(body) } -> std::same_as<ruvia::HttpResponse>;
    { context.newResponse(body) } -> std::same_as<ruvia::HttpResponse>;
};

template <typename T>
concept HasStdStringResponseBody = requires(const T& context, std::string body) {
    context.body(body);
};

template <typename T>
concept HasStdStringNewResponseBody = requires(const T& context, std::string body) {
    context.newResponse(body);
};

template <typename T>
concept HasContextSetHeaderAlias = requires(T& context) {
    context.setHeader(std::string_view{}, std::string_view{});
};

template <typename T>
concept HasResponseSetHeaderAlias = requires(T& response) {
    response.setHeader(std::string_view{}, std::string_view{});
};

template <typename T>
concept HasResponseHeaderSetter = requires(T& response) {
    { response.header(std::string_view{}, std::string_view{}) } -> std::same_as<void>;
};

template <typename T>
concept HasResponseAppendHeaderAlias = requires(T& response) {
    response.appendHeader(std::string_view{}, std::string_view{});
};

template <typename T>
concept HasResponseRemoveHeaderAlias = requires(T& response) {
    response.removeHeader(std::string_view{});
};

template <typename T>
concept HasResponseHasHeaderAlias = requires(T& response) {
    response.hasHeader(std::string_view{});
};

template <typename T>
concept HasResponseHeaderOptionsSetter = requires(T& response) {
    { response.header(
        std::string_view{},
        std::string_view{},
        ruvia::HttpResponse::HeaderOptions{.append = true}) } -> std::same_as<void>;
};

template <typename T>
concept HasResponseHeaderRemoveSetter = requires(T& response) {
    { response.header(std::string_view{}, std::nullopt) } -> std::same_as<void>;
};

template <typename T>
concept HasResponseHeaderInitInitializerListConstructor = requires {
    T(std::initializer_list<ruvia::HttpHeaderView>{});
};

template <typename T>
concept HasContextDirectHeaderInitializerList = requires(const T& context) {
    context.body(std::string_view{}, std::uint16_t{200}, std::initializer_list<ruvia::HttpHeaderView>{});
    context.newResponse(std::string_view{}, std::uint16_t{200}, std::initializer_list<ruvia::HttpHeaderView>{});
    context.text(std::string_view{}, std::uint16_t{200}, std::initializer_list<ruvia::HttpHeaderView>{});
    context.html(std::string_view{}, std::uint16_t{200}, std::initializer_list<ruvia::HttpHeaderView>{});
    context.json(std::uint32_t{1}, std::uint16_t{200}, std::initializer_list<ruvia::HttpHeaderView>{});
};

template <typename T>
concept HasResponseHeadersEraseAlias = requires(T& response) {
    response.headers().erase(std::string_view{});
};

template <typename T>
concept HasResponseHeadersGetAlias = requires(T& response) {
    response.headers().get(std::string_view{});
};

template <typename T>
concept HasResponseHeadersEntriesAlias = requires(T& response) {
    response.headers().entries();
};

template <typename T>
concept HasResponseHeadersHasAlias = requires(T& response) {
    response.headers().has(std::string_view{});
};

template <typename T>
concept HasResponseHeadersSetAlias = requires(T& response) {
    response.headers().set(std::string_view{}, std::string_view{});
};

template <typename T>
concept HasResponseHeadersAppendAlias = requires(T& response) {
    response.headers().append(std::string_view{}, std::string_view{});
};

template <typename T>
concept HasResponseHeadersRemoveAlias = requires(T& response) {
    response.headers().remove(std::string_view{});
};

template <typename T>
concept HasResponseSetStatusAlias = requires(T& response) {
    response.setStatus(std::uint16_t{200}, std::string_view{});
};

template <typename T>
concept HasResponseStatusSetter = requires(T& response) {
    { response.status(std::uint16_t{200}, std::string_view{}) } -> std::same_as<void>;
};

template <typename T>
concept HasResponseStatusCodeAlias = requires(const T& response) {
    response.statusCode();
};

template <typename T>
concept HasResponseStatusGetter = requires(const T& response) {
    { response.status() } -> std::same_as<std::uint16_t>;
};

template <typename T>
concept HasResponseSetBodyOwnedAlias = requires(T& response, std::pmr::string body) {
    response.setBodyOwned(std::move(body));
};
template <typename T>
concept HasFetchResponseStatusCodeField = requires {
    requires std::is_member_object_pointer_v<decltype(&T::statusCode)>;
};

template <typename T>
concept HasFetchResponseStatusGetter = requires(const T& response) {
    { response.status() } -> std::same_as<std::uint16_t>;
};

template <typename T>
concept HasFetchOptionsHeaderViews = requires(T& options, std::span<const ruvia::HttpHeaderView> headers) {
    options.headers = headers;
    { std::span<const ruvia::HttpHeaderView>(options.headers) } -> std::same_as<std::span<const ruvia::HttpHeaderView>>;
};

template <typename T>
concept HasFetchOptionsHeaderArray = requires(T& options, const ruvia::HttpHeaderView (&headers)[1]) {
    options.headers = headers;
};

template <typename T>
concept HasFetchOptionsHeaderVector = requires(T& options, const std::vector<ruvia::HttpHeaderView>& headers) {
    options.headers = headers;
};

template <typename T>
concept HasFetchOptionsInitializerListHeaders = requires(
    T& options,
    std::initializer_list<ruvia::HttpHeaderView> headers) {
    options.headers = headers;
};

template <typename T>
concept HasOutboundClientFacet = requires(T& value) {
    value.client();
};

template <typename T>
concept HasUseHttpClient = requires(T& value, ruvia::HttpOrigin origin) {
    value.useHttpClient(std::move(origin));
};

template <typename T>
concept HasRuntimeHttpClientMutation = requires(
    T& value,
    ruvia::HttpOrigin origin) {
    value.addHttpClient("upstream", std::move(origin));
    value.removeHttpClient("upstream");
};

#ifdef RUVIA_ENABLE_MARIADB
template <typename T>
concept HasDbHandleDefaultParams = requires(const T& handle) {
    handle.query(std::string_view{});
    handle.execute(std::string_view{});
    handle.queryStream(std::string_view{});
};

template <typename T>
concept HasDbHandleInitializerListParams = requires(
    const T& handle,
    std::initializer_list<ruvia::DbValue> params) {
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
concept HasDbTransactionInitializerListParams = requires(
    T& transaction,
    std::initializer_list<ruvia::DbValue> params) {
    transaction.query(std::string_view{}, params);
    transaction.execute(std::string_view{}, params);
};
#endif
template <typename T>
concept HasFetchResponseHeadersField = requires {
    requires std::is_member_object_pointer_v<decltype(&T::headers)>;
};

template <typename T>
concept HasFetchResponseHeadersGetter = requires(const T& response) {
    { response.headers() } -> std::same_as<std::span<const ruvia::FetchResponseHeader>>;
};

template <typename T>
concept HasFetchResponseBodyField = requires {
    requires std::is_member_object_pointer_v<decltype(&T::body)>;
};

template <typename T>
concept HasFetchResponseBodyGetter = requires(const T& response) {
    { response.body() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasFetchResponseHeaderNameField = requires {
    requires std::is_member_object_pointer_v<decltype(&T::name)>;
};

template <typename T>
concept HasFetchResponseHeaderNameGetter = requires(const T& header) {
    { header.name() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasFetchResponseHeaderValueField = requires {
    requires std::is_member_object_pointer_v<decltype(&T::value)>;
};

template <typename T>
concept HasFetchResponseHeaderValueGetter = requires(const T& header) {
    { header.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasCompleteType = requires {
    sizeof(T);
};

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
    part.partBegin;
    part.partEnd;
};

template <typename T>
concept HasMultipartStreamPartCanonicalReadAccessors = requires(const T& part) {
    { part.name() } -> std::same_as<std::string_view>;
    { part.filename() } -> std::same_as<std::string_view>;
    { part.contentType() } -> std::same_as<std::string_view>;
    { part.body() } -> std::same_as<std::string_view>;
    { part.partBegin() } -> std::same_as<bool>;
    { part.partEnd() } -> std::same_as<bool>;
};

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
concept HasWebSocketPublicCallbackConstructor = requires(
    void* target,
    typename T::Read read,
    typename T::Write write,
    typename T::Close close) {
    T(target, read, write, close);
};

template <typename T>
concept HasContextGetIfAlias = requires(T& context) {
    context.template getIf<std::string_view>(std::string_view{});
};

template <typename T>
concept HasContextVarIfAlias = requires(T& context) {
    context.template varIf<std::string_view>(std::string_view{});
};

template <typename T>
concept HasContextVarHasAlias = requires(T& context) {
    context.var().template has<CurrentUser>(kCurrentUser);
    context.var().template has<CurrentUser>(std::string_view{});
};

template <typename T>
concept HasConstContextVarHasAlias = requires(const T& context) {
    context.var().template has<CurrentUser>(kCurrentUser);
    context.var().template has<CurrentUser>(std::string_view{});
};

template <typename T>
concept HasContextJsonErrorAlias = requires(const T& context) {
    context.jsonError(std::uint16_t{500}, std::string_view{}, std::string_view{});
};

template <typename T>
concept HasRequestCookiesAlias = requires(const T& request) {
    request.cookies();
};

template <typename T>
concept HasContextRequestQueryListAccessor = requires(const T& request) {
    request.query();
};

template <typename T>
concept HasContextRequestQueriesListAccessor = requires(const T& request) {
    request.queries();
};

template <typename T>
concept HasContextRequestCookieListAccessor = requires(const T& request) {
    request.cookie();
};

template <typename T>
concept HasContextRequestParamListAccessor = requires(const T& request) {
    request.param();
};

template <typename T>
concept HasRequestNameValueListGetAllAlias = requires(const T& list) {
    list.getAll(std::string_view{});
};

template <typename T>
concept HasRequestNameValueListSpanAlias = requires(const T& list) {
    list.span();
};

template <typename T>
concept HasRequestNameValueListKeysAllocator = requires(const T& list) {
    list.keys();
};

template <typename T>
concept HasRequestNameValueListValuesAllocator = requires(const T& list) {
    list.values();
};

template <typename T>
concept HasRequestNameValueListNamedValuesAllocator = requires(const T& list) {
    list.values(std::string_view{});
};

template <typename T>
concept HasRequestNameValueListNameIndexAlias = requires(const T& list) {
    list[std::string_view{}];
};

template <typename T>
concept HasRequestNameValueListHasAlias = requires(const T& list) {
    list.has(std::string_view{});
};

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
    list.push_back(ruvia::RequestNameValueView{});
    list.emplace_back(ruvia::RequestNameValueView{});
};

template <typename T>
concept HasRequestNameValueListMutableAccess = requires(T& list) {
    { list.begin() } -> std::same_as<typename T::iterator>;
    { list.end() } -> std::same_as<typename T::iterator>;
    { list.data() } -> std::same_as<ruvia::RequestNameValueView*>;
    { list[std::size_t{}] } -> std::same_as<ruvia::RequestNameValueView&>;
};

template <typename T>
concept HasRequestNameValueListMutableIteratorAlias = requires {
    typename T::iterator;
};

template <typename T>
concept HasRequestNameValueListCanonicalAccessors = requires(const T& list) {
    { list.get(std::string_view{}) } -> std::same_as<std::optional<std::string_view>>;
    { list.count(std::string_view{}) } -> std::same_as<std::size_t>;
    { list.entries() } -> std::same_as<std::span<const ruvia::RequestNameValueView>>;
};

template <typename T>
concept HasRequestValueGroupListGetAllAlias = requires(const T& list) {
    list.getAll(std::string_view{});
};

template <typename T>
concept HasRequestValueGroupListGetAlias = requires(const T& list) {
    list.get(std::string_view{});
};

template <typename T>
concept HasRequestValueGroupPublicMutator = requires(T& group) {
    group.add(std::string_view{});
};

template <typename T>
concept HasRequestValueGroupFirstAlias = requires(const T& group) {
    group.first();
};

template <typename T>
concept HasRequestValueGroupCanonicalAccessors = requires(const T& group) {
    { group.name() } -> std::same_as<std::string_view>;
    { group.values() } -> std::same_as<std::span<const std::string_view>>;
    { group.size() } -> std::same_as<std::size_t>;
    { group.empty() } -> std::same_as<bool>;
};

template <typename T>
concept HasRequestValueGroupListSpanAlias = requires(const T& list) {
    list.span();
};

template <typename T>
concept HasRequestValueGroupListKeysAllocator = requires(const T& list) {
    list.keys();
};

template <typename T>
concept HasRequestValueGroupListValuesAllocator = requires(const T& list) {
    list.values();
};

template <typename T>
concept HasRequestValueGroupListNameIndexAlias = requires(const T& list) {
    list[std::string_view{}];
};

template <typename T>
concept HasRequestValueGroupListHasAlias = requires(const T& list) {
    list.has(std::string_view{});
};

template <typename T>
concept HasRequestValueGroupListFirstAlias = requires(const T& list) {
    list.first(std::string_view{});
};

template <typename T>
concept HasRequestValueGroupListGroupInternal = requires(const T& list) {
    list.group(std::string_view{});
};

template <typename T>
concept HasRequestValueGroupListPublicMutators = requires(T& list) {
    list.reserve(std::size_t{1});
    list.push_back(std::declval<ruvia::RequestValueGroup>());
    list.emplace_back(std::pmr::get_default_resource(), std::string_view{});
};

template <typename T>
concept HasRequestValueGroupListMutableAccess = requires(T& list) {
    { list.begin() } -> std::same_as<typename T::iterator>;
    { list.end() } -> std::same_as<typename T::iterator>;
    { list.data() } -> std::same_as<ruvia::RequestValueGroup*>;
    { list[std::size_t{}] } -> std::same_as<ruvia::RequestValueGroup&>;
};

template <typename T>
concept HasRequestValueGroupListMutableIteratorAlias = requires {
    typename T::iterator;
};

template <typename T>
concept HasRequestValueGroupListCanonicalAccessors = requires(const T& list) {
    { list.values(std::string_view{}) } -> std::same_as<std::span<const std::string_view>>;
    { list.entries() } -> std::same_as<std::span<const ruvia::RequestValueGroup>>;
};

template <typename T>
concept HasAppErrorHandlerSetterAlias = requires(T& app) {
    app.setErrorHandler(static_cast<ruvia::HttpErrorHandler>(nullptr));
};

template <typename T>
concept HasAppNotFoundHandlerSetterAlias = requires(T& app) {
    app.setNotFoundHandler(static_cast<ruvia::HttpNotFoundHandler>(nullptr));
};

template <typename T>
concept HasAppSetRateLimitAlias = requires(T& app) {
    app.setRateLimit(std::size_t{1}, std::chrono::milliseconds{1000});
};

template <typename T>
concept HasAppUseMiddlewareTemplate = requires {
    &T::template use<AppUseProbeMiddleware>;
};

template <typename T>
concept HasControllerRouteBuilderPublicRegisterRoute = requires(
    const T& builder,
    ruvia::detail::ControllerRouteHandler handler) {
    builder.registerRoute(
        ruvia::Get,
        std::string_view{"/"},
        handler,
        ruvia::detail::RequestBodyMode::kBuffered,
        std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
};

template <typename T>
concept HasControllerRouteBuilderPublicRegisterStreamRoute = requires(
    const T& builder,
    ruvia::detail::ControllerRouteStreamHandler handler) {
    builder.registerStreamRoute(
        ruvia::Get,
        std::string_view{"/"},
        handler,
        ruvia::detail::ResponseBodyMode::kStream,
        std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
};

template <typename T>
concept HasControllerRouteBuilderPublicCreateScope = requires(const T& builder) {
    builder.createScope(std::string_view{"/"});
};

template <typename T>
concept HasControllerMiddlewareDescriptorPublicCallbackConstructor = requires(
    typename T::Invoke invoke,
    typename T::Create create,
    typename T::Destroy destroy) {
    T(invoke, create, destroy);
};

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
concept HasDbRowPublicMutators = requires(T& row, ruvia::DbField field) {
    row.reserve(std::size_t{1});
    row.push_back(std::move(field));
    row.emplace_back(nullptr, std::pmr::get_default_resource());
    row.emplace_back(std::string_view{}, std::pmr::get_default_resource());
};

template <typename T>
concept HasDbValuePmrStringConstructor = requires(std::pmr::string value) {
    T(std::move(value));
};

template <typename T>
concept HasDbRowCanonicalReadAccessors = requires(const T& row) {
    { row.empty() } -> std::same_as<bool>;
    { row.size() } -> std::same_as<std::size_t>;
    { row[std::size_t{}] } -> std::same_as<const ruvia::DbField&>;
    { row.begin() } -> std::same_as<const ruvia::DbField*>;
    { row.end() } -> std::same_as<const ruvia::DbField*>;
};

template <typename T>
concept HasDbFieldCanonicalReadAccessors = requires(const T& field) {
    { field.isNull() } -> std::same_as<bool>;
    { field.text() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasQueryResultCanonicalReadAccessors = requires(const T& result) {
    { result.rows() } -> std::same_as<std::span<const ruvia::DbRow>>;
    { result.affectedRows() } -> std::same_as<std::uint64_t>;
    { result.lastInsertId() } -> std::same_as<std::uint64_t>;
};

template <typename T>
concept HasDbMigrationReportCanonicalReadAccessors = requires(const T& report) {
    { report.applied() } -> std::same_as<std::span<const std::pmr::string>>;
    { report.skipped() } -> std::same_as<std::span<const std::pmr::string>>;
    { report.changed() } -> std::same_as<bool>;
};

template <typename T>
concept HasJwtClaimPublicFields = requires(T& claim) {
    claim.name;
    claim.value;
};

template <typename T>
concept HasJwtClaimStringViewConstructor = requires {
    T(std::string_view{}, std::string_view{});
};

template <typename T>
concept HasJwtClaimResourceConstructor = requires(std::pmr::memory_resource* resource) {
    T(std::string_view{}, std::string_view{}, resource);
};

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
    { value.integer() } -> std::same_as<std::int64_t>;
    { value.array() } -> std::same_as<std::span<const ruvia::RedisValue>>;
};

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
    { result.cursor() } -> std::same_as<std::uint64_t>;
    { result.values() } -> std::same_as<std::span<const std::pmr::string>>;
};

template <typename T>
concept HasRedisHashScanResultPublicFields = requires(T& result) {
    result.cursor;
    result.entries;
};

template <typename T>
concept HasRedisHashScanResultCanonicalReadAccessors = requires(const T& result) {
    { result.cursor() } -> std::same_as<std::uint64_t>;
    { result.entries() } -> std::same_as<std::span<const ruvia::RedisKeyValue>>;
};

template <typename T>
concept HasRedisZScanResultPublicFields = requires(T& result) {
    result.cursor;
    result.entries;
};

template <typename T>
concept HasRedisZScanResultCanonicalReadAccessors = requires(const T& result) {
    { result.cursor() } -> std::same_as<std::uint64_t>;
    { result.entries() } -> std::same_as<std::span<const ruvia::RedisScoredValue>>;
};

template <typename T>
concept HasAppGlobalRateLimitRuleSetter = requires(T& app) {
    { app.setGlobalRateLimit(ruvia::RateLimitRule{}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppGlobalRateLimitTupleSetter = requires(T& app) {
    { app.setGlobalRateLimit(std::size_t{1}, std::chrono::milliseconds{1000}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppDocumentRootConfigSetter = requires(T& app) {
    { app.setDocumentRoot(ruvia::DocumentRootConfig{}) } -> std::same_as<ruvia::App&>;
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
concept HasAppHttpListenPortSetter = requires(T& app) {
    { app.setHttpListenPort(std::uint16_t{8080}) } -> std::same_as<ruvia::App&>;
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
concept HasAccessLogRecordCanonicalReadAccessors = requires(const T& record) {
    { record.method() } -> std::same_as<ruvia::HttpMethod>;
    { record.path() } -> std::same_as<std::string_view>;
    { record.remoteAddress() } -> std::same_as<std::string_view>;
    { record.status() } -> std::same_as<std::uint16_t>;
    { record.durationMicros() } -> std::same_as<std::uint64_t>;
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
    info.detailsJson;
};

template <typename T>
concept HasHttpErrorInfoCanonicalReadAccessors = requires(const T& info) {
    { info.status() } -> std::same_as<std::uint16_t>;
    { info.statusText() } -> std::same_as<std::string_view>;
    { info.code() } -> std::same_as<std::string_view>;
    { info.message() } -> std::same_as<std::string_view>;
    { info.detailsJson() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttpParseResultPublicFields = requires(T& result) {
    result.status;
    result.error;
    result.request;
    result.consumedBytes;
};

template <typename T>
concept HasHttpParseResultCanonicalReadAccessors = requires(const T& result) {
    { result.status() } -> std::same_as<ruvia::HttpParseStatus>;
    { result.error() } -> std::same_as<ruvia::HttpParseError>;
    { result.request() } -> std::same_as<const ruvia::HttpRequest&>;
    { result.consumedBytes() } -> std::same_as<std::size_t>;
};

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
concept HasContextSetRendererAlias = requires(T& context) {
    context.setRenderer(static_cast<ruvia::Context::Renderer>(nullptr));
};

template <typename T>
concept HasContextRendererSetter = requires(T& context) {
    { context.renderer(static_cast<ruvia::Context::Renderer>(nullptr)) } -> std::same_as<void>;
};

template <typename T>
concept HasContextSetLayoutAlias = requires(T& context) {
    context.setLayout(static_cast<ruvia::Context::Layout>(nullptr));
};

template <typename T>
concept HasContextGetLayoutAlias = requires(const T& context) {
    context.getLayout();
};

template <typename T>
concept HasContextLayoutSetter = requires(T& context) {
    { context.layout(static_cast<ruvia::Context::Layout>(nullptr)) } -> std::same_as<ruvia::Context::Layout>;
};

template <typename T>
concept HasContextLayoutGetter = requires(const T& context) {
    { context.layout() } -> std::same_as<ruvia::Context::Layout>;
};

static_assert(!std::is_copy_constructible_v<ruvia::Next>);
static_assert(!std::is_copy_assignable_v<ruvia::Next>);
static_assert(!std::is_move_constructible_v<ruvia::Next>);
static_assert(!std::is_move_assignable_v<ruvia::Next>);
static_assert(!std::is_copy_constructible_v<ruvia::Next::Awaitable>);
static_assert(!std::is_copy_assignable_v<ruvia::Next::Awaitable>);
static_assert(!std::is_move_constructible_v<ruvia::Next::Awaitable>);
static_assert(!std::is_move_assignable_v<ruvia::Next::Awaitable>);
static_assert(!HasPlainAddressOf<const ruvia::Next>);
static_assert(!HasPlainAddressOf<ruvia::Next::Awaitable>);
static_assert(!HasLvalueAwait<ruvia::Next::Awaitable>);
static_assert(!std::is_copy_constructible_v<ruvia::RequestNameValueList>);
static_assert(!std::is_copy_assignable_v<ruvia::RequestNameValueList>);
static_assert(!std::is_default_constructible_v<ruvia::RequestNameValueList>);
static_assert(std::is_move_constructible_v<ruvia::RequestNameValueList>);
static_assert(std::is_move_assignable_v<ruvia::RequestNameValueList>);
static_assert(!std::is_constructible_v<ruvia::RequestNameValueList, std::pmr::memory_resource*>);
static_assert(!std::is_copy_constructible_v<ruvia::RequestValueGroup>);
static_assert(!std::is_copy_assignable_v<ruvia::RequestValueGroup>);
static_assert(!std::is_default_constructible_v<ruvia::RequestValueGroup>);
static_assert(std::is_move_constructible_v<ruvia::RequestValueGroup>);
static_assert(std::is_move_assignable_v<ruvia::RequestValueGroup>);
static_assert(!std::is_constructible_v<ruvia::RequestValueGroup, std::pmr::memory_resource*, std::string_view>);
static_assert(!HasRequestValueGroupPublicMutator<ruvia::RequestValueGroup>);
static_assert(!std::is_copy_constructible_v<ruvia::RequestValueGroupList>);
static_assert(!std::is_copy_assignable_v<ruvia::RequestValueGroupList>);
static_assert(!std::is_default_constructible_v<ruvia::RequestValueGroupList>);
static_assert(std::is_move_constructible_v<ruvia::RequestValueGroupList>);
static_assert(std::is_move_assignable_v<ruvia::RequestValueGroupList>);
static_assert(!std::is_constructible_v<ruvia::RequestValueGroupList, std::pmr::memory_resource*>);
static_assert(!noexcept(std::declval<const ruvia::ContextRequest&>().query(std::string_view{})));
static_assert(!noexcept(std::declval<const ruvia::ContextRequest&>().header(std::string_view{})));
static_assert(!noexcept(std::declval<const ruvia::ContextRequest&>().param(std::string_view{})));
static_assert(!HasDefaultValid<CurrentUser>);
static_assert(!HasUnaryContextHeader<ruvia::Context>);
static_assert(!HasUnaryContextQuery<ruvia::Context>);
static_assert(!HasUnaryContextCookie<ruvia::Context>);
static_assert(!HasUnaryContextParam<ruvia::Context>);
static_assert(!HasContextStatusTextSetter<ruvia::Context>);
static_assert(!HasResponseHeadersAlias<ruvia::HttpResponse>);
static_assert(HasRequestBytesAlias<ruvia::ContextRequest>);
static_assert(!HasRequestBlobArrayBufferAlias<ruvia::ContextRequest::RequestBlob>);
static_assert(HasRequestJsonValueAlias<ruvia::ContextRequest>);
static_assert(!HasMemberCloneRawRequestAlias<ruvia::ContextRequest>);
static_assert(!HasRawRequestCloneTextAlias<ruvia::ContextRequest::RawRequestClone>);
static_assert(HasRawRequestCloneBodyGetter<ruvia::ContextRequest::RawRequestClone>);
static_assert(HasRawRequestCloneBytesGetter<ruvia::ContextRequest::RawRequestClone>);
static_assert(HasRawRequestCloneCanonicalReadAccessors<ruvia::ContextRequest::RawRequestClone>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RawRequestClone,
    std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RawRequestClone::Header,
    std::pmr::memory_resource*,
    std::string_view,
    std::string_view>);
static_assert(!HasRawRequestCloneArrayBufferAlias<ruvia::ContextRequest::RawRequestClone>);
static_assert(!HasRawRequestCloneMethodEnumAlias<ruvia::ContextRequest::RawRequestClone>);
static_assert(!HasRawRequestCloneHeadersAlias<ruvia::ContextRequest::RawRequestClone>);
static_assert(!HasRawRequestCloneTargetAlias<ruvia::ContextRequest::RawRequestClone>);
static_assert(!HasRawRequestCloneQueryStringAlias<ruvia::ContextRequest::RawRequestClone>);
static_assert(!HasRawRequestCloneHttpVersionAlias<ruvia::ContextRequest::RawRequestClone>);
static_assert(!HasRawRequestCloneRemoteAddressAlias<ruvia::ContextRequest::RawRequestClone>);
static_assert(!HasRawRequestCloneClientCertificateAlias<ruvia::ContextRequest::RawRequestClone>);
static_assert(!HasRequestMethodEnumAlias<ruvia::ContextRequest>);
static_assert(!HasRequestTargetAlias<ruvia::ContextRequest>);
static_assert(!HasRequestHeadersAlias<ruvia::ContextRequest>);
static_assert(!HasContextRequestHeaderListAccessor<ruvia::ContextRequest>);
static_assert(!HasRequestQueryStringAlias<ruvia::ContextRequest>);
static_assert(!HasRequestRoutePathAlias<ruvia::ContextRequest>);
static_assert(!HasRequestMatchedRoutesAlias<ruvia::ContextRequest>);
static_assert(!HasRequestRouteIndexAlias<ruvia::ContextRequest>);
static_assert(!HasRequestHttpVersionAlias<ruvia::ContextRequest>);
static_assert(!HasRequestDecodedPathAlias<ruvia::ContextRequest>);
static_assert(!HasRequestRemoteAddressAlias<ruvia::ContextRequest>);
static_assert(!HasRequestClientCertificateAlias<ruvia::ContextRequest>);
static_assert(!HasRequestIsSecureAlias<ruvia::ContextRequest>);
static_assert(!HasFormValueToStringView<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueToStringView<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(!HasFormValueTextAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueTextAlias<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(HasFormValueGetter<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(HasFormValueGetter<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(!HasFormValueValueOrAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueValueOrAlias<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(!HasFormValueTextsAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueTextsAlias<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(!HasFormValueValuesGetter<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueValuesGetter<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(!HasFormValueArrowOperator<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueArrowOperator<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(!HasFormValueExistsAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueExistsAlias<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(!HasFormValueIsArrayAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueIsArrayAlias<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(!HasFormValueIsFileAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueIsFileAlias<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestFormData::Entry,
    std::pmr::memory_resource*,
    std::string_view,
    bool,
    ruvia::ContextRequest::RequestFormData::SingleValueSelection>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestFormData::Value,
    const ruvia::ContextRequest::RequestFormData::Entry*>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestFormData::PathValue,
    const ruvia::ContextRequest::RequestFormData::Entry*>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestFormData::PathValue,
    std::pmr::vector<const ruvia::ContextRequest::RequestFormField*>&&,
    ruvia::ContextRequest::RequestFormData::SingleValueSelection>);
static_assert(!HasFormFieldBooleanMethodAliases<ruvia::ContextRequest::RequestFormField>);
#ifndef _MSC_VER
static_assert(!HasFormFieldPublicFields<ruvia::ContextRequest::RequestFormField>);
#endif
static_assert(HasFormFieldCanonicalAccessors<ruvia::ContextRequest::RequestFormField>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestFormField,
    std::pmr::memory_resource*,
    std::pmr::string&&,
    std::pmr::string&&,
    std::pmr::string&&,
    std::pmr::string&&,
    bool,
    bool>);
static_assert(!HasFormFieldTextAlias<ruvia::ContextRequest::RequestFormField>);
static_assert(!HasFormFieldFileNameAlias<ruvia::ContextRequest::RequestFormField>);
static_assert(!HasFormFieldMediaTypeAlias<ruvia::ContextRequest::RequestFormField>);
static_assert(!HasFormFieldArrayBufferAlias<ruvia::ContextRequest::RequestFormField>);
static_assert(!HasFormValueFileNameAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueFileNameAlias<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(!HasFormValueMediaTypeAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueMediaTypeAlias<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(std::is_trivially_copyable_v<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(HasFormValueZeroAllocationAccessors<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(HasFormValueZeroAllocationAccessors<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(!std::is_default_constructible_v<ruvia::HttpRequest>);
static_assert(HasHttpRequestQueryGetter<ruvia::HttpRequest>);
static_assert(!HasHttpRequestDecodedPathAlias<ruvia::HttpRequest>);
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
static_assert(!HasFormDataPathAliases<ruvia::ContextRequest::RequestFormData>);
static_assert(HasFormDataCanonicalAccessors<ruvia::ContextRequest::RequestFormData>);
static_assert(noexcept(std::declval<const ruvia::ContextRequest::RequestFormData&>().at(std::string_view{})));
static_assert(!HasFormObjectGetAllAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectKeysAllocator<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectEntriesAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectIndexAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectGetAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectValueAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectHasAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectNamedValuesAllocator<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(HasFormObjectCanonicalAccessors<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(noexcept(std::declval<const ruvia::ContextRequest::RequestFormData::Object&>().at(std::string_view{})));
static_assert(!std::is_default_constructible_v<ruvia::JsonValue>);
static_assert(!std::is_constructible_v<ruvia::JsonValue, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::JsonValue, std::string_view, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::JsonObject>);
static_assert(!std::is_constructible_v<ruvia::JsonObject, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::JsonObject, std::string_view, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::FormObject>);
static_assert(!std::is_constructible_v<ruvia::FormObject, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::FormObject, std::string_view, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::RequestObject>);
static_assert(!std::is_constructible_v<ruvia::RequestObject, ruvia::RequestObjectKind, std::string_view>);
static_assert(!std::is_constructible_v<
    ruvia::RequestObject,
    ruvia::RequestObjectKind,
    std::string_view,
    std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<
    ruvia::RequestObject,
    const ruvia::RequestNameValueList&,
    std::pmr::memory_resource*>);
static_assert(!HasModelBodyAccessor<ClonePayload>);
static_assert(!std::is_constructible_v<ClonePayload, ruvia::RequestObject>);
static_assert(HasByteSpanResponseBody<ruvia::Context>);
static_assert(!HasStdStringResponseBody<ruvia::Context>);
static_assert(!HasStdStringNewResponseBody<ruvia::Context>);
static_assert(!HasContextSetHeaderAlias<ruvia::Context>);
static_assert(!HasResponseSetHeaderAlias<ruvia::HttpResponse>);
static_assert(HasResponseHeaderSetter<ruvia::HttpResponse>);
static_assert(!HasResponseAppendHeaderAlias<ruvia::HttpResponse>);
static_assert(!HasResponseRemoveHeaderAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHasHeaderAlias<ruvia::HttpResponse>);
static_assert(HasResponseHeaderOptionsSetter<ruvia::HttpResponse>);
static_assert(!HasResponseHeaderInitInitializerListConstructor<ruvia::Context::ResponseHeaderInit>);
static_assert(!HasContextDirectHeaderInitializerList<ruvia::Context>);
static_assert(HasResponseHeaderRemoveSetter<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersEraseAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersGetAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersEntriesAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersHasAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersSetAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersAppendAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHeadersRemoveAlias<ruvia::HttpResponse>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::HttpResponse&>().headers()),
    const ruvia::HttpResponseHeaders&>);
static_assert(!HasResponseSetStatusAlias<ruvia::HttpResponse>);
static_assert(HasResponseStatusSetter<ruvia::HttpResponse>);
static_assert(!HasResponseStatusCodeAlias<ruvia::HttpResponse>);
static_assert(HasResponseStatusGetter<ruvia::HttpResponse>);
static_assert(!HasResponseSetBodyOwnedAlias<ruvia::HttpResponse>);
static_assert(!HasFetchResponseStatusCodeField<ruvia::FetchResponse>);
static_assert(HasFetchResponseStatusGetter<ruvia::FetchResponse>);
static_assert(HasFetchOptionsHeaderViews<ruvia::HttpFetchOptions>);
static_assert(HasFetchOptionsHeaderArray<ruvia::HttpFetchOptions>);
static_assert(!HasFetchOptionsHeaderVector<ruvia::HttpFetchOptions>);
static_assert(!HasFetchOptionsInitializerListHeaders<ruvia::HttpFetchOptions>);
static_assert(!HasOutboundClientFacet<ruvia::Context>);
static_assert(!HasUseHttpClient<ruvia::App>);
static_assert(!HasRuntimeHttpClientMutation<ruvia::App>);
static_assert(!HasFetchResponseHeadersField<ruvia::FetchResponse>);
static_assert(HasFetchResponseHeadersGetter<ruvia::FetchResponse>);
static_assert(!HasFetchResponseBodyField<ruvia::FetchResponse>);
static_assert(HasFetchResponseBodyGetter<ruvia::FetchResponse>);
#ifdef RUVIA_ENABLE_MARIADB
static_assert(HasDbHandleDefaultParams<ruvia::DbHandle>);
static_assert(!HasDbHandleInitializerListParams<ruvia::DbHandle>);
static_assert(HasDbTransactionDefaultParams<ruvia::DbTransaction>);
static_assert(!HasDbTransactionInitializerListParams<ruvia::DbTransaction>);
#endif
static_assert(!std::is_default_constructible_v<ruvia::FetchResponse>);
static_assert(!std::is_constructible_v<ruvia::FetchResponse, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::FetchResponseHeader>);
static_assert(!std::is_constructible_v<
    ruvia::FetchResponseHeader,
    std::string_view,
    std::string_view,
    std::pmr::memory_resource*>);
static_assert(!HasFetchResponseHeaderNameField<ruvia::FetchResponseHeader>);
static_assert(HasFetchResponseHeaderNameGetter<ruvia::FetchResponseHeader>);
static_assert(!HasFetchResponseHeaderValueField<ruvia::FetchResponseHeader>);
static_assert(HasFetchResponseHeaderValueGetter<ruvia::FetchResponseHeader>);
static_assert(!HasCompleteType<ruvia::detail::FetchResponseHeaderAccess>);
static_assert(!HasCompleteType<ruvia::detail::FetchResponseAccess>);
static_assert(!HasCompleteType<ruvia::detail::HttpParseResultAccess>);
static_assert(!HasCompleteType<ruvia::detail::MultipartPartAccess>);
static_assert(!HasCompleteType<ruvia::detail::RequestNameValueViewAccess>);
static_assert(!HasCompleteType<ruvia::detail::MultipartStreamPartAccess>);
static_assert(!HasCompleteType<ruvia::detail::WebSocketMessageAccess>);
static_assert(!HasCompleteType<ruvia::detail::AccessLogRecordAccess>);
static_assert(!HasCompleteType<ruvia::detail::DotenvResultAccess>);
static_assert(!HasCompleteType<ruvia::detail::RequestFormFieldAccess>);
static_assert(!HasCompleteType<ruvia::detail::StreamingAccess>);
static_assert(!HasCompleteType<ruvia::detail::SessionAccess>);
static_assert(!HasCompleteType<ruvia::detail::RequestObjectAccess>);
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
#ifndef _MSC_VER
static_assert(!HasWebSocketMessagePublicFields<ruvia::WebSocketMessage>);
#endif
static_assert(HasWebSocketMessageCanonicalReadAccessors<ruvia::WebSocketMessage>);
static_assert(!std::is_default_constructible_v<ruvia::WebSocketMessage>);
static_assert(!std::is_constructible_v<ruvia::WebSocketMessage, ruvia::WebSocketOpcode, std::string_view>);
static_assert(!HasWebSocketPublicCallbackConstructor<ruvia::WebSocket>);
static_assert(!HasContextGetIfAlias<ruvia::Context>);
static_assert(!HasContextVarIfAlias<ruvia::Context>);
static_assert(!HasContextVarHasAlias<ruvia::Context>);
static_assert(!HasConstContextVarHasAlias<ruvia::Context>);
static_assert(!HasContextJsonErrorAlias<ruvia::Context>);
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
static_assert(!HasRequestValueGroupFirstAlias<ruvia::RequestValueGroup>);
static_assert(HasRequestValueGroupCanonicalAccessors<ruvia::RequestValueGroup>);
static_assert(!HasRequestValueGroupListGetAllAlias<ruvia::RequestValueGroupList>);
static_assert(!HasRequestValueGroupListGetAlias<ruvia::RequestValueGroupList>);
static_assert(!HasRequestValueGroupListSpanAlias<ruvia::RequestValueGroupList>);
static_assert(!HasRequestValueGroupListKeysAllocator<ruvia::RequestValueGroupList>);
static_assert(!HasRequestValueGroupListValuesAllocator<ruvia::RequestValueGroupList>);
static_assert(!HasRequestValueGroupListNameIndexAlias<ruvia::RequestValueGroupList>);
static_assert(!HasRequestValueGroupListHasAlias<ruvia::RequestValueGroupList>);
static_assert(!HasRequestValueGroupListFirstAlias<ruvia::RequestValueGroupList>);
static_assert(!HasRequestValueGroupListGroupInternal<ruvia::RequestValueGroupList>);
static_assert(!HasRequestValueGroupListPublicMutators<ruvia::RequestValueGroupList>);
static_assert(!HasRequestValueGroupListMutableAccess<ruvia::RequestValueGroupList>);
static_assert(!HasRequestValueGroupListMutableIteratorAlias<ruvia::RequestValueGroupList>);
static_assert(std::is_pointer_v<ruvia::RequestValueGroupList::const_iterator>);
static_assert(HasRequestValueGroupListCanonicalAccessors<ruvia::RequestValueGroupList>);
static_assert(!HasAppErrorHandlerSetterAlias<ruvia::App>);
static_assert(!HasAppNotFoundHandlerSetterAlias<ruvia::App>);
static_assert(!HasAppSetRateLimitAlias<ruvia::App>);
static_assert(!HasAppUseMiddlewareTemplate<ruvia::App>);
static_assert(!std::is_constructible_v<ruvia::detail::ControllerRouteBuilder, ruvia::Router&, std::string_view>);
#ifndef _MSC_VER
static_assert(!HasControllerRouteBuilderPublicRegisterRoute<ruvia::detail::ControllerRouteBuilder>);
static_assert(!HasControllerRouteBuilderPublicRegisterStreamRoute<ruvia::detail::ControllerRouteBuilder>);
static_assert(!HasControllerRouteBuilderPublicCreateScope<ruvia::detail::ControllerRouteBuilder>);
#endif
static_assert(!HasControllerMiddlewareDescriptorPublicCallbackConstructor<ruvia::detail::ControllerMiddlewareDescriptor>);
static_assert(!HasControllerStorePublicMutators<ruvia::detail::ControllerStore>);
static_assert(!HasControllerStorePublicSize<ruvia::detail::ControllerStore>);
static_assert(!HasDbRowPublicMutators<ruvia::DbRow>);
static_assert(!HasDbValuePmrStringConstructor<ruvia::DbValue>);
static_assert(!std::is_default_constructible_v<ruvia::DbRow>);
static_assert(!std::is_constructible_v<ruvia::DbRow, std::pmr::memory_resource*>);
static_assert(HasDbRowCanonicalReadAccessors<ruvia::DbRow>);
static_assert(!std::is_default_constructible_v<ruvia::DbField>);
static_assert(!std::is_constructible_v<ruvia::DbField, std::nullptr_t, std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<ruvia::DbField, std::string_view, std::pmr::memory_resource*>);
static_assert(HasDbFieldCanonicalReadAccessors<ruvia::DbField>);
static_assert(!std::is_default_constructible_v<ruvia::QueryResult>);
static_assert(!std::is_constructible_v<ruvia::QueryResult, std::pmr::memory_resource*>);
static_assert(HasQueryResultCanonicalReadAccessors<ruvia::QueryResult>);
static_assert(!std::is_default_constructible_v<ruvia::DbMigrationReport>);
static_assert(!std::is_constructible_v<ruvia::DbMigrationReport, std::pmr::memory_resource*>);
static_assert(HasDbMigrationReportCanonicalReadAccessors<ruvia::DbMigrationReport>);
static_assert(!std::is_default_constructible_v<ruvia::JwtClaim>);
static_assert(HasJwtClaimStringViewConstructor<ruvia::JwtClaim>);
static_assert(!HasJwtClaimResourceConstructor<ruvia::JwtClaim>);
#ifndef _MSC_VER
static_assert(!HasJwtClaimPublicFields<ruvia::JwtClaim>);
#endif
static_assert(HasJwtClaimCanonicalReadAccessors<ruvia::JwtClaim>);
static_assert(!std::is_default_constructible_v<ruvia::JwtPayload>);
static_assert(!std::is_constructible_v<ruvia::JwtPayload, std::pmr::memory_resource*>);
static_assert(HasJwtPayloadCanonicalReadAccessors<ruvia::JwtPayload>);
static_assert(!std::is_default_constructible_v<ruvia::RedisValue>);
static_assert(!std::is_constructible_v<ruvia::RedisValue, std::pmr::memory_resource*>);
static_assert(!HasRedisValuePublicFactories<ruvia::RedisValue>);
static_assert(HasRedisValueCanonicalReadAccessors<ruvia::RedisValue>);
static_assert(!std::is_default_constructible_v<ruvia::RedisKeyValue>);
static_assert(!std::is_constructible_v<ruvia::RedisKeyValue, std::string_view, std::string_view, std::pmr::memory_resource*>);
#ifndef _MSC_VER
static_assert(!HasRedisKeyValuePublicFields<ruvia::RedisKeyValue>);
#endif
static_assert(HasRedisKeyValueCanonicalReadAccessors<ruvia::RedisKeyValue>);
static_assert(!std::is_default_constructible_v<ruvia::RedisScoredValue>);
static_assert(!std::is_constructible_v<ruvia::RedisScoredValue, std::string_view, double, std::pmr::memory_resource*>);
#ifndef _MSC_VER
static_assert(!HasRedisScoredValuePublicFields<ruvia::RedisScoredValue>);
#endif
static_assert(HasRedisScoredValueCanonicalReadAccessors<ruvia::RedisScoredValue>);
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
static_assert(HasAppGlobalRateLimitRuleSetter<ruvia::App>);
static_assert(!HasAppGlobalRateLimitTupleSetter<ruvia::App>);
static_assert(HasAppDocumentRootConfigSetter<ruvia::App>);
static_assert(!HasAppDocumentRootPathSetter<ruvia::App>);
static_assert(HasAppListenAddressSetter<ruvia::App>);
static_assert(!HasAppListenAddressPortSetter<ruvia::App>);
static_assert(HasAppHttpListenPortSetter<ruvia::App>);
static_assert(!std::is_default_constructible_v<ruvia::AccessLogRecord>);
static_assert(!std::is_constructible_v<
    ruvia::AccessLogRecord,
    ruvia::HttpMethod,
    std::string_view,
    std::string_view,
    std::uint16_t,
    std::uint64_t,
    bool>);
#ifndef _MSC_VER
static_assert(!HasAccessLogRecordPublicFields<ruvia::AccessLogRecord>);
#endif
static_assert(HasAccessLogRecordCanonicalReadAccessors<ruvia::AccessLogRecord>);
static_assert(!std::is_default_constructible_v<ruvia::ValidationIssue>);
static_assert(!std::is_constructible_v<
    ruvia::ValidationIssue,
    std::string_view,
    std::string_view,
    std::string_view,
    std::pmr::memory_resource*>);
#ifndef _MSC_VER
static_assert(!HasValidationIssuePublicFields<ruvia::ValidationIssue>);
#endif
static_assert(HasValidationIssueCanonicalReadAccessors<ruvia::ValidationIssue>);
static_assert(!HasHttpErrorInfoPublicFields<ruvia::HttpErrorInfo>);
static_assert(HasHttpErrorInfoCanonicalReadAccessors<ruvia::HttpErrorInfo>);
static_assert(!HasCompleteType<ruvia::detail::RouteRateLimitResult>);
static_assert(!std::is_default_constructible_v<ruvia::HttpParseResult>);
#ifndef _MSC_VER
static_assert(!HasHttpParseResultPublicFields<ruvia::HttpParseResult>);
#endif
static_assert(HasHttpParseResultCanonicalReadAccessors<ruvia::HttpParseResult>);
static_assert(!std::is_default_constructible_v<ruvia::DotenvResult>);
#ifndef _MSC_VER
static_assert(!HasDotenvResultPublicFields<ruvia::DotenvResult>);
#endif
static_assert(HasDotenvResultCanonicalReadAccessors<ruvia::DotenvResult>);
static_assert(!HasContextSetRendererAlias<ruvia::Context>);
static_assert(HasContextRendererSetter<ruvia::Context>);
static_assert(!HasContextSetLayoutAlias<ruvia::Context>);
static_assert(!HasContextGetLayoutAlias<ruvia::Context>);
static_assert(HasContextLayoutSetter<ruvia::Context>);
static_assert(HasContextLayoutGetter<ruvia::Context>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().status(204)),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().header(std::string_view{}, std::string_view{})),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().header(
        std::string_view{},
        std::string_view{},
        ruvia::Context::HeaderOptions{.append = true})),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().header(std::string_view{}, std::nullopt)),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().set(kCurrentUser, CurrentUser{})),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().set(std::string_view{}, std::string_view{})),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().get<CurrentUser>(kCurrentUser)),
    CurrentUser*>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::Context&>().get<CurrentUser>(kCurrentUser)),
    const CurrentUser*>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().get<std::string_view>(std::string_view{})),
    std::string_view*>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::Context&>().var().get<CurrentUser>(kCurrentUser)),
    const CurrentUser*>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().res(std::declval<ruvia::HttpResponse&&>())),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().setCookie(std::string_view{}, std::string_view{})),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().setCookie(
        std::string_view{},
        std::string_view{},
        std::declval<const ruvia::CookieOptions&>())),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().deleteCookie(std::string_view{})),
    std::optional<std::string_view>>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().setSignedCookie(
        std::string_view{},
        std::string_view{},
        std::string_view{})),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ContextRequest&>().signedCookie(
        std::string_view{},
        std::string_view{})),
    std::optional<std::string_view>>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::Context&>().generateCookie(
        std::string_view{},
        std::string_view{})),
    std::pmr::string>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::Context&>().generateSignedCookie(
        std::string_view{},
        std::string_view{},
        std::string_view{})),
    std::pmr::string>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::ResponseStreamWriter&>().writeln(std::string_view{})),
    ruvia::Task<void>>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::ResponseStreamWriter&>().sleep(std::chrono::milliseconds{1})),
    ruvia::Task<void>>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ResponseStreamWriter&>().aborted()),
    bool>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::SseWriter&>().sleep(std::chrono::milliseconds{1})),
    ruvia::Task<void>>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::SseWriter&>().aborted()),
    bool>);
static_assert(!std::is_constructible_v<ruvia::SseWriter, ruvia::ResponseStreamWriter&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ContextRequest&>().queries(std::string_view{})),
    std::optional<std::span<const std::string_view>>>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ContextRequest&>().header(std::string_view{})),
    std::optional<std::string_view>>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ContextRequest&>().query(std::string_view{})),
    std::optional<std::string_view>>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ContextRequest&>().param(std::string_view{})),
    std::optional<std::string_view>>);

void appendUnsigned(std::pmr::string& output, std::uint64_t value) {
    char buffer[32]{};
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec == std::errc{}) {
        output.append(buffer, static_cast<std::size_t>(ptr - buffer));
    }
}

std::string_view jsonKindName(ruvia::JsonValue::Kind kind) noexcept {
    switch (kind) {
        case ruvia::JsonValue::Kind::kObject:
            return "object";
        case ruvia::JsonValue::Kind::kArray:
            return "array";
        case ruvia::JsonValue::Kind::kString:
            return "string";
        case ruvia::JsonValue::Kind::kNumber:
            return "number";
        case ruvia::JsonValue::Kind::kBoolean:
            return "boolean";
        case ruvia::JsonValue::Kind::kNull:
            return "null";
    }
    return "unknown";
}

}  // namespace

ruvia::Task<ruvia::HttpResponse> surfaceLayout(
    ruvia::Context& c,
    std::string_view body,
    ruvia::Context::RenderOptions options) {
    std::pmr::string html(c.allocator<char>());
    html.append("<!doctype html><html><head>");
    if (!options.head.empty()) {
        html.append(options.head);
    } else if (!options.title.empty()) {
        html.append("<title>");
        html.append(options.title);
        html.append("</title>");
    }
    html.append("</head><body><section data-layout=\"surface\"><main>");
    html.append(body);
    html.append("</main></section></body></html>");
    co_return c.html(html);
}

ruvia::Task<ruvia::HttpResponse> surfaceRenderer(
    ruvia::Context& c,
    std::string_view body,
    ruvia::Context::RenderOptions options) {
    if (const auto layout = c.layout(); layout != nullptr) {
        co_return co_await layout(c, body, options);
    }

    std::pmr::string html(c.allocator<char>());
    html.append("<!doctype html><html><head>");
    if (!options.head.empty()) {
        html.append(options.head);
    } else if (!options.title.empty()) {
        html.append("<title>");
        html.append(options.title);
        html.append("</title>");
    }
    html.append("</head><body><main>");
    html.append(body);
    html.append("</main></body></html>");
    co_return c.html(html);
}

ruvia::Task<ruvia::HttpResponse> surfaceNotFound(ruvia::Context& c) {
    constexpr ruvia::HttpHeaderView headers[] = {{"X-Surface-Not-Found", "true"}};
    co_return c.text(
        "surface not found\n",
        404,
        headers);
}

class SurfaceContextMiddleware final : public ruvia::Middleware<SurfaceContextMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        c.set(kCurrentUser, CurrentUser{.id = 7, .name = "surface-user"});
        c.set("traceId", std::string_view("surface-trace"));
        c.renderer(&surfaceRenderer);
        co_await next();
        if (c.error()) {
            const bool hadDownstreamErrorResponse = c.finalized();
            const auto downstreamStatus = hadDownstreamErrorResponse
                ? c.res().status()
                : 0;
            auto response = c.text("caught by middleware\n", 500);
            response.header("X-Surface-Error", "true");
            response.header(
                "X-Surface-Error-Response",
                hadDownstreamErrorResponse ? "true" : "false");
            response.header(
                "X-Surface-Error-Status",
                downstreamStatus == 500 ? "500" : "other");
            c.res(std::move(response));
            co_return;
        }
        c.header("X-Surface-Finalized", c.finalized() ? "true" : "false");
        c.res().header("X-Surface-Middleware", "after-next", {.append = true});
    }
};

class SurfaceReturnMiddleware final : public ruvia::Middleware<SurfaceReturnMiddleware> {
public:
    ruvia::Task<ruvia::HttpResponse> handle(ruvia::Context& c, ruvia::Next&) {
        co_return c.text("returned by middleware\n", 209);
    }
};

class SurfacePreDirectResponseMiddleware final : public ruvia::Middleware<SurfacePreDirectResponseMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        c.res().header("X-Surface-Pre-Direct", "true");
        co_await next();
    }
};

class SurfaceLayoutMiddleware final : public ruvia::Middleware<SurfaceLayoutMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        [[maybe_unused]] const auto installedLayout = c.layout(&surfaceLayout);
        co_await next();
    }
};

class SurfaceResSlotOnlyMiddleware final : public ruvia::Middleware<SurfaceResSlotOnlyMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next&) {
        c.res().header("X-Surface-Res-Slot-Only", "true");
        co_return;
    }
};

class ApiSurfaceController final : public ruvia::Controller<ApiSurfaceController> {
public:
    RUVIA_CONTROLLER_GROUP("/surface", SurfaceContextMiddleware)

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/request", requestInfo);
    RUVIA_GET("/context", contextInfo);
    RUVIA_GET("/raw", rawBody);
    RUVIA_GET("/res", responseSlot);
    RUVIA_GET("/res-slot-only", resSlotOnly, SurfaceResSlotOnlyMiddleware);
    RUVIA_GET("/html", htmlBody);
    RUVIA_GET("/json-response", jsonResponse);
    RUVIA_GET("/null-body", nullBody);
    RUVIA_GET("/binary-body", binaryBody);
    RUVIA_GET("/render", renderBody);
    RUVIA_GET("/render-head", renderHeadBody);
    RUVIA_GET("/render-layout", renderLayoutBody, SurfaceLayoutMiddleware);
    RUVIA_GET("/header-remove", headerRemove);
    RUVIA_GET("/redirect-unicode", redirectUnicode);
    RUVIA_GET("/redirect-prepared-location", redirectPreparedLocation);
    RUVIA_GET("/error", appError);
    RUVIA_GET("/throw", throwError);
    RUVIA_GET_STREAM("/stream-throw", streamThrow);
    RUVIA_GET("/missing", missing);
    RUVIA_GET("/middleware-return", middlewareReturnHandler, SurfaceReturnMiddleware);
    RUVIA_GET("/pre-direct-res", preDirectResponse, SurfacePreDirectResponseMiddleware);
    RUVIA_GET("/res-direct-buffered", directBufferedResponse);
    RUVIA_GET("/res-remove-buffered", removeBufferedResponse);
    RUVIA_GET("/res-assigned-prepared", assignedPreparedResponse);
    RUVIA_POST("/multipart", bufferedMultipart);
    RUVIA_POST("/parse-body", parsedBody);
    RUVIA_POST("/form-data", formDataBody);
    RUVIA_POST("/array-buffer", arrayBufferBody);
    RUVIA_POST("/bytes", bytesBody);
    RUVIA_POST("/blob", blobBody);
    RUVIA_POST("/json-object", jsonValueBody);
    RUVIA_POST("/json-value", jsonValueBody);
    RUVIA_POST("/clone-raw", cloneRawRequest);
    RUVIA_POST("/clone-raw-form", cloneRawFormRequest);
    RUVIA_POST("/discard", discard);
    RUVIA_PUT("/items/:id", replaceItem);
    RUVIA_PATCH("/items/:id", patchItem);
    RUVIA_DELETE("/items/:id", deleteItem);
    RUVIA_GET("/cookies", cookies);
    RUVIA_GET("/signed-cookies", signedCookies);
    RUVIA_ALL("/any", anyMethod);
    RUVIA_ON((::ruvia::Put, ::ruvia::Delete), ("/on-item/:id", "/on-legacy/:id"), onItem);
    RUVIA_GET("/manual/copy", manualCopy);
    RUVIA_GET("/manual/view", manualView);
    RUVIA_PUT_STREAM("/upload/:id", streamPut);
    RUVIA_PATCH_STREAM("/upload/:id", streamPatch);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> requestInfo(ruvia::Context& c) {
        const auto& request = c.req();
        std::pmr::string body(c.allocator<char>());
        body.append("method=");
        body.append(request.method());
        body.append("\nurl=");
        body.append(request.url());
        body.append("\npath=");
        body.append(request.path());
        body.append("\nroute-path=");
        body.append(ruvia::routePath(c));
        body.append("\nroute-path-first=");
        body.append(ruvia::routePath(c, 0));
        body.append("\nroute-path-last=");
        body.append(ruvia::routePath(c, -1));
        body.append("\nshortcut-header-host=");
        body.append(c.req().header("Host").value_or(""));
        body.append("\nshortcut-header-x-dupe=");
        body.append(c.req().header("X-Dupe").value_or(""));
        body.append("\nrequest-header-x-dupe=");
        body.append(request.header("X-Dupe").value_or(""));
        body.append("\nrequest-header-missing=");
        body.append(request.header("X-Missing").has_value() ? "present" : "missing");
        body.append("\nrequest-query-tag=");
        body.append(c.req().query("tag").value_or(""));
        body.append("\nrequest-cookie-surface=");
        if (auto surfaceCookie = c.req().cookie("surface")) {
            body.append(*surfaceCookie);
        }
        body.append("\nmatched-routes=");
        appendUnsigned(body, ruvia::matchedRoutes(c).size());
        body.append("\nparam-id=");
        body.append(c.req().param("id").value_or(""));
        body.append("\ntag-values=");
        const auto tags = request.queries("tag");
        appendUnsigned(body, tags.has_value() ? tags->size() : 0);
        body.append("\ntag-first=");
        if (tags.has_value() && !tags->empty()) {
            body.append(tags->front());
        }
        body.append("\ntag-missing=");
        body.append(request.queries("missing").has_value() ? "present" : "missing");
        body.append("\naccepts-json=");
        body.append(c.req().accepts("application/json") ? "true" : "false");
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> contextInfo(ruvia::Context& c) {
        const auto vars = c.var();
        const auto* user = c.get<CurrentUser>(kCurrentUser);
        const auto* traceId = c.get<std::string_view>("traceId");
        const auto* missing = c.get<std::uint32_t>("missing");
        std::pmr::string body(c.allocator<char>());
        body.append("user=");
        body.append(user == nullptr ? "missing" : user->name);
        body.append("\nid=");
        appendUnsigned(body, user == nullptr ? 0 : user->id);
        body.append("\ntrace=");
        body.append(traceId == nullptr ? "missing" : *traceId);
        body.append("\nmissing-var=");
        body.append(missing == nullptr && vars.get<std::uint32_t>("missing") == nullptr ? "true" : "false");
        body.append("\nenv-vars=");
        appendUnsigned(body, c.env().size());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> rawBody(ruvia::Context& c) {
        c.header("X-Raw", "first");
        c.header("X-Raw", "second", {.append = true});
        const ruvia::HttpHeaderView headers[] = {{"X-Raw-Init", "true"}};
        co_return c.body("raw body\n", {.status = 202, .headers = headers});
    }

    ruvia::Task<ruvia::HttpResponse> responseSlot(ruvia::Context& c) {
        c.header("X-Response-Prepared", "true");
        ruvia::HttpResponse response(c.resource());
        response.status(203, {});
        response.header("X-Response-Remove", "drop");
        response.setBodyCopy("response slot\n");
        c.res(std::move(response));
        c.res().header("X-Response-Slot", "true", {.append = true});
        c.res().header("X-Response-Remove", std::nullopt);
        co_return std::move(c.res());
    }

    ruvia::Task<ruvia::HttpResponse> resSlotOnly(ruvia::Context& c) {
        co_return c.text("handler should not run\n", 500);
    }

    ruvia::Task<ruvia::HttpResponse> htmlBody(ruvia::Context& c) {
        co_return c.html("<strong>html body</strong>\n");
    }

    ruvia::Task<ruvia::HttpResponse> jsonResponse(ruvia::Context& c) {
        SurfaceJsonResponse response(c);
        response.message("json response");
        co_return c.json(response);
    }

    ruvia::Task<ruvia::HttpResponse> nullBody(ruvia::Context& c) {
        constexpr ruvia::HttpHeaderView headers[] = {{"X-Null-Body", "true"}};
        co_return c.newResponse(
            nullptr,
            202,
            headers);
    }

    ruvia::Task<ruvia::HttpResponse> binaryBody(ruvia::Context& c) {
        static constexpr std::array<std::byte, 3> bytes{
            std::byte{0x00},
            std::byte{0x41},
            std::byte{0xff}};
        constexpr ruvia::HttpHeaderView headers[] = {{"X-Binary-Body", "true"}};
        co_return c.newResponse(
            std::span<const std::byte>(bytes),
            206,
            headers);
    }

    ruvia::Task<ruvia::HttpResponse> renderBody(ruvia::Context& c) {
        co_return co_await c.render(
            "<h1>rendered body</h1>",
            {.title = "surface"});
    }

    ruvia::Task<ruvia::HttpResponse> renderHeadBody(ruvia::Context& c) {
        co_return co_await c.render(
            "<h1>rendered head body</h1>",
            "<title>surface head</title>");
    }

    ruvia::Task<ruvia::HttpResponse> renderLayoutBody(ruvia::Context& c) {
        co_return co_await c.render(
            "<h1>rendered layout body</h1>",
            {.title = "surface layout"});
    }

    ruvia::Task<ruvia::HttpResponse> headerRemove(ruvia::Context& c) {
        c.header("X-Remove-Me", "drop");
        c.header("X-Remove-Too", "drop");
        c.header("X-Keep-Me", "keep");
        c.header("X-Remove-Me", std::nullopt);
        c.header("X-Remove-Too", std::nullopt);
        co_return c.text("header remove\n");
    }

    ruvia::Task<ruvia::HttpResponse> redirectUnicode(ruvia::Context& c) {
        co_return c.redirect("/目标?x=值", 303);
    }

    ruvia::Task<ruvia::HttpResponse> redirectPreparedLocation(ruvia::Context& c) {
        c.header("Location", "/surface/wrong");
        co_return c.redirect("/surface/right", 302);
    }

    ruvia::Task<ruvia::HttpResponse> appError(ruvia::Context& c) {
        c.header("X-Error-Prepared", "true");
        co_return c.error(418, "teapot", "short and stout", "I'm a Teapot");
    }

    ruvia::Task<ruvia::HttpResponse> throwError(ruvia::Context&) {
        throw std::runtime_error("surface route failed");
    }

    ruvia::Task<void> streamThrow(ruvia::Context&) {
        throw std::runtime_error("surface stream failed");
    }

    ruvia::Task<ruvia::HttpResponse> missing(ruvia::Context& c) {
        c.header("X-Not-Found-Prepared", "true");
        co_return co_await c.notFound();
    }

    ruvia::Task<ruvia::HttpResponse> middlewareReturnHandler(ruvia::Context& c) {
        co_return c.text("handler should not run\n", 500);
    }

    ruvia::Task<ruvia::HttpResponse> preDirectResponse(ruvia::Context& c) {
        co_return c.text("pre direct response\n");
    }

    ruvia::Task<ruvia::HttpResponse> directBufferedResponse(ruvia::Context& c) {
        c.header("X-Direct-Buffered", "true");
        c.res().setBodyCopy("direct buffered response\n");
        co_return std::move(c.res());
    }

    ruvia::Task<ruvia::HttpResponse> removeBufferedResponse(ruvia::Context& c) {
        c.header("X-Remove-Buffered", "drop");
        c.res().header("X-Remove-Buffered", std::nullopt);
        ruvia::HttpResponse response(c.resource());
        response.setBodyCopy("removed buffered response\n");
        c.res(std::move(response));
        co_return std::move(c.res());
    }

    ruvia::Task<ruvia::HttpResponse> assignedPreparedResponse(ruvia::Context& c) {
        c.header("X-Surface-Prepared-Assigned", "true");
        ruvia::HttpResponse response(c.resource());
        response.header("Content-Type", "text/plain; charset=UTF-8");
        response.setBodyCopy("assigned prepared response\n");
        c.res(std::move(response));
        co_return std::move(c.res());
    }

    ruvia::Task<ruvia::HttpResponse> bufferedMultipart(ruvia::Context& c) {
        auto parts = co_await c.req().multipart();
        std::pmr::string body(c.allocator<char>());
        body.append("parts=");
        appendUnsigned(body, parts.size());
        for (const auto& part : parts) {
            body.append("\nname=");
            body.append(part.name());
            body.append(";filename=");
            body.append(part.filename());
            body.append(";content-type=");
            body.append(part.contentType());
            body.append(";bytes=");
            appendUnsigned(body, part.body().size());
        }
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> parsedBody(ruvia::Context& c) {
        auto form = co_await c.req().parseBody({.all = true, .dot = true});
        std::pmr::string body(c.allocator<char>());
        body.append("fields=");
        appendUnsigned(body, form.fields().size());
        body.append("\nentries=");
        appendUnsigned(body, form.fields().size());
        body.append("\ngroups=");
        appendUnsigned(body, form.groups().size());
        body.append("\nkeys=");
        appendUnsigned(body, form.groups().size());
        body.append("\nvalues=");
        appendUnsigned(body, form.fields().size());
        body.append("\nfirst-value-file=");
        body.append(!form.fields().empty() && form.fields().front().file() ? "true" : "false");
        body.append("\nhas-title=");
        body.append(static_cast<bool>(form.get("title")) ? "true" : "false");
        const auto title = form.get("title");
        if (auto titleText = title.value()) {
            body.append("\ntitle=");
            body.append(*titleText);
        }
        body.append("\nhas-obj=");
        body.append(static_cast<bool>(form.get("obj")) ? "true" : "false");
        body.append("\nhas-obj-key1=");
        body.append(static_cast<bool>(form.get("obj.key1")) ? "true" : "false");
        if (auto directNested = form.get("obj.key1").value()) {
            body.append("\nobj.key1-direct=");
            body.append(*directNested);
        }
        body.append("\ntag-count=");
        appendUnsigned(body, form.count("tag"));
        if (auto tag = form.get("tag").value()) {
            body.append("\ntag-single=");
            body.append(*tag);
        }
        body.append("\ntag-array-count=");
        appendUnsigned(body, form.count("tag[]"));
        body.append("\ntag-array-values=");
        appendUnsigned(body, form.get("tag[]").size());
        body.append("\ntag-array=");
        body.append(form.get("tag[]").array() ? "true" : "false");
        body.append("\ntag-is-array=");
        body.append(form.get("tag").array() ? "true" : "false");
        const auto nestedObject = form.object("obj");
        const auto nested = nestedObject.at("key1");
        if (auto nestedText = nested.value()) {
            body.append("\nobj.key1=");
            body.append(*nestedText);
        }
        if (auto nestedValue = nestedObject.at("key1").value()) {
            body.append("\nobj.key1-value=");
            body.append(*nestedValue);
        }
        const auto exactNested = form.at("obj.key1");
        if (auto exactNestedText = exactNested.value()) {
            body.append("\nobj.key1-at=");
            body.append(*exactNestedText);
        }
        body.append("\nobj.key1-at-all=");
        appendUnsigned(body, exactNested.size());
        body.append("\nobj.key1-at-array=");
        body.append(exactNested.array() ? "true" : "false");
        body.append("\nobj.key1-all=");
        appendUnsigned(body, nestedObject.count("key1"));
        body.append("\nobj.key1-values=");
        appendUnsigned(body, nestedObject.count("key1"));
        body.append("\nobj.key1-array=");
        body.append(nestedObject.at("key1").array() ? "true" : "false");
        body.append("\nobj.key-count=");
        appendUnsigned(body, nestedObject.count("key1"));
        body.append("\nobj.entries=");
        appendUnsigned(body, nestedObject.groups().size());
        body.append("\nobj.groups=");
        appendUnsigned(body, nestedObject.groups().size());
        body.append("\nobj.keys=");
        appendUnsigned(body, nestedObject.groups().size());
        const auto childObject = nestedObject.object("child");
        body.append("\nobj.child.keys=");
        appendUnsigned(body, childObject.groups().size());
        for (const auto& field : form.fields()) {
            body.append("\n");
            body.append(field.name());
            body.push_back('=');
            body.append(field.value());
            if (field.file()) {
                const auto blob = field.blob();
                body.append(";filename=");
                body.append(field.filename());
                body.append(";type=");
                body.append(blob.type());
                body.append(";bytes=");
                appendUnsigned(body, blob.size());
            }
            const auto path = field.path();
            if (!path.empty()) {
                body.append(";path=");
                for (std::size_t i = 0; i < path.size(); ++i) {
                    if (i != 0) {
                        body.push_back('/');
                    }
                    body.append(path[i]);
                }
            }
        }
        for (const auto& group : form.groups()) {
            body.append("\ngroup=");
            body.append(group.name());
            body.append(";values=");
            appendUnsigned(body, group.size());
            body.append(";array=");
            body.append(group.array() ? "true" : "false");
        }
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> formDataBody(ruvia::Context& c) {
        auto form = co_await c.req().formData();
        auto tags = form.get("tag");
        std::pmr::string body(c.allocator<char>());
        body.append("fields=");
        appendUnsigned(body, form.fields().size());
        body.append("\nentries=");
        appendUnsigned(body, form.fields().size());
        body.append("\ngroups=");
        appendUnsigned(body, form.groups().size());
        body.append("\nkeys=");
        appendUnsigned(body, form.groups().size());
        body.append("\nvalues=");
        appendUnsigned(body, form.fields().size());
        body.append("\nfirst-value-file=");
        body.append(!form.fields().empty() && form.fields().front().file() ? "true" : "false");
        body.append("\nhas-title=");
        body.append(static_cast<bool>(form.get("title")) ? "true" : "false");
        body.append("\ntag-count=");
        appendUnsigned(body, tags.size());
        if (auto tag = form.get("tag").value()) {
            body.append("\ntag-single=");
            body.append(*tag);
        }
        body.append("\ntag-array-count=");
        appendUnsigned(body, form.count("tag[]"));
        const auto title = form.get("title");
        if (auto titleText = title.value()) {
            body.append("\ntitle=");
            body.append(*titleText);
        }
        if (auto literalDot = form.get("obj.key1").value()) {
            body.append("\nliteral-obj-key1=");
            body.append(*literalDot);
        }
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> arrayBufferBody(ruvia::Context& c) {
        const auto bytes = co_await c.req().arrayBuffer();
        std::pmr::string body(c.allocator<char>());
        body.append("array-buffer bytes=");
        appendUnsigned(body, bytes.size());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> bytesBody(ruvia::Context& c) {
        const auto bytes = co_await c.req().bytes();
        std::pmr::string body(c.allocator<char>());
        body.append("bytes bytes=");
        appendUnsigned(body, bytes.size());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> blobBody(ruvia::Context& c) {
        const auto blob = co_await c.req().blob();
        const auto bytes = blob.bytes();
        const auto text = blob.text();
        std::pmr::string body(c.allocator<char>());
        body.append("blob bytes=");
        appendUnsigned(body, blob.size());
        body.append("\nbytes=");
        appendUnsigned(body, bytes.size());
        body.append("\ntext bytes=");
        appendUnsigned(body, text.size());
        body.append("\ntype=");
        body.append(blob.type());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> jsonValueBody(ruvia::Context& c) {
        const auto json = co_await c.req().json();
        std::pmr::string body(c.allocator<char>());
        body.append("json-value kind=");
        body.append(jsonKindName(json.kind()));
        body.append(" bytes=");
        appendUnsigned(body, json.view().size());
        if (auto message = json.get<ruvia::String>("message")) {
            body.append("\nmessage=");
            body.append(message->view());
        }
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> cloneRawRequest(ruvia::Context& c) {
        const auto consumed = co_await c.req().text();
        auto clone = co_await ruvia::cloneRawRequest(c.req());
        std::pmr::string body(c.allocator<char>());
        body.append("method=");
        body.append(clone.method());
        body.append("\npath=");
        body.append(clone.path());
        body.append("\ncontent-type=");
        body.append(clone.header("Content-Type"));
        body.append("\nbody=");
        body.append(clone.body());
        body.append("\ntext=");
        body.append(clone.body());
        const auto parsed = clone.json<ClonePayload>();
        if (auto message = parsed.message()) {
            body.append("\njson-message=");
            body.append(message->view());
        }
        body.append("\nconsumed=");
        body.append(consumed);
        body.append("\ntype=");
        body.append(clone.blob().type());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> cloneRawFormRequest(ruvia::Context& c) {
        const auto consumed = co_await c.req().text();
        auto clone = co_await ruvia::cloneRawRequest(c.req());
        auto form = clone.formData();
        auto parsed = clone.parseBody({.all = true, .dot = true});
        std::pmr::string body(c.allocator<char>());
        body.append("fields=");
        appendUnsigned(body, form.fields().size());
        body.append("\nentries=");
        appendUnsigned(body, form.fields().size());
        if (auto title = form.get("title").value()) {
            body.append("\ntitle=");
            body.append(*title);
        }
        body.append("\ntag-count=");
        appendUnsigned(body, form.count("tag"));
        body.append("\ntag-array-count=");
        appendUnsigned(body, form.count("tag[]"));
        if (auto nested = parsed.object("obj").at("key1").value()) {
            body.append("\nobj.key1=");
            body.append(*nested);
        }
        body.append("\nconsumed=");
        body.append(consumed);
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> discard(ruvia::Context& c) {
        co_await c.req().discardBody();
        c.status(204);
        co_return c.text("");
    }

    ruvia::Task<ruvia::HttpResponse> replaceItem(ruvia::Context& c) {
        const auto body = co_await c.req().text();
        std::pmr::string output(c.allocator<char>());
        output.append("replace id=");
        output.append(c.req().param("id").value_or(""));
        output.append(" bytes=");
        appendUnsigned(output, body.size());
        output.push_back('\n');
        co_return c.text(output);
    }

    ruvia::Task<ruvia::HttpResponse> patchItem(ruvia::Context& c) {
        const auto body = co_await c.req().text();
        std::pmr::string output(c.allocator<char>());
        output.append("patch id=");
        output.append(c.req().param("id").value_or(""));
        output.append(" bytes=");
        appendUnsigned(output, body.size());
        output.push_back('\n');
        co_return c.text(output);
    }

    ruvia::Task<ruvia::HttpResponse> deleteItem(ruvia::Context& c) {
        std::pmr::string output(c.allocator<char>());
        output.append("deleted id=");
        output.append(c.req().param("id").value_or(""));
        output.push_back('\n');
        co_return c.text(output);
    }

    ruvia::Task<ruvia::HttpResponse> cookies(ruvia::Context& c) {
        ruvia::CookieOptions options;
        options.httpOnly = true;
        options.sameSite = "Lax";
        options.maxAge = 3600;
        c.setCookie("session", "example", options);
        c.setCookie("theme", "light");
        ruvia::CookieOptions hostOptions;
        hostOptions.secure = true;
        hostOptions.httpOnly = true;
        hostOptions.sameSite = "None";
        hostOptions.priority = "high";
        hostOptions.partitioned = true;
        hostOptions.prefix = ruvia::CookiePrefix::kHost;
        hostOptions.expires = std::chrono::system_clock::now() + std::chrono::hours(1);
        c.setCookie("chip", "value", hostOptions);
        const auto deleted = c.deleteCookie("legacy-session");
        std::pmr::string body(c.allocator<char>());
        body.append("cookies set\nlegacy-session=");
        body.append(deleted.value_or(""));
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> anyMethod(ruvia::Context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("all method=");
        body.append(c.req().method());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> onItem(ruvia::Context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("on method=");
        body.append(c.req().method());
        body.append(" id=");
        body.append(c.req().param("id").value_or(""));
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> signedCookies(ruvia::Context& c) {
        static constexpr std::string_view kSecret = "surface-signing-secret";
        c.setSignedCookie("signed-session", "signed-value", kSecret);
        const auto verified = c.req().signedCookie("signed-session", kSecret);
        const auto absent = c.req().signedCookie("absent", kSecret);
        std::pmr::string body(c.allocator<char>());
        body.append("signed=");
        body.append(verified.value_or("missing"));
        body.append("\nabsent=");
        body.append(absent.has_value() ? "present" : "missing");
        body.append("\ngenerated=");
        body.append(c.generateCookie("gen", "value", {.httpOnly = true}));
        body.append("\ngenerated-signed-bytes=");
        appendUnsigned(body, c.generateSignedCookie("gen-signed", "value", kSecret).size());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> manualCopy(ruvia::Context& c) {
        ruvia::HttpResponse response(c.resource());
        response.status(202, "Accepted");
        response.header("Content-Type", "text/plain; charset=UTF-8");
        response.header("X-Manual-Body", "copy");
        response.setBodyCopy(std::string_view("copied body\n"));
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> manualView(ruvia::Context& c) {
        ruvia::HttpResponse response(c.resource());
        response.header("Content-Type", "text/plain; charset=UTF-8");
        response.header("X-Manual-Body", "view");
        response.setBodyView("borrowed static view\n");
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> streamPut(ruvia::Context& c) {
        co_return co_await countStreamingBody(c, "put");
    }

    ruvia::Task<ruvia::HttpResponse> streamPatch(ruvia::Context& c) {
        co_return co_await countStreamingBody(c, "patch");
    }

    static ruvia::Task<ruvia::HttpResponse> countStreamingBody(ruvia::Context& c, std::string_view verb) {
        std::uint64_t bytes = 0;
        auto& reader = c.req().bodyReader();
        while (auto chunk = co_await reader.read()) {
            bytes += chunk->size();
        }

        std::pmr::string body(c.allocator<char>());
        body.append(verb);
        body.append(" stream id=");
        body.append(c.req().param("id").value_or(""));
        body.append(" bytes=");
        appendUnsigned(body, bytes);
        body.push_back('\n');
        co_return c.text(body);
    }
};

class FastSurfaceController final : public ruvia::Controller<FastSurfaceController> {
public:
    RUVIA_CONTROLLER_GROUP("/surface-fast")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/res-slot-merge", responseSlotMerge);
    RUVIA_GET("/res-setter-headers", responseSetterHeaders);
    RUVIA_GET("/new-response", newResponseBody);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> responseSlotMerge(ruvia::Context& c) {
        c.res().header("X-Res-Slot", "kept");
        co_return c.text("response slot merge\n");
    }

    ruvia::Task<ruvia::HttpResponse> responseSetterHeaders(ruvia::Context& c) {
        c.res().header("X-Setter-Override", "slot");
        c.res().header("Content-Type", "application/slot");
        auto response = c.text("response setter headers\n");
        response.header("X-Setter-Override", "response");
        response.header("X-Assigned-Only", "response");
        c.res(std::move(response));
        co_return std::move(c.res());
    }

    ruvia::Task<ruvia::HttpResponse> newResponseBody(ruvia::Context& c) {
        c.header("X-New-Prepared", "true");
        const ruvia::HttpHeaderView headers[] = {{"X-New-Response", "true"}};
        co_return c.newResponse(
            "new response\n",
            ruvia::Context::ResponseInit{
                .status = 201,
                .headers = headers});
    }
};

int main() {
    ruvia::app()
        .setListenAddress("0.0.0.0")
        .setHttpListenPort(8088)
        .setThreadNum(2)
        .notFound(&surfaceNotFound)
        .run();
}
