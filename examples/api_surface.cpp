#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
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
#include "ruvia/web/db/DbTransaction.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/Csrf.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpClientRedirect.h"
#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/web/Error.h"
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

#ifdef RUVIA_MODEL
#error "RUVIA_MODEL must not be public; declare RUVIA_REQUEST_MODEL or RUVIA_RESPONSE_MODEL explicitly"
#endif

namespace {

struct CurrentUser final {
    std::uint32_t id{0};
    std::string_view name;
};

struct AppUseProbeMiddleware;

RUVIA_REQUEST_MODEL(ClonePayload,
    RUVIA_FIELD(message, ruvia::String)
);

RUVIA_RESPONSE_MODEL(SurfaceJsonResponse,
    RUVIA_FIELD(message, ruvia::String)
);

inline constexpr ruvia::ContextKey<CurrentUser> kCurrentUser("currentUser");

using DetailRequestBodyMode = ruvia::detail::RequestBodyMode;
static_assert(std::is_enum_v<DetailRequestBodyMode>);

template <typename T>
concept HasPlainAddressOf = requires(T& value) {
    &value;
};

template <typename T>
concept HasLvalueAwait = requires(T& value) {
    value.operator co_await();
};

template <typename T>
concept HasTypeOnlyValid = requires(const ruvia::ContextRequest& request) {
    request.template valid<T>();
};

template <typename Request, typename T>
concept HasPublicValidatedDataInjection = requires(const Request& request) {
    request.addValidatedData(T{});
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
concept HasContextCookieGenerator = requires(const T& context) {
    context.generateCookie(std::string_view{}, std::string_view{});
};

template <typename T>
concept HasContextSignedCookieGenerator = requires(const T& context) {
    context.generateSignedCookie(
        std::string_view{},
        std::string_view{},
        std::string_view{});
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
concept HasContextRequestBytes = requires(const T& request) {
    { request.bytes() } -> std::same_as<ruvia::Task<std::span<const std::byte>>>;
};

template <typename T>
concept HasRequestArrayBufferAlias = requires(const T& request) {
    request.arrayBuffer();
};

template <typename T>
concept HasContextRequestCanonicalMethodAccessors = requires(const T& request) {
    { request.method() } -> std::same_as<std::string_view>;
    { request.knownMethod() } -> std::same_as<ruvia::HttpKnownMethod>;
};

template <typename T>
concept HasRequestBlobArrayBufferAlias = requires(const T& blob) {
    blob.arrayBuffer();
};

template <typename T>
concept HasRequestBlobTypeAlias = requires(const T& blob) {
    blob.type();
};

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
concept HasRequestJsonValueAlias = requires(const T& request) {
    { request.json() } -> std::same_as<ruvia::Task<ruvia::JsonValue>>;
};

template <typename T>
concept HasRequestCloneMethod = requires(const T& request) {
    request.clone();
};

template <typename T>
concept HasRawRequestCloneType = requires {
    typename T::RawRequestClone;
};

template <typename T>
concept HasRequestUrl = requires(const T& request) {
    request.url();
};

template <typename T>
concept HasRequestFormDataAlias = requires(const T& request) {
    request.formData();
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
concept HasRequestRoutePath = requires(const T& request) {
    request.routePath();
};

template <typename T>
concept HasRequestMatchedRoutes = requires(const T& request) {
    request.matchedRoutes();
};

template <typename T>
concept HasRequestRouteIndexAlias = requires(const T& request) {
    request.routeIndex();
};

template <typename T>
concept HasRequestHttpVersionAlias = requires(const T& request) {
    request.protocolVersion();
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
concept HasConnInfoCanonicalReadAccessors = requires(const T& info) {
    { info.remote().address() } -> std::same_as<std::string_view>;
    { info.plain() } -> std::same_as<const ruvia::PlainConnectionTransport*>;
    { info.tls() } -> std::same_as<const ruvia::TlsConnectionTransport*>;
};

template <typename T>
concept HasLegacyConnInfoScalarAccessors = requires(const T& info) {
    info.secure();
    info.clientCertificateSubject();
};

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
concept ExposesAnyRvalueRequestFormFieldBorrow =
    requires { std::declval<const T&&>().name(); } ||
    requires { std::declval<const T&&>().value(); } ||
    requires { std::declval<const T&&>().filename(); } ||
    requires { std::declval<const T&&>().contentType(); } ||
    requires { std::declval<const T&&>().path(); } ||
    requires { std::declval<const T&&>().blob(); };

template <typename T>
concept ExposesRvalueRequestFormEntryFields = requires {
    std::declval<const T&&>().fields();
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
concept HasFormDataEntryLookup = requires(const T& form) {
    form.entry(std::string_view{});
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
};

template <typename T>
concept ExposesAnyRvalueRequestFormDataBorrow =
    requires { std::declval<const T&&>().fields(); } ||
    requires { std::declval<const T&&>().groups(); } ||
    requires { std::declval<const T&&>().get(std::string_view{}); } ||
    requires { std::declval<const T&&>().object(std::string_view{}); };

template <typename T>
concept HasFormAtLookup = requires(const T& form) {
    form.at(std::string_view{});
};

template <typename T>
concept HasFormPathValueType = requires {
    typename T::PathValue;
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
    { object.get(std::string_view{}) } -> std::same_as<ruvia::ContextRequest::RequestFormData::Value>;
    { object.count(std::string_view{}) } -> std::same_as<std::size_t>;
};

template <typename T>
concept ExposesRvalueRequestFormObjectGroups = requires {
    std::declval<const T&&>().groups();
};

template <typename T>
concept HasModelInputAccessor = requires(const T& model) {
    model.body();
};

template <typename T>
concept HasModelDynamicGet = requires(const T& model) {
    model.get(std::string_view{});
};

template <typename T>
concept HasModelTypedDynamicGet = requires(const T& model) {
    model.template get<ruvia::String>(std::string_view{});
};

template <typename T>
concept HasModelCompileTimeGetAlias = requires(const T& model) {
    model.template get<"message">();
};

template <typename T>
concept HasModelPublicBodyParseHooks =
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
concept HasModelPublicJsonDepthHook = requires {
    T::ruviaParseJsonBodyDepth(
        std::string_view{},
        static_cast<std::pmr::memory_resource*>(nullptr),
        std::size_t{});
};

template <typename T>
concept HasModelPublicFormFieldsHook = requires {
    T::ruviaParseFormFields(
        std::declval<const ruvia::RequestNameValueList&>(),
        static_cast<std::pmr::memory_resource*>(nullptr));
};

template <typename T>
concept HasModelNonConstMessageGetter = requires {
    static_cast<const std::optional<ruvia::String>& (T::*)()>(&T::message);
};

template <typename T>
concept HasModelPublicJsonWriterHooks =
    requires(const T& model, std::pmr::string& output) {
        model.ruviaAppendJson(output);
    } || requires(const T& model) {
        model.ruviaJsonSizeHint();
    };

template <typename T>
concept HasModelPublicFieldStateHook = requires(const T& model) {
    model.template ruviaFieldState<"message">();
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
concept ExposesAnyRvalueGeneratedMessageMember =
    requires { std::declval<const T&&>().message(); } ||
    requires { std::declval<T&&>().messageEnsure(); } ||
    requires { std::declval<T&&>().message(std::string_view{}); };

struct ModelBodyDuckProbe final {
    static int ruviaParseJsonBody(std::string_view, std::pmr::memory_resource*);
    static int ruviaParseFormBody(std::string_view, std::pmr::memory_resource*);
};

template <typename T>
concept HasByteSpanResponseBody = requires(const T& context, std::span<const std::byte> body) {
    { context.body(body) } -> std::same_as<ruvia::HttpResponse>;
};

template <typename T>
concept HasStdStringResponseBody = requires(const T& context, std::string body) {
    context.body(body);
};

template <typename T>
concept HasPmrStringResponseBuilders = requires(
    const T& context,
    std::pmr::string& lvalue,
    const std::pmr::string& constLvalue) {
    { context.body(lvalue) } -> std::same_as<ruvia::HttpResponse>;
    { context.text(constLvalue) } -> std::same_as<ruvia::HttpResponse>;
    { context.html(std::move(lvalue)) } -> std::same_as<ruvia::HttpResponse>;
};

template <typename T>
concept HasContextNewResponseAlias = requires(const T& context) {
    context.newResponse(std::string_view{});
};

template <typename T>
concept HasContextSetHeaderAlias = requires(T& context) {
    context.setHeader(std::string_view{}, std::string_view{});
};

template <typename T>
concept HasContextResponseSlotAlias = requires(T& context, ruvia::HttpResponse response) {
    context.res(std::move(response));
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
concept HasContextBuilderMetadataArguments = requires(const T& context) {
    context.body(std::string_view{}, std::uint16_t{200});
    context.text(std::string_view{}, std::uint16_t{200});
    context.html(std::string_view{}, std::uint16_t{200});
    context.json(std::uint32_t{1}, std::uint16_t{200});
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
    { response.status(std::uint16_t{200}) } -> std::same_as<void>;
};

template <typename T>
concept HasResponseReasonPhraseSetter = requires(T& response) {
    response.status(std::uint16_t{200}, std::string_view{});
};

template <typename T>
concept HasResponseStatusCodeAlias = requires(const T& response) {
    response.statusCode();
};

template <typename T>
concept HasResponseStatusGetter = requires(const T& response) {
    { response.status() } -> std::same_as<std::uint16_t>;
};

using ResponseHeadersGetter = const ruvia::HttpResponseHeaders& (
    ruvia::HttpResponse::*)() const & noexcept;

template <typename T>
concept HasResponseSetBodyOwnedAlias = requires(T& response, std::pmr::string body) {
    response.setBodyOwned(std::move(body));
};

template <typename T>
concept HasResponseSetBodyCopyAlias = requires(T& response) {
    response.setBodyCopy(std::string_view{});
};

template <typename T>
concept HasResponseSetBodyViewAlias = requires(T& response) {
    response.setBodyView(std::string_view{});
};

template <typename T>
concept HasResponseBodySetter = requires(T& response) {
    { response.body(std::string_view{}) } -> std::same_as<void>;
};
template <typename T>
concept HasHttpClientResponseStatusCodeField = requires {
    requires std::is_member_object_pointer_v<decltype(&T::statusCode)>;
};

template <typename T>
concept HasHttpClientResponseStatusGetter = requires(const T& response) {
    { response.status() } -> std::same_as<std::uint16_t>;
};

template <typename T>
concept HasHttpClientRequestHeaderViews = requires(T& options, std::span<const ruvia::HttpHeaderView> headers) {
    options.headers = headers;
    { std::span<const ruvia::HttpHeaderView>(options.headers) } -> std::same_as<std::span<const ruvia::HttpHeaderView>>;
};

template <typename T>
concept HasHttpClientRequestHeaderArray = requires(T& options, const ruvia::HttpHeaderView (&headers)[1]) {
    options.headers = headers;
};

template <typename T>
concept HasHttpClientRequestHeaderVector = requires(T& options, const std::vector<ruvia::HttpHeaderView>& headers) {
    options.headers = headers;
};

template <typename T>
concept HasHttpClientRequestInitializerListHeaders = requires(
    T& options,
    std::initializer_list<ruvia::HttpHeaderView> headers) {
    options.headers = headers;
};

template <typename T>
concept HasHttpClientRequestTarget = requires(T& request) {
    { request.target } -> std::same_as<std::string_view&>;
};

template <typename T>
concept HasRawHttpClientRequestBody = requires(T& request) {
    { request.body } -> std::same_as<std::string_view&>;
};

template <typename T>
concept HasDiscriminatedHttpClientRequestContent = requires(T& request) {
    { request.content.withoutContent() } ->
        std::same_as<const ruvia::HttpClientRequestWithoutContent*>;
    { request.content.borrowedBytes() } ->
        std::same_as<const ruvia::HttpClientRequestBytes*>;
};

template <typename T>
concept HasAnyRvalueHttpClientRequestContentAccessor =
    requires(T&& content) { std::move(content).withoutContent(); } ||
    requires(T&& content) { std::move(content).borrowedBytes(); };

template <typename T>
concept HasStaleHttpClientRequestContentTuple = requires(T& request) {
    request.content.mode();
    request.content.value();
};

template <typename T>
concept HasHttpClientRequestContentBytesFactory = requires(T&& value) {
    { ruvia::HttpClientRequestContent::bytes(std::forward<T>(value)) } ->
        std::same_as<ruvia::HttpClientRequestContent>;
};

template <typename T>
concept HasTypedHttpOriginAccessors = requires(const T& origin) {
    { origin.scheme() } -> std::same_as<ruvia::HttpScheme>;
    { origin.host() } -> std::same_as<std::string_view>;
    { origin.port() } -> std::same_as<std::uint16_t>;
};

template <typename T>
concept HasHttpOriginRvalueHostFactory = requires(T&& host) {
    ruvia::HttpOrigin::http(std::forward<T>(host));
};

template <typename T>
concept HasHttpOriginLvalueHostFactory = requires(T& host) {
    ruvia::HttpOrigin::http(host);
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

#ifdef RUVIA_ENABLE_DATABASE
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
concept HasHttpClientResponseHeadersField = requires {
    requires std::is_member_object_pointer_v<decltype(&T::headers)>;
};

template <typename T>
concept HasHttpClientResponseHeadersGetter = requires(const T& response) {
    { response.headers() } -> std::same_as<std::span<const ruvia::HttpClientResponseHeader>>;
};

template <typename T>
concept HasHttpClientResponseBodyField = requires {
    requires std::is_member_object_pointer_v<decltype(&T::body)>;
};

template <typename T>
concept HasHttpClientResponseBodyGetter = requires(const T& response) {
    { response.body() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttpClientResponseHeaderNameField = requires {
    requires std::is_member_object_pointer_v<decltype(&T::name)>;
};

template <typename T>
concept HasHttpClientResponseHeaderNameGetter = requires(const T& header) {
    { header.name() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttpClientResponseHeaderValueField = requires {
    requires std::is_member_object_pointer_v<decltype(&T::value)>;
};

template <typename T>
concept HasHttpClientResponseHeaderValueGetter = requires(const T& header) {
    { header.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept ExposesAnyRvalueHttpClientOwnedView =
    requires(T&& value) { std::move(value).name(); } ||
    requires(T&& value) { std::move(value).value(); } ||
    requires(T&& value) { std::move(value).headers(); } ||
    requires(T&& value) { std::move(value).body(); };

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
concept HasAnyRvalueMultipartPollAccessor =
    requires(T&& result) { std::move(result).needInput(); } ||
    requires(T&& result) { std::move(result).part(); } ||
    requires(T&& result) { std::move(result).done(); } ||
    requires(T&& result) { std::move(result).failure(); };

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
concept HasWebSocketRuntimeCallbacks = requires {
    typename T::Read;
    typename T::Write;
    typename T::Close;
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
concept HasContextVarFacade = requires(T& context) {
    context.var();
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
concept HasAppInstanceAlias = requires {
    T::instance();
};

template <typename T>
concept HasWebWorkerCorePostEscape = requires(const T& worker) {
    worker.core();
};

template <typename T>
concept HasRateLimitSlotCount = requires(T& rule) {
    rule.slotCount;
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
        ruvia::HttpKnownMethod::kGet,
        std::string_view{"/"},
        handler,
        ruvia::detail::RequestBodyMode::kBuffered,
        std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
};

template <typename T>
concept HasControllerRouteBuilderPublicRegisterResponseStreamRoute = requires(
    const T& builder,
    ruvia::detail::ControllerRouteStreamHandler handler) {
    builder.registerResponseStreamRoute(
        ruvia::HttpKnownMethod::kGet,
        std::string_view{"/"},
        handler,
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
concept HasControllerPublicGroupPrefix = requires {
    T::ruviaControllerGroupPrefix();
};

template <typename T>
concept HasControllerPublicGroupMiddlewares = requires {
    T::ruviaControllerGroupMiddlewares();
};

template <typename T>
concept HasControllerPublicRegisterRoutes = requires(T& controller, ruvia::Router& router) {
    controller.registerRoutes(router);
};

template <typename T>
concept HasControllerPublicRegistrationState = requires {
    T::ruviaControllerRegistered_;
};

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
    { app.setGlobalRateLimit(ruvia::RateLimitRule::fixedWindow(
        std::size_t{1}, std::chrono::seconds(1))) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppGlobalRateLimitTupleSetter = requires(T& app) {
    { app.setGlobalRateLimit(std::size_t{1}, std::chrono::milliseconds{1000}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasAppGlobalRateLimitDisable = requires(T& app) {
    { app.setGlobalRateLimit(std::nullopt) } -> std::same_as<ruvia::App&>;
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
concept HasCanonicalAccessLogCallback = requires(
    T& app,
    ruvia::AccessLogCallback callback) {
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
concept HasAccessLogRecordCanonicalReadAccessors = requires(const T& record) {
    { record.method() } -> std::same_as<std::string_view>;
    { record.knownMethod() } -> std::same_as<ruvia::HttpKnownMethod>;
    { record.path() } -> std::same_as<std::string_view>;
    { record.remoteAddress() } -> std::same_as<std::string_view>;
    { record.status() } -> std::same_as<std::uint16_t>;
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
concept HasAnyRvalueHttp1RequestParseAccessor =
    requires(T&& result) { std::move(result).needMore(); } ||
    requires(T&& result) { std::move(result).parsed(); } ||
    requires(T&& result) { std::move(result).failure(); };

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
concept HasStaleHttp1RequestFramingAccessor = requires(const T& plan) {
    plan.mode();
};

template <typename T>
concept HasHttp1RequestBodyContentLength = requires(const T& framing) {
    { framing.contentLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasHttp1RequestBodyTransferCodings = requires(const T& framing) {
    { framing.transferCodings() } ->
        std::same_as<ruvia::detail::HttpTransferCodings>;
} && requires(const T&& framing) {
    { std::move(framing).transferCodings() } ->
        std::same_as<ruvia::detail::HttpTransferCodings>;
};

template <typename T>
concept HasPublicHttp1RequestBodyPlanFactories = requires {
    T::makeWithoutBody();
    T::makeKnownLength(std::size_t{});
    T::makeChunked(ruvia::detail::HttpTransferCodings{});
};

template <typename T>
concept HasHttp1ClientDiscriminatedParseAccessors = requires(const T& result) {
    { result.needMore() } ->
        std::same_as<const ruvia::Http1ClientResponseNeedMore*>;
    { result.parsed() } ->
        std::same_as<const ruvia::Http1ParsedClientResponseHead*>;
    { result.failure() } ->
        std::same_as<const ruvia::Http1ClientResponseParseFailure*>;
};

template <typename T>
concept HasAnyRvalueHttp1ClientResponseParseAccessor =
    requires(T&& result) { std::move(result).needMore(); } ||
    requires(T&& result) { std::move(result).parsed(); } ||
    requires(const T&& result) { std::move(result).parsed(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasHttp1ClientRequestPrepareAccessors = requires(const T& result) {
    { result.bufferTooSmall() } ->
        std::same_as<const ruvia::Http1ClientRequestBufferTooSmall*>;
    { result.prepared() } ->
        std::same_as<const ruvia::PreparedHttp1ClientRequest*>;
    { result.failure() } ->
        std::same_as<const ruvia::Http1ClientRequestPrepareFailure*>;
};

template <typename T>
concept HasResultKindDiscriminator = requires(const T& result) {
    result.kind();
};

template <typename T>
concept HasAnyRvalueHttp1ClientRequestPrepareAccessor =
    requires(T&& result) { std::move(result).bufferTooSmall(); } ||
    requires(T&& result) { std::move(result).prepared(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasHttp1ClientPreparedContentPlan = requires(const T& prepared) {
    { prepared.contentPlan().withoutContent() } ->
        std::same_as<const ruvia::Http1ClientRequestWithoutContent*>;
    { prepared.contentPlan().immediate() } ->
        std::same_as<const ruvia::Http1ClientImmediateRequestContent*>;
    { prepared.contentPlan().continueGated() } ->
        std::same_as<const ruvia::Http1ClientContinueGatedRequestContent*>;
};

template <typename T>
concept HasAnyRvalueHttp1ClientRequestContentPlanAccessor =
    requires(T&& plan) { std::move(plan).withoutContent(); } ||
    requires(T&& plan) { std::move(plan).immediate(); } ||
    requires(T&& plan) { std::move(plan).continueGated(); };

template <typename T>
concept HasHttp1ClientExpectationAlternatives = requires(const T& policy) {
    { policy.noExpectation() } ->
        std::same_as<const ruvia::Http1ClientNoRequestExpectation*>;
    { policy.continueExpectation() } ->
        std::same_as<const ruvia::Http1ClientContinueExpectation*>;
};

template <typename T>
concept HasAnyRvalueHttp1ClientRequestWirePolicyAccessor =
    requires(T&& policy) { std::move(policy).noExpectation(); } ||
    requires(T&& policy) { std::move(policy).continueExpectation(); };

template <typename T>
concept HasStaleHttp1ClientPreparedContentTuple = requires(const T& prepared) {
    prepared.contentPlan().disposition();
    prepared.contentPlan().bytes();
};

template <typename T>
concept HasStaleHttp1ClientResponseContext = requires(const T& prepared) {
    prepared.responseContext();
};

template <typename T>
concept HasHttp1ClientRequestWriterContract = requires(
    const T& writer,
    const ruvia::HttpOrigin& origin,
    const ruvia::HttpClientRequest& request,
    std::span<char> buffer,
    std::span<const ruvia::HttpHeaderView> headers) {
    { writer.prepare(origin, request, buffer) } ->
        std::same_as<ruvia::Http1ClientRequestPrepareResult>;
    { writer.prepareConnect(origin, headers, buffer) } ->
        std::same_as<ruvia::Http1ClientRequestPrepareResult>;
    { writer.prepare(
        origin,
        request,
        buffer,
        ruvia::Http1ClientRequestWirePolicy::expectContinue()) } ->
        std::same_as<ruvia::Http1ClientRequestPrepareResult>;
};

template <typename T>
concept HasHttp1ClientResponseParserContract = requires(
    T& parser,
    std::string_view buffer) {
    { parser.parse(buffer) } ->
        std::same_as<ruvia::Http1ClientResponseParseResult>;
    { parser.completeRequestContent() } ->
        std::same_as<ruvia::Http1ClientRequestContentCompletionStatus>;
};

template <typename T>
concept HasHttp1ClientRequestContentSignal = requires(const T& plan) {
    { plan.requestContentSignal() } ->
        std::same_as<std::optional<
            ruvia::Http1ClientRequestContentSignal>>;
};

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
concept HasStaleHttp1ClientResponseMode = requires(const T& plan) {
    plan.mode();
};

template <typename T>
concept HasStaleHttp1ClientResponseConnectionAccessor = requires(const T& plan) {
    plan.connectionDisposition();
};

template <typename T>
concept HasHttp1ClientResponseContentLength = requires(const T& framing) {
    { framing.contentLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasHttp1ClientResponseTransferCodings = requires(const T& framing) {
    { framing.transferCodings() } ->
        std::same_as<ruvia::detail::HttpTransferCodings>;
} && requires(const T&& framing) {
    { std::move(framing).transferCodings() } ->
        std::same_as<ruvia::detail::HttpTransferCodings>;
};

template <typename T>
concept HasHttp1ClientResponsePersistence = requires(const T& framing) {
    { framing.persistence() } ->
        std::same_as<ruvia::Http1ClientResponsePersistence>;
};

template <typename T>
concept HasHttpClientHeaderLookupAccessors = requires(const T& result) {
    { result.absent() } ->
        std::same_as<const ruvia::HttpClientResponseHeaderAbsent*>;
    { result.found() } ->
        std::same_as<const ruvia::HttpClientResponseHeaderFound*>;
    { result.repeated() } ->
        std::same_as<const ruvia::HttpClientResponseHeaderRepeated*>;
};

template <typename T>
concept HasAnyRvalueHttpClientHeaderLookupAccessor =
    requires(T&& result) { std::move(result).absent(); } ||
    requires(T&& result) { std::move(result).found(); } ||
    requires(T&& result) { std::move(result).repeated(); };

template <typename T>
concept HasHttpClientHeaderValue = requires(const T& result) {
    { result.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasHttpClientRedirectTargetAccessors = requires(const T& result) {
    { result.target() } ->
        std::same_as<const ruvia::HttpClientRedirectTarget*>;
    { result.failure() } ->
        std::same_as<const ruvia::HttpClientRedirectTargetFailure*>;
};

template <typename T>
concept HasAnyRvalueHttpClientRedirectTargetAccessor =
    requires(T&& result) { std::move(result).target(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept AcceptsTemporaryHttpClientResponseHeaderLookup =
    requires(T&& response) {
        ruvia::lookupUniqueHttpClientResponseHeader(
            std::move(response), std::string_view{});
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
concept HasStaleHttp1ClientHeadOffset = requires(const T& head) {
    head.bodyOffset();
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
concept ExposesAnyRvalueEnvBorrow =
    requires { std::declval<const T&&>().get("NAME"); } ||
    requires { std::declval<const T&&>().template get<std::string_view>("NAME"); };

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
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestFormData::Object,
    const ruvia::ContextRequest::RequestFormData&,
    std::string_view>);
static_assert(!HasLegacyParseBodyFlags<ruvia::ContextRequest::ParseBodyOptions>);
static_assert(
    ruvia::ContextRequest::ParseBodyOptions{}.repeatedScalars ==
    ruvia::ContextRequest::RepeatedScalarPolicy::kLastValue);
static_assert(
    ruvia::ContextRequest::ParseBodyOptions{}.dottedNames ==
    ruvia::ContextRequest::DottedNamePolicy::kLiteral);
static_assert(HasRequestJsonValueAlias<ruvia::ContextRequest>);
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
static_assert(!HasLegacyConnInfoScalarAccessors<ruvia::ConnInfo>);
static_assert(!HasRvalueConnInfoTransportAccess<ruvia::ConnInfo>);
static_assert(HasGetConnInfo<ruvia::Context>);
static_assert(!std::is_default_constructible_v<ruvia::ConnInfo>);
static_assert(!std::is_default_constructible_v<
    ruvia::PlainConnectionTransport>);
static_assert(!std::is_default_constructible_v<
    ruvia::TlsConnectionTransport>);
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
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestFormData::Entry,
    std::pmr::memory_resource*,
    std::string_view,
    bool>);
static_assert(!std::is_constructible_v<
    ruvia::ContextRequest::RequestFormData::Value,
    const ruvia::ContextRequest::RequestFormData::Entry*>);
static_assert(!HasFormFieldBooleanMethodAliases<ruvia::ContextRequest::RequestFormField>);
#ifndef _MSC_VER
static_assert(!HasFormFieldPublicFields<ruvia::ContextRequest::RequestFormField>);
#endif
static_assert(HasFormFieldCanonicalAccessors<ruvia::ContextRequest::RequestFormField>);
static_assert(!ExposesAnyRvalueRequestFormFieldBorrow<
    ruvia::ContextRequest::RequestFormField>);
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
static_assert(!HasFormValueMediaTypeAlias<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(std::is_trivially_copyable_v<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(HasFormValueZeroAllocationAccessors<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!std::is_default_constructible_v<ruvia::HttpRequest>);
static_assert(std::derived_from<ruvia::HttpProtocolError, std::exception>);
static_assert(std::is_nothrow_constructible_v<
    ruvia::HttpProtocolError,
    std::uint16_t,
    std::string_view>);
static_assert(HasHttpRequestQueryGetter<ruvia::HttpRequest>);
static_assert(!HasHttpRequestDecodedPathAlias<ruvia::HttpRequest>);
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
static_assert(!ExposesRvalueRequestFormEntryFields<
    ruvia::ContextRequest::RequestFormData::Entry>);
static_assert(!ExposesAnyRvalueRequestFormDataBorrow<
    ruvia::ContextRequest::RequestFormData>);
static_assert(noexcept(std::declval<const ruvia::ContextRequest::RequestFormData&>().get(std::string_view{})));
static_assert(!HasFormObjectGetAllAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectKeysAllocator<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectEntriesAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectIndexAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectValueAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectHasAlias<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormObjectNamedValuesAllocator<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(HasFormObjectCanonicalAccessors<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!ExposesRvalueRequestFormObjectGroups<
    ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!HasFormAtLookup<ruvia::ContextRequest::RequestFormData::Object>);
static_assert(noexcept(std::declval<const ruvia::ContextRequest::RequestFormData::Object&>().get(std::string_view{})));
static_assert(!std::is_default_constructible_v<ruvia::JsonValue>);
static_assert(!std::is_constructible_v<ruvia::JsonValue, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::JsonValue, std::string_view, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::JsonObject>);
static_assert(!std::is_constructible_v<ruvia::JsonObject, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::JsonObject, std::string_view, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::FormObject>);
static_assert(!std::is_constructible_v<ruvia::FormObject, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::FormObject, std::string_view, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::detail::ModelInput>);
static_assert(!std::is_constructible_v<
    ruvia::detail::ModelInput,
    ruvia::detail::ModelInputKind,
    std::string_view,
    std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<
    ruvia::detail::ModelInput,
    const ruvia::RequestNameValueList&,
    std::pmr::memory_resource*>);
static_assert(!HasModelInputAccessor<ClonePayload>);
static_assert(!HasModelDynamicGet<ClonePayload>);
static_assert(!HasModelTypedDynamicGet<ClonePayload>);
static_assert(!HasModelCompileTimeGetAlias<ClonePayload>);
static_assert(!HasModelPublicBodyParseHooks<ClonePayload>);
static_assert(!HasModelPublicJsonDepthHook<ClonePayload>);
static_assert(!HasModelPublicFormFieldsHook<ClonePayload>);
static_assert(!HasModelNonConstMessageGetter<ClonePayload>);
static_assert(!HasModelPublicJsonWriterHooks<ClonePayload>);
static_assert(!HasModelPublicFieldStateHook<ClonePayload>);
static_assert(!ExposesAnyRvalueModelStringBorrow<ruvia::String>);
static_assert(!ExposesRvalueFixedStringView<ruvia::FixedString<6>>);
static_assert(!ExposesAnyRvalueModelListBorrow<ruvia::List<ruvia::Int32>>);
static_assert(!ExposesAnyRvalueGeneratedMessageMember<ClonePayload>);
static_assert(!ExposesAnyRvalueGeneratedMessageMember<SurfaceJsonResponse>);
static_assert(std::is_base_of_v<ruvia::detail::RequestModelSchemaTag, ClonePayload>);
static_assert(ruvia::JsonBody<ClonePayload>::value);
static_assert(!ruvia::detail::isResponseModel<ClonePayload>);
static_assert(std::is_base_of_v<ruvia::detail::ResponseModelSchemaTag, SurfaceJsonResponse>);
static_assert(!ruvia::JsonBody<SurfaceJsonResponse>::value);
static_assert(!ruvia::FormBody<SurfaceJsonResponse>::value);
static_assert(ruvia::detail::isResponseModel<SurfaceJsonResponse>);
static_assert(!ruvia::JsonBody<ModelBodyDuckProbe>::value);
static_assert(!ruvia::FormBody<ModelBodyDuckProbe>::value);
static_assert(!std::is_constructible_v<ClonePayload, ruvia::detail::ModelInput>);
static_assert(HasByteSpanResponseBody<ruvia::Context>);
static_assert(!HasStdStringResponseBody<ruvia::Context>);
static_assert(HasPmrStringResponseBuilders<ruvia::Context>);
static_assert(!HasContextNewResponseAlias<ruvia::Context>);
static_assert(!HasContextSetHeaderAlias<ruvia::Context>);
static_assert(!HasContextResponseSlotAlias<ruvia::Context>);
static_assert(!HasResponseSetHeaderAlias<ruvia::HttpResponse>);
static_assert(HasResponseHeaderSetter<ruvia::HttpResponse>);
static_assert(!HasResponseAppendHeaderAlias<ruvia::HttpResponse>);
static_assert(!HasResponseRemoveHeaderAlias<ruvia::HttpResponse>);
static_assert(!HasResponseHasHeaderAlias<ruvia::HttpResponse>);
static_assert(HasResponseHeaderOptionsSetter<ruvia::HttpResponse>);
static_assert(!HasContextBuilderMetadataArguments<ruvia::Context>);
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
static_assert(!HasResponseReasonPhraseSetter<ruvia::HttpResponse>);
static_assert(!HasResponseStatusCodeAlias<ruvia::HttpResponse>);
static_assert(HasResponseStatusGetter<ruvia::HttpResponse>);
static_assert(std::same_as<
    decltype(static_cast<ResponseHeadersGetter>(
        &ruvia::HttpResponse::headers)),
    ResponseHeadersGetter>);
static_assert(!std::default_initializable<ruvia::HttpResponseHeaders>);
static_assert(!std::constructible_from<
    ruvia::HttpResponseHeaders,
    std::pmr::memory_resource*>);
static_assert(!HasResponseSetBodyOwnedAlias<ruvia::HttpResponse>);
static_assert(!HasResponseSetBodyCopyAlias<ruvia::HttpResponse>);
static_assert(!HasResponseSetBodyViewAlias<ruvia::HttpResponse>);
static_assert(HasResponseBodySetter<ruvia::HttpResponse>);
static_assert(!HasHttpClientResponseStatusCodeField<ruvia::HttpClientResponse>);
static_assert(HasHttpClientResponseStatusGetter<ruvia::HttpClientResponse>);
static_assert(std::is_move_constructible_v<ruvia::HttpClientResponse>);
static_assert(!std::is_move_assignable_v<ruvia::HttpClientResponse>);
static_assert(HasHttpClientRequestHeaderViews<ruvia::HttpClientRequest>);
static_assert(HasHttpClientRequestHeaderArray<ruvia::HttpClientRequest>);
static_assert(!HasHttpClientRequestHeaderVector<ruvia::HttpClientRequest>);
static_assert(!HasHttpClientRequestInitializerListHeaders<ruvia::HttpClientRequest>);
static_assert(HasHttpClientRequestTarget<ruvia::HttpClientRequest>);
static_assert(!HasRawHttpClientRequestBody<ruvia::HttpClientRequest>);
static_assert(HasDiscriminatedHttpClientRequestContent<ruvia::HttpClientRequest>);
static_assert(!HasStaleHttpClientRequestContentTuple<ruvia::HttpClientRequest>);
static_assert(!HasAnyRvalueHttpClientRequestContentAccessor<
    ruvia::HttpClientRequestContent>);
static_assert(!std::is_default_constructible_v<ruvia::HttpClientRequestContent>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestBytes>);
static_assert(HasHttpClientRequestContentBytesFactory<std::string&>);
static_assert(!HasHttpClientRequestContentBytesFactory<std::string>);
static_assert(!HasHttpClientRequestContentBytesFactory<std::pmr::string>);
static_assert(!std::is_default_constructible_v<ruvia::HttpOrigin>);
static_assert(HasTypedHttpOriginAccessors<ruvia::HttpOrigin>);
static_assert(std::is_same_v<
    decltype(ruvia::HttpOrigin::https("example.test")),
    ruvia::HttpOrigin>);
static_assert(!noexcept(ruvia::HttpOrigin::http("example.test")));
static_assert(!std::is_constructible_v<
    ruvia::HttpOrigin,
    ruvia::HttpScheme,
    std::string_view,
    std::uint16_t>);
static_assert(HasHttpOriginLvalueHostFactory<std::string>);
static_assert(!HasHttpOriginRvalueHostFactory<std::string>);
static_assert(!HasHttpOriginRvalueHostFactory<std::pmr::string>);
static_assert(!HasOutboundClientFacet<ruvia::Context>);
static_assert(!HasUseHttpClient<ruvia::App>);
static_assert(!HasRuntimeHttpClientMutation<ruvia::App>);
static_assert(!HasHttpClientResponseHeadersField<ruvia::HttpClientResponse>);
static_assert(HasHttpClientResponseHeadersGetter<ruvia::HttpClientResponse>);
static_assert(!HasHttpClientResponseBodyField<ruvia::HttpClientResponse>);
static_assert(HasHttpClientResponseBodyGetter<ruvia::HttpClientResponse>);
#ifdef RUVIA_ENABLE_DATABASE
static_assert(HasDbHandleDefaultParams<ruvia::DbHandle>);
static_assert(!HasDbHandleInitializerListParams<ruvia::DbHandle>);
static_assert(HasDbTransactionDefaultParams<ruvia::DbTransaction>);
static_assert(!HasDbTransactionInitializerListParams<ruvia::DbTransaction>);
#endif
static_assert(!std::is_default_constructible_v<ruvia::HttpClientResponse>);
static_assert(!std::is_constructible_v<ruvia::HttpClientResponse, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::HttpClientResponseHeader>);
static_assert(!std::is_constructible_v<
    ruvia::HttpClientResponseHeader,
    std::string_view,
    std::string_view,
    std::pmr::memory_resource*>);
static_assert(!HasHttpClientResponseHeaderNameField<ruvia::HttpClientResponseHeader>);
static_assert(HasHttpClientResponseHeaderNameGetter<ruvia::HttpClientResponseHeader>);
static_assert(!HasHttpClientResponseHeaderValueField<ruvia::HttpClientResponseHeader>);
static_assert(HasHttpClientResponseHeaderValueGetter<ruvia::HttpClientResponseHeader>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::HttpClientResponseHeader>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::HttpClientResponse>);
static_assert(!HasCompleteType<ruvia::detail::HttpClientResponseHeaderAccess>);
static_assert(!HasCompleteType<ruvia::detail::HttpClientResponseAccess>);
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
static_assert(!std::is_constructible_v<
    ruvia::MultipartParser,
    std::string_view,
    std::pmr::memory_resource*>);
static_assert(std::is_constructible_v<
    ruvia::MultipartParser,
    ruvia::MultipartBoundary,
    std::pmr::memory_resource*>);
static_assert(!std::is_copy_constructible_v<ruvia::MultipartParser>);
static_assert(!std::is_copy_assignable_v<ruvia::MultipartParser>);
static_assert(!std::is_move_constructible_v<ruvia::MultipartParser>);
static_assert(!std::is_move_assignable_v<ruvia::MultipartParser>);
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
static_assert(!HasContextGetIfAlias<ruvia::Context>);
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
static_assert(!ExposesAnyRvalueRequestNameValueListBorrow<
    ruvia::RequestNameValueList>);
static_assert(!HasAppErrorHandlerSetterAlias<ruvia::App>);
static_assert(!HasAppNotFoundHandlerSetterAlias<ruvia::App>);
static_assert(!HasAppSetRateLimitAlias<ruvia::App>);
static_assert(!HasRateLimitSlotCount<ruvia::RateLimitRule>);
static_assert(!std::default_initializable<ruvia::RateLimitRule>);
static_assert(std::same_as<
    decltype(ruvia::RateLimitRule::fixedWindow(
        std::size_t{1}, std::chrono::seconds(1))),
    ruvia::RateLimitRule>);
static_assert(!HasAppUseMiddlewareTemplate<ruvia::App>);
static_assert(!std::is_constructible_v<ruvia::detail::ControllerRouteBuilder, ruvia::Router&, std::string_view>);
#ifndef _MSC_VER
static_assert(!HasControllerRouteBuilderPublicRegisterRoute<ruvia::detail::ControllerRouteBuilder>);
static_assert(!HasControllerRouteBuilderPublicRegisterResponseStreamRoute<
    ruvia::detail::ControllerRouteBuilder>);
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
static_assert(std::is_move_assignable_v<ruvia::DbRow>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::DbRow>);
static_assert(!std::is_default_constructible_v<ruvia::DbField>);
static_assert(!std::is_constructible_v<ruvia::DbField, std::nullptr_t, std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<ruvia::DbField, std::string_view, std::pmr::memory_resource*>);
static_assert(HasDbFieldCanonicalReadAccessors<ruvia::DbField>);
static_assert(std::is_move_assignable_v<ruvia::DbField>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::DbField>);
static_assert(!std::is_default_constructible_v<ruvia::QueryResult>);
static_assert(!std::is_constructible_v<ruvia::QueryResult, std::pmr::memory_resource*>);
static_assert(HasQueryResultCanonicalReadAccessors<ruvia::QueryResult>);
static_assert(std::is_move_constructible_v<ruvia::QueryResult>);
static_assert(!std::is_move_assignable_v<ruvia::QueryResult>);
static_assert(std::is_move_constructible_v<ruvia::DbStreamResult>);
static_assert(!std::is_move_assignable_v<ruvia::DbStreamResult>);
static_assert(std::is_move_constructible_v<ruvia::DbTransaction>);
static_assert(!std::is_move_assignable_v<ruvia::DbTransaction>);
static_assert(!std::is_default_constructible_v<ruvia::DbMigrationReport>);
static_assert(!std::is_constructible_v<ruvia::DbMigrationReport, std::pmr::memory_resource*>);
static_assert(HasDbMigrationReportCanonicalReadAccessors<ruvia::DbMigrationReport>);
static_assert(std::is_move_constructible_v<ruvia::DbMigrationReport>);
static_assert(!std::is_move_assignable_v<ruvia::DbMigrationReport>);
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
static_assert(std::is_move_assignable_v<ruvia::RedisValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisValue>);
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
static_assert(HasAppGlobalRateLimitRuleSetter<ruvia::App>);
static_assert(HasAppGlobalRateLimitDisable<ruvia::App>);
static_assert(!HasAppGlobalRateLimitTupleSetter<ruvia::App>);
static_assert(HasAppDocumentRootConfigSetter<ruvia::App>);
static_assert(!HasAppDocumentRootPathSetter<ruvia::App>);
static_assert(HasAppListenAddressSetter<ruvia::App>);
static_assert(!HasAppListenAddressPortSetter<ruvia::App>);
static_assert(HasAppHttpListenPortSetter<ruvia::App>);
static_assert(HasCanonicalAccessLogCallback<ruvia::App>);
static_assert(HasBoundAccessLogCallback<ruvia::AccessLogCallback>);
static_assert(!std::is_default_constructible_v<ruvia::AccessLogRecord>);
static_assert(!std::is_constructible_v<
    ruvia::AccessLogRecord,
    std::string_view,
    ruvia::HttpKnownMethod,
    std::string_view,
    std::string_view,
    std::uint16_t,
    std::uint64_t,
    bool>);
#ifndef _MSC_VER
static_assert(!HasAccessLogRecordPublicFields<ruvia::AccessLogRecord>);
#endif
static_assert(HasAccessLogRecordCanonicalReadAccessors<ruvia::AccessLogRecord>);
static_assert(!HasLegacyAccessLogHttp2Flag<ruvia::AccessLogRecord>);
static_assert(std::is_nothrow_copy_constructible_v<ruvia::AccessLogRecord>);
static_assert(!std::is_copy_assignable_v<ruvia::AccessLogRecord>);
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
static_assert(!ExposesAnyRvalueValidationIssueBorrow<ruvia::ValidationIssue>);
static_assert(!ExposesAnyRvalueValidationErrorBorrow<ruvia::ValidationError>);
static_assert(!ExposesRvalueValidatorIssues<ruvia::Validator>);
static_assert(!AcceptsAnyRvalueValidatorMutation<ruvia::Validator>);
static_assert(!ExposesRvalueHttpErrorInfo<ruvia::HttpError>);
static_assert(!HasHttpErrorInfoPublicFields<ruvia::HttpErrorInfo>);
static_assert(HasHttpErrorInfoCanonicalReadAccessors<ruvia::HttpErrorInfo>);
static_assert(!HasCompleteType<ruvia::detail::RouteRateLimitResult>);
static_assert(!std::is_default_constructible_v<ruvia::Http1RequestParseResult>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ParsedRequest>);
static_assert(!HasStaleHttpParseTupleAccessors<ruvia::Http1RequestParseResult>);
static_assert(HasHttp1DiscriminatedParseAccessors<ruvia::Http1RequestParseResult>);
static_assert(!HasResultKindDiscriminator<ruvia::Http1RequestParseResult>);
static_assert(!HasAnyRvalueHttp1RequestParseAccessor<
    ruvia::Http1RequestParseResult>);
static_assert(HasHttp1RequestBodyPlanAlternatives<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(!HasStaleHttp1RequestFramingAccessor<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(!HasHttp1RequestBodyContentLength<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(!HasHttp1RequestBodyTransferCodings<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(HasHttp1RequestBodyContentLength<
    ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!HasHttp1RequestBodyContentLength<
    ruvia::detail::Http1ChunkedRequestBody>);
static_assert(HasHttp1RequestBodyTransferCodings<
    ruvia::detail::Http1ChunkedRequestBody>);
static_assert(!HasHttp1RequestBodyTransferCodings<
    ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!HasPublicHttp1RequestBodyPlanFactories<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(!std::constructible_from<
    ruvia::detail::Http1RequestBodyPlan,
    ruvia::detail::HttpRequestExpectations>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1RequestWithoutBody>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1ChunkedRequestBody>);
static_assert(!std::is_default_constructible_v<
    ruvia::Http1ClientRequestPrepareResult>);
static_assert(HasHttp1ClientRequestPrepareAccessors<
    ruvia::Http1ClientRequestPrepareResult>);
static_assert(!HasResultKindDiscriminator<
    ruvia::Http1ClientRequestPrepareResult>);
static_assert(!HasAnyRvalueHttp1ClientRequestPrepareAccessor<
    ruvia::Http1ClientRequestPrepareResult>);
static_assert(HasHttp1ClientPreparedContentPlan<
    ruvia::PreparedHttp1ClientRequest>);
static_assert(!HasAnyRvalueHttp1ClientRequestContentPlanAccessor<
    ruvia::Http1ClientRequestContentPlan>);
static_assert(HasHttp1ClientExpectationAlternatives<
    ruvia::Http1ClientRequestWirePolicy>);
static_assert(!HasAnyRvalueHttp1ClientRequestWirePolicyAccessor<
    ruvia::Http1ClientRequestWirePolicy>);
static_assert(!HasStaleHttp1ClientPreparedContentTuple<
    ruvia::PreparedHttp1ClientRequest>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientRequestWithoutContent>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientImmediateRequestContent>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientContinueGatedRequestContent>);
static_assert(!HasStaleHttp1ClientResponseContext<
    ruvia::PreparedHttp1ClientRequest>);
static_assert(!std::is_default_constructible_v<
    ruvia::Http1ClientRequestWirePolicy>);
static_assert(HasHttp1ClientRequestWriterContract<
    ruvia::Http1ClientRequestWriter>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientResponseParseResult>);
static_assert(!std::is_copy_constructible_v<ruvia::Http1ClientResponseParseResult>);
static_assert(std::is_move_constructible_v<ruvia::Http1ClientResponseParseResult>);
static_assert(!std::is_move_assignable_v<ruvia::Http1ClientResponseParseResult>);
static_assert(std::is_move_constructible_v<ruvia::Http1ParsedClientResponseHead>);
static_assert(!std::is_move_assignable_v<ruvia::Http1ParsedClientResponseHead>);
static_assert(HasHttp1ClientDiscriminatedParseAccessors<
    ruvia::Http1ClientResponseParseResult>);
static_assert(!HasResultKindDiscriminator<
    ruvia::Http1ClientResponseParseResult>);
static_assert(!HasAnyRvalueHttp1ClientResponseParseAccessor<
    ruvia::Http1ClientResponseParseResult>);
static_assert(HasHttp1ClientResponseParserContract<
    ruvia::Http1ClientResponseParser>);
static_assert(!std::is_default_constructible_v<
    ruvia::Http1ClientResponseParser>);
static_assert(!std::is_copy_constructible_v<
    ruvia::Http1ClientResponseParser>);
static_assert(!std::is_move_constructible_v<
    ruvia::Http1ClientResponseParser>);
static_assert(std::is_constructible_v<
    ruvia::Http1ClientResponseParser,
    const ruvia::PreparedHttp1ClientRequest&>);
static_assert(HasHttp1ClientRequestContentSignal<
    ruvia::Http1ClientResponsePlan>);
static_assert(HasHttp1ClientResponsePlanAlternatives<
    ruvia::Http1ClientResponsePlan>);
static_assert(!HasAnyRvalueHttp1ClientResponsePlanAccessor<
    ruvia::Http1ClientResponsePlan>);
static_assert(!HasStaleHttp1ClientResponseMode<
    ruvia::Http1ClientResponsePlan>);
static_assert(!HasStaleHttp1ClientResponseConnectionAccessor<
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
static_assert(!std::default_initializable<
    ruvia::Http1ClientResponsePlan>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientInformationalResponse>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientKnownLengthResponse>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientConnectTunnel>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientProtocolUpgrade>);
static_assert(!HasStaleHttp1ClientHeadOffset<
    ruvia::Http1ParsedClientResponseHead>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::Http1ClientChunkedResponse>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!AcceptsTemporaryHttpClientResponseHeaderLookup<
    ruvia::HttpClientResponse>);
static_assert(std::same_as<
    decltype(ruvia::lookupUniqueHttpClientResponseHeader(
        std::declval<const ruvia::HttpClientResponse&>(),
        std::string_view{})),
    ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!std::is_default_constructible_v<
    ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(HasHttpClientHeaderLookupAccessors<
    ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!HasAnyRvalueHttpClientHeaderLookupAccessor<
    ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!HasHttpClientHeaderValue<
    ruvia::HttpClientResponseHeaderAbsent>);
static_assert(HasHttpClientHeaderValue<
    ruvia::HttpClientResponseHeaderFound>);
static_assert(!HasHttpClientHeaderValue<
    ruvia::HttpClientResponseHeaderRepeated>);
static_assert(!HasHttpClientRedirectStatus<
    ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(std::same_as<
    decltype(ruvia::resolveHttpClientSameOriginRedirectTarget(
        std::declval<const ruvia::HttpOrigin&>(),
        std::string_view{},
        std::string_view{},
        std::declval<std::pmr::memory_resource*>())),
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!std::is_default_constructible_v<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!std::is_copy_constructible_v<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(std::is_move_constructible_v<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!std::is_move_assignable_v<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(std::is_move_constructible_v<ruvia::HttpClientRedirectTarget>);
static_assert(!std::is_move_assignable_v<ruvia::HttpClientRedirectTarget>);
static_assert(HasHttpClientRedirectTargetAccessors<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!HasAnyRvalueHttpClientRedirectTargetAccessor<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::HttpClientRedirectTarget>);
static_assert(!HasHttpClientRedirectError<
    ruvia::HttpClientRedirectTarget>);
static_assert(HasHttpClientRedirectError<
    ruvia::HttpClientRedirectTargetFailure>);
static_assert(!HasHttpClientRedirectStatus<
    ruvia::HttpClientRedirectTargetResult>);
static_assert(!std::is_default_constructible_v<ruvia::DotenvResult>);
#ifndef _MSC_VER
static_assert(!HasDotenvResultPublicFields<ruvia::DotenvResult>);
#endif
static_assert(HasDotenvResultCanonicalReadAccessors<ruvia::DotenvResult>);
static_assert(!ExposesAnyRvalueEnvBorrow<ruvia::Env>);
static_assert(!HasContextRenderPipeline<ruvia::Context>);
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
    decltype(std::declval<ruvia::Context&>().respond(std::declval<ruvia::HttpResponse&&>())),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::Context&>().response()),
    const ruvia::HttpResponse*>);
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
    void>);
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
static_assert(!HasContextCookieGenerator<ruvia::Context>);
static_assert(!HasContextSignedCookieGenerator<ruvia::Context>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::ResponseStreamWriter&>().writeln(std::string_view{})),
    ruvia::Task<void>>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::ResponseStreamWriter&>().sleep(std::chrono::milliseconds{1})),
    ruvia::Task<void>>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::ResponseStreamWriter&>().end(
        std::declval<std::span<const ruvia::HttpHeaderView>>())),
    ruvia::Task<void>>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ResponseStreamWriter&>().aborted()),
    bool>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().streamSse()),
    ruvia::SseWriter>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::SseWriter&>().sleep(std::chrono::milliseconds{1})),
    ruvia::Task<void>>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::SseWriter&>().aborted()),
    bool>);
static_assert(!std::is_constructible_v<ruvia::SseWriter, ruvia::ResponseStreamWriter&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ContextRequest&>().queries(std::string_view{})),
    std::span<const std::string_view>>);
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

ruvia::Task<ruvia::HttpResponse> surfaceNotFound(ruvia::Context& c) {
    c.status(404);
    c.header("X-Surface-Not-Found", "true");
    co_return c.text("surface not found\n");
}

class SurfaceContextMiddleware final : public ruvia::Middleware<SurfaceContextMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        c.set(kCurrentUser, CurrentUser{.id = 7, .name = "surface-user"});
        c.set("traceId", std::string_view("surface-trace"));
        co_await next();
        if (c.error()) {
            const auto* downstreamResponse = c.response();
            const bool hadDownstreamErrorResponse = downstreamResponse != nullptr;
            const auto downstreamStatus = downstreamResponse == nullptr
                ? 0
                : downstreamResponse->status();
            c.status(500);
            auto response = c.text("caught by middleware\n");
            response.header("X-Surface-Error", "true");
            response.header(
                "X-Surface-Error-Response",
                hadDownstreamErrorResponse ? "true" : "false");
            response.header(
                "X-Surface-Error-Status",
                downstreamStatus == 500 ? "500" : "other");
            c.respond(std::move(response));
            co_return;
        }
        c.header("X-Surface-Finalized", c.response() != nullptr ? "true" : "false");
        c.header("X-Surface-Middleware", "after-next", {.append = true});
    }
};

class SurfaceReturnMiddleware final : public ruvia::Middleware<SurfaceReturnMiddleware> {
public:
    ruvia::Task<ruvia::HttpResponse> handle(ruvia::Context& c, ruvia::Next&) {
        c.status(209);
        co_return c.text("returned by middleware\n");
    }
};

class SurfacePreDirectResponseMiddleware final : public ruvia::Middleware<SurfacePreDirectResponseMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        c.header("X-Surface-Pre-Direct", "true");
        co_await next();
    }
};

class SurfaceResSlotOnlyMiddleware final : public ruvia::Middleware<SurfaceResSlotOnlyMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next&) {
        c.header("X-Surface-Res-Slot-Only", "true");
        c.respond(c.body(nullptr));
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
    RUVIA_POST("/bytes", bytesBody);
    RUVIA_POST("/blob", blobBody);
    RUVIA_POST("/json-object", jsonValueBody);
    RUVIA_POST("/json-value", jsonValueBody);
    RUVIA_POST("/discard", discard);
    RUVIA_PUT("/items/:id", replaceItem);
    RUVIA_PATCH("/items/:id", patchItem);
    RUVIA_DELETE("/items/:id", deleteItem);
    RUVIA_GET("/cookies", cookies);
    RUVIA_GET("/signed-cookies", signedCookies);
    RUVIA_ALL("/any", anyMethod);
    RUVIA_ON(
        (::ruvia::HttpKnownMethod::kPut, ::ruvia::HttpKnownMethod::kDelete),
        ("/on-item/:id", "/on-legacy/:id"),
        onItem);
    RUVIA_GET("/manual/body", manualBody);
    RUVIA_PUT_STREAM("/upload/:id", streamPut);
    RUVIA_PATCH_STREAM("/upload/:id", streamPatch);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> requestInfo(ruvia::Context& c) {
        const auto& request = c.req();
        std::pmr::string body(c.allocator<char>());
        body.append("method=");
        body.append(request.method());
        body.append("\npath=");
        body.append(request.path());
        body.append("\nroute-path=");
        body.append(request.routePath());
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
        body.append("\nparam-id=");
        body.append(c.req().param("id").value_or(""));
        body.append("\ntag-values=");
        const auto tags = request.queries("tag");
        appendUnsigned(body, tags.size());
        body.append("\ntag-first=");
        if (!tags.empty()) {
            body.append(tags.front());
        }
        body.append("\ntag-missing=");
        body.append(request.queries("missing").empty() ? "missing" : "present");
        body.append("\naccepts-json=");
        body.append(c.req().accepts("application/json") ? "true" : "false");
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> contextInfo(ruvia::Context& c) {
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
        body.append(missing == nullptr ? "true" : "false");
        body.append("\nenv-vars=");
        appendUnsigned(body, c.env().size());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> rawBody(ruvia::Context& c) {
        c.status(202);
        c.header("X-Raw", "first");
        c.header("X-Raw", "second", {.append = true});
        c.header("X-Raw-Init", "true");
        co_return c.body("raw body\n");
    }

    ruvia::Task<ruvia::HttpResponse> responseSlot(ruvia::Context& c) {
        c.header("X-Response-Prepared", "true");
        ruvia::HttpResponse response(c.resource());
        response.status(203);
        response.header("X-Response-Remove", "drop");
        response.body("response slot\n");
        response.header("X-Response-Slot", "true", {.append = true});
        response.header("X-Response-Remove", std::nullopt);
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> resSlotOnly(ruvia::Context& c) {
        c.status(500);
        co_return c.text("handler should not run\n");
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
        c.status(202);
        c.header("X-Null-Body", "true");
        co_return c.body(nullptr);
    }

    ruvia::Task<ruvia::HttpResponse> binaryBody(ruvia::Context& c) {
        static constexpr std::array<std::byte, 3> bytes{
            std::byte{0x00},
            std::byte{0x41},
            std::byte{0xff}};
        c.status(206);
        c.header("X-Binary-Body", "true");
        co_return c.body(std::span<const std::byte>(bytes));
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
        c.status(500);
        co_return c.text("handler should not run\n");
    }

    ruvia::Task<ruvia::HttpResponse> preDirectResponse(ruvia::Context& c) {
        co_return c.text("pre direct response\n");
    }

    ruvia::Task<ruvia::HttpResponse> directBufferedResponse(ruvia::Context& c) {
        c.header("X-Direct-Buffered", "true");
        co_return c.body("direct buffered response\n");
    }

    ruvia::Task<ruvia::HttpResponse> removeBufferedResponse(ruvia::Context& c) {
        c.header("X-Remove-Buffered", "drop");
        c.header("X-Remove-Buffered", std::nullopt);
        co_return c.body("removed buffered response\n");
    }

    ruvia::Task<ruvia::HttpResponse> assignedPreparedResponse(ruvia::Context& c) {
        c.header("X-Surface-Prepared-Assigned", "true");
        ruvia::HttpResponse response(c.resource());
        response.header("Content-Type", "text/plain; charset=UTF-8");
        response.body("assigned prepared response\n");
        co_return response;
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
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> parsedBody(ruvia::Context& c) {
        auto form = co_await c.req().parseBody({
            .repeatedScalars = ruvia::ContextRequest::RepeatedScalarPolicy::kRetainAll,
            .dottedNames = ruvia::ContextRequest::DottedNamePolicy::kExpandPath,
        });
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
        const auto nested = nestedObject.get("key1");
        if (auto nestedText = nested.value()) {
            body.append("\nobj.key1=");
            body.append(*nestedText);
        }
        if (auto nestedValue = nestedObject.get("key1").value()) {
            body.append("\nobj.key1-value=");
            body.append(*nestedValue);
        }
        const auto exactNested = form.get("obj.key1");
        if (auto exactNestedText = exactNested.value()) {
            body.append("\nobj.key1-exact=");
            body.append(*exactNestedText);
        }
        body.append("\nobj.key1-exact-all=");
        appendUnsigned(body, exactNested.size());
        body.append("\nobj.key1-exact-array=");
        body.append(exactNested.array() ? "true" : "false");
        body.append("\nobj.key1-all=");
        appendUnsigned(body, nestedObject.count("key1"));
        body.append("\nobj.key1-values=");
        appendUnsigned(body, nestedObject.count("key1"));
        body.append("\nobj.key1-array=");
        body.append(nestedObject.get("key1").array() ? "true" : "false");
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
                body.append(";content-type=");
                body.append(blob.contentType());
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
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> bytesBody(ruvia::Context& c) {
        const auto bytes = co_await c.req().bytes();
        std::pmr::string body(c.allocator<char>());
        body.append("bytes bytes=");
        appendUnsigned(body, bytes.size());
        body.push_back('\n');
        co_return c.text(std::move(body));
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
        body.append("\ncontent-type=");
        body.append(blob.contentType());
        body.push_back('\n');
        co_return c.text(std::move(body));
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
        co_return c.text(std::move(body));
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
        co_return c.text(std::move(output));
    }

    ruvia::Task<ruvia::HttpResponse> patchItem(ruvia::Context& c) {
        const auto body = co_await c.req().text();
        std::pmr::string output(c.allocator<char>());
        output.append("patch id=");
        output.append(c.req().param("id").value_or(""));
        output.append(" bytes=");
        appendUnsigned(output, body.size());
        output.push_back('\n');
        co_return c.text(std::move(output));
    }

    ruvia::Task<ruvia::HttpResponse> deleteItem(ruvia::Context& c) {
        std::pmr::string output(c.allocator<char>());
        output.append("deleted id=");
        output.append(c.req().param("id").value_or(""));
        output.push_back('\n');
        co_return c.text(std::move(output));
    }

    ruvia::Task<ruvia::HttpResponse> cookies(ruvia::Context& c) {
        ruvia::CookieOptions options;
        options.httpOnly = true;
        options.sameSite = ruvia::CookieSameSite::kLax;
        options.maxAge = std::chrono::seconds(3600);
        c.setCookie("session", "example", options);
        c.setCookie("theme", "light");
        ruvia::CookieOptions hostOptions;
        hostOptions.secure = true;
        hostOptions.httpOnly = true;
        hostOptions.sameSite = ruvia::CookieSameSite::kNone;
        hostOptions.priority = ruvia::CookiePriority::kHigh;
        hostOptions.partitioned = true;
        hostOptions.prefix = ruvia::CookiePrefix::kHost;
        hostOptions.expires = std::chrono::system_clock::now() + std::chrono::hours(1);
        c.setCookie("chip", "value", hostOptions);
        const auto deleted = c.req().cookie("legacy-session");
        c.deleteCookie("legacy-session");
        std::pmr::string body(c.allocator<char>());
        body.append("cookies set\nlegacy-session=");
        body.append(deleted.value_or(""));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> anyMethod(ruvia::Context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("all method=");
        body.append(c.req().method());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> onItem(ruvia::Context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("on method=");
        body.append(c.req().method());
        body.append(" id=");
        body.append(c.req().param("id").value_or(""));
        body.push_back('\n');
        co_return c.text(std::move(body));
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
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> manualBody(ruvia::Context& c) {
        ruvia::HttpResponse response(c.resource());
        response.status(202);
        response.header("Content-Type", "text/plain; charset=UTF-8");
        response.header("X-Manual-Body", "owned");
        response.body("copied body\n");
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
        co_return c.text(std::move(body));
    }
};

class FastSurfaceController final : public ruvia::Controller<FastSurfaceController> {
public:
    RUVIA_CONTROLLER_GROUP("/surface-fast")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/res-slot-merge", responseSlotMerge);
    RUVIA_GET("/res-setter-headers", responseSetterHeaders);
    RUVIA_GET("/body-response", bodyResponse);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> responseSlotMerge(ruvia::Context& c) {
        c.header("X-Res-Slot", "kept");
        co_return c.text("response slot merge\n");
    }

    ruvia::Task<ruvia::HttpResponse> responseSetterHeaders(ruvia::Context& c) {
        c.header("X-Setter-Override", "slot");
        c.header("Content-Type", "application/slot");
        auto response = c.text("response setter headers\n");
        response.header("X-Setter-Override", "response");
        response.header("X-Assigned-Only", "response");
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> bodyResponse(ruvia::Context& c) {
        c.status(201);
        c.header("X-Body-Prepared", "true");
        c.header("X-Body-Response", "true");
        co_return c.body("body response\n");
    }
};

class UngroupedControllerProbe final : public ruvia::Controller<UngroupedControllerProbe> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_ROUTES_END
};

class ControllerBaseSurfaceProbe final : public ruvia::Controller<ControllerBaseSurfaceProbe> {
public:
    template <typename T>
    inline static constexpr bool hasLegacyMiddlewareFactory = requires {
        T::template ruviaMakeMiddlewares<>();
    };

    template <typename T>
    inline static constexpr bool hasLegacyRouteRegistration = requires {
        &T::ruviaAddRoute;
    };
};

#ifndef _MSC_VER
static_assert(!HasControllerPublicGroupPrefix<FastSurfaceController>);
static_assert(!HasControllerPublicGroupMiddlewares<FastSurfaceController>);
static_assert(!HasControllerPublicRegisterRoutes<FastSurfaceController>);
static_assert(!HasControllerPublicRegistrationState<FastSurfaceController>);
static_assert(!HasControllerPublicRegisterRoutes<UngroupedControllerProbe>);
static_assert(!HasControllerRegistrationAccessPublicHooks<
    ruvia::detail::ControllerRegistrationAccess<FastSurfaceController>>);
static_assert(!ControllerBaseSurfaceProbe::hasLegacyMiddlewareFactory<ControllerBaseSurfaceProbe>);
static_assert(!ControllerBaseSurfaceProbe::hasLegacyRouteRegistration<ControllerBaseSurfaceProbe>);
#endif

int main() {
    ruvia::app()
        .setListenAddress("0.0.0.0")
        .setHttpListenPort(8088)
        .setThreadNum(2)
        .notFound(&surfaceNotFound)
        .run();
}
