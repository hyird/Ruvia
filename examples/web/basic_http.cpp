// Basic HTTP server: controller/group macros, middleware, path params,
// wildcard routes, query/header/cookie helpers, body reads, url_for links,
// text/JSON/redirect/error responses including HEAD and OPTIONS, and
// prefix-scoped not_found/on_error fallbacks layered under the app-wide one.
// Run ruvia_example_basic_http; curl http://127.0.0.1:8080/api/hello.
// Try /api/users/42, /api/files/a/b, and /api/inputs?name=Ada.
// Use Ctrl+C to stop servers built with process signal handlers installed.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <variant>

#include "ruvia/core/integer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"

class request_id_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Example", "basic-http");
    }
};

class admin_auth_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        if (c.req().header("X-Admin-Token").value_or("") != "secret") {
            c.respond(c.error({.status_ = ruvia::http_status::unauthorized,
                .code_ = "unauthorized",
                .message_ = "missing admin token"}));
            co_return;
        }
        co_await next_value();
    }
};

RUVIA_MODEL(user_response, RUVIA_OPTIONAL_FIELD(id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string), RUVIA_OPTIONAL_FIELD(active, ruvia::bool_value));

ruvia::task<ruvia::http_response> example_error_handler(
    ruvia::context& c, ruvia::http_error_info error) {
    co_return c.error({.status_ = error.status(),
        .code_ = error.code(),
        .message_ = error.message(),
        .status_text_ = error.status_text()});
}

// Prefix-scoped fallbacks: the longest matching registered prefix wins, on
// whole path segments ("/api" scopes "/api/x" but never "/apix"); requests
// outside every prefix keep using the app-wide handlers above.
ruvia::task<ruvia::http_response> api_not_found(ruvia::context& c) {
    c.status(ruvia::http_status::not_found);
    co_return c.error({.status_ = ruvia::http_status::not_found,
        .code_ = "api_not_found",
        .message_ = "no such API endpoint"});
}

ruvia::task<ruvia::http_response> api_error(ruvia::context& c, ruvia::http_error_info error) {
    c.header("X-Api-Error", "true");
    co_return c.error({.status_ = error.status(),
        .code_ = error.code(),
        .message_ = error.message(),
        .status_text_ = error.status_text()});
}

class basic_http_controller final : public ruvia::controller<basic_http_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/api", request_id_middleware)

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
    RUVIA_GROUP_BEGIN("/admin", admin_auth_middleware)
    RUVIA_GET("/status", admin_status);
    RUVIA_GROUP_END
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> hello(ruvia::context& c) {
        co_return c.text("hello from ruvia\n");
    }

    ruvia::task<ruvia::http_response> user(ruvia::context& c) {
        user_response response({.resource_ = c.arena()});
        response.set<"id">(c.req().param("id").value_or("unknown"))
            .set<"name">("example-user")
            .set<"active">(ruvia::bool_value{true});
        co_return c.json(response);
    }

    ruvia::task<ruvia::http_response> wildcard(ruvia::context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("wildcard=");
        body.append(c.req().param("*").value_or(""));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> inputs(ruvia::context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("remote=");
        body.append(c.conn().remote().address());
        body.append("\nuser-agent=");
        body.append(c.req().header("User-Agent").value_or(""));
        body.append("\npage=");
        std::uint32_t page = 1;
        if (auto raw = c.req().query("page")) {
            auto parsed_value = ruvia::parse_integer<std::uint32_t>(*raw);
            if (parsed_value.index() != 0) {
                co_return c.error({.status_ = ruvia::http_status::bad_request,
                    .code_ = "invalid_page",
                    .message_ = "page must be a uint32 decimal integer"});
            }
            page = std::max(std::uint32_t{1}, std::get<0>(parsed_value));
        }
        char buffer[16]{};
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), page);
        if (ec == std::errc{}) {
            body.append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
        body.append("\nsession=");
        body.append(c.req().cookie("session").value_or(""));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        const auto body = co_await c.req().text();
        std::pmr::string owned(c.allocator<char>());
        owned.assign(body.data(), body.size());
        c.status(ruvia::http_status::created);
        c.header("X-Echo", "true");
        co_return c.text(std::move(owned));
    }

    ruvia::task<ruvia::http_response> redirect(ruvia::context& c) {
        co_return c.redirect({.location_ = "/api/hello"});
    }

    // url_for builds request paths from registered route patterns -- the
    // pattern is the route's identity, values are percent-encoded, and an
    // unregistered pattern throws at build time instead of emitting a dead
    // link. Works in handlers, middleware and fallback handlers alike.
    ruvia::task<ruvia::http_response> links(ruvia::context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("user=");
        body.append(c.url_for("/api/users/:id", {c.req().param("id").value_or("0")}));
        body.append("\nfile=");
        body.append(c.url_for("/api/files/*", {"docs/guide.md"}));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> fail(ruvia::context&) {
        throw ruvia::http_error({.status_ = ruvia::http_status::bad_request,
            .code_ = "example_error",
            .message_ = "the example handler threw an HttpError"});
    }

    ruvia::task<ruvia::http_response> health(ruvia::context& c) {
        co_return c.text("ok\n");
    }

    ruvia::task<ruvia::http_response> options(ruvia::context& c) {
        c.status(ruvia::http_status::no_content);
        c.header("Allow", "GET, HEAD, OPTIONS");
        co_return c.text("");
    }

    ruvia::task<ruvia::http_response> admin_status(ruvia::context& c) {
        co_return c.text("admin ok\n");
    }
};

int main() {
    ruvia::memory_pool_config memory;
    memory.request_initial_buffer_bytes_ = 4096;

    ruvia::app()
        .listen({.address_ = "0.0.0.0", .http_ = 8080})
        .server({
            .worker_count_ = 2,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
            .idle_timeout_ = std::chrono::seconds(75),
            .request_header_timeout_ = std::chrono::seconds(60),
            .request_body_timeout_ = std::chrono::seconds(60),
            .write_timeout_ = std::chrono::seconds(60),
            .max_connections_per_worker_ = 10000,
            .max_requests_per_connection_ = 1000,
            .memory_pool_ = memory,
        })
        .on_error(&example_error_handler)
        .on_error({.prefix_ = "/api", .handler_ = &api_error})
        .on_not_found({.prefix_ = "/api", .handler_ = &api_not_found})
        .run();
}
