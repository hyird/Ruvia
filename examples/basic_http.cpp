#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"
#include "ruvia/http/Error.h"

class RequestIdMiddleware final : public ruvia::Middleware<RequestIdMiddleware> {
public:
    ruvia::Task<ruvia::HttpResponse> handle(ruvia::Context& c, const ruvia::Next& next) {
        auto response = co_await next(c);
        response.setHeader("X-Example", "basic-http");
        co_return response;
    }
};

class AdminAuthMiddleware final : public ruvia::Middleware<AdminAuthMiddleware> {
public:
    ruvia::Task<ruvia::HttpResponse> handle(ruvia::Context& c, const ruvia::Next& next) {
        if (c.header("X-Admin-Token") != "secret") {
            co_return c.error(401, "unauthorized", "missing admin token");
        }
        co_return co_await next(c);
    }
};

RUVIA_MODEL(UserResponse,
    RUVIA_FIELD(id, ruvia::String),
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(active, ruvia::Bool)
);

ruvia::Task<ruvia::HttpResponse> exampleErrorHandler(ruvia::Context& c, ruvia::HttpErrorInfo error) {
    return ruvia::makeErrorResponse(c, error, true, nullptr);
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
        response
            .id(c.param("id").toStringView().value_or("unknown"))
            .name("example-user")
            .active(ruvia::Bool{true});
        co_return c.json(response);
    }

    ruvia::Task<ruvia::HttpResponse> wildcard(ruvia::Context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("wildcard=");
        body.append(c.param("*").toStringView().value_or(""));
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> inputs(ruvia::Context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("remote=");
        body.append(c.remoteAddress());
        body.append("\nuser-agent=");
        body.append(c.header(ruvia::HttpRequest::KnownHeader::kUserAgent));
        body.append("\npage=");
        if (const auto page = c.query("page").toUInt32()) {
            char buffer[16]{};
            const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), *page);
            if (ec == std::errc{}) {
                body.append(buffer, static_cast<std::size_t>(ptr - buffer));
            }
        }
        body.append("\nsession=");
        body.append(c.cookie("session").value_or(""));
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> echo(ruvia::Context& c) {
        const auto body = co_await c.body();
        std::pmr::string owned(c.allocator<char>());
        owned.assign(body.data(), body.size());
        co_return c.status(201).setHeader("X-Echo", "true").text(owned);
    }

    ruvia::Task<ruvia::HttpResponse> redirect(ruvia::Context& c) {
        co_return c.redirect("/api/hello", 302);
    }

    ruvia::Task<ruvia::HttpResponse> fail(ruvia::Context&) {
        throw ruvia::HttpError(418, "teapot", "the example handler threw an HttpError");
    }

    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& c) {
        co_return c.text("ok\n");
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        co_return c.status(204).setHeader("Allow", "GET, HEAD, OPTIONS").text("");
    }

    ruvia::Task<ruvia::HttpResponse> adminStatus(ruvia::Context& c) {
        co_return c.text("admin ok\n");
    }
};

int main() {
    ruvia::MemoryPoolConfig memory;
    memory.requestInitialBufferBytes = 4096;

    ruvia::app()
        .setListenAddress("0.0.0.0", 8080)
        .setThreadNum(2)
        .setIdleTimeout(std::chrono::seconds(60))
        .setHeaderTimeout(std::chrono::seconds(15))
        .setBodyTimeout(std::chrono::seconds(30))
        .setWriteTimeout(std::chrono::seconds(30))
        .setMaxConnectionsPerWorker(10000)
        .setMaxRequestsPerConnection(1000)
        .setMemoryPoolConfig(memory)
        .setErrorHandler(&exampleErrorHandler)
        .run();
}
