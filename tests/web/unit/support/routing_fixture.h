#pragma once

#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/streaming.h"

#include "context/context_access.h"
#include "context_services_fixture.h"
#include "http/streaming_access.h"
#include "router/route_resolution.h"
#include "router/route_table.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "server/http_response_stream_state.h"
#include "test_harness.h"
#include "websocket/websocket_access.h"

RUVIA_MODEL(scoped_validation_request, RUVIA_OPTIONAL_FIELD(value, ruvia::string));

namespace routing_test {

using ruvia::http_known_method;
using ruvia::detail::controller_middleware_descriptor;
using ruvia::detail::request_body_mode;
using ruvia::detail::route_handler_type;
using ruvia::detail::route_match;

class first_int_validator final : public ruvia::middleware {
public:
    using ruvia_validation_body_type = int;

    ruvia::task<void> handle(ruvia::context&, ruvia::next&) {
        co_return;
    }
};

class second_int_validator final : public ruvia::middleware {
public:
    using ruvia_validation_body_type = int;

    ruvia::task<void> handle(ruvia::context&, ruvia::next&) {
        co_return;
    }
};

class validation_scope_probe final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next& next_value) {
        co_await next_value();
        try {
            (void)context_value.req().validated<scoped_validation_request>();
        } catch (const std::logic_error&) {
            released_after_next = true;
        }
    }

    static inline bool released_after_next{false};
};

inline bool scoped_validation_handler_read{false};
inline bool scoped_validation_raw_read{false};
inline bool scoped_validation_handler_throws{false};

inline ruvia::task<ruvia::http_response> scoped_validation_handler(void*, ruvia::context& context_value) {
    const auto& model = context_value.req().validated<scoped_validation_request>();
    const auto json = context_value.req().validated_json<scoped_validation_request>();
    scoped_validation_handler_read =
        model.get<"value">().has_value() && model.get<"value">()->view() == "ok";
    scoped_validation_raw_read = &json.value() == &model && json.raw() == R"({"value":"ok"})";
    if (scoped_validation_handler_throws) {
        throw std::runtime_error("validated handler failure");
    }
    co_return context_value.text("validated");
}

// Never invoked — resolve() only needs a registered route with a valid handler.
inline ruvia::task<ruvia::http_response> dummy_handler(void*, ruvia::context&) {
    co_return ruvia::http_response({.resource_ = std::pmr::get_default_resource()});
}

inline ruvia::task<void> dummy_stream_handler(void*, ruvia::context&) {
    co_return;
}

inline std::pmr::string path(std::string_view value) {
    return std::pmr::string(value, std::pmr::get_default_resource());
}

inline ruvia::http_request make_request(ruvia::request_memory& memory, std::string_view method,
    std::string_view target, std::span<const ruvia::http_header_view> headers = {},
    std::string_view body = {}) {
    const auto body_bytes = std::as_bytes(std::span(body.data(), body.size()));
    auto [request, error] = ruvia::make_parsed_http_request(
        method, target, headers, body_bytes, memory.resource());
    if (error.has_value()) {
        throw std::invalid_argument("invalid parsed HTTP request fixture");
    }
    return std::move(request);
}

inline void add_route(
    ruvia::detail::router_impl& impl, http_known_method method, std::string_view route) {
    impl.register_route(method, path(route), route_handler_type(nullptr, &dummy_handler),
        request_body_mode::buffered, std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
}

inline void add_route(ruvia::detail::router_impl& impl, std::string_view route) {
    add_route(impl, http_known_method::get, route);
}

// Registers the given routes and reports whether finalize() rejects them as a
// dynamic route-shape conflict.
inline bool finalize_conflicts(std::initializer_list<std::string_view> routes_value) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    for (const auto route : routes_value) {
        add_route(impl, route);
    }
    try {
        impl.finalize();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

struct router final {
    ruvia::detail::router router_;
    ruvia::detail::router_impl& impl_ = ruvia::detail::router_impl::from(router_);

    void finalize() {
        impl_.finalize();
    }

    bool matches(std::string_view p) {
        const auto resolution = impl_.route_table().resolve(http_known_method::get, p);
        return resolution.resolved() != nullptr;
    }

    std::string_view route_path_of(std::string_view p) {
        return route_path_of(http_known_method::get, p);
    }

    std::string_view route_path_of(http_known_method method, std::string_view p) {
        const auto res = impl_.route_table().resolve(method, p);
        const auto* resolved = res.resolved();
        return resolved != nullptr ? resolved->route().path() : "<none>";
    }

    // Returns the single captured param value, or "<none>" if unmatched / no param.
    std::string_view param_of(std::string_view p) {
        const auto res = impl_.route_table().resolve(http_known_method::get, p);
        const auto* resolved = res.resolved();
        if (resolved == nullptr || resolved->match().size() != 1) {
            return "<none>";
        }
        return resolved->match().values()[0];
    }
};

}  // namespace routing_test

namespace routing_test {

// Records the order middlewares and the handler run: positive on entry (before
// next()), negative on unwind (after next()), 0 for the handler.
inline std::vector<int> g_chain_order;

class chain_mw_a final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        g_chain_order.push_back(1);
        co_await next_value();
        g_chain_order.push_back(-1);
    }
};

class chain_mw_b final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        g_chain_order.push_back(2);
        co_await next_value();
        g_chain_order.push_back(-2);
    }
};

class chain_mw_override_after_next final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next& next_value) {
        co_await next_value();
        context_value.respond(context_value.body("override"));
    }
};

// Short-circuits: sets a response and does NOT call next().
class chain_mw_stop final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next&) {
        g_chain_order.push_back(9);
        context_value.respond(context_value.body("stopped"));
        co_return;
    }
};

// Misuse: respond() ends the middleware chain, so a later next() must not run
// downstream handlers and silently replace the response.
class chain_mw_respond_then_next final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next& next_value) {
        context_value.respond(context_value.body("early"));
        co_await next_value();
    }
};

// Misuse: calls next() twice. The second invocation must be rejected rather than
// re-entering the downstream chain (which would run the handler -- and its side
// effects -- a second time).
class chain_mw_double_next final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
        co_await next_value();
    }
};

inline bool g_websocket_unavailable_after_next = false;

class chain_mw_probe_websocket_after_next final
    : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next& next_value) {
        co_await next_value();
        try {
            (void)context_value.get_websocket();
        } catch (const std::logic_error&) {
            g_websocket_unavailable_after_next = true;
        }
    }
};

class chain_mw_throws_after_next final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
        throw std::runtime_error("middleware post failed");
    }
};

// Throws before calling next(): the chain is short-circuited and the exception
// must be mapped to an error response (never escaping the dispatch).
class chain_mw_throws final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next&) {
        throw ruvia::http_error({.status_ = ruvia::http_status::unauthorized,
            .code_ = "mw_rejected",
            .message_ = "middleware rejected the request"});
        co_return;  // unreachable
    }
};

inline ruvia::task<ruvia::http_response> chain_handler(void*, ruvia::context& context_value) {
    g_chain_order.push_back(0);
    co_return context_value.body("ok");
}

inline std::string dispatch_chain(
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares = {}) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_route(http_known_method::get, path("/chain"), route_handler_type(nullptr, &chain_handler),
        request_body_mode::buffered, controller_middlewares, route_middlewares);
    impl.finalize();
    const auto& table_value = impl.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    ruvia::http_request request = make_request(memory, "GET", "/chain");

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(
            table_value.dispatch(request, memory, ruvia::test::test_context_services())),
        asio::use_future);
    ctx.run();
    auto response = future.get();
    const auto body = response.body_bytes();
    return std::string(body.data(), body.size());
}

// A capture sink whose committed flag flips true on the first write, mirroring the
// real streaming sink (which commits the head before the first body chunk).
struct stream_capture_sink final {
    std::pmr::string scratch_{std::pmr::get_default_resource()};
    bool committed_flag_ = false;
    bool ended_flag_ = false;
    bool context_released_ = false;
    std::vector<std::string> writes_;
};

inline ruvia::task<void> sc_write(void* target, std::string_view chunk) {
    auto* sink_value = static_cast<stream_capture_sink*>(target);
    sink_value->committed_flag_ = true;
    sink_value->writes_.emplace_back(chunk);
    co_return;
}
inline ruvia::task<void> sc_end(void* target, std::span<const ruvia::http_header_view>) {
    auto* sink_value = static_cast<stream_capture_sink*>(target);
    sink_value->committed_flag_ = true;
    sink_value->ended_flag_ = true;
    co_return;
}
inline ruvia::task<ruvia::timer_sleep_result> sc_sleep(
    void*, std::chrono::milliseconds, const ruvia::stop_token&) {
    co_return ruvia::timer_sleep_result::elapsed;
}
inline void sc_bind(void*, ruvia::context*, ruvia::task<ruvia::http_response> (*)(ruvia::context&)) noexcept {}
inline void sc_release_context(void* target) noexcept {
    static_cast<stream_capture_sink*>(target)->context_released_ = true;
}
inline bool sc_committed(void* target) noexcept {
    return static_cast<stream_capture_sink*>(target)->committed_flag_;
}
inline bool sc_aborted(void*) noexcept {
    return false;
}

inline ruvia::response_stream_writer sc_make_writer(stream_capture_sink& sink_value) noexcept {
    return ruvia::detail::streaming_access::make_response_stream_writer(
        *ruvia::detail::process_resource(), &sink_value, &sc_write, &sc_end, &sc_sleep, &sc_bind, &sc_release_context, &sc_committed, &sc_aborted);
}

struct empty_stream_dispatch_observation final {
    bool handled_{false};
    bool buffered_{false};
    bool threw_{false};
    bool ended_{false};
    bool committed_{false};
    bool context_released_{false};
    std::string buffered_body_;
};

inline empty_stream_dispatch_observation dispatch_empty_stream_with(
    const controller_middleware_descriptor& middleware_value) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_response_stream_route(http_known_method::get, path("/empty-stream"),
        ruvia::detail::route_stream_handler_type(nullptr, &dummy_stream_handler),
        std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>(&middleware_value, 1));
    impl.finalize();
    const auto& table_value = impl.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    ruvia::http_request request = make_request(memory, "GET", "/empty-stream");

    const auto resolution = table_value.resolve(http_known_method::get, "/empty-stream");
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::logic_error("empty stream test route did not resolve");
    }

    stream_capture_sink sink;
    auto writer = sc_make_writer(sink);
    empty_stream_dispatch_observation observation;
    asio::io_context context_value(1);
    asio::co_spawn(
        context_value,
        [&]() -> asio::awaitable<void> {
            try {
                auto result_value = co_await ruvia::as_awaitable(table_value.dispatch_response_stream(
                    request, *resolved, memory, writer, ruvia::test::test_context_services()));
                observation.handled_ = !result_value.has_value();
                if (result_value.has_value()) {
                    observation.buffered_ = true;
                    auto response = std::move(*result_value);
                    const auto body = response.body_bytes();
                    observation.buffered_body_.assign(body.data(), body.size());
                }
            } catch (...) {
                observation.threw_ = true;
            }
        },
        asio::detached);
    context_value.run();
    observation.ended_ = sink.ended_flag_;
    observation.committed_ = sink.committed_flag_;
    observation.context_released_ = sink.context_released_;
    return observation;
}

struct websocket_dispatch_observation final {
    bool terminal_invoked_{false};
    bool capability_available_in_terminal_{false};
    bool buffered_{false};
    std::string buffered_body_;
};

struct websocket_terminal_target final {
    websocket_dispatch_observation* observation_;
    ruvia::websocket* websocket_;
};

inline ruvia::task<void> websocket_terminal(void* target, ruvia::context& context_value) {
    auto& terminal = *static_cast<websocket_terminal_target*>(target);
    terminal.observation_->terminal_invoked_ = true;
    ruvia::detail::context_access::mark_websocket_handshake_started(context_value);
    ruvia::detail::context_websocket_binding binding(context_value, *terminal.websocket_);
    terminal.observation_->capability_available_in_terminal_ =
        &context_value.get_websocket() == terminal.websocket_;
    g_chain_order.push_back(0);
    co_return;
}

inline websocket_dispatch_observation dispatch_websocket_with(
    const controller_middleware_descriptor& middleware_value) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_websocket_route(http_known_method::get, path("/ws-middleware"),
        ruvia::detail::route_stream_handler_type(nullptr, &dummy_stream_handler),
        std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>(&middleware_value, 1), {});
    impl.finalize();
    const auto& table_value = impl.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto request = make_request(memory, "GET", "/ws-middleware");
    const auto resolution = table_value.resolve(http_known_method::get, "/ws-middleware");
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::logic_error("websocket middleware test route did not resolve");
    }

    websocket_dispatch_observation observation;
    auto websocket_value = ruvia::detail::websocket_access::make(
        *ruvia::detail::process_resource(), nullptr, nullptr, nullptr, nullptr);
    websocket_terminal_target terminal_target{&observation, &websocket_value};
    const auto terminal = ruvia::detail::route_stream_handler_type(&terminal_target, &websocket_terminal);
    asio::io_context context_value(1);
    auto future = asio::co_spawn(context_value,
        ruvia::as_awaitable(table_value.dispatch_websocket(
            request, *resolved, memory, terminal, ruvia::test::test_context_services())),
        asio::use_future);
    context_value.run();
    auto response = future.get();
    if (response.has_value()) {
        observation.buffered_ = true;
        const auto body = response->body_bytes();
        observation.buffered_body_.assign(body.data(), body.size());
    }
    return observation;
}

// Writes a chunk (committing the stream) then throws mid-body.
inline ruvia::task<void> stream_commit_then_throw(void*, ruvia::context& context_value) {
    co_await context_value.stream().write("partial");
    throw std::runtime_error("mid-stream handler failure");
}

}  // namespace routing_test

namespace routing_test {

// Simulates the real sinks' body-suppressed commit for an explicit HEAD stream
// route: the head commits, then the first body write raises the head-only
// completion signal instead of accepting the chunk.
inline bool g_head_only_handler_resumed_past_first_write = false;

inline ruvia::task<void> head_only_write(void* target, std::string_view) {
    static_cast<stream_capture_sink*>(target)->committed_flag_ = true;
    throw ruvia::detail::response_stream_head_only_complete();
    co_return;  // unreachable
}

inline ruvia::response_stream_writer make_head_only_writer(stream_capture_sink& sink_value) noexcept {
    return ruvia::detail::streaming_access::make_response_stream_writer(*ruvia::detail::process_resource(), &sink_value, &head_only_write, &sc_end,
        &sc_sleep, &sc_bind, &sc_release_context, &sc_committed, &sc_aborted);
}

inline ruvia::task<void> head_only_probe_stream_handler(void*, ruvia::context& context_value) {
    g_chain_order.push_back(0);
    co_await context_value.stream().write("event-1");
    g_head_only_handler_resumed_past_first_write = true;
    co_await context_value.stream().write("event-2");
}

struct head_only_dispatch_observation final {
    bool handled_{false};
    bool buffered_{false};
    bool threw_{false};
    bool ended_{false};
};

inline head_only_dispatch_observation dispatch_head_only_stream(
    std::span<const controller_middleware_descriptor> middlewares) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_response_stream_route(http_known_method::head, path("/head-only-stream"),
        ruvia::detail::route_stream_handler_type(nullptr, &head_only_probe_stream_handler),
        std::span<const controller_middleware_descriptor>{}, middlewares);
    impl.finalize();
    const auto& table_value = impl.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    ruvia::http_request request = make_request(memory, "HEAD", "/head-only-stream");

    const auto resolution = table_value.resolve(http_known_method::head, "/head-only-stream");
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::logic_error("head-only stream test route did not resolve for HEAD");
    }

    stream_capture_sink sink;
    auto writer = make_head_only_writer(sink);
    head_only_dispatch_observation observation;
    asio::io_context context_value(1);
    asio::co_spawn(
        context_value,
        [&]() -> asio::awaitable<void> {
            try {
                auto result_value = co_await ruvia::as_awaitable(table_value.dispatch_response_stream(
                    request, *resolved, memory, writer, ruvia::test::test_context_services()));
                observation.handled_ = !result_value.has_value();
                observation.buffered_ = result_value.has_value();
            } catch (...) {
                observation.threw_ = true;
            }
        },
        asio::detached);
    context_value.run();
    observation.ended_ = sink.ended_flag_;
    return observation;
}

}  // namespace routing_test

namespace routing_test {

inline ruvia::task<ruvia::http_response> throws_http_error_handler(void*, ruvia::context&) {
    throw ruvia::http_error(
        {.status_ = ruvia::http_status::forbidden, .code_ = "forbidden", .message_ = "nope"});
    co_return ruvia::http_response({.resource_ = std::pmr::get_default_resource()});  // unreachable
}

inline ruvia::task<ruvia::http_response> throws_generic_handler(void*, ruvia::context&) {
    throw std::runtime_error("boom");
    co_return ruvia::http_response({.resource_ = std::pmr::get_default_resource()});  // unreachable
}

inline ruvia::task<ruvia::http_response> throws_invalid_argument_handler(void*, ruvia::context&) {
    throw std::invalid_argument("application bug");
    co_return ruvia::http_response({.resource_ = std::pmr::get_default_resource()});  // unreachable
}

inline ruvia::task<ruvia::http_response> throws_protocol_error_handler(void*, ruvia::context&) {
    throw ruvia::http_protocol_error(
        ruvia::http_status::content_too_large, "request body is too large");
    co_return ruvia::http_response({.resource_ = std::pmr::get_default_resource()});  // unreachable
}

inline ruvia::task<ruvia::http_response> ok_handler(void*, ruvia::context& context_value) {
    co_return context_value.body("ok");
}

inline ruvia::task<ruvia::http_response> reads_request_body_handler(void*, ruvia::context& context_value) {
    (void)co_await context_value.req().text();
    co_return context_value.body("ok");
}

// The dispatched response's storage lives in the per-request arena, which is
// destroyed when the helper returns -- so values must be copied out here, while
// the arena is still alive, rather than returning the http_response itself.
struct dispatch_result final {
    std::uint16_t status_{0};
    std::string body_;
    std::string allow_;
    std::string connection_;
    std::string accept_encoding_;
};

inline dispatch_result extract_dispatch_result(const ruvia::http_response& response) {
    dispatch_result result;
    result.status_ = response.status().value();
    const auto body = response.body_bytes();
    result.body_.assign(body.data(), body.size());
    const auto allow = response.header("Allow").value_or(std::string_view{});
    result.allow_.assign(allow.data(), allow.size());
    const auto connection = response.header("Connection").value_or(std::string_view{});
    result.connection_.assign(connection.data(), connection.size());
    const auto accept_encoding = response.header("Accept-Encoding").value_or(std::string_view{});
    result.accept_encoding_.assign(accept_encoding.data(), accept_encoding.size());
    return result;
}

// Registers GET /x with `handler`, dispatches the exact wire method token and
// path, then returns the rendered result.
inline dispatch_result dispatch_one_token(route_handler_type handler, std::string_view method,
    std::string_view p, std::string_view content_encoding = {}, std::string_view body = {}) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_route(http_known_method::get, path("/x"), handler, request_body_mode::buffered,
        std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    impl.finalize();
    const auto& table_value = impl.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    const std::array headers{ruvia::http_header_view{"Content-Encoding", content_encoding}};
    const auto header_span = content_encoding.empty()
                                 ? std::span<const ruvia::http_header_view>{}
                                 : std::span<const ruvia::http_header_view>(headers);
    ruvia::http_request request = make_request(memory, method, p, header_span, body);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(
            table_value.dispatch(request, memory, ruvia::test::test_context_services())),
        asio::use_future);
    ctx.run();
    return extract_dispatch_result(future.get());  // arena still alive here
}

inline dispatch_result dispatch_one(
    route_handler_type handler, http_known_method method, std::string_view p) {
    return dispatch_one_token(handler, ruvia::known_http_method_token(method), p);
}

}  // namespace routing_test

namespace routing_test {

using ruvia::http_error_handler_type;
using ruvia::http_error_info;
using ruvia::http_not_found_handler_type;

inline ruvia::task<ruvia::http_response> custom_not_found(ruvia::context& context_value) {
    context_value.status(ruvia::http_status::not_found);
    co_return context_value.body("custom-not-found");
}

inline ruvia::task<ruvia::http_response> custom_error(ruvia::context& context_value, http_error_info info) {
    context_value.status(info.status());
    co_return context_value.body("custom-error");
}

inline dispatch_result dispatch_with_handlers_token(route_handler_type handler,
    const http_error_handler_type& error_h, const http_not_found_handler_type& not_found_h, std::string_view method,
    std::string_view p, std::string_view content_encoding = {}, std::string_view body = {}) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    if (error_h != nullptr) {
        impl.set_error_handler(ruvia::detail::callback_access::ref(error_h));
    }
    if (not_found_h != nullptr) {
        impl.set_not_found_handler(ruvia::detail::callback_access::ref(not_found_h));
    }
    impl.register_route(http_known_method::get, path("/x"), handler, request_body_mode::buffered,
        std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    impl.finalize();
    const auto& table_value = impl.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    const std::array headers{ruvia::http_header_view{"Content-Encoding", content_encoding}};
    const auto header_span = content_encoding.empty()
                                 ? std::span<const ruvia::http_header_view>{}
                                 : std::span<const ruvia::http_header_view>(headers);
    ruvia::http_request request = make_request(memory, method, p, header_span, body);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(
            table_value.dispatch(request, memory, ruvia::test::test_context_services())),
        asio::use_future);
    ctx.run();
    return extract_dispatch_result(future.get());  // arena still alive here
}

inline dispatch_result dispatch_with_handlers(route_handler_type handler, const http_error_handler_type& error_h,
    const http_not_found_handler_type& not_found_h, http_known_method method, std::string_view p) {
    return dispatch_with_handlers_token(
        handler, error_h, not_found_h, ruvia::known_http_method_token(method), p);
}

}  // namespace routing_test

namespace routing_test {

inline ruvia::task<ruvia::http_response> api_scoped_not_found(ruvia::context& context_value) {
    context_value.status(ruvia::http_status::not_found);
    co_return context_value.body("api-scope-404");
}

inline ruvia::task<ruvia::http_response> v2_scoped_not_found(ruvia::context& context_value) {
    context_value.status(ruvia::http_status::not_found);
    co_return context_value.body("v2-scope-404");
}

inline ruvia::task<ruvia::http_response> api_scoped_error(
    ruvia::context& context_value, http_error_info info) {
    context_value.status(info.status());
    co_return context_value.body("api-scope-error");
}

inline dispatch_result dispatch_on(
    const ruvia::detail::route_table& table_value, std::string_view method, std::string_view p) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    ruvia::http_request request = make_request(memory, method, p);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(
            table_value.dispatch(request, memory, ruvia::test::test_context_services())),
        asio::use_future);
    ctx.run();
    return extract_dispatch_result(future.get());  // arena still alive here
}

}  // namespace routing_test

namespace routing_test {

inline ruvia::task<ruvia::http_response> url_for_echo_handler(void*, ruvia::context& context_value) {
    co_return context_value.body(context_value.url_for("/users/:id", {"7"}));
}

inline ruvia::task<ruvia::http_response> json_model_echo_handler(void*, ruvia::context& context_value) {
    const auto& body = context_value.req().validated<scoped_validation_request>();
    co_return context_value.body(
        body.get<"value">().has_value() ? body.get<"value">()->view() : "missing");
}

inline ruvia::task<ruvia::http_response> form_model_echo_handler(void*, ruvia::context& context_value) {
    const auto& body = context_value.req().validated<scoped_validation_request>();
    co_return context_value.body(
        body.get<"value">().has_value() ? body.get<"value">()->view() : "missing");
}

inline ruvia::task<ruvia::http_response> json_if_echo_handler(void*, ruvia::context& context_value) {
    const auto body = co_await context_value.req().json_if<scoped_validation_request>();
    co_return context_value.body(body.has_value() && body->get<"value">().has_value()
                                     ? body->get<"value">()->view()
                                     : "no-json");
}

inline ruvia::task<ruvia::http_response> form_if_echo_handler(void*, ruvia::context& context_value) {
    const auto body = co_await context_value.req().form_if<scoped_validation_request>();
    co_return context_value.body(body.has_value() && body->get<"value">().has_value()
                                     ? body->get<"value">()->view()
                                     : "no-form");
}

// Dispatches one GET /x with an optional Content-Type header and body.
inline dispatch_result dispatch_body_request(route_handler_type handler, std::string_view content_type_value,
    std::string_view body,
    std::span<const controller_middleware_descriptor> route_middlewares = {}) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_route(http_known_method::get, path("/x"), handler, request_body_mode::buffered,
        std::span<const controller_middleware_descriptor>{}, route_middlewares);
    impl.finalize();
    const auto& table_value = impl.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    const std::array headers{ruvia::http_header_view{"Content-Type", content_type_value}};
    const auto header_span = content_type_value.empty()
                                 ? std::span<const ruvia::http_header_view>{}
                                 : std::span<const ruvia::http_header_view>(headers);
    ruvia::http_request request = make_request(memory, "GET", "/x", header_span, body);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(
            table_value.dispatch(request, memory, ruvia::test::test_context_services())),
        asio::use_future);
    ctx.run();
    return extract_dispatch_result(future.get());
}

}  // namespace routing_test

using namespace routing_test;  // NOLINT(google-build-using-namespace)
