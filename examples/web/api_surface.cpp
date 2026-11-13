// A server exercising the HTTP context surface: route metadata and decoded
// paths, Accept checks, buffered multipart, explicit body discard, response
// cookies, manual http_response body ownership and PUT/PATCH streaming.
// Run ruvia_example_api_surface on port 8088; GET /surface/request?tag=demo.
// Use curl -i to inspect response slots, cookies, redirects and middleware.
// POST -F file=@file.bin to /surface/multipart for buffered upload parsing.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"

#include "backend_config.h"
RUVIA_MODEL(surface_json_message, RUVIA_OPTIONAL_FIELD(message, ruvia::string));

ruvia::task<ruvia::http_response> surface_not_found(ruvia::context& c) {
    c.draft().status(ruvia::http_status::not_found);
    c.draft().header("X-Surface-Not-Found", "true");
    co_return c.text("surface not found\n");
}

class surface_context_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        if (c.committed()) {
            co_return;
        }
        if (c.exception()) {
            const auto* downstream_response = c.response();
            const bool had_downstream_error_response = downstream_response != nullptr;
            const bool downstream_was_internal_error =
                downstream_response != nullptr &&
                downstream_response->status() == ruvia::http_status::internal_server_error;
            // The framework already mapped the failure to its status (400 for
            // malformed JSON, 415 for an unsupported content coding, a handler's
            // http_error); keep it. Without a mapped response it is a server fault.
            // 替换从独立草稿构造；仅显式 policy 字段跨替换传播。
            c.draft().status(downstream_response != nullptr ? downstream_response->status()
                                                            : ruvia::http_status::internal_server_error);
            c.draft().header("X-Surface-Error", "true");
            c.draft().header("X-Surface-Error-Response", had_downstream_error_response ? "true" : "false");
            c.draft().header("X-Surface-Error-Status", downstream_was_internal_error ? "internal-server-error" : "other");
            c.respond(c.text("caught by middleware\n"));
            co_return;
        }
        c.edit_response().header("X-Surface-Finalized", c.response() != nullptr ? "true" : "false");
        c.edit_response().header(
            "X-Surface-Middleware", "after-next", {.mode_ = ruvia::http_response_header_mode::append});
    }
};

class surface_return_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next&) {
        c.draft().status(ruvia::http_status::accepted);
        c.respond(c.text("returned by middleware\n"));
        co_return;
    }
};

class surface_pre_direct_response_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        c.policy().header("X-Surface-Pre-Direct", "true");
        co_await next_value();
    }
};

class surface_res_slot_only_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next&) {
        c.draft().header("X-Surface-Res-Slot-Only", "true");
        c.respond(c.body());
        co_return;
    }
};

class api_surface_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/surface", surface_context_middleware)

    RUVIA_ROUTES_BEGIN(api_surface_controller)
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
    RUVIA_GET("/stream-throw", stream_throw);
    RUVIA_GET("/missing", missing);
    RUVIA_GET("/middleware-return", middleware_return_handler, surface_return_middleware);
    RUVIA_GET("/pre-direct-res", pre_direct_response, surface_pre_direct_response_middleware);
    RUVIA_GET("/res-direct-buffered", direct_buffered_response);
    RUVIA_GET("/res-remove-buffered", remove_buffered_response);
    RUVIA_GET("/res-assigned-prepared", assigned_prepared_response);
    RUVIA_POST("/multipart", buffered_multipart);
    RUVIA_POST("/bytes", bytes_body);
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
    RUVIA_PUT("/upload/:id", stream_put, ruvia::stream_body);
    RUVIA_PATCH("/upload/:id", stream_patch, ruvia::stream_body);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> request_info(ruvia::context& c) {
        const auto& request = c.req();
        std::string body;
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
        ruvia::http_append_unsigned(body, tags.size());
        body.append("\ntag-first=");
        if (!tags.empty()) {
            body.append(tags.front());
        }
        body.append("\ntag-missing=");
        body.append(request.queries("missing").empty() ? "missing" : "present");
        body.append("\naccepts-json=");
        const auto accepts_json =
            request.negotiate(ruvia::context_request::negotiable_field::media_type, {"application/json"});
        body.append(accepts_json.has_value() ? "true" : "false");
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> context_info(ruvia::context& c) {
        std::string body;
        // Session capability requires session_middleware; see redis.cpp for
        // its Redis setup. This standalone example only installs application services.
        body.append("env-vars=");
        ruvia::http_append_unsigned(body, c.env().size());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> raw_body(ruvia::context& c) {
        c.draft().status(ruvia::http_status::accepted);
        c.draft().header("X-Raw", "first");
        c.draft().header("X-Raw", "second", {.mode_ = ruvia::http_response_header_mode::append});
        c.draft().header("X-Raw-Init", "true");
        co_return c.body("raw body\n");
    }

    ruvia::task<ruvia::http_response> response_slot(ruvia::context& c) {
        c.policy().header("X-Response-Prepared", "true");
        ruvia::http_response response;
        response.status(ruvia::http_status::non_authoritative_information);
        response.header("X-Response-Remove", "drop");
        response.body("response slot\n");
        response.header(
            "X-Response-Slot", "true", {.mode_ = ruvia::http_response_header_mode::append});
        response.remove_header("X-Response-Remove");
        co_return response;
    }

    ruvia::task<ruvia::http_response> res_slot_only(ruvia::context& c) {
        c.draft().status(ruvia::http_status::internal_server_error);
        co_return c.text("handler should not run\n");
    }

    ruvia::task<ruvia::http_response> html_body(ruvia::context& c) {
        co_return c.html("<strong>html body</strong>\n");
    }

    ruvia::task<ruvia::http_response> json_response(ruvia::context& c) {
        surface_json_message response;
        response.set<"message">("json response");
        co_return c.json(response);
    }

    ruvia::task<ruvia::http_response> null_body(ruvia::context& c) {
        c.draft().status(ruvia::http_status::accepted);
        c.draft().header("X-Null-Body", "true");
        co_return c.body();
    }

    ruvia::task<ruvia::http_response> binary_body(ruvia::context& c) {
        static constexpr std::array<std::byte, 3> bytes_value{
            std::byte{0x00}, std::byte{0x41}, std::byte{0xff}};
        c.draft().header("X-Binary-Body", "true");
        co_return c.body(std::span<const std::byte>(bytes_value));
    }

    ruvia::task<ruvia::http_response> header_remove(ruvia::context& c) {
        c.draft().header("X-Remove-Me", "drop");
        c.draft().header("X-Remove-Too", "drop");
        c.draft().header("X-Keep-Me", "keep");
        c.draft().remove_header("X-Remove-Me");
        c.draft().remove_header("X-Remove-Too");
        co_return c.text("header remove\n");
    }

    ruvia::task<ruvia::http_response> redirect_unicode(ruvia::context& c) {
        co_return c.redirect({.location_ = "/目标?x=值", .status_ = ruvia::http_status::see_other});
    }

    ruvia::task<ruvia::http_response> redirect_prepared_location(ruvia::context& c) {
        c.draft().header("Location", "/surface/wrong");
        co_return c.redirect({.location_ = "/surface/right"});
    }

    ruvia::task<ruvia::http_response> app_error(ruvia::context& c) {
        c.policy().header("X-Error-Prepared", "true");
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
        c.policy().header("X-Not-Found-Prepared", "true");
        co_return co_await c.not_found();
    }

    ruvia::task<ruvia::http_response> middleware_return_handler(ruvia::context& c) {
        c.draft().status(ruvia::http_status::internal_server_error);
        co_return c.text("handler should not run\n");
    }

    ruvia::task<ruvia::http_response> pre_direct_response(ruvia::context& c) {
        co_return c.text("pre direct response\n");
    }

    ruvia::task<ruvia::http_response> direct_buffered_response(ruvia::context& c) {
        c.draft().header("X-Direct-Buffered", "true");
        co_return c.body("direct buffered response\n");
    }

    ruvia::task<ruvia::http_response> remove_buffered_response(ruvia::context& c) {
        c.draft().header("X-Remove-Buffered", "drop");
        c.draft().remove_header("X-Remove-Buffered");
        co_return c.body("removed buffered response\n");
    }

    ruvia::task<ruvia::http_response> assigned_prepared_response(ruvia::context& c) {
        c.policy().header("X-Surface-Prepared-Assigned", "true");
        ruvia::http_response response;
        response.header("Content-Type", "text/plain; charset=UTF-8");
        response.body("assigned prepared response\n");
        co_return response;
    }

    ruvia::task<ruvia::http_response> buffered_multipart(ruvia::context& c) {
        auto parts = co_await c.req().multipart();
        std::string body;
        body.append("parts=");
        ruvia::http_append_unsigned(body, parts.size());
        for (const auto& part : parts) {
            body.append("\nname=");
            body.append(part.name());
            body.append(";filename=");
            body.append(part.filename());
            body.append(";content-type=");
            body.append(part.content_type());
            body.append(";bytes=");
            ruvia::http_append_unsigned(body, part.body().size());
        }
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> bytes_body(ruvia::context& c) {
        const auto bytes_value = co_await c.req().bytes();
        std::string body;
        body.append("bytes bytes=");
        ruvia::http_append_unsigned(body, bytes_value.size());
        body.append("\ncontent-type=");
        body.append(c.req().header("Content-Type").value_or(std::string_view{}));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> json_message(ruvia::context& c) {
        const auto& json = c.req().validated<surface_json_message>();
        const auto raw = co_await c.req().text();
        std::string body;
        body.append("json-value bytes=");
        ruvia::http_append_unsigned(body, raw.size());
        if (const auto& message = json.get<"message">()) {
            body.append("\nmessage=");
            body.append(message->view());
        }
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> discard(ruvia::context& c) {
        co_await c.req().discard_body();
        c.draft().status(ruvia::http_status::no_content);
        co_return c.text("");
    }

    ruvia::task<ruvia::http_response> replace_item(ruvia::context& c) {
        const auto body = co_await c.req().text();
        std::string output;
        output.append("replace id=");
        output.append(c.req().param("id").value_or(""));
        output.append(" bytes=");
        ruvia::http_append_unsigned(output, body.size());
        output.push_back('\n');
        co_return c.text(std::move(output));
    }

    ruvia::task<ruvia::http_response> patch_item(ruvia::context& c) {
        const auto body = co_await c.req().text();
        std::string output;
        output.append("patch id=");
        output.append(c.req().param("id").value_or(""));
        output.append(" bytes=");
        ruvia::http_append_unsigned(output, body.size());
        output.push_back('\n');
        co_return c.text(std::move(output));
    }

    ruvia::task<ruvia::http_response> delete_item(ruvia::context& c) {
        std::string output;
        output.append("deleted id=");
        output.append(c.req().param("id").value_or(""));
        output.push_back('\n');
        co_return c.text(std::move(output));
    }

    ruvia::task<ruvia::http_response> cookies(ruvia::context& c) {
        c.draft().set_cookie({
            .name_ = "session",
            .value_ = "example",
            .attributes_ =
                ruvia::cookie_options{
                    .same_site_ = ruvia::cookie_same_site::lax,
                    .max_age_ = std::chrono::seconds(3600),
                    .http_only_ = ruvia::cookie_attribute_policy::emit,
                },
        });
        c.draft().set_cookie({.name_ = "theme", .value_ = "light"});
        c.draft().set_cookie({
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
        c.draft().delete_cookie({.name_ = "legacy-session"});
        std::string body;
        body.append("cookies set\nlegacy-session=");
        body.append(deleted.value_or(""));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> any_method(ruvia::context& c) {
        std::string body;
        body.append("all method=");
        body.append(c.req().method());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> on_item(ruvia::context& c) {
        std::string body;
        body.append("on method=");
        body.append(c.req().method());
        body.append(" id=");
        body.append(c.req().param("id").value_or(""));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> signed_cookies(ruvia::context& c) {
        static constexpr std::string_view secret = "surface-signing-secret";
        c.draft().set_signed_cookie({.name_ = "signed-session", .value_ = "signed-value", .secret_ = secret});
        const auto verified = c.req().signed_cookie({.name_ = "signed-session", .secret_ = secret});
        const auto absent = c.req().signed_cookie({.name_ = "absent", .secret_ = secret});
        std::string body;
        body.append("signed=");
        body.append(verified.value_or("missing"));
        body.append("\nabsent=");
        body.append(absent.has_value() ? "present" : "missing");
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> manual_body(ruvia::context& c) {
        ruvia::http_response response;
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
        auto& reader_value = c.req().body_reader();
        while (auto chunk = co_await reader_value.read()) {
            bytes_value += chunk->size();
        }

        std::string body;
        body.append(verb);
        body.append(" stream id=");
        body.append(c.req().param("id").value_or(""));
        body.append(" bytes=");
        ruvia::http_append_unsigned(body, bytes_value);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }
};

int main() {
    try {
        ruvia::app()
            .listen({.address_ = "0.0.0.0", .http_ = 8088})
            .server({.worker_count_ = 2,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .on_not_found(&surface_not_found)
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}

// Request policies and local dispatch; no external services are required.
// Build: cmake --build build --config Release --target ruvia_example_api_surface -j$(nproc)
// Run the executable, then try:
//   curl -i -d hello http://127.0.0.1:8097/policies/small
//   curl -i -H "X-Demo-Name: reader" http://127.0.0.1:8097/policies/dispatch
// Expected: an echo and "reader (subrequest)" respectively.
// GET /policies/csrf issues XSRF-TOKEN. For POST, send that cookie AND copy its
// value into X-XSRF-TOKEN; a missing or mismatched value is rejected with 403.

namespace request_policies_example {

struct request_label final {
    std::string_view name_;
};

class label_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        // The binding borrows this local value until next() finishes. Neither
        // the label nor its request-backed string_view may escape the request.
        const request_label label{c.req().header("X-Demo-Name").value_or("guest")};
        auto binding = c.bind_request_state(label);
        co_await next_value();
    }
};

class policy_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/policies", label_middleware)
    RUVIA_ROUTES_BEGIN(policy_controller)
    RUVIA_POST("/small", small, ruvia::body_limit<16>);
    RUVIA_GET("/label", label);
    RUVIA_GET("/dispatch", dispatch);
    RUVIA_GET("/csrf", csrf, ruvia::csrf_protection);
    RUVIA_POST("/csrf", csrf, ruvia::csrf_protection);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> small(ruvia::context& c) {
        // body_limit is checked before buffering. More than 16 bytes produces
        // 413; a route may tighten, but cannot enlarge, the server-wide limit.
        const auto body = co_await c.req().text();
        co_return c.text(body);
    }

    ruvia::task<ruvia::http_response> label(ruvia::context& c) {
        const auto& value = c.request_state<request_label>();
        std::string text(value.name_);
        text.append(c.is_subrequest() ? " (subrequest)\n" : " (network request)\n");
        co_return c.text(std::move(text));
    }

    ruvia::task<ruvia::http_response> dispatch(ruvia::context& c) {
        // Re-enter the production router on this worker. Child requests have
        // fresh state, so credentials and other headers must be passed explicitly.
        // Only buffered requests/responses are supported; streams, files, SSE,
        // and upgrades belong on their own network routes. Nesting is bounded.
        const std::array<ruvia::http_header_view, 1> headers{{
            {"X-Demo-Name", c.request_state<request_label>().name_},
        }};
        auto response = co_await c.dispatch({
            .method_ = "GET",
            .target_ = "/policies/label",
            .headers_ = headers,
        });
        c.draft().status(response.status());
        // dispatch_response owns pooled bytes; text() copies them into this
        // response before the child result is destroyed on the same worker.
        co_return c.text(response.body());
    }

    ruvia::task<ruvia::http_response> csrf(ruvia::context& c) {
        co_return c.text("CSRF check passed\n");
    }
};

}  // namespace request_policies_example

[[maybe_unused]] int run_request_policies_main() {
    using namespace request_policies_example;
    try {
        ruvia::app()
            .server({.process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
                .max_buffered_body_bytes_ = 1024})
            .listen({.address_ = "127.0.0.1", .http_ = 8097})
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// Middleware continuation, response decoration, and early responses.
// Run ruvia_example_api_surface on port 8089.
// curl -i http://127.0.0.1:8089/middleware-next
// /middleware-next/protected returns 401 unless X-Demo-Key: example is present.

class decorate_response final {
public:
    ruvia::task<void> invoke(ruvia::context& context_value, ruvia::next& next_value) {
        context_value.draft().header("X-Before", "set");
        // next() is a single-shot continuation. Await it exactly once; do not
        // store it, detach it, or attempt to restart the downstream handler.
        co_await next_value();
        context_value.edit_response().header("X-After", "set");
    }
};

class require_demo_key final {
public:
    ruvia::task<void> invoke(ruvia::context& context_value, ruvia::next& next_value) {
        if (context_value.req().header("X-Demo-Key") != "example") {
            // An early response skips this branch. Outer middleware still
            // resumes after its own awaited next(). A 401 must carry a
            // challenge (RFC 9110 15.5.2); this demo's credential is the
            // X-Demo-Key header, so it names its own scheme rather than
            // Bearer, which would tell clients to send Authorization.
            context_value.draft().status(ruvia::http_status::unauthorized);
            context_value.draft().header("WWW-Authenticate", R"(DemoKey realm="middleware-next")");
            context_value.respond(context_value.text("demo key required\n"));
            co_return;
        }
        co_await next_value();
    }
};

class middleware_next_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/middleware-next", decorate_response)

    RUVIA_ROUTES_BEGIN(middleware_next_controller)
    RUVIA_GET("/", ok);
    RUVIA_GET("/protected", ok, require_demo_key);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> ok(ruvia::context& context) {
        co_return context.text("ok\n");
    }
};

[[maybe_unused]] int run_middleware_next_main() {
    try {
        ruvia::app()
            .listen({.address_ = "0.0.0.0", .http_ = 8089})
            .server({.worker_count_ = 1,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// Static files: c.file(...), c.static_file(...), static_root, a document root,
// response-validator/range-request policies and gzip configuration.
//
// A standalone static_root is immutable. application document roots refresh every
// second; see the commented interval configuration below.
// Run ruvia_example_api_surface, then GET http://127.0.0.1:8083/index.html
// or /files/assets/hello.txt. /files/download describes a file-backed response.
// Try Range: bytes=0-4 (206), If-None-Match with the returned ETag (304), and
// Accept-Encoding: gzip. Compression depends on type, size, and negotiation.
// The document root refreshes; the explicitly constructed static_root is a
// snapshot. Keep it alive until application::run() returns and all file responses retire.

namespace files_static_example {

std::unique_ptr<ruvia::static_root> g_assets;

std::filesystem::path examples_root() {
    return std::filesystem::path(RUVIA_EXAMPLES_SOURCE_DIR);
}

// c.file() copies its path before returning its task; still build a fixed path
// once, not per request. UTF-8 text such as a std::string or a literal works too.
const std::filesystem::path& download_path() {
    static const std::filesystem::path path = examples_root() / "public" / "hello.txt";
    return path;
}

class files_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/files")

    RUVIA_ROUTES_BEGIN(files_controller)
    RUVIA_GET("/download", download);
    RUVIA_GET("/assets/*", asset);
    RUVIA_ROUTES_END

private:
    // file() and static_file() open the file on the blocking pool and
    // complete with the settled response (status, validators, framing).
    ruvia::task<ruvia::http_response> download(ruvia::context& c) {
        co_return co_await c.file({.path_ = download_path(), .content_type_ = "text/plain; charset=utf-8"});
    }

    ruvia::task<ruvia::http_response> asset(ruvia::context& c) {
        co_return co_await c.static_file(
            *g_assets, {.relative_path_ = c.req().param("*").value_or("index.html")});
    }
};

}  // namespace files_static_example

[[maybe_unused]] int run_files_static_main() {
    using namespace files_static_example;
    try {
        g_assets = std::make_unique<ruvia::static_root>(examples_root() / "public",
            ruvia::static_root_options{
                .cache_control_ = "public, max-age=3600",
                .index_file_ = "index.html",
                .mime_types_ = {{"txt", "text/plain; charset=utf-8"}},
                .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::only,
                    .extensions_ = {"html", "txt"}},
                .range_requests_ = ruvia::static_range_request_policy::honor,
                .response_validators_ = ruvia::static_response_validator_policy::emit,
                .dotfiles_ = ruvia::static_dotfile_policy::deny,
            });

        auto document_root = ruvia::document_root_config{
            .root_ = examples_root() / "public",
            .static_options_ =
                {
                    .cache_control_ = "public, max-age=3600",
                    .index_file_ = "index.html",
                },
            .precompress_gzip_ = true,
        };
        // Document roots refresh every second by default. To tune the interval:
        // .refresh_interval_ = std::chrono::milliseconds(500),
        // .precompress_gzip_ refresh-builds gzip variants for changed text assets.
        // Configure resident files through .static_options_.resident_max_bytes_
        // and .static_options_.resident_budget_bytes_; zero disables them.
        // The application blocking pool is enabled by default; file opens and
        // reads run on it.

        ruvia::app()
            .listen({.address_ = "0.0.0.0", .http_ = 8083})
            .server({.worker_count_ = 2,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
                .compression_ = ruvia::compression_config{}})
            .document_root(std::move(document_root))
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// In-memory application testing: test_app/test_request/dispatch_response drive the
// production dispatch pipeline -- routing, params, model bodies, middleware,
// fallbacks, url_for and worker state -- without opening a socket. This is the
// pattern an application's own test suite uses; the example doubles as a
// runnable check and exits non-zero on any mismatch.
// Run ruvia_example_api_surface with no arguments or external services.
// Assertions below check functional responses; test_app owns startup/teardown.

RUVIA_MODEL(note_request, RUVIA_OPTIONAL_FIELD(text, ruvia::string));

namespace testing_example {

struct note_counter final {
    int stored_{0};
};

class audit_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Audited", "yes");
    }
};

class notes_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/notes")
    RUVIA_ROUTES_BEGIN(notes_controller)
    RUVIA_GET("/:id", note);
    RUVIA_POST("/", create, ruvia::json_body<note_request>);
    RUVIA_GET("/", stats);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> note(ruvia::context& c) {
        // url_for builds links from registered patterns; the pattern is the
        // route's identity.
        std::string body;
        body.append("note ");
        body.append(c.req().param("id").value_or("?"));
        body.append(" self=");
        body.append(c.url_for("/notes/:id", {c.req().param("id").value_or("0")}));
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        const auto& note = c.req().validated<note_request>();
        ++c.worker_state<note_counter>().stored_;
        c.draft().status(ruvia::http_status::created);
        co_return c.body(note.get<"text">().has_value() ? note.get<"text">()->view() : "empty");
    }

    ruvia::task<ruvia::http_response> stats(ruvia::context& c) {
        std::string body;
        body.append("stored=");
        body.append(std::to_string(c.worker_state<note_counter>().stored_));
        co_return c.text(std::move(body));
    }
};

ruvia::task<ruvia::http_response> notes_missing(ruvia::context& c) {
    c.draft().status(ruvia::http_status::not_found);
    co_return c.text("no such note");
}

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAILED: %s\n", what);
    }
}

}  // namespace testing_example

[[maybe_unused]] int run_testing_main() {
    using namespace testing_example;
    ruvia::test_app app;
    app.use<audit_middleware>().on_not_found({.prefix_ = "/notes", .handler_ = &notes_missing});
    app.use_worker_state<note_counter>();

    // Routing, params and url_for.
    const auto note = app.request(ruvia::test_request::get("/notes/7"));
    expect(note.status() == ruvia::http_status::ok, "GET /notes/7 is 200");
    expect(note.body() == "note 7 self=/notes/7", "url_for builds the note link");
    expect(
        note.header("X-Audited").value_or("") == "yes", "global middleware stamped the response");

    // Model bodies keep their production status split: 415 for the wrong
    // media type, 400 for a malformed body of the right type.
    const auto created =
        app.request(ruvia::test_request::post("/notes").json(R"({"text":"remember"})"));
    expect(created.status() == ruvia::http_status::created, "valid JSON is 201");
    expect(created.body() == "remember", "created body echoes the model field");
    const auto wrong_type =
        app.request(ruvia::test_request::post("/notes").body("text", "text/plain"));
    expect(
        wrong_type.status() == ruvia::http_status::unsupported_media_type, "wrong media type is 415");

    // Worker state persisted across the requests above.
    const auto stats = app.request(ruvia::test_request::get("/notes"));
    expect(stats.body() == "stored=1", "worker state persisted");

    // The prefix-scoped not_found handled the miss under /notes.
    const auto missing = app.request(ruvia::test_request::get("/notes/9/edit"));
    expect(missing.status() == ruvia::http_status::not_found, "miss is 404");
    expect(missing.body() == "no such note", "prefix not_found rendered the miss");

    if (g_failures == 0) {
        std::puts("testing facade example: all checks passed");
    }
    return g_failures == 0 ? 0 : 1;
}

// Streaming: streaming request bodies, typed multipart chunk phases, chunked
// response streaming and server-sent events.
// Run ruvia_example_api_surface on port 8082. Try:
// curl -N http://127.0.0.1:8082/streaming/events
// curl --data-binary @file.bin http://127.0.0.1:8082/streaming/upload/raw
// curl -F file=@file.bin http://127.0.0.1:8082/streaming/upload/multipart
// A chunk borrows reusable reader storage; consume it before the next read.

class streaming_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/streaming")

    RUVIA_ROUTES_BEGIN(streaming_controller)
    RUVIA_POST("/upload/raw", upload_raw, ruvia::stream_body);
    RUVIA_POST("/upload/multipart", upload_multipart, ruvia::stream_body);
    RUVIA_GET("/chunks", chunks);
    RUVIA_GET("/events", events_value);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> upload_raw(ruvia::context& c) {
        std::size_t bytes_value = 0;
        auto& reader_value = c.req().body_reader();
        // Each read waits at most 30 s for the next chunk; a timeout throws
        // std::system_error(std::errc::timed_out) and keeps the body readable.
        while (auto chunk = co_await reader_value.read({.timeout_ = std::chrono::seconds(30)})) {
            bytes_value += chunk->size();
        }

        std::string body;
        body.append("uploaded bytes=");
        ruvia::http_append_unsigned(body, bytes_value);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> upload_multipart(ruvia::context& c) {
        std::size_t parts = 0;
        std::size_t bytes_value = 0;
        auto reader_value = c.req().multipart_reader();
        while (auto part = co_await reader_value.read()) {
            if (part->phase() == ruvia::multipart_chunk_phase::first ||
                part->phase() == ruvia::multipart_chunk_phase::complete) {
                ++parts;
            }
            bytes_value += part->body().size();
        }

        std::string body;
        body.append("multipart parts=");
        ruvia::http_append_unsigned(body, parts);
        body.append(" bytes=");
        ruvia::http_append_unsigned(body, bytes_value);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<void> chunks(ruvia::context& c) {
        auto& stream = c.stream_text();
        if (co_await stream.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await stream.write("part 1\n");
        co_await stream.writeln("part 2");
        if (co_await stream.sleep(std::chrono::milliseconds(20)) ==
            ruvia::timer_sleep_result::stop_requested) {
            co_return;
        }
        if (!stream.aborted()) {
            co_await stream.write("part 3\n");
        }
    }

    ruvia::task<void> events_value(ruvia::context& c) {
        auto events_value = c.stream_sse();
        if (co_await events_value.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        // 客户端断开（FIN/RST）会停止 c.stop_token()；把它传给业务订阅等待。
        const auto stop = c.stop_token();
        co_await events_value.write({.data_ = "connected", .event_ = "open", .id_ = "1"});
        if (co_await ruvia::sleep_for(c.worker(), std::chrono::milliseconds(20), stop) ==
            ruvia::timer_sleep_result::stop_requested) {
            co_return;
        }
        if (!events_value.aborted()) {
            co_await events_value.write({.data_ = "heartbeat",
                .event_ = "tick",
                .id_ = "2",
                .retry_ = std::chrono::milliseconds{3000}});
        }
    }
};

[[maybe_unused]] int run_streaming_main() {
    try {
        ruvia::app()
            .listen({.address_ = "0.0.0.0", .http_ = 8082})
            .server({
                .worker_count_ = 2,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
                .max_buffered_body_bytes_ = 16 * 1024 * 1024,
                .max_stream_body_bytes_ = std::nullopt,
            })
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// HTTP/1, HTTP/2 and HTTP/3 server features through the Web API.
// Run ruvia_example_api_surface, then query it with any HTTP/1.1, HTTP/2 or HTTP/3 client.
// Plain HTTP listens on 127.0.0.1:8093 and accepts HTTP/2 prior knowledge.
// Set RUVIA_TLS_CERT and RUVIA_TLS_KEY to enable HTTPS and HTTP/3 on port 8444;
// the certificate must cover localhost.

namespace http_features_example {

class feature_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/features")
    RUVIA_ROUTES_BEGIN(feature_controller)
    RUVIA_GET("/info", info);
    RUVIA_GET("/asset", asset);
    RUVIA_GET("/safe", safe);
    RUVIA_GET("/redirect", redirect);
    RUVIA_GET("/download", download);
    RUVIA_POST("/upload", upload, ruvia::stream_body, ruvia::body_limit<1024 * 1024>);
    RUVIA_GET_WS("/ws", websocket);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> info(ruvia::context& c) {
        const auto connection = c.conn();
        const std::array<ruvia::http_header_view, 1> hints{{
            {"link", "</features/asset>; rel=preload; as=style"},
        }};
        // An interim head is separate from the final response. The Web API
        // copies its input, but the operation must still finish in this request.
        co_await c.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, hints));
        c.draft().priority({.urgency_ = 2, .incremental_ = true});
        const auto protocol = c.req().protocol_version();
        if ((protocol == ruvia::http_protocol_version::http2 ||
                protocol == ruvia::http_protocol_version::http3) &&
            connection.tls()) {
            // Advertisements never change the client's trusted origin.
            const std::array<std::string_view, 1> origins{"https://localhost:8444"};
            c.advertise_origins(origins);
            if (protocol == ruvia::http_protocol_version::http2) {
                c.advertise_alternative_service("h2=\":8444\"; ma=60");
            }
        }
        if (protocol == ruvia::http_protocol_version::http2) {
            // Peer settings or capacity can decline push; the asset remains
            // available through an ordinary request.
            const bool pushed = c.push({
                .scheme_ = connection.tls() ? "https" : "http",
                .authority_ = c.req().header("host").value_or(connection.tls() ? "localhost:8444" : "localhost:8093"),
                .path_ = "/features/asset",
            });
            c.draft().header("X-Push-Accepted", pushed ? "yes" : "no");
        }
        co_return c.text("interim responses, priorities, advertisements, and push\n");
    }

    ruvia::task<ruvia::http_response> asset(ruvia::context& c) {
        c.draft().header("content-type", "text/css");
        co_return c.body(ruvia::static_text("body { color: #234; }\n"));
    }

    ruvia::task<ruvia::http_response> safe(ruvia::context& c) {
        co_return c.text("read-only resource\n");
    }

    ruvia::task<ruvia::http_response> redirect(ruvia::context& c) {
        co_return c.redirect({.location_ = "/features/safe", .status_ = ruvia::http_status::see_other});
    }

    ruvia::task<void> download(ruvia::context& c) {
        auto& output = c.stream_text();
        c.draft().header("Trailer", "X-Example-Complete");
        if (co_await output.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await output.write("first\n");
        // Await each write for bounded backpressure. No subsequent header or
        // cookie mutation is allowed once the first write publishes the head.
        co_await output.write("second\n");
        const std::array<ruvia::http_header_view, 1> trailers{{{"X-Example-Complete", "yes"}}};
        co_await output.end(trailers);
    }

    ruvia::task<ruvia::http_response> upload(ruvia::context& c) {
        std::size_t bytes_value = 0;
        auto& body = c.req().body_reader();
        while (const auto chunk = co_await body.read()) {
            bytes_value += chunk->size();
        }
        // Request trailers are available only after the request body reaches EOF.
        std::string result_value;
        result_value.append("uploaded=").append(std::to_string(bytes_value));
        result_value.append(" trailers=").append(std::to_string(c.req().trailers().size()));
        result_value.push_back('\n');
        co_return c.text(std::move(result_value));
    }

    ruvia::task<void> websocket(ruvia::context& c) {
        auto& socket = c.websocket();
        while (auto message = co_await socket.read()) {
            // Borrow the payload for the echo; send() holds the read lane until
            // the frame is written, so the next read cannot replace it early.
            co_await socket.send(std::move(*message));
        }
    }
};

}  // namespace http_features_example

[[maybe_unused]] int run_http_features_main() {
    using namespace http_features_example;
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        ruvia::listen_config listener_value{.address_ = "127.0.0.1", .http_ = 8093};
        const auto certificate = env_value.get("RUVIA_TLS_CERT");
        const auto key = env_value.get("RUVIA_TLS_KEY");
        if (certificate.has_value() != key.has_value()) {
            throw std::invalid_argument("set both RUVIA_TLS_CERT and RUVIA_TLS_KEY");
        }
        if (certificate && key) {
            listener_value.https_ = 8444;
#if defined(RUVIA_ENABLE_HTTP3)
            listener_value.http3_ = 8444;
#endif
            listener_value.tls_.identity_ = {
                .certificate_chain_file_ = std::filesystem::path(*certificate),
                .private_key_file_ = std::filesystem::path(*key),
            };
        }
        app.server({.worker_count_ = 2,
                       .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
                       .compression_ = ruvia::compression_config{.min_bytes_ = 16}})
            .listen(std::move(listener_value))
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// HTTP/1 and HTTP/2 CONNECT byte streams and CONNECT-UDP capsules.
// Run ruvia_example_api_surface for the server on port 8094; RUVIA_TLS_CERT/KEY
// enables TLS on port 8445. Any CONNECT or CONNECT-UDP (RFC 9298) client can
// drive it. The server echoes locally and does not forward to an arbitrary host.

namespace tunnels_example {

class tunnel_controller final {
public:
    RUVIA_ROUTES_BEGIN(tunnel_controller)
    RUVIA_GET("/health", health);
    // datagrams_ declares the Capsule Protocol (Capsule-Protocol: ?1 on the
    // 2xx, and the tunnel's udp()/datagrams()/capsules() facades); connect-udp
    // implies it, so here it only documents the route's protocol.
    const ruvia::http_tunnel_route_config options{
        .peer_transport_fin_timeout_ = std::chrono::seconds(2), .datagrams_ = true};
    RUVIA_CONNECT("echo.example:443", bytes);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/udp/:host/:port", udp, options);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> health(ruvia::context& c) {
        co_return c.text("ready\n");
    }

    ruvia::task<void> bytes(ruvia::context& c) {
        auto& tunnel = c.tunnel();
        while (auto chunk = co_await tunnel.read()) {
            // 借用块保持有效至下一次 read；write 在返回任务前拥有化输入。
            co_await tunnel.write(*chunk);
        }
        co_await tunnel.finish();
    }

    ruvia::task<void> udp(ruvia::context& c) {
        // This protocol route has already negotiated CONNECT-UDP. Capsule
        // limits can be passed directly to udp(http_capsule_config).
        auto stream = c.tunnel().udp();
        while (auto packet = co_await stream.read()) {
            // The adapter handles context ID 0 and ignores unknown contexts.
            // A zero-byte payload is a valid UDP packet, not an EOF marker.
            co_await stream.send(packet->payload());
        }
        co_await stream.finish();
    }
};

}  // namespace tunnels_example

[[maybe_unused]] int run_tunnels_main() {
    using namespace tunnels_example;
    try {
        std::cout << std::unitbuf;
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        ruvia::listen_config listener_value{.address_ = "127.0.0.1", .http_ = 8094};
        const auto cert = env_value.get("RUVIA_TLS_CERT");
        const auto key = env_value.get("RUVIA_TLS_KEY");
        if (cert.has_value() != key.has_value()) {
            throw std::invalid_argument("set both RUVIA_TLS_CERT and RUVIA_TLS_KEY");
        }
        if (cert && key) {
            listener_value.https_ = 8445;
            listener_value.tls_.identity_ = {
                .certificate_chain_file_ = std::filesystem::path(*cert),
                .private_key_file_ = std::filesystem::path(*key),
            };
        }
        app.server({.worker_count_ = 2,
                       .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .listen(std::move(listener_value))
            .on_connection_failure([](const ruvia::connection_failure_record& record) noexcept {
                try {
                    std::cerr << "tunnel connection failed: " << ruvia::exception_text(record.exception()) << '\n';
                } catch (...) {
                    std::cerr << "tunnel connection failed\n";
                }
            })
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// websocket: upgrade routes, subprotocol options, lifecycle timeouts,
// text/binary echo and the RFC close handshake.
// Run ruvia_example_api_surface, then connect to ws://localhost:8084/ws/echo.
// /ws/chat requires one of chat_options()'s advertised subprotocols: an upgrade
// offering none (send Sec-WebSocket-Protocol: chat.v1) is refused with 400.
// /ws/ticker pushes ticks and then closes with 4001 while its reader waits.
// Read until close; join the close handshake before releasing the connection.

class websocket_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/ws")

    RUVIA_ROUTES_BEGIN(websocket_controller)
    const auto chat_options = ruvia::websocket_route_config{
        .subprotocols_ = {"chat.v1"},
        .require_subprotocol_ = true,
        .lifecycle_ =
            {
                .heartbeat_ =
                    {
                        .ping_interval_ = std::chrono::seconds(30),
                        .pong_timeout_ = std::chrono::seconds(10),
                    },
                .close_handshake_timeout_ = std::chrono::seconds(5),
            },
    };
    RUVIA_GET_WS("/echo", echo);
    RUVIA_GET_WS("/chat", chat, chat_options);
    RUVIA_GET_WS("/ticker", ticker);
    RUVIA_ROUTES_END

private:
    ruvia::task<void> echo(ruvia::context& c) {
        auto& ws = c.websocket();
        while (auto message = co_await ws.read()) {
            // Zero-copy echo: the frame borrows the read buffer and keeps the
            // read lane until it is written, preserving the message opcode.
            co_await ws.send(std::move(*message));
        }
    }

    ruvia::task<void> chat(ruvia::context& c) {
        auto& ws = c.websocket();
        co_await ws.text("welcome");
        while (auto message = co_await ws.read()) {
            if (message->text()) {
                co_await ws.send(std::move(*message));
            }
        }
        co_await ws.close({.code_ = ruvia::websocket_close_status::normal_closure, .reason_ = "bye"});
    }

    // close() claims only the write lane, so the pending read below returns
    // nullopt once the peer answers the 4001 Close (or abort() ends it).
    ruvia::task<void> ticker(ruvia::context& c) {
        auto& ws = c.websocket();
        ruvia::task_scope readers(c.worker());
        readers.spawn(drain(ws));
        std::exception_ptr failure;
        try {
            for (int tick = 0; tick < 5; ++tick) {
                co_await ws.text("tick");
                (void)co_await ruvia::sleep_for(c.worker(), std::chrono::seconds(1));
            }
            co_await ws.close({.code_ = ruvia::websocket_close_code::from_value(4001), .reason_ = "subscription ended"});
        } catch (...) {
            failure = std::current_exception();
            ws.abort();
        }
        co_await readers.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    static ruvia::task<void> drain(ruvia::websocket& ws) {
        for (;;) {
            try {
                if (!co_await ws.read({.timeout_ = std::chrono::seconds(30)})) {
                    co_return;
                }
            } catch (const std::system_error& error) {
                // A read deadline ends only that read; the session stays open.
                if (error.code() != std::errc::timed_out) {
                    throw;
                }
            }
        }
    }
};

[[maybe_unused]] int run_websocket_main() {
    try {
        ruvia::app()
            .listen({.address_ = "0.0.0.0", .http_ = 8084})
            .server({
                .worker_count_ = 2,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
                .max_websocket_message_bytes_ = 16 * 1024 * 1024,
            })
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}
