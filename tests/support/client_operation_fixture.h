#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"
// Per-operation timeouts and stop tokens of the public outbound client, driven
// against the application's own routes over each protocol's real transport.
namespace ruvia::test::client_operations {

using namespace std::chrono_literals;

// Each route stalls where a client operation is interrupted, far longer than
// that interruption, then completes so an interrupted read can resume.
inline constexpr auto server_stall = 300ms;
inline constexpr auto interruption = 30ms;

class routes final {
public:
    RUVIA_ROUTES_BEGIN(routes)
    RUVIA_GET("/operation/stalled-body", stalled_body);
    RUVIA_POST("/operation/held-upload", held_upload, ruvia::stream_body);
    RUVIA_POST("/operation/echo-upload", echo_upload, ruvia::stream_body);
    RUVIA_POST("/operation/late-head", late_head, ruvia::stream_body);
    RUVIA_CONNECT("operation.test:443", echo_tunnel);
    RUVIA_ROUTES_END

    ruvia::task<void> stalled_body(ruvia::context& context) {
        auto& writer = context.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("first-");
        (void)co_await writer.sleep(server_stall);
        co_await writer.write("second");
        co_await writer.end();
    }
    // Never reads its content, so a 100-continue upload is never released.
    ruvia::task<ruvia::http_response> held_upload(ruvia::context& context) {
        (void)co_await ruvia::sleep_for(context.worker(), 1s, context.stop_token());
        co_return context.text("held");
    }
    ruvia::task<ruvia::http_response> echo_upload(ruvia::context& context) {
        std::string echoed("echo:");
        auto& reader = context.req().body_reader();
        while (auto chunk = co_await reader.text()) {
            echoed.append(*chunk);
        }
        co_return context.text(echoed);
    }
    ruvia::task<ruvia::http_response> late_head(ruvia::context& context) {
        auto& reader = context.req().body_reader();
        while (co_await reader.read()) {
        }
        (void)co_await ruvia::sleep_for(context.worker(), server_stall, context.stop_token());
        co_return context.text("late");
    }
    ruvia::task<void> echo_tunnel(ruvia::context& context) {
        auto& tunnel = context.tunnel();
        while (auto chunk = co_await tunnel.read()) {
            co_await tunnel.write(std::move(*chunk));
        }
        co_await tunnel.finish();
    }
};

template <typename value_type>
[[nodiscard]] ruvia::task<std::optional<std::error_code>> failure_of(ruvia::task<value_type> operation) {
    try {
        (void)co_await std::move(operation);
    } catch (const ruvia::http_client_error& error) {
        co_return error.code();
    }
    co_return std::nullopt;
}

[[nodiscard]] inline ruvia::task<void> stop_after(const ruvia::worker_ref& worker, ruvia::stop_source& stop) {
    (void)co_await ruvia::sleep_for(worker, interruption);
    stop.request_stop();
}

inline const auto timed_out = std::make_error_code(std::errc::timed_out);
inline const auto cancelled = std::make_error_code(std::errc::operation_canceled);

inline void append(std::string& target, const std::optional<std::span<const std::byte>>& chunk) {
    if (chunk) {
        target.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
    }
}

// A read timeout, a stop request and an already-stopped token each end only
// their own wait: the stalled remainder still arrives intact afterwards.
[[nodiscard]] inline ruvia::task<void> check_body_reads(ruvia::http_client_ref client, const ruvia::worker_ref& worker) {
    auto response = co_await client.get("/operation/stalled-body").send();
    std::string body;
    append(body, co_await response.read());
    EXPECT_EQ(body, "first-");
    EXPECT_TRUE(co_await failure_of(response.read({.timeout_ = interruption})) == timed_out);
    ruvia::stop_source stop;
    ruvia::task_scope stopper(worker);
    stopper.spawn(stop_after(worker, stop));
    EXPECT_TRUE(co_await failure_of(response.stream().text({.stop_token_ = stop.token()})) == cancelled);
    co_await stopper.join();
    EXPECT_TRUE(co_await failure_of(response.read({.stop_token_ = stop.token()})) == cancelled);
    EXPECT_THROW((void)response.read({.timeout_ = 0ms}), std::invalid_argument);
    while (auto chunk = co_await response.read()) {
        append(body, chunk);
    }
    EXPECT_EQ(body, "first-second");
}

// An interrupted collection keeps what it received for the next one.
[[nodiscard]] inline ruvia::task<void> check_body_collection(ruvia::http_client_ref client) {
    auto response = co_await client.get("/operation/stalled-body").send();
    EXPECT_TRUE(co_await failure_of(response.bytes(ruvia::default_max_buffered_body_bytes, {.timeout_ = interruption})) == timed_out);
    const auto rest = co_await response.bytes();
    EXPECT_EQ(rest.text(), "first-second");
}

// An upload write that cannot complete in time, or is stopped, aborts the
// exchange: no further content is accepted and no response follows.
[[nodiscard]] inline ruvia::task<void> check_upload_abort(
    ruvia::http_client_ref client, const ruvia::worker_ref& worker, bool cancel) {
    auto exchange = co_await client.post("/operation/held-upload")
                        .upload({.expectation_ = ruvia::http_client_request_expectation::continue_value,
                            .continue_timeout_ = 10s});
    ruvia::stop_source stop;
    ruvia::task_scope stopper(worker);
    ruvia::operation_options operation{.timeout_ = interruption};
    if (cancel) {
        operation = {.stop_token_ = stop.token()};
        stopper.spawn(stop_after(worker, stop));
    }
    EXPECT_TRUE(co_await failure_of(exchange.body().write("held", std::move(operation))) == (cancel ? cancelled : timed_out));
    co_await stopper.join();
    EXPECT_THROW((void)exchange.body().write("more"), ruvia::http_client_error);
    EXPECT_TRUE((co_await failure_of(exchange.response())).has_value());
}

// An already-stopped token sends nothing and leaves the upload usable.
[[nodiscard]] inline ruvia::task<void> check_stopped_upload(ruvia::http_client_ref client) {
    ruvia::stop_source stop;
    stop.request_stop();
    auto exchange = co_await client.post("/operation/echo-upload").upload({});
    EXPECT_TRUE(co_await failure_of(exchange.body().write("lost", {.stop_token_ = stop.token()})) == cancelled);
    EXPECT_THROW((void)exchange.body().write("lost", {.timeout_ = 0ms}), std::invalid_argument);
    co_await exchange.body().write("kept");
    EXPECT_TRUE(co_await failure_of(exchange.body().end({}, {.stop_token_ = stop.token()})) == cancelled);
    co_await exchange.body().end();
    auto response = co_await exchange.response();
    const auto echoed = co_await response.text();
    EXPECT_EQ(echoed.text(), "echo:kept");
}

// Waiting for the final head is a read: its timeout ends only that wait.
[[nodiscard]] inline ruvia::task<void> check_response_wait(ruvia::http_client_ref client) {
    auto exchange = co_await client.post("/operation/late-head").upload({});
    co_await exchange.body().end();
    EXPECT_TRUE(co_await failure_of(exchange.response({.timeout_ = interruption})) == timed_out);
    auto response = co_await exchange.response();
    const auto text = co_await response.text();
    EXPECT_EQ(text.text(), "late");
}

// A tunnel read timeout keeps the tunnel; stopped output sends nothing.
[[nodiscard]] inline ruvia::task<void> check_tunnel(ruvia::http_client_ref client) {
    auto opened = co_await client.connect("operation.test:443").open();
    auto* tunnel = opened.tunnel();
    EXPECT_TRUE(tunnel != nullptr);
    if (tunnel == nullptr) {
        co_return;
    }
    EXPECT_TRUE(co_await failure_of(tunnel->read({.timeout_ = interruption})) == timed_out);
    ruvia::stop_source stop;
    stop.request_stop();
    EXPECT_TRUE(co_await failure_of(tunnel->write("lost", {.stop_token_ = stop.token()})) == cancelled);
    EXPECT_TRUE(co_await failure_of(tunnel->finish({.stop_token_ = stop.token()})) == cancelled);
    EXPECT_THROW((void)tunnel->text({.timeout_ = 0ms}), std::invalid_argument);
    co_await tunnel->write("ping");
    co_await tunnel->finish();
    std::string echoed;
    while (auto chunk = co_await tunnel->text()) {
        echoed.append(*chunk);
    }
    EXPECT_EQ(echoed, "ping");
}

[[nodiscard]] inline ruvia::task<void> exercise(ruvia::http_client_ref client, const ruvia::worker_ref& worker) {
    co_await check_body_reads(client, worker);
    co_await check_body_collection(client);
    co_await check_upload_abort(client, worker, false);
    co_await check_upload_abort(client, worker, true);
    co_await check_stopped_upload(client);
    co_await check_response_wait(client);
    co_await check_tunnel(client);
}

}  // namespace ruvia::test::client_operations
