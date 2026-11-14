#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

#include "app.h"
#include "context.h"
#include "controller.h"
#include "http_server_fixture.h"
#include "streaming.h"
#include "worker_task_fixture.h"
// Idle and completion deadlines of server tunnels and WebSockets, and the
// inbound budget over WebSocket message reassembly. This executable owns its
// application with a short scan interval and small inbound budgets.

namespace {

using namespace std::chrono_literals;

constexpr auto idle_timeout = 400ms;
constexpr auto message_completion_timeout = 400ms;
// Fits the read buffer and a 512 KiB reassembly, not the 1 MiB the
// reassembly grows to past 512 KiB.
constexpr std::size_t connection_budget = 1024 * 1024;
constexpr std::size_t fragment_size = 64 * 1024;

[[nodiscard]] ruvia::server_config streaming_bounds_config() {
    return {
        .worker_count_ = 1,
        .connection_scan_interval_ = 10ms,
        .max_inbound_buffer_bytes_per_worker_ = 4 * connection_budget,
        .max_inbound_buffer_bytes_per_connection_ = connection_budget,
    };
}

class streaming_bounds_routes final {
    RUVIA_ROUTES_BEGIN(streaming_bounds_routes)
    const auto idle_tunnel = ruvia::http_tunnel_route_config{.idle_timeout_ = idle_timeout};
    RUVIA_CONNECT("idle-tunnel.test:443", echo_tunnel, idle_tunnel);
    const auto idle_websocket = ruvia::websocket_route_config{.lifecycle_ = {.idle_timeout_ = idle_timeout}};
    RUVIA_GET_WS("/idle-websocket", echo_websocket, idle_websocket);
    // Pings far more often than the message deadline, answered by the peer.
    const auto slow_message = ruvia::websocket_route_config{.lifecycle_ = {.heartbeat_ = {.ping_interval_ = 50ms, .pong_timeout_ = 2s}, .message_completion_timeout_ = message_completion_timeout}};
    RUVIA_GET_WS("/slow-message", echo_websocket, slow_message);
    RUVIA_GET_WS("/budget-websocket", echo_websocket);
    RUVIA_ROUTES_END

    ruvia::task<void> echo_tunnel(ruvia::context& context_value) {
        auto& tunnel_value = context_value.tunnel();
        try {
            while (const auto chunk = co_await tunnel_value.read()) {
                co_await tunnel_value.write(*chunk);
            }
        } catch (const std::system_error&) {
            // The idle timeout aborted the transport under this read or write.
        }
    }

    ruvia::task<void> echo_websocket(ruvia::context& context_value) {
        auto& websocket_value = context_value.websocket();
        while (auto message = co_await websocket_value.read()) {
            co_await websocket_value.send(std::move(*message));
        }
    }
};

struct server_frame final {
    unsigned opcode_;
    std::string payload_;
};

// A blocking peer over the test's io_context; every wait is bounded.
class raw_client final {
public:
    raw_client(asio::io_context& io, const asio::ip::tcp::endpoint& server_endpoint) :
        io_(io),
        socket_(io) {
        socket_.connect(server_endpoint);
    }

    // False once the server ended the connection.
    bool send(std::string_view bytes) {
        std::error_code error;
        asio::write(socket_, asio::buffer(bytes), error);
        return !error;
    }

    [[nodiscard]] std::optional<std::string> read_head() {
        std::optional<std::size_t> found;
        asio::async_read_until(socket_, asio::dynamic_buffer(input_), "\r\n\r\n", [&found](const std::error_code& error, std::size_t bytes) {
            if (!error) {
                found = bytes;
            }
        });
        run_for(5s);
        if (!found) {
            return std::nullopt;
        }
        std::string head(input_, 0, *found);
        input_.erase(0, *found);
        return head;
    }

    // nullopt when the stream ends (closed() turns true) or `timeout` passes.
    [[nodiscard]] std::optional<std::string> read_exactly(std::size_t size, std::chrono::milliseconds timeout) {
        if (!fill(size, timeout)) {
            return std::nullopt;
        }
        std::string bytes(input_, 0, size);
        input_.erase(0, size);
        return bytes;
    }

    // One unmasked server frame; nullopt as for read_exactly().
    [[nodiscard]] std::optional<server_frame> read_frame(std::chrono::milliseconds timeout) {
        if (!fill(2, timeout)) {
            return std::nullopt;
        }
        std::size_t header = 2;
        std::uint64_t length = static_cast<unsigned char>(input_[1]) & 0x7fU;
        if (length >= 126) {
            header += length == 126 ? 2 : 8;
            if (!fill(header, timeout)) {
                return std::nullopt;
            }
            length = 0;
            for (std::size_t index = 2; index != header; ++index) {
                length = (length << 8U) | static_cast<unsigned char>(input_[index]);
            }
        }
        if (!fill(header + static_cast<std::size_t>(length), timeout)) {
            return std::nullopt;
        }
        server_frame frame{static_cast<unsigned char>(input_[0]) & 0x0fU, input_.substr(header, static_cast<std::size_t>(length))};
        input_.erase(0, header + static_cast<std::size_t>(length));
        return frame;
    }

    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }

    void close() {
        std::error_code ignored;
        socket_.close(ignored);
    }

private:
    // Whether input_ holds `size` bytes; a timeout keeps what arrived.
    bool fill(std::size_t size, std::chrono::milliseconds timeout) {
        if (input_.size() >= size || closed_) {
            return input_.size() >= size;
        }
        std::optional<std::error_code> result;
        asio::async_read(socket_, asio::dynamic_buffer(input_), asio::transfer_exactly(size - input_.size()), [&result](const std::error_code& error, std::size_t) {
            result = error;
        });
        if (!run_for(timeout)) {
            return false;
        }
        if (result.has_value() && *result) {
            closed_ = true;
        }
        return input_.size() >= size;
    }

    // False when `timeout` passed before the pending operation completed.
    bool run_for(std::chrono::milliseconds timeout) {
        io_.restart();
        io_.run_for(timeout);
        if (io_.stopped()) {
            return true;
        }
        std::error_code ignored;
        socket_.cancel(ignored);
        io_.restart();
        io_.run();
        return false;
    }

    asio::io_context& io_;
    asio::ip::tcp::socket socket_;
    std::string input_{};
    bool closed_{false};
};

// A client frame under the all-zero mask, which leaves the payload as is.
[[nodiscard]] std::string masked_frame(unsigned first_byte, std::string_view payload) {
    std::string frame(1, static_cast<char>(first_byte));
    const auto size = payload.size();
    if (size < 126) {
        frame.push_back(static_cast<char>(0x80U | size));
    } else if (size <= 0xffffU) {
        frame.push_back(static_cast<char>(0xfeU));
        frame.push_back(static_cast<char>(size >> 8U));
        frame.push_back(static_cast<char>(size));
    } else {
        frame.push_back(static_cast<char>(0xffU));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>(static_cast<std::uint64_t>(size) >> static_cast<unsigned>(shift)));
        }
    }
    frame.append(4, '\0');
    frame.append(payload);
    return frame;
}

constexpr unsigned binary_first_fragment = 0x02;
constexpr unsigned continuation_fragment = 0x00;
constexpr unsigned final_continuation = 0x80;
constexpr unsigned text_message = 0x81;
constexpr unsigned pong_frame = 0x8a;
constexpr unsigned ping_opcode = 0x9;
constexpr unsigned close_opcode = 0x8;

[[nodiscard]] bool open_websocket(raw_client& client, std::string_view path) {
    client.send(
        "GET " + std::string(path) +
        " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    const auto head = client.read_head();
    return head && head->starts_with("HTTP/1.1 101 ");
}

[[nodiscard]] std::chrono::milliseconds elapsed_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

}  // namespace

TEST(Ruvia, tunnel_idle_timeout_aborts_a_tunnel_without_traffic) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, streaming_bounds_config());
    raw_client client(io, server.endpoint());
    client.send("CONNECT idle-tunnel.test:443 HTTP/1.1\r\nHost: idle-tunnel.test:443\r\n\r\n");
    const auto head = client.read_head();
    EXPECT_TRUE(head && head->starts_with("HTTP/1.1 200 "));
    // Traffic in both directions keeps the tunnel open well past its idle timeout.
    const auto start = std::chrono::steady_clock::now();
    while (elapsed_since(start) < 2 * idle_timeout) {
        EXPECT_TRUE(client.send("x"));
        EXPECT_TRUE(client.read_exactly(1, 2s) == std::optional<std::string>("x"));
        std::this_thread::sleep_for(50ms);
    }
    const auto idle_start = std::chrono::steady_clock::now();
    EXPECT_FALSE(client.read_exactly(1, 5s).has_value());
    EXPECT_TRUE(client.closed());
    EXPECT_GE(elapsed_since(idle_start), idle_timeout - 50ms);
    client.close();
    server.finish();
}

TEST(Ruvia, websocket_idle_timeout_aborts_a_session_without_traffic) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, streaming_bounds_config());
    raw_client client(io, server.endpoint());
    EXPECT_TRUE(open_websocket(client, "/idle-websocket"));
    const auto start = std::chrono::steady_clock::now();
    while (elapsed_since(start) < 2 * idle_timeout) {
        EXPECT_TRUE(client.send(masked_frame(text_message, "hi")));
        const auto echo = client.read_frame(2s);
        EXPECT_TRUE(echo && echo->opcode_ == 1 && echo->payload_ == "hi");
        std::this_thread::sleep_for(50ms);
    }
    const auto idle_start = std::chrono::steady_clock::now();
    // The session ends without a Close frame: an aborted transport.
    EXPECT_FALSE(client.read_frame(5s).has_value());
    EXPECT_TRUE(client.closed());
    EXPECT_GE(elapsed_since(idle_start), idle_timeout - 50ms);
    client.close();
    server.finish();
}

TEST(Ruvia, websocket_message_completion_timeout_is_not_extended_by_pongs_or_fragments) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, streaming_bounds_config());
    raw_client client(io, server.endpoint());
    EXPECT_TRUE(open_websocket(client, "/slow-message"));
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(client.send(masked_frame(binary_first_fragment, "a")));
    unsigned pongs = 0;
    auto last_fragment = start;
    // A peer that answers every Ping and keeps sending fragments, but never
    // the final one. Fragments are spaced beyond the Ping interval, so the
    // server finds the connection quiet often enough to probe it.
    while (!client.closed() && elapsed_since(start) < 5s) {
        if (const auto frame = client.read_frame(20ms)) {
            EXPECT_EQ(frame->opcode_, ping_opcode);
            if (frame->opcode_ == ping_opcode && client.send(masked_frame(pong_frame, frame->payload_))) {
                ++pongs;
            }
        }
        if (elapsed_since(last_fragment) >= 100ms) {
            client.send(masked_frame(continuation_fragment, "b"));
            last_fragment = std::chrono::steady_clock::now();
        }
    }
    EXPECT_TRUE(client.closed());
    EXPECT_GE(pongs, 2U);
    EXPECT_GE(elapsed_since(start), message_completion_timeout - 50ms);
    EXPECT_LT(elapsed_since(start), 5s);
    client.close();
    server.finish();
}

TEST(Ruvia, websocket_reassembly_counts_against_the_connection_inbound_budget) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, streaming_bounds_config());
    raw_client client(io, server.endpoint());
    EXPECT_TRUE(open_websocket(client, "/budget-websocket"));
    const std::string fragment(fragment_size, 'f');
    // A fragmented message within the budget is reassembled and echoed.
    EXPECT_TRUE(client.send(masked_frame(binary_first_fragment, fragment)));
    EXPECT_TRUE(client.send(masked_frame(final_continuation, fragment)));
    const auto echo = client.read_frame(5s);
    EXPECT_TRUE(echo && echo->opcode_ == 2 && echo->payload_ == fragment + fragment);
    // Far below the 16 MiB message limit, this message outgrows the
    // connection's inbound budget at its ninth fragment, which is the last
    // one sent, so the server consumed everything before it closes.
    EXPECT_TRUE(client.send(masked_frame(binary_first_fragment, fragment)));
    for (int index = 1; index != 9; ++index) {
        EXPECT_TRUE(client.send(masked_frame(continuation_fragment, fragment)));
    }
    // Close 1009 (RFC 6455 section 7.4.1), then the end of the transport.
    const auto close = client.read_frame(5s);
    EXPECT_TRUE(close && close->opcode_ == close_opcode && close->payload_.starts_with(std::string_view("\x03\xf1", 2)));
    EXPECT_FALSE(client.read_frame(5s).has_value());
    EXPECT_TRUE(client.closed());
    client.close();
    server.finish();
}
