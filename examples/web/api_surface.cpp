// A server exercising the HTTP context surface: route metadata and decoded
// paths, Accept checks, buffered multipart, explicit body discard, response
// cookies, manual HttpResponse body ownership and PUT/PATCH streaming.
//
// The compile-time assertions about what this surface deliberately does NOT
// offer live in tests/web/guards/web_api_surface_guard.cpp.

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
#include <vector>

#include "ruvia/web/App.h"
#include "ruvia/web/auth/Jwt.h"
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
#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/RateLimit.h"
#include "ruvia/web/SecurityHeaders.h"
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

struct SurfaceJsonResponse final {
    RUVIA_MODEL(SurfaceJsonResponse,
        RUVIA_OPTIONAL_FIELD(message, ruvia::String));
};

}  // namespace

ruvia::Task<ruvia::HttpResponse> surfaceNotFound(ruvia::Context& c) {
    c.status(ruvia::http_status::kNotFound);
    c.header("X-Surface-Not-Found", "true");
    co_return c.text("surface not found\n");
}

class SurfaceContextMiddleware final : public ruvia::Middleware<SurfaceContextMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        if (c.exception()) {
            const auto* downstreamResponse = c.response();
            const bool hadDownstreamErrorResponse = downstreamResponse != nullptr;
            const bool downstreamWasInternalError = downstreamResponse != nullptr && downstreamResponse->status() == ruvia::http_status::kInternalServerError;
            c.status(ruvia::http_status::kInternalServerError);
            auto response = c.text("caught by middleware\n");
            response.header("X-Surface-Error", "true");
            response.header("X-Surface-Error-Response", hadDownstreamErrorResponse ? "true" : "false");
            response.header("X-Surface-Error-Status", downstreamWasInternalError ? "internal-server-error" : "other");
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
        c.status(ruvia::http_status::kAccepted);
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
    RUVIA_ON((::ruvia::HttpKnownMethod::kPut, ::ruvia::HttpKnownMethod::kDelete), ("/on-item/:id", "/on-legacy/:id"), onItem);
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
        std::pmr::string body(c.allocator<char>());
        body.append("session=");
        body.append(c.session());
        body.append("\nenv-vars=");
        appendUnsigned(body, c.env().size());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> rawBody(ruvia::Context& c) {
        c.status(ruvia::http_status::kAccepted);
        c.header("X-Raw", "first");
        c.header("X-Raw", "second", {.append = true});
        c.header("X-Raw-Init", "true");
        co_return c.body("raw body\n");
    }

    ruvia::Task<ruvia::HttpResponse> responseSlot(ruvia::Context& c) {
        c.header("X-Response-Prepared", "true");
        ruvia::HttpResponse response(c.resource());
        response.status(ruvia::http_status::kNonAuthoritativeInformation);
        response.header("X-Response-Remove", "drop");
        response.body("response slot\n");
        response.header("X-Response-Slot", "true", {.append = true});
        response.removeHeader("X-Response-Remove");
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> resSlotOnly(ruvia::Context& c) {
        c.status(ruvia::http_status::kInternalServerError);
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
        c.status(ruvia::http_status::kAccepted);
        c.header("X-Null-Body", "true");
        co_return c.body(nullptr);
    }

    ruvia::Task<ruvia::HttpResponse> binaryBody(ruvia::Context& c) {
        static constexpr std::array<std::byte, 3> bytes{std::byte{0x00}, std::byte{0x41}, std::byte{0xff}};
        c.status(ruvia::http_status::kPartialContent);
        c.header("X-Binary-Body", "true");
        co_return c.body(std::span<const std::byte>(bytes));
    }

    ruvia::Task<ruvia::HttpResponse> headerRemove(ruvia::Context& c) {
        c.header("X-Remove-Me", "drop");
        c.header("X-Remove-Too", "drop");
        c.header("X-Keep-Me", "keep");
        c.removeHeader("X-Remove-Me");
        c.removeHeader("X-Remove-Too");
        co_return c.text("header remove\n");
    }

    ruvia::Task<ruvia::HttpResponse> redirectUnicode(ruvia::Context& c) {
        co_return c.redirect("/目标?x=值", ruvia::http_status::kSeeOther);
    }

    ruvia::Task<ruvia::HttpResponse> redirectPreparedLocation(ruvia::Context& c) {
        c.header("Location", "/surface/wrong");
        co_return c.redirect("/surface/right", ruvia::http_status::kFound);
    }

    ruvia::Task<ruvia::HttpResponse> appError(ruvia::Context& c) {
        c.header("X-Error-Prepared", "true");
        co_return c.error(ruvia::http_status::kBadRequest, "example_error", "the example request was rejected");
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
        c.status(ruvia::http_status::kInternalServerError);
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
        c.removeHeader("X-Remove-Buffered");
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
        body.append(!form.fields().empty() && form.fields().front().isFile() ? "true" : "false");
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
        body.append(form.get("tag[]").isArray() ? "true" : "false");
        body.append("\ntag-is-array=");
        body.append(form.get("tag").isArray() ? "true" : "false");
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
        body.append(exactNested.isArray() ? "true" : "false");
        body.append("\nobj.key1-all=");
        appendUnsigned(body, nestedObject.count("key1"));
        body.append("\nobj.key1-values=");
        appendUnsigned(body, nestedObject.count("key1"));
        body.append("\nobj.key1-array=");
        body.append(nestedObject.get("key1").isArray() ? "true" : "false");
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
            if (field.isFile()) {
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
            body.append(group.isArray() ? "true" : "false");
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
        c.status(ruvia::http_status::kNoContent);
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
        response.status(ruvia::http_status::kAccepted);
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

int main() {
    ruvia::app().setListeners({ruvia::ListenerConfig::http("0.0.0.0", 8088)}).setWorkersPerListener(2).setSignalShutdown(true).onNotFound(&surfaceNotFound).run();
}
