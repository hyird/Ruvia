#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

#include "app.h"
#include "context.h"
#include "controller.h"
#include "http_client.h"
#include "http_server_fixture.h"
#include "streaming.h"
#include "worker_task_fixture.h"
#include "worker_timer.h"
#if defined(RUVIA_ENABLE_HTTP2)
#include "http2_frame.h"
#include "http2_hpack.h"
#endif
// Inbound buffer budgets on the streamed request body and CONNECT tunnel
// paths. This executable owns its application with small inbound budgets.

namespace {

constexpr auto scan_interval = std::chrono::milliseconds(10);
// A tunnel's reused read window takes a little over 4 KiB: one fits the
// connection budget with room for a small chunk, two fit the worker budget,
// three do not. A streamed body's 8 KiB read buffer fits neither.
constexpr std::size_t connection_budget = 6000;
constexpr std::size_t worker_budget = 10000;

[[nodiscard]] ruvia::server_config inbound_memory_config() {
    return {
        .connection_scan_interval_ = scan_interval,
        .max_inbound_buffer_bytes_per_worker_ = worker_budget,
        .max_inbound_buffer_bytes_per_connection_ = connection_budget,
    };
}

// What the held-view route saw: the size of its first read, the test's
// release, and whether the view of that read still held its bytes then.
struct held_view_observation final {
    std::atomic<std::size_t> first_read_bytes_{};
    std::atomic<bool> release_{};
    std::atomic<bool> intact_{};
};
held_view_observation& held_view() {
    static held_view_observation value;
    return value;
}

// Releases the held-view route when its case ends, also by a failed assertion
// or a throwing read, so the handler is never left parked.
struct held_view_release final {
    ~held_view_release() {
        held_view().release_ = true;
    }
};

class inbound_memory_routes final {
    RUVIA_ROUTES_BEGIN(inbound_memory_routes)
    RUVIA_POST("/inbound-buffered", buffered_body);
    RUVIA_POST("/inbound-stream", stream_body, ruvia::stream_body);
    RUVIA_POST("/inbound-held-view", held_view_body, ruvia::stream_body);
    RUVIA_CONNECT("inbound-tunnel.test:443", tunnel);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> buffered_body(ruvia::context& context_value) {
        const auto body = co_await context_value.req().text();
        co_return context_value.text(std::string(std::to_string(body.size())));
    }

    ruvia::task<ruvia::http_response> stream_body(ruvia::context& context_value) {
        auto& reader = context_value.request_body();
        std::size_t received = 0;
        while (const auto chunk = co_await reader.read()) {
            received += chunk->size();
        }
        co_return context_value.text(std::string(std::to_string(received)));
    }

    // Keeps the view of its first read until the test has seen the rest of
    // the body exhaust the inbound budget, then reads again.
    ruvia::task<ruvia::http_response> held_view_body(ruvia::context& context_value) {
        auto& reader = context_value.request_body();
        const auto first = co_await reader.read();
        if (!first || first->empty()) {
            throw std::runtime_error("held view request carried no content");
        }
        const std::string_view view(reinterpret_cast<const char*>(first->data()), first->size());
        const std::string copy(view);
        held_view().first_read_bytes_ = view.size();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!held_view().release_.load()) {
            if (std::chrono::steady_clock::now() >= deadline || co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(1), context_value.stop_token()) != ruvia::timer_sleep_result::elapsed) {
                throw std::runtime_error("held view was not released");
            }
        }
        held_view().intact_ = view == copy;
        // The body failed meanwhile: this read reports the exhausted budget.
        (void)co_await reader.read();
        co_return context_value.text("unreachable");
    }

    // Acknowledges each borrowed chunk and reports inbound budget exhaustion.
    ruvia::task<void> tunnel(ruvia::context& context_value) {
        auto& tunnel_value = context_value.tunnel();
        bool limited = false;
        for (;;) {
            std::optional<std::span<const std::byte>> chunk;
            try {
                chunk = co_await tunnel_value.read();
            } catch (const std::bad_alloc&) {
                limited = true;
            }
            if (!chunk) {
                break;
            }
            co_await tunnel_value.write("ok\n");
        }
        co_await tunnel_value.write(limited ? "limited\n" : "end\n");
    }
};

class test_client final {
public:
    test_client(asio::io_context& io, const asio::ip::tcp::endpoint& server_endpoint) :
        io_(io),
        socket_(io) {
        socket_.connect(server_endpoint);
    }

    void send(std::string_view bytes) {
        asio::write(socket_, asio::buffer(bytes));
    }

    void finish_sending() {
        socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
    }

    // Everything up to and including `delimiter`, or nullopt when the stream
    // ends or 5 s pass first.
    [[nodiscard]] std::optional<std::string> read_until(std::string_view delimiter) {
        std::optional<std::size_t> found;
        asio::async_read_until(socket_, asio::dynamic_buffer(input_), delimiter, [&found](const std::error_code& error, std::size_t bytes) {
            if (!error) {
                found = bytes;
            }
        });
        run();
        if (!found) {
            return std::nullopt;
        }
        std::string result(std::string_view(input_).substr(0, *found));
        input_.erase(0, *found);
        return result;
    }

    // Everything until the server ends the stream (or 5 s pass).
    [[nodiscard]] std::string read_to_end() {
        asio::async_read(socket_, asio::dynamic_buffer(input_), [](const std::error_code&, std::size_t) {});
        run();
        return std::exchange(input_, std::string{});
    }

    void close() {
        std::error_code ignored;
        socket_.close(ignored);
    }

private:
    void run() {
        io_.restart();
        io_.run_for(std::chrono::seconds(5));
        if (!io_.stopped()) {
            std::error_code ignored;
            socket_.cancel(ignored);
            io_.restart();
            io_.run();
        }
    }

    asio::io_context& io_;
    asio::ip::tcp::socket socket_;
    std::string input_{};
};

constexpr std::string_view connect_request = "CONNECT inbound-tunnel.test:443 HTTP/1.1\r\nHost: inbound-tunnel.test:443\r\n\r\n";

[[nodiscard]] bool open_tunnel(test_client& client) {
    client.send(connect_request);
    const auto head = client.read_until("\r\n\r\n");
    return head && head->starts_with("HTTP/1.1 200 ");
}

}  // namespace

TEST(Ruvia, streamed_request_body_reads_count_against_the_connection_inbound_budget) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());
    {
        // A body that arrives with its head needs no reader buffer.
        test_client client(io, server.endpoint());
        client.send(
            "POST /inbound-stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16\r\nConnection: close\r\n\r\n"
            "0123456789abcdef");
        const auto response = client.read_to_end();
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(response.ends_with("\r\n\r\n16"));
        client.close();
    }
    {
        // The reader's 8 KiB socket buffer exceeds the connection budget.
        test_client client(io, server.endpoint());
        client.send("POST /inbound-stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: 20000\r\nConnection: close\r\n\r\n" + std::string(20000, 'b'));
        const auto response = client.read_to_end();
        EXPECT_TRUE(response.starts_with("HTTP/1.1 503 "));
        EXPECT_TRUE(response.find("inbound_buffer_limit") != std::string::npos);
        client.close();
    }
    server.finish();
}

TEST(Ruvia, tunnel_reads_count_against_the_connection_inbound_budget) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());
    test_client client(io, server.endpoint());
    EXPECT_TRUE(open_tunnel(client));
    client.send(std::string(20000, 't'));
    client.finish_sending();
    const auto output = client.read_to_end();
    EXPECT_TRUE(output.ends_with("limited\n"));
    client.close();
    server.finish();
}

TEST(Ruvia, tunnel_reads_count_against_the_worker_inbound_budget) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());
    // Two tunnels each hold their read window, within their own connection
    // budgets; a third tunnel's first read exceeds the worker's.
    test_client first(io, server.endpoint());
    test_client second(io, server.endpoint());
    for (auto* const client : {&first, &second}) {
        EXPECT_TRUE(open_tunnel(*client));
        client->send("a");
        EXPECT_TRUE(client->read_until("\n") == std::optional<std::string>(std::in_place, "ok\n"));
    }
    test_client third(io, server.endpoint());
    EXPECT_TRUE(open_tunnel(third));
    EXPECT_TRUE(third.read_to_end() == "limited\n");
    third.close();
    for (auto* const client : {&first, &second}) {
        client->finish_sending();
        EXPECT_TRUE(client->read_to_end() == "end\n");
        client->close();
    }
    server.finish();
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http2_request_body_inbound_budget_exhaustion_answers_with_503) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());

    auto& client_io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(client_io);
    const auto url = "http://127.0.0.1:" + std::to_string(server.endpoint().port());

    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = url, .versions_ = {.http1_ = false, .http2_ = true}});
        const std::string payload(12000, 'h');
        for (const std::string_view path : {"/inbound-buffered", "/inbound-stream"}) {
            auto response = co_await client.client().post(path).body(payload).send();
            EXPECT_EQ(response.status(), ruvia::http_status::service_unavailable);
            const auto body = co_await response.bytes();
            EXPECT_NE(body.text().find("inbound_buffer_limit"), std::string_view::npos);
        }
        co_await client.shutdown();
        attachment.request_stop();
    };
    auto root = attachment.loop().start(run());
    std::jthread driver([&] {
        attachment.run();
    });
    driver.join();
    std::move(root).get();
    server.finish();
}

// The view of a streamed body read stays valid until the next read starts
// (body_reader::read). Exhausting the inbound budget meanwhile fails the body
// and frees the content nobody read, not the chunk the handler still holds.
// The request is written as frames: a PING sent after the body is
// acknowledged only once the session has handled every frame before it.
TEST(Ruvia, http2_inbound_budget_exhaustion_keeps_the_last_body_view_valid) {
    auto& state = held_view();
    state.first_read_bytes_ = 0;
    state.release_ = false;
    state.intact_ = false;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());
    const held_view_release release_at_exit{};
    asio::ip::tcp::socket socket(io);
    socket.connect(server.endpoint());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string wire(ruvia::http2_client_preface);
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    asio::write(socket, asio::buffer(wire));
    for (;;) {
        const auto frame = ruvia::test::read_frame(socket, deadline);
        if (frame.header_.type_ == static_cast<unsigned>(ruvia::http2_frame_type::settings) && (frame.header_.flags_ & 1U) == 0) {
            break;
        }
    }
    wire.clear();
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 1, 0, {});
    std::string block;
    for (const auto& field : std::array<std::pair<std::string_view, std::string_view>, 4>{{{":method", "POST"}, {":scheme", "http"}, {":authority", "localhost"}, {":path", "/inbound-held-view"}}}) {
        ruvia::hpack_encoder::encode_header(block, field.first, field.second);
    }
    // END_HEADERS, then the chunk the handler reads and keeps.
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 4, 1, block);
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::data, 0, 1, std::string(512, 'v'));
    asio::write(socket, asio::buffer(wire));
    while (state.first_read_bytes_.load() == 0) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // The rest outgrows the connection budget while the handler is parked.
    wire.clear();
    const std::string rest(4096, 'r');
    for (unsigned sent = 0; sent != 4; ++sent) {
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::data, sent == 3 ? 1 : 0, 1, rest);
    }
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, "heldview");
    asio::write(socket, asio::buffer(wire));
    for (;;) {
        const auto frame = ruvia::test::read_frame(socket, deadline);
        if (frame.header_.type_ == static_cast<unsigned>(ruvia::http2_frame_type::ping) && (frame.header_.flags_ & 1U) != 0) {
            break;
        }
    }
    state.release_ = true;

    block.clear();
    for (bool complete = false; !complete;) {
        const auto frame = ruvia::test::read_frame(socket, deadline);
        const auto type = frame.header_.type_;
        // Credit of the dropped content returns as WINDOW_UPDATE.
        if (frame.header_.stream_id_ != 1 || type == static_cast<unsigned>(ruvia::http2_frame_type::window_update)) {
            continue;
        }
        ASSERT_TRUE(type == static_cast<unsigned>(ruvia::http2_frame_type::headers) || type == static_cast<unsigned>(ruvia::http2_frame_type::continuation));
        block.append(frame.payload_);
        complete = (frame.header_.flags_ & 4U) != 0;
    }
    std::string status;
    ruvia::hpack_decoder decoder;
    const auto error = decoder.decode(block, [&status](std::string_view name, std::string_view value) {
        if (name == ":status") {
            status = value;
        }
        return true;
    });
    EXPECT_FALSE(error.has_value());
    EXPECT_EQ(status, "503");
    EXPECT_TRUE(state.intact_.load());
    std::error_code ignored;
    socket.close(ignored);
    server.finish();
}
#endif
