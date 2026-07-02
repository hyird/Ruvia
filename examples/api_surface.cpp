#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

namespace {

struct CurrentUser final {
    std::uint32_t id{0};
    std::string_view name;
};

RUVIA_MODEL(ClonePayload,
    RUVIA_FIELD(message, ruvia::String)
);

RUVIA_MODEL(SurfaceJsonResponse,
    RUVIA_FIELD(message, ruvia::String)
);

inline constexpr ruvia::ContextKey<CurrentUser> kCurrentUser("currentUser");
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
concept HasRequestJsonValueAlias = requires(const T& request) {
    { request.json() } -> std::same_as<ruvia::Task<ruvia::JsonValue>>;
};

template <typename T>
concept HasMemberCloneRawRequestAlias = requires(const T& request) {
    request.cloneRawRequest();
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
static_assert(std::is_move_constructible_v<ruvia::RequestNameValueList>);
static_assert(std::is_move_assignable_v<ruvia::RequestNameValueList>);
static_assert(!std::is_copy_constructible_v<ruvia::RequestValueGroup>);
static_assert(!std::is_copy_assignable_v<ruvia::RequestValueGroup>);
static_assert(std::is_move_constructible_v<ruvia::RequestValueGroup>);
static_assert(std::is_move_assignable_v<ruvia::RequestValueGroup>);
static_assert(!std::is_copy_constructible_v<ruvia::RequestValueGroupList>);
static_assert(!std::is_copy_assignable_v<ruvia::RequestValueGroupList>);
static_assert(std::is_move_constructible_v<ruvia::RequestValueGroupList>);
static_assert(std::is_move_assignable_v<ruvia::RequestValueGroupList>);
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
static_assert(HasRequestJsonValueAlias<ruvia::ContextRequest>);
static_assert(!HasMemberCloneRawRequestAlias<ruvia::ContextRequest>);
static_assert(!HasRequestMethodEnumAlias<ruvia::ContextRequest>);
static_assert(!HasRequestTargetAlias<ruvia::ContextRequest>);
static_assert(!HasRequestHeadersAlias<ruvia::ContextRequest>);
static_assert(!HasRequestQueryStringAlias<ruvia::ContextRequest>);
static_assert(HasRequestRoutePathAlias<ruvia::ContextRequest>);
static_assert(HasRequestMatchedRoutesAlias<ruvia::ContextRequest>);
static_assert(HasRequestRouteIndexAlias<ruvia::ContextRequest>);
static_assert(!HasRequestHttpVersionAlias<ruvia::ContextRequest>);
static_assert(!HasRequestDecodedPathAlias<ruvia::ContextRequest>);
static_assert(!HasRequestRemoteAddressAlias<ruvia::ContextRequest>);
static_assert(!HasRequestClientCertificateAlias<ruvia::ContextRequest>);
static_assert(!HasRequestIsSecureAlias<ruvia::ContextRequest>);
static_assert(!HasFormValueToStringView<ruvia::ContextRequest::RequestFormData::Value>);
static_assert(!HasFormValueToStringView<ruvia::ContextRequest::RequestFormData::PathValue>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().setRenderer(static_cast<ruvia::Context::Renderer>(nullptr))),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().setLayout(static_cast<ruvia::Context::Layout>(nullptr))),
    ruvia::Context::Layout>);
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
    decltype(std::declval<ruvia::Context&>().setHeader(std::string_view{}, std::string_view{})),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().setHeader(
        std::string_view{},
        std::string_view{},
        ruvia::Context::HeaderOptions{.append = true})),
    void>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::Context&>().setHeader(std::string_view{}, std::nullopt)),
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
    decltype(std::declval<const ruvia::ContextRequest&>().cookie()),
    const ruvia::RequestNameValueList&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ContextRequest&>().cookies()),
    const ruvia::RequestNameValueList&>);
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
    if (const auto layout = c.getLayout(); layout != nullptr) {
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
    co_return c.text(
        "surface not found\n",
        404,
        {{"X-Surface-Not-Found", "true"}});
}

class SurfaceContextMiddleware final : public ruvia::Middleware<SurfaceContextMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        c.set(kCurrentUser, CurrentUser{.id = 7, .name = "surface-user"});
        c.set("traceId", std::string_view("surface-trace"));
        c.setRenderer(&surfaceRenderer);
        co_await next();
        if (c.error()) {
            const bool hadDownstreamErrorResponse = c.finalized();
            const auto downstreamStatus = hadDownstreamErrorResponse
                ? c.res().statusCode()
                : 0;
            auto response = c.text("caught by middleware\n", 500);
            response.setHeader("X-Surface-Error", "true");
            response.setHeader(
                "X-Surface-Error-Response",
                hadDownstreamErrorResponse ? "true" : "false");
            response.setHeader(
                "X-Surface-Error-Status",
                downstreamStatus == 500 ? "500" : "other");
            c.res(std::move(response));
            co_return;
        }
        c.setHeader("X-Surface-Finalized", c.finalized() ? "true" : "false");
        c.res().headers().append("X-Surface-Middleware", "after-next");
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
        c.res().setHeader("X-Surface-Pre-Direct", "true");
        co_await next();
    }
};

class SurfaceLayoutMiddleware final : public ruvia::Middleware<SurfaceLayoutMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        [[maybe_unused]] const auto installedLayout = c.setLayout(&surfaceLayout);
        co_await next();
    }
};

class SurfaceResSlotOnlyMiddleware final : public ruvia::Middleware<SurfaceResSlotOnlyMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next&) {
        c.res().headers().set("X-Surface-Res-Slot-Only", "true");
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
        body.append("\nrequest-route-path=");
        body.append(request.routePath());
        body.append("\nheaders=");
        appendUnsigned(body, c.req().header().size());
        body.append("\nheader-entries=");
        appendUnsigned(body, c.req().header().entries().size());
        body.append("\nheader-keys=");
        appendUnsigned(body, c.req().header().keys().size());
        body.append("\nheader-values=");
        appendUnsigned(body, c.req().header().values().size());
        body.append("\nheader-host-all=");
        appendUnsigned(body, request.header().getAll("host").size());
        body.append("\nheader-x-dupe-object=");
        body.append(request.header()["x-dupe"]);
        body.append("\nparams=");
        appendUnsigned(body, c.req().param().size());
        body.append("\nquery-fields=");
        appendUnsigned(body, request.query().size());
        body.append("\nquery-entries=");
        appendUnsigned(body, request.query().entries().size());
        body.append("\nquery-tag=");
        body.append(request.query()["tag"]);
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
        body.append("\nquery-groups=");
        appendUnsigned(body, request.queries().size());
        body.append("\nquery-group-entries=");
        appendUnsigned(body, request.queries().entries().size());
        body.append("\nquery-group-keys=");
        appendUnsigned(body, request.queries().keys().size());
        body.append("\nquery-group-values=");
        appendUnsigned(body, request.queries().values().size());
        body.append("\nquery-tag-group=");
        appendUnsigned(body, request.queries()["tag"].size());
        body.append("\nquery-tag-first=");
        if (auto tag = request.queries().first("tag")) {
            body.append(*tag);
        }
        body.append("\nquery-shared-first=");
        const auto singleTag = request.query().get("tag");
        const auto groupedTag = request.queries().first("tag");
        body.append(
            singleTag.has_value() && groupedTag.has_value() && singleTag->data() == groupedTag->data()
                    && singleTag->size() == groupedTag->size()
                ? "true"
                : "false");
        body.append("\ncookies=");
        appendUnsigned(body, request.cookie().size());
        body.append("\ncookie-entries=");
        appendUnsigned(body, request.cookie().entries().size());
        body.append("\ncookie-keys=");
        appendUnsigned(body, request.cookie().keys().size());
        body.append("\ncookie-values=");
        appendUnsigned(body, request.cookie().values().size());
        body.append("\ncookie-surface-object=");
        body.append(request.cookie()["surface"]);
        body.append("\ncookie-surface-all=");
        appendUnsigned(body, request.cookie().getAll("surface").size());
        body.append("\nmatched-routes=");
        appendUnsigned(body, ruvia::matchedRoutes(c).size());
        body.append("\nrequest-matched-routes=");
        appendUnsigned(body, request.matchedRoutes().size());
        body.append("\nrequest-route-index=");
        appendUnsigned(body, request.routeIndex());
        body.append("\nparam-id=");
        body.append(request.param()["id"]);
        body.append("\nrequest-param-id=");
        body.append(c.req().param("id").value_or(""));
        body.append("\ntag-values=");
        const auto tags = request.queries("tag");
        appendUnsigned(body, tags.has_value() ? tags->size() : 0);
        body.append("\ntag-group-get-all=");
        appendUnsigned(body, request.queries().getAll("tag").size());
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
        body.append(missing == nullptr && !vars.has<std::uint32_t>("missing") ? "true" : "false");
        body.append("\nenv-vars=");
        appendUnsigned(body, c.env().size());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> rawBody(ruvia::Context& c) {
        c.header("X-Raw", "first");
        c.header("X-Raw", "second", {.append = true});
        co_return c.body("raw body\n", {.status = 202, .headers = {{"X-Raw-Init", "true"}}});
    }

    ruvia::Task<ruvia::HttpResponse> responseSlot(ruvia::Context& c) {
        c.setHeader("X-Response-Prepared", "true");
        ruvia::HttpResponse response(c.resource());
        response.setStatus(203, {});
        response.setHeader("X-Response-Remove", "drop");
        response.setBodyCopy("response slot\n");
        c.res(std::move(response));
        c.res().headers().append("X-Response-Slot", "true");
        c.res().headers().remove("X-Response-Remove");
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
        co_return c.newResponse(
            nullptr,
            202,
            {{"X-Null-Body", "true"}});
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
        c.setHeader("X-Remove-Too", "drop");
        c.header("X-Keep-Me", "keep");
        c.header("X-Remove-Me", std::nullopt);
        c.setHeader("X-Remove-Too", std::nullopt);
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
        c.setHeader("X-Error-Prepared", "true");
        co_return c.error(418, "teapot", "short and stout", "I'm a Teapot");
    }

    ruvia::Task<ruvia::HttpResponse> throwError(ruvia::Context&) {
        throw std::runtime_error("surface route failed");
    }

    ruvia::Task<void> streamThrow(ruvia::Context&) {
        throw std::runtime_error("surface stream failed");
    }

    ruvia::Task<ruvia::HttpResponse> missing(ruvia::Context& c) {
        c.setHeader("X-Not-Found-Prepared", "true");
        co_return co_await c.notFound();
    }

    ruvia::Task<ruvia::HttpResponse> middlewareReturnHandler(ruvia::Context& c) {
        co_return c.text("handler should not run\n", 500);
    }

    ruvia::Task<ruvia::HttpResponse> preDirectResponse(ruvia::Context& c) {
        co_return c.text("pre direct response\n");
    }

    ruvia::Task<ruvia::HttpResponse> directBufferedResponse(ruvia::Context& c) {
        c.setHeader("X-Direct-Buffered", "true");
        c.res().setBodyCopy("direct buffered response\n");
        co_return std::move(c.res());
    }

    ruvia::Task<ruvia::HttpResponse> removeBufferedResponse(ruvia::Context& c) {
        c.setHeader("X-Remove-Buffered", "drop");
        c.res().headers().remove("X-Remove-Buffered");
        ruvia::HttpResponse response(c.resource());
        response.setBodyCopy("removed buffered response\n");
        c.res(std::move(response));
        co_return std::move(c.res());
    }

    ruvia::Task<ruvia::HttpResponse> assignedPreparedResponse(ruvia::Context& c) {
        c.header("X-Surface-Prepared-Assigned", "true");
        ruvia::HttpResponse response(c.resource());
        response.setHeader("Content-Type", "text/plain; charset=UTF-8");
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
            body.append(part.name);
            body.append(";filename=");
            body.append(part.filename);
            body.append(";content-type=");
            body.append(part.contentType);
            body.append(";bytes=");
            appendUnsigned(body, part.body.size());
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
        appendUnsigned(body, form.entries().size());
        body.append("\ngroups=");
        appendUnsigned(body, form.groups().size());
        body.append("\nkeys=");
        appendUnsigned(body, form.keys().size());
        body.append("\nvalues=");
        appendUnsigned(body, form.values().size());
        body.append("\nfirst-value-file=");
        body.append(!form.values().empty() && form.values().front().isFile() ? "true" : "false");
        body.append("\nhas-title=");
        body.append(form.has("title") ? "true" : "false");
        const auto title = form.get("title");
        if (auto titleText = title.value()) {
            body.append("\ntitle=");
            body.append(*titleText);
        }
        body.append("\nhas-obj=");
        body.append(form.has("obj") ? "true" : "false");
        body.append("\nhas-obj-key1=");
        body.append(form.has("obj.key1") ? "true" : "false");
        if (auto directNested = form.get("obj.key1").value()) {
            body.append("\nobj.key1-direct=");
            body.append(*directNested);
        }
        body.append("\ntag-count=");
        appendUnsigned(body, form.getAll("tag").values().size());
        if (auto tag = form.get("tag").value()) {
            body.append("\ntag-single=");
            body.append(*tag);
        }
        body.append("\ntag-array-count=");
        appendUnsigned(body, form.count("tag[]"));
        body.append("\ntag-array-values=");
        appendUnsigned(body, form["tag[]"].values().size());
        body.append("\ntag-array=");
        body.append(form["tag[]"].isArray() ? "true" : "false");
        body.append("\ntag-is-array=");
        body.append(form["tag"].isArray() ? "true" : "false");
        const auto nestedObject = form.object("obj");
        const auto nested = nestedObject.get("key1");
        if (auto nestedText = nested.value()) {
            body.append("\nobj.key1=");
            body.append(*nestedText);
        }
        if (auto nestedValue = nestedObject.value("key1")) {
            body.append("\nobj.key1-value=");
            body.append(*nestedValue);
        }
        const auto exactNested = form.at("obj.key1");
        if (auto exactNestedText = exactNested.value()) {
            body.append("\nobj.key1-at=");
            body.append(*exactNestedText);
        }
        body.append("\nobj.key1-at-all=");
        appendUnsigned(body, form.getAllAt("obj.key1").size());
        body.append("\nobj.key1-at-array=");
        body.append(form.isArrayAt("obj.key1") ? "true" : "false");
        body.append("\nobj.key1-all=");
        appendUnsigned(body, nestedObject.getAll("key1").size());
        body.append("\nobj.key1-values=");
        appendUnsigned(body, nestedObject.values("key1").size());
        body.append("\nobj.key1-array=");
        body.append(nestedObject.get("key1").isArray() ? "true" : "false");
        body.append("\nobj.key-count=");
        appendUnsigned(body, nestedObject.count("key1"));
        body.append("\nobj.entries=");
        appendUnsigned(body, nestedObject.entries().size());
        body.append("\nobj.groups=");
        appendUnsigned(body, nestedObject.groups().size());
        body.append("\nobj.keys=");
        appendUnsigned(body, nestedObject.keys().size());
        const auto childObject = nestedObject.object("child");
        body.append("\nobj.child.keys=");
        appendUnsigned(body, childObject.keys().size());
        for (const auto& field : form.entries()) {
            body.append("\n");
            body.append(field.name);
            body.push_back('=');
            body.append(field.text());
            if (field.isFile()) {
                const auto blob = field.blob();
                body.append(";filename=");
                body.append(field.fileName());
                body.append(";type=");
                body.append(blob.type());
                body.append(";bytes=");
                appendUnsigned(body, blob.size());
            }
            if (!field.path.empty()) {
                body.append(";path=");
                for (std::size_t i = 0; i < field.path.size(); ++i) {
                    if (i != 0) {
                        body.push_back('/');
                    }
                    body.append(field.path[i]);
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
        auto tags = form.getAll("tag");
        std::pmr::string body(c.allocator<char>());
        body.append("fields=");
        appendUnsigned(body, form.fields().size());
        body.append("\nentries=");
        appendUnsigned(body, form.entries().size());
        body.append("\ngroups=");
        appendUnsigned(body, form.groups().size());
        body.append("\nkeys=");
        appendUnsigned(body, form.keys().size());
        body.append("\nvalues=");
        appendUnsigned(body, form.values().size());
        body.append("\nfirst-value-file=");
        body.append(!form.values().empty() && form.values().front().isFile() ? "true" : "false");
        body.append("\nhas-title=");
        body.append(form.has("title") ? "true" : "false");
        body.append("\ntag-count=");
        appendUnsigned(body, tags.values().size());
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
        const auto arrayBuffer = blob.arrayBuffer();
        const auto text = blob.text();
        std::pmr::string body(c.allocator<char>());
        body.append("blob bytes=");
        appendUnsigned(body, blob.size());
        body.append("\narray-buffer bytes=");
        appendUnsigned(body, arrayBuffer.size());
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
        body.append("\nheaders=");
        appendUnsigned(body, clone.headers().size());
        body.append("\nbody=");
        body.append(clone.body());
        body.append("\ntext=");
        body.append(clone.text());
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
        appendUnsigned(body, form.entries().size());
        if (auto title = form.get("title").value()) {
            body.append("\ntitle=");
            body.append(*title);
        }
        body.append("\ntag-count=");
        appendUnsigned(body, form.getAll("tag").values().size());
        body.append("\ntag-array-count=");
        appendUnsigned(body, form.count("tag[]"));
        if (auto nested = parsed.object("obj").value("key1")) {
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
        const auto deleted = c.deleteCookie("legacy-session");
        std::pmr::string body(c.allocator<char>());
        body.append("cookies set\nlegacy-session=");
        body.append(deleted.value_or(""));
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> manualCopy(ruvia::Context& c) {
        ruvia::HttpResponse response(c.resource());
        response.setStatus(202, "Accepted");
        response.setHeader("Content-Type", "text/plain; charset=UTF-8");
        response.setHeader("X-Manual-Body", "copy");
        response.setBodyCopy(std::string_view("copied body\n"));
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> manualView(ruvia::Context& c) {
        ruvia::HttpResponse response(c.resource());
        response.setHeader("Content-Type", "text/plain; charset=UTF-8");
        response.setHeader("X-Manual-Body", "view");
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
        c.res().headers().set("X-Res-Slot", "kept");
        co_return c.text("response slot merge\n");
    }

    ruvia::Task<ruvia::HttpResponse> responseSetterHeaders(ruvia::Context& c) {
        c.res().headers().set("X-Setter-Override", "slot");
        c.res().headers().set("Content-Type", "application/slot");
        auto response = c.text("response setter headers\n");
        response.setHeader("X-Setter-Override", "response");
        response.setHeader("X-Assigned-Only", "response");
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
        .setListenAddress("0.0.0.0", 8088)
        .setThreadNum(2)
        .notFound(&surfaceNotFound)
        .run();
}
