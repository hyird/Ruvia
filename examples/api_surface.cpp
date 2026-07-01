#include <charconv>
#include <cstddef>
#include <cstdint>
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

inline constexpr ruvia::ContextKey<CurrentUser> kCurrentUser("currentUser");
template <typename T>
concept HasPlainAddressOf = requires(T& value) {
    &value;
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
static_assert(!noexcept(std::declval<const ruvia::ContextRequest&>().query(std::string_view{})));
static_assert(!noexcept(std::declval<const ruvia::ContextRequest&>().param(std::string_view{})));

void appendUnsigned(std::pmr::string& output, std::uint64_t value) {
    char buffer[32]{};
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec == std::errc{}) {
        output.append(buffer, static_cast<std::size_t>(ptr - buffer));
    }
}

}  // namespace

ruvia::Task<ruvia::HttpResponse> surfaceRenderer(
    ruvia::Context& c,
    std::string_view body,
    ruvia::Context::RenderOptions options) {
    std::pmr::string html(c.allocator<char>());
    html.append("<!doctype html><html><head>");
    html.append(options.head);
    html.append("</head><body><main>");
    html.append(body);
    html.append("</main></body></html>");
    co_return c.html(html);
}

class SurfaceContextMiddleware final : public ruvia::Middleware<SurfaceContextMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, const ruvia::Next& next) {
        c.set(kCurrentUser, CurrentUser{.id = 7, .name = "surface-user"});
        c.set("traceId", std::string_view("surface-trace"));
        c.setRenderer(&surfaceRenderer);
        co_await next();
        if (c.error()) {
            c.res(c.text("caught by middleware\n", 500));
            co_return;
        }
        c.setHeader("X-Surface-Finalized", c.finalized() ? "true" : "false");
        c.res().headers().append("X-Surface-Middleware", "after-next");
    }
};

class SurfaceReturnMiddleware final : public ruvia::Middleware<SurfaceReturnMiddleware> {
public:
    ruvia::Task<ruvia::HttpResponse> handle(ruvia::Context& c, const ruvia::Next&) {
        co_return c.text("returned by middleware\n", 209);
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
    RUVIA_GET("/html", htmlBody);
    RUVIA_GET("/render", renderBody);
    RUVIA_GET("/error", appError);
    RUVIA_GET("/throw", throwError);
    RUVIA_GET("/missing", missing);
    RUVIA_GET("/middleware-return", middlewareReturnHandler, SurfaceReturnMiddleware);
    RUVIA_POST("/multipart", bufferedMultipart);
    RUVIA_POST("/parse-body", parsedBody);
    RUVIA_POST("/form-data", formDataBody);
    RUVIA_POST("/array-buffer", arrayBufferBody);
    RUVIA_POST("/blob", blobBody);
    RUVIA_POST("/clone-raw", cloneRawRequest);
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
        body.append("\ntarget=");
        body.append(request.target());
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
        body.append("\nquery=");
        body.append(request.queryString());
        body.append("\nheaders=");
        appendUnsigned(body, c.req().header().size());
        body.append("\nheader-entries=");
        appendUnsigned(body, c.req().header().entries().size());
        body.append("\nheader-keys=");
        appendUnsigned(body, c.req().header().keys().size());
        body.append("\nheader-values=");
        appendUnsigned(body, c.req().header().values().size());
        body.append("\nraw-headers=");
        appendUnsigned(body, c.req().headers().size());
        body.append("\nparams=");
        appendUnsigned(body, c.req().param().size());
        body.append("\nquery-fields=");
        appendUnsigned(body, request.query().size());
        body.append("\nquery-entries=");
        appendUnsigned(body, request.query().entries().size());
        body.append("\nquery-tag=");
        body.append(request.query()["tag"]);
        body.append("\nshortcut-header-host=");
        body.append(c.header("Host"));
        body.append("\nshortcut-query-tag=");
        body.append(c.query("tag").value_or(""));
        body.append("\nshortcut-cookie-surface=");
        if (auto surfaceCookie = c.cookie("surface")) {
            body.append(*surfaceCookie);
        }
        body.append("\nquery-groups=");
        appendUnsigned(body, request.queries().size());
        body.append("\nquery-tag-group=");
        appendUnsigned(body, request.queries()["tag"].size());
        body.append("\nquery-tag-first=");
        if (auto tag = request.queries().first("tag")) {
            body.append(*tag);
        }
        body.append("\ncookies=");
        appendUnsigned(body, request.cookie().size());
        body.append("\nmatched-routes=");
        appendUnsigned(body, request.matchedRoutes().size());
        body.append("\nmatched-routes-helper=");
        appendUnsigned(body, ruvia::matchedRoutes(c).size());
        body.append("\nroute-index=");
        appendUnsigned(body, request.routeIndex());
        body.append("\nparam-id=");
        body.append(request.param()["id"]);
        body.append("\nshortcut-param-id=");
        body.append(c.param("id").value_or(""));
        body.append("\ntag-values=");
        const auto tags = request.queries("tag");
        appendUnsigned(body, tags.size());
        body.append("\ntag-first=");
        if (!tags.empty()) {
            body.append(tags.front());
        }
        body.append("\nversion=");
        body.append(request.httpVersion());
        body.append("\naccepts-json=");
        body.append(c.req().accepts("application/json") ? "true" : "false");
        body.append("\ndecoded-path=");
        if (auto decoded = c.req().decodedPath().toString()) {
            body.append(*decoded);
        }
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> contextInfo(ruvia::Context& c) {
        const auto vars = c.var();
        const auto& user = vars[kCurrentUser];
        const auto* traceId = vars.getIf<std::string_view>("traceId");
        std::pmr::string body(c.allocator<char>());
        body.append("user=");
        body.append(user.name);
        body.append("\nid=");
        appendUnsigned(body, user.id);
        body.append("\ntrace=");
        body.append(traceId == nullptr ? "missing" : *traceId);
        body.append("\nmissing-var=");
        body.append(vars.has<std::uint32_t>("missing") ? "false" : "true");
        body.append("\nenv-vars=");
        appendUnsigned(body, c.env().size());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> rawBody(ruvia::Context& c) {
        co_return c
            .header("X-Raw", "first")
            .header("X-Raw", "second", {.append = true})
            .body("raw body\n", {.status = 202, .headers = {{"X-Raw-Init", "true"}}});
    }

    ruvia::Task<ruvia::HttpResponse> responseSlot(ruvia::Context& c) {
        c.setHeader("X-Response-Prepared", "true");
        ruvia::HttpResponse response(c.resource());
        response.setStatus(203, {});
        response.setHeader("X-Response-Remove", "drop");
        response.setBodyCopy("response slot\n");
        c.res(std::move(response));
        c.res().responseHeaders().append("X-Response-Slot", "true");
        c.res().responseHeaders().remove("X-Response-Remove");
        co_return std::move(c.res());
    }

    ruvia::Task<ruvia::HttpResponse> htmlBody(ruvia::Context& c) {
        co_return c.html("<strong>html body</strong>\n");
    }

    ruvia::Task<ruvia::HttpResponse> renderBody(ruvia::Context& c) {
        co_return co_await c.render(
            "<h1>rendered body</h1>",
            {.head = "<title>surface</title>"});
    }

    ruvia::Task<ruvia::HttpResponse> appError(ruvia::Context& c) {
        c.setHeader("X-Error-Prepared", "true");
        co_return c.error(418, "teapot", "short and stout", "I'm a Teapot");
    }

    ruvia::Task<ruvia::HttpResponse> throwError(ruvia::Context&) {
        throw std::runtime_error("surface route failed");
    }

    ruvia::Task<ruvia::HttpResponse> missing(ruvia::Context& c) {
        c.setHeader("X-Not-Found-Prepared", "true");
        co_return co_await c.notFound();
    }

    ruvia::Task<ruvia::HttpResponse> middlewareReturnHandler(ruvia::Context& c) {
        co_return c.text("handler should not run\n", 500);
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
        if (auto titleText = title.toStringView()) {
            body.append("\ntitle=");
            body.append(*titleText);
        }
        body.append("\ntag-count=");
        appendUnsigned(body, form.getAll("tag").values().size());
        if (auto tag = form.get("tag").toStringView()) {
            body.append("\ntag-single=");
            body.append(*tag);
        }
        body.append("\ntag-array-count=");
        appendUnsigned(body, form.count("tag[]"));
        body.append("\ntag-array-values=");
        appendUnsigned(body, form["tag[]"].values().size());
        body.append("\ntag-array=");
        body.append(form["tag[]"].isArray() ? "true" : "false");
        const auto nested = form.object("obj")["key1"];
        if (auto nestedText = nested.toStringView()) {
            body.append("\nobj.key1=");
            body.append(*nestedText);
        }
        body.append("\nobj.key-count=");
        appendUnsigned(body, form.object("obj").count("key1"));
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
        if (auto tag = form.get("tag").toStringView()) {
            body.append("\ntag-single=");
            body.append(*tag);
        }
        body.append("\ntag-array-count=");
        appendUnsigned(body, form.count("tag[]"));
        const auto title = form.get("title");
        if (auto titleText = title.toStringView()) {
            body.append("\ntitle=");
            body.append(*titleText);
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

    ruvia::Task<ruvia::HttpResponse> blobBody(ruvia::Context& c) {
        const auto blob = co_await c.req().blob();
        std::pmr::string body(c.allocator<char>());
        body.append("blob bytes=");
        appendUnsigned(body, blob.size());
        body.append("\ntype=");
        body.append(blob.type());
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
        body.append("\nconsumed=");
        body.append(consumed);
        body.append("\ntype=");
        body.append(clone.blob().type());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> discard(ruvia::Context& c) {
        co_await c.req().discardBody();
        co_return c.status(204).text("");
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
        co_return c
            .setCookie("session", "example", options)
            .setCookie("theme", "light")
            .deleteCookie("legacy-session")
            .text("cookies set\n");
    }

    ruvia::Task<ruvia::HttpResponse> manualCopy(ruvia::Context& c) {
        ruvia::HttpResponse response(c.resource());
        response.setStatus(202, "Accepted");
        response.setHeader("Content-Type", "text/plain; charset=utf-8");
        response.setHeader("X-Manual-Body", "copy");
        response.setBodyCopy(std::string_view("copied body\n"));
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> manualView(ruvia::Context& c) {
        ruvia::HttpResponse response(c.resource());
        response.setHeader("Content-Type", "text/plain; charset=utf-8");
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

int main() {
    ruvia::app()
        .setListenAddress("0.0.0.0", 8088)
        .setThreadNum(2)
        .run();
}
