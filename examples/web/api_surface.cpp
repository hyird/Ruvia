// A server exercising the HTTP context surface: route metadata and decoded
// paths, Accept checks, buffered multipart, explicit body discard, response
// cookies, manual http_response body ownership and PUT/PATCH streaming.
// Run ruvia_example_api_surface on port 8088; GET /surface/request?tag=demo.
// Use curl -i to inspect response slots, cookies, redirects and middleware.
// POST -F file=@file.bin to /surface/multipart for buffered upload parsing.

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

#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http1_request_parser.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_client_redirect.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/web/app.h"
#include "ruvia/web/auth/jwt.h"
#include "ruvia/web/conn_info.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/csrf.h"
#include "ruvia/web/db/db_migration.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_transaction.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/error.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/redis/redis_types.h"
#include "ruvia/web/security_headers.h"
#include "ruvia/web/session.h"
#include "ruvia/web/websocket.h"

RUVIA_MODEL(surface_json_message, RUVIA_OPTIONAL_FIELD(message, ruvia::string));

namespace {

void append_unsigned(std::pmr::string& output, std::uint64_t value) {
    char buffer[32]{};
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec == std::errc{}) {
        output.append(buffer, static_cast<std::size_t>(ptr - buffer));
    }
}

RUVIA_MODEL(surface_json_response, RUVIA_OPTIONAL_FIELD(message, ruvia::string));

}  // namespace

ruvia::task<ruvia::http_response> surface_not_found(ruvia::context& c) {
    c.status(ruvia::http_status::not_found);
    c.header("X-Surface-Not-Found", "true");
    co_return c.text("surface not found\n");
}

class surface_context_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        if (c.exception()) {
            const auto* downstream_response = c.response();
            const bool had_downstream_error_response = downstream_response != nullptr;
            const bool downstream_was_internal_error =
                downstream_response != nullptr &&
                downstream_response->status() == ruvia::http_status::internal_server_error;
            c.status(ruvia::http_status::internal_server_error);
            auto response = c.text("caught by middleware\n");
            response.header("X-Surface-Error", "true");
            response.header(
                "X-Surface-Error-Response", had_downstream_error_response ? "true" : "false");
            response.header("X-Surface-Error-Status",
                downstream_was_internal_error ? "internal-server-error" : "other");
            c.respond(std::move(response));
            co_return;
        }
        c.header("X-Surface-Finalized", c.response() != nullptr ? "true" : "false");
        c.header(
            "X-Surface-Middleware", "after-next", {.mode_ = ruvia::http_response_header_mode::append});
    }
};

class surface_return_middleware final : public ruvia::middleware {
public:
    ruvia::task<ruvia::http_response> handle(ruvia::context& c, ruvia::next&) {
        c.status(ruvia::http_status::accepted);
        co_return c.text("returned by middleware\n");
    }
};

class surface_pre_direct_response_middleware final
    : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        c.header("X-Surface-Pre-Direct", "true");
        co_await next_value();
    }
};

class surface_res_slot_only_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next&) {
        c.header("X-Surface-Res-Slot-Only", "true");
        c.respond(c.body(nullptr));
        co_return;
    }
};

class api_surface_controller final : public ruvia::controller<api_surface_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/surface", surface_context_middleware)

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/request", request_info);
    RUVIA_GET("/context", context_info);
    RUVIA_GET("/raw", raw_body);
    RUVIA_GET("/res", response_slot);
    RUVIA_GET("/res-slot-only", res_slot_only, surface_res_slot_only_middleware);
    RUVIA_GET("/html", html_body);
    RUVIA_GET("/json-response", json_response);
    RUVIA_GET("/null-body", null_body);
    RUVIA_GET("/binary-body", binary_body);
    RUVIA_GET("/header-remove", header_remove);
    RUVIA_GET("/redirect-unicode", redirect_unicode);
    RUVIA_GET("/redirect-prepared-location", redirect_prepared_location);
    RUVIA_GET("/error", app_error);
    RUVIA_GET("/throw", throw_error);
    RUVIA_GET_STREAM("/stream-throw", stream_throw);
    RUVIA_GET("/missing", missing);
    RUVIA_GET("/middleware-return", middleware_return_handler, surface_return_middleware);
    RUVIA_GET("/pre-direct-res", pre_direct_response, surface_pre_direct_response_middleware);
    RUVIA_GET("/res-direct-buffered", direct_buffered_response);
    RUVIA_GET("/res-remove-buffered", remove_buffered_response);
    RUVIA_GET("/res-assigned-prepared", assigned_prepared_response);
    RUVIA_POST("/multipart", buffered_multipart);
    RUVIA_POST("/bytes", bytes_body);
    RUVIA_POST("/blob", blob_body);
    RUVIA_POST("/json-object", json_message, ruvia::json_body<surface_json_message>);
    RUVIA_POST("/json-raw", json_message, ruvia::json_body<surface_json_message>);
    RUVIA_POST("/discard", discard);
    RUVIA_PUT("/items/:id", replace_item);
    RUVIA_PATCH("/items/:id", patch_item);
    RUVIA_DELETE("/items/:id", delete_item);
    RUVIA_GET("/cookies", cookies);
    RUVIA_GET("/signed-cookies", signed_cookies);
    RUVIA_ALL("/any", any_method);
    RUVIA_ON((::ruvia::http_known_method::put, ::ruvia::http_known_method::delete_value),
        ("/on-item/:id", "/on-legacy/:id"), on_item);
    RUVIA_GET("/manual/body", manual_body);
    RUVIA_PUT_STREAM("/upload/:id", stream_put);
    RUVIA_PATCH_STREAM("/upload/:id", stream_patch);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> request_info(ruvia::context& c) {
        const auto& request = c.req();
        std::pmr::string body(c.allocator<char>());
        body.append("method=");
        body.append(request.method());
        body.append("\npath=");
        body.append(request.path());
        body.append("\nroute-path=");
        body.append(request.route_path());
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
        if (auto surface_cookie = c.req().cookie("surface")) {
            body.append(*surface_cookie);
        }
        body.append("\nparam-id=");
        body.append(c.req().param("id").value_or(""));
        body.append("\ntag-values=");
        const auto tags = request.queries("tag");
        append_unsigned(body, tags.size());
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

    ruvia::task<ruvia::http_response> context_info(ruvia::context& c) {
        std::pmr::string body(c.allocator<char>());
        // Session capability requires session_middleware; see sessions.cpp for
        // its Redis setup. This standalone example only installs application services.
        body.append("env-vars=");
        append_unsigned(body, c.env().size());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> raw_body(ruvia::context& c) {
        c.status(ruvia::http_status::accepted);
        c.header("X-Raw", "first");
        c.header("X-Raw", "second", {.mode_ = ruvia::http_response_header_mode::append});
        c.header("X-Raw-Init", "true");
        co_return c.body("raw body\n");
    }

    ruvia::task<ruvia::http_response> response_slot(ruvia::context& c) {
        c.header("X-Response-Prepared", "true");
        ruvia::http_response response({.resource_ = c.arena()});
        response.status(ruvia::http_status::non_authoritative_information);
        response.header("X-Response-Remove", "drop");
        response.body("response slot\n");
        response.header(
            "X-Response-Slot", "true", {.mode_ = ruvia::http_response_header_mode::append});
        response.remove_header("X-Response-Remove");
        co_return response;
    }

    ruvia::task<ruvia::http_response> res_slot_only(ruvia::context& c) {
        c.status(ruvia::http_status::internal_server_error);
        co_return c.text("handler should not run\n");
    }

    ruvia::task<ruvia::http_response> html_body(ruvia::context& c) {
        co_return c.html("<strong>html body</strong>\n");
    }

    ruvia::task<ruvia::http_response> json_response(ruvia::context& c) {
        surface_json_response response({.resource_ = c.arena()});
        response.set<"message">("json response");
        co_return c.json(response);
    }

    ruvia::task<ruvia::http_response> null_body(ruvia::context& c) {
        c.status(ruvia::http_status::accepted);
        c.header("X-Null-Body", "true");
        co_return c.body(nullptr);
    }

    ruvia::task<ruvia::http_response> binary_body(ruvia::context& c) {
        static constexpr std::array<std::byte, 3> bytes_value{
            std::byte{0x00}, std::byte{0x41}, std::byte{0xff}};
        c.status(ruvia::http_status::partial_content);
        c.header("X-Binary-Body", "true");
        co_return c.body(std::span<const std::byte>(bytes_value));
    }

    ruvia::task<ruvia::http_response> header_remove(ruvia::context& c) {
        c.header("X-Remove-Me", "drop");
        c.header("X-Remove-Too", "drop");
        c.header("X-Keep-Me", "keep");
        c.remove_header("X-Remove-Me");
        c.remove_header("X-Remove-Too");
        co_return c.text("header remove\n");
    }

    ruvia::task<ruvia::http_response> redirect_unicode(ruvia::context& c) {
        co_return c.redirect({.location_ = "/目标?x=值", .status_ = ruvia::http_status::see_other});
    }

    ruvia::task<ruvia::http_response> redirect_prepared_location(ruvia::context& c) {
        c.header("Location", "/surface/wrong");
        co_return c.redirect({.location_ = "/surface/right"});
    }

    ruvia::task<ruvia::http_response> app_error(ruvia::context& c) {
        c.header("X-Error-Prepared", "true");
        co_return c.error({.status_ = ruvia::http_status::bad_request,
            .code_ = "example_error",
            .message_ = "the example request was rejected"});
    }

    ruvia::task<ruvia::http_response> throw_error(ruvia::context&) {
        throw std::runtime_error("surface route failed");
    }

    ruvia::task<void> stream_throw(ruvia::context&) {
        throw std::runtime_error("surface stream failed");
    }

    ruvia::task<ruvia::http_response> missing(ruvia::context& c) {
        c.header("X-Not-Found-Prepared", "true");
        co_return co_await c.not_found();
    }

    ruvia::task<ruvia::http_response> middleware_return_handler(ruvia::context& c) {
        c.status(ruvia::http_status::internal_server_error);
        co_return c.text("handler should not run\n");
    }

    ruvia::task<ruvia::http_response> pre_direct_response(ruvia::context& c) {
        co_return c.text("pre direct response\n");
    }

    ruvia::task<ruvia::http_response> direct_buffered_response(ruvia::context& c) {
        c.header("X-Direct-Buffered", "true");
        co_return c.body("direct buffered response\n");
    }

    ruvia::task<ruvia::http_response> remove_buffered_response(ruvia::context& c) {
        c.header("X-Remove-Buffered", "drop");
        c.remove_header("X-Remove-Buffered");
        co_return c.body("removed buffered response\n");
    }

    ruvia::task<ruvia::http_response> assigned_prepared_response(ruvia::context& c) {
        c.header("X-Surface-Prepared-Assigned", "true");
        ruvia::http_response response({.resource_ = c.arena()});
        response.header("Content-Type", "text/plain; charset=UTF-8");
        response.body("assigned prepared response\n");
        co_return response;
    }

    ruvia::task<ruvia::http_response> buffered_multipart(ruvia::context& c) {
        auto parts = co_await c.req().multipart();
        std::pmr::string body(c.allocator<char>());
        body.append("parts=");
        append_unsigned(body, parts.size());
        for (const auto& part : parts) {
            body.append("\nname=");
            body.append(part.name());
            body.append(";filename=");
            body.append(part.filename());
            body.append(";content-type=");
            body.append(part.content_type());
            body.append(";bytes=");
            append_unsigned(body, part.body().size());
        }
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> bytes_body(ruvia::context& c) {
        const auto bytes_value = co_await c.req().bytes();
        std::pmr::string body(c.allocator<char>());
        body.append("bytes bytes=");
        append_unsigned(body, bytes_value.size());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> blob_body(ruvia::context& c) {
        const auto blob = co_await c.req().blob();
        const auto bytes_value = blob.bytes();
        const auto text = blob.text();
        std::pmr::string body(c.allocator<char>());
        body.append("blob bytes=");
        append_unsigned(body, blob.size());
        body.append("\nbytes=");
        append_unsigned(body, bytes_value.size());
        body.append("\ntext bytes=");
        append_unsigned(body, text.size());
        body.append("\ncontent-type=");
        body.append(blob.content_type());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> json_message(ruvia::context& c) {
        const auto json = c.req().validated_json<surface_json_message>();
        std::pmr::string body(c.allocator<char>());
        body.append("json-value bytes=");
        append_unsigned(body, json.raw().size());
        if (const auto& message = json.value().get<"message">()) {
            body.append("\nmessage=");
            body.append(message->view());
        }
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> discard(ruvia::context& c) {
        co_await c.req().discard_body();
        c.status(ruvia::http_status::no_content);
        co_return c.text("");
    }

    ruvia::task<ruvia::http_response> replace_item(ruvia::context& c) {
        const auto body = co_await c.req().text();
        std::pmr::string output(c.allocator<char>());
        output.append("replace id=");
        output.append(c.req().param("id").value_or(""));
        output.append(" bytes=");
        append_unsigned(output, body.size());
        output.push_back('\n');
        co_return c.text(std::move(output));
    }

    ruvia::task<ruvia::http_response> patch_item(ruvia::context& c) {
        const auto body = co_await c.req().text();
        std::pmr::string output(c.allocator<char>());
        output.append("patch id=");
        output.append(c.req().param("id").value_or(""));
        output.append(" bytes=");
        append_unsigned(output, body.size());
        output.push_back('\n');
        co_return c.text(std::move(output));
    }

    ruvia::task<ruvia::http_response> delete_item(ruvia::context& c) {
        std::pmr::string output(c.allocator<char>());
        output.append("deleted id=");
        output.append(c.req().param("id").value_or(""));
        output.push_back('\n');
        co_return c.text(std::move(output));
    }

    ruvia::task<ruvia::http_response> cookies(ruvia::context& c) {
        c.set_cookie({
            .name_ = "session",
            .value_ = "example",
            .attributes_ =
                ruvia::cookie_options{
                    .same_site_ = ruvia::cookie_same_site::lax,
                    .max_age_ = std::chrono::seconds(3600),
                    .http_only_ = ruvia::cookie_attribute_policy::emit,
                },
        });
        c.set_cookie({.name_ = "theme", .value_ = "light"});
        c.set_cookie({
            .name_ = "chip",
            .value_ = "value",
            .attributes_ =
                ruvia::cookie_options{
                    .same_site_ = ruvia::cookie_same_site::none,
                    .priority_ = ruvia::cookie_priority::high,
                    .expires_ = std::chrono::system_clock::now() + std::chrono::hours(1),
                    .prefix_ = ruvia::cookie_prefix::host,
                    .http_only_ = ruvia::cookie_attribute_policy::emit,
                    .secure_ = ruvia::cookie_attribute_policy::emit,
                    .partitioned_ = ruvia::cookie_attribute_policy::emit,
                },
        });
        const auto deleted = c.req().cookie("legacy-session");
        c.delete_cookie({.name_ = "legacy-session"});
        std::pmr::string body(c.allocator<char>());
        body.append("cookies set\nlegacy-session=");
        body.append(deleted.value_or(""));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> any_method(ruvia::context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("all method=");
        body.append(c.req().method());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> on_item(ruvia::context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("on method=");
        body.append(c.req().method());
        body.append(" id=");
        body.append(c.req().param("id").value_or(""));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> signed_cookies(ruvia::context& c) {
        static constexpr std::string_view secret = "surface-signing-secret";
        c.set_signed_cookie({.name_ = "signed-session", .value_ = "signed-value", .secret_ = secret});
        const auto verified = c.req().signed_cookie({.name_ = "signed-session", .secret_ = secret});
        const auto absent = c.req().signed_cookie({.name_ = "absent", .secret_ = secret});
        std::pmr::string body(c.allocator<char>());
        body.append("signed=");
        body.append(verified.value_or("missing"));
        body.append("\nabsent=");
        body.append(absent.has_value() ? "present" : "missing");
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> manual_body(ruvia::context& c) {
        ruvia::http_response response({.resource_ = c.arena()});
        response.status(ruvia::http_status::accepted);
        response.header("Content-Type", "text/plain; charset=UTF-8");
        response.header("X-Manual-Body", "owned");
        response.body("copied body\n");
        co_return response;
    }

    ruvia::task<ruvia::http_response> stream_put(ruvia::context& c) {
        co_return co_await count_streaming_body(c, "put");
    }

    ruvia::task<ruvia::http_response> stream_patch(ruvia::context& c) {
        co_return co_await count_streaming_body(c, "patch");
    }

    static ruvia::task<ruvia::http_response> count_streaming_body(
        ruvia::context& c, std::string_view verb) {
        std::uint64_t bytes_value = 0;
        auto& reader_value = c.req().get_body_reader();
        while (auto chunk = co_await reader_value.read()) {
            bytes_value += chunk->size();
        }

        std::pmr::string body(c.allocator<char>());
        body.append(verb);
        body.append(" stream id=");
        body.append(c.req().param("id").value_or(""));
        body.append(" bytes=");
        append_unsigned(body, bytes_value);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }
};

int main() {
    ruvia::app()
        .listen({.address_ = "0.0.0.0", .http_ = 8088})
        .server({.worker_count_ = 2,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
        .on_not_found(&surface_not_found)
        .run();
}
