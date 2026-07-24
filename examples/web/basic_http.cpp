// Basic HTTP server: controller/group macros, middleware, path params,
// wildcard routes, query/header/cookie helpers, body reads, urlFor links,
// text/JSON/redirect/error responses including HEAD and OPTIONS, and
// prefix-scoped notFound/onError fallbacks layered under the app-wide one.

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/Error.h"

class RequestIdMiddleware final : public ruvia::Middleware<RequestIdMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Example", "basic-http");
    }
};

class AdminAuthMiddleware final : public ruvia::Middleware<AdminAuthMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        if (c.req().header("X-Admin-Token").value_or("") != "secret") {
            c.respond(c.error(ruvia::http_status::kUnauthorized, "unauthorized", "missing admin token"));
            co_return;
        }
        co_await next();
    }
};

struct UserResponse final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(active, ruvia::Bool);
    RUVIA_MODEL(UserResponse, id, name, active);
};

ruvia::Task<ruvia::HttpResponse> exampleErrorHandler(ruvia::Context& c, ruvia::HttpErrorInfo error) {
    co_return c.error(error.status(), error.code(), error.message(), error.statusText());
}

// Prefix-scoped fallbacks: the longest matching registered prefix wins, on
// whole path segments ("/api" scopes "/api/x" but never "/apix"); requests
// outside every prefix keep using the app-wide handlers above.
ruvia::Task<ruvia::HttpResponse> apiNotFound(ruvia::Context& c) {
    c.status(ruvia::http_status::kNotFound);
    co_return c.error(ruvia::http_status::kNotFound, "api_not_found", "no such API endpoint");
}

ruvia::Task<ruvia::HttpResponse> apiError(ruvia::Context& c, ruvia::HttpErrorInfo error) {
    c.header("X-Api-Error", "true");
    co_return c.error(error.status(), error.code(), error.message(), error.statusText());
}

std::optional<std::uint32_t> parseUInt32(std::optional<std::string_view> input) noexcept {
    if (!input || input->empty()) {
        return std::nullopt;
    }

    std::uint32_t value{};
    const auto* const begin = input->data();
    const auto* const end = begin + input->size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

class BasicHttpController final : public ruvia::Controller<BasicHttpController> {
public:
    RUVIA_CONTROLLER_GROUP("/api", RequestIdMiddleware)

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/hello", hello);
    RUVIA_GET("/users/:id", user);
    RUVIA_GET("/files/*", wildcard);
    RUVIA_GET("/inputs", inputs);
    RUVIA_POST("/echo", echo);
    RUVIA_GET("/redirect", redirect);
    RUVIA_GET("/links/:id", links);
    RUVIA_GET("/fail", fail);
    RUVIA_HEAD("/health", health);
    RUVIA_OPTIONS("/health", options);
    RUVIA_GROUP_BEGIN("/admin", AdminAuthMiddleware)
    RUVIA_GET("/status", adminStatus);
    RUVIA_GROUP_END
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> hello(ruvia::Context& c) {
        co_return c.text("hello from ruvia\n");
    }

    ruvia::Task<ruvia::HttpResponse> user(ruvia::Context& c) {
        UserResponse response(c);
        response.id(c.req().param("id").value_or("unknown")).name("example-user").active(ruvia::Bool{true});
        co_return c.json(response);
    }

    ruvia::Task<ruvia::HttpResponse> wildcard(ruvia::Context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("wildcard=");
        body.append(c.req().param("*").value_or(""));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> inputs(ruvia::Context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("remote=");
        body.append(getConnInfo(c).remote().address());
        body.append("\nuser-agent=");
        body.append(c.req().header("User-Agent").value_or(""));
        body.append("\npage=");
        if (const auto page = parseUInt32(c.req().query("page"))) {
            char buffer[16]{};
            const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), *page);
            if (ec == std::errc{}) {
                body.append(buffer, static_cast<std::size_t>(ptr - buffer));
            }
        }
        body.append("\nsession=");
        body.append(c.req().cookie("session").value_or(""));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> echo(ruvia::Context& c) {
        const auto body = co_await c.req().text();
        std::pmr::string owned(c.allocator<char>());
        owned.assign(body.data(), body.size());
        c.status(ruvia::http_status::kCreated);
        c.header("X-Echo", "true");
        co_return c.text(std::move(owned));
    }

    ruvia::Task<ruvia::HttpResponse> redirect(ruvia::Context& c) {
        co_return c.redirect("/api/hello", ruvia::http_status::kFound);
    }

    // urlFor builds request paths from registered route patterns -- the
    // pattern is the route's identity, values are percent-encoded, and an
    // unregistered pattern throws at build time instead of emitting a dead
    // link. Works in handlers, middleware and fallback handlers alike.
    ruvia::Task<ruvia::HttpResponse> links(ruvia::Context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("user=");
        body.append(c.urlFor("/api/users/:id", {c.req().param("id").value_or("0")}));
        body.append("\nfile=");
        body.append(c.urlFor("/api/files/*", {"docs/guide.md"}));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> fail(ruvia::Context&) {
        throw ruvia::HttpError(ruvia::http_status::kBadRequest, "example_error", "the example handler threw an HttpError");
    }

    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& c) {
        co_return c.text("ok\n");
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        c.status(ruvia::http_status::kNoContent);
        c.header("Allow", "GET, HEAD, OPTIONS");
        co_return c.text("");
    }

    ruvia::Task<ruvia::HttpResponse> adminStatus(ruvia::Context& c) {
        co_return c.text("admin ok\n");
    }
};

int main() {
    ruvia::MemoryPoolConfig memory;
    memory.requestInitialBufferBytes = 4096;

    ruvia::app().setListenAddress("0.0.0.0").setServerTopology(ruvia::ServerTopology::http(8080)).setWorkersPerListener(2).setSignalShutdown(true).setKeepaliveTimeout(std::chrono::seconds(75)).setClientHeaderTimeout(std::chrono::seconds(60)).setClientBodyTimeout(std::chrono::seconds(60)).setSendTimeout(std::chrono::seconds(60)).setMaxConnectionsPerWorker(10000).setKeepaliveRequests(1000).setMemoryPoolConfig(memory).onError(&exampleErrorHandler).onError("/api", &apiError).notFound("/api", &apiNotFound).run();
}
