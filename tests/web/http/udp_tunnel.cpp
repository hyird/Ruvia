#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include <asio.hpp>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_tunnel.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace udp_tunnel_test {
struct udp_tunnel_observation {
    std::atomic<bool> eof_{};
    std::atomic<bool> read_overlap_rejected_{};
    std::atomic<bool> write_overlap_rejected_{};
    std::atomic<bool> capsule_bound_rejected_{};
    std::atomic<bool> failed_{};
    std::atomic<bool> dynamic_invoked_{};
    std::atomic<bool> ws_subprotocol_matched_{};
    std::atomic<bool> ws_peer_close_matched_{};
};

struct retained_tunnel_results {
    std::optional<ruvia::http_capsule> capsule_;
    std::optional<ruvia::http_datagram> datagram_;
    bool cold_operations_expired_{};
};

const auto udp_observation = std::make_shared<udp_tunnel_observation>();
[[maybe_unused]] const bool udp_state_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<udp_tunnel_observation>>(
        [] { return udp_observation; });
    ruvia::app().use_worker_state<retained_tunnel_results>(
        [] { return retained_tunnel_results{}; });
    return true;
}();

class udp_tunnel_routes final {
    RUVIA_ROUTES_BEGIN(udp_tunnel_routes)
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/target.test/53/", echo);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/cancel.test/53/", delayed);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/retained-capsule.test/53/", retain_capsule);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/retained-datagram.test/53/", retain_datagram);
    RUVIA_GET("/udp-tunnel-retained", take_retained);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/udp-tunnel/:host/:port", dynamic);
    RUVIA_GET("/udp-tunnel-sibling", sibling);
    const ruvia::websocket_route_config ws_config{
        .subprotocols_ = {"chat.v2", "chat.v1"}, .require_subprotocol_ = true};
    RUVIA_GET_WS("/udp-tunnel-ws", ws_inspect, ws_config);
    RUVIA_ROUTES_END

    ruvia::task<void> retain_capsule(ruvia::context& c) {
        auto& retained = c.worker_state<retained_tunnel_results>();
        auto& tunnel = c.tunnel();
        std::optional stream(tunnel.capsules());
        unsigned adapted_rejected{};
        try {
            (void)tunnel.capsules();
        } catch (const std::logic_error&) {
            ++adapted_rejected;
        }
        try {
            (void)tunnel.datagrams();
        } catch (const std::logic_error&) {
            ++adapted_rejected;
        }
        try {
            (void)tunnel.udp();
        } catch (const std::logic_error&) {
            ++adapted_rejected;
        }
        try {
            (void)tunnel.read();
        } catch (const std::logic_error&) {
            ++adapted_rejected;
        }
        try {
            (void)tunnel.write("raw");
        } catch (const std::logic_error&) {
            ++adapted_rejected;
        }
        retained.capsule_ = co_await stream->read();
        auto read = stream->read();
        auto write = stream->write(42, "unstarted");
        auto finish = stream->finish();
        co_await stream->finish();
        stream.reset();
        unsigned expired{};
        try {
            (void)co_await std::move(read);
        } catch (const std::logic_error&) {
            ++expired;
        }
        try {
            co_await std::move(write);
        } catch (const std::logic_error&) {
            ++expired;
        }
        try {
            co_await std::move(finish);
        } catch (const std::logic_error&) {
            ++expired;
        }
        retained.cold_operations_expired_ = expired == 3 && adapted_rejected == 5;
    }

    ruvia::task<void> retain_datagram(ruvia::context& c) {
        auto& retained = c.worker_state<retained_tunnel_results>();
        auto tunnel = c.tunnel().udp();
        retained.datagram_ = co_await tunnel.read();
        co_await tunnel.finish();
    }

    ruvia::task<ruvia::http_response> take_retained(ruvia::context& c) {
        auto& retained = c.worker_state<retained_tunnel_results>();
        if (retained.capsule_) {
            auto response = c.text(retained.cold_operations_expired_ && retained.capsule_->type() == 42 &&
                                           retained.capsule_->bytes().size() == retained.capsule_->text().size()
                                       ? retained.capsule_->text()
                                       : std::string_view("invalid-capsule-state"));
            retained.capsule_.reset();
            co_return response;
        }
        if (retained.datagram_) {
            auto response = c.text(retained.datagram_->bytes().size() == retained.datagram_->text().size()
                                       ? retained.datagram_->text()
                                       : std::string_view("invalid-datagram-state"));
            retained.datagram_.reset();
            co_return response;
        }
        co_return c.text("empty");
    }

    ruvia::task<void> dynamic(ruvia::context& c) {
        c.worker_state<std::shared_ptr<udp_tunnel_observation>>()->dynamic_invoked_ = true;
        auto tunnel = c.tunnel().udp();
        co_await tunnel.finish();
    }

    ruvia::task<void> echo(ruvia::context& c) {
        auto& observation = *c.worker_state<std::shared_ptr<udp_tunnel_observation>>();
        ruvia::http_capsule_config config{.max_capsule_length_ = 13};
        auto original = c.tunnel().udp(config);
        try {
            try {
                co_await original.send(std::string(13, 'x'));
            } catch (const std::length_error&) {
                observation.capsule_bound_rejected_ = true;
            }
            auto first_read = original.read();
            auto last_read = original.read();
            auto first_write = original.send(std::pmr::string("ready", c.pool()));
            auto last_write = original.send("second-ready");
            auto finish = original.finish();
            auto tunnel = std::move(original);
            {
                auto discarded = tunnel.send("discarded");
                static_cast<void>(discarded);
            }
            std::optional<ruvia::http_datagram> datagram;
            ruvia::task_scope reads(c.worker());
            const auto receive = [&]() -> ruvia::task<void> {
                datagram = co_await std::move(first_read);
            };
            reads.spawn(receive());
            try {
                auto overlap = tunnel.read();
                static_cast<void>(co_await std::move(overlap));
            } catch (const std::logic_error&) {
                observation.read_overlap_rejected_ = true;
            }
            ruvia::task_scope writes(c.worker());
            const auto send = [&]() -> ruvia::task<void> {
                co_await std::move(first_write);
            };
            writes.spawn(send());
            try {
                auto overlap = tunnel.send("overlap");
                co_await std::move(overlap);
            } catch (const std::logic_error&) {
                observation.write_overlap_rejected_ = true;
            }
            co_await writes.join();
            co_await std::move(last_write);
            co_await reads.join();
            observation.eof_ = !(co_await std::move(last_read));
            if (datagram) {
                co_await tunnel.send(datagram->payload());
            }
            co_await std::move(finish);
        } catch (...) {
            observation.failed_ = true;
            throw;
        }
    }

    ruvia::task<void> delayed(ruvia::context& c) {
        (void)co_await ruvia::sleep_for(c.worker(), std::chrono::milliseconds(30));
        auto tunnel = c.tunnel().udp();
        co_await tunnel.finish();
    }

    ruvia::task<ruvia::http_response> sibling(ruvia::context& c) {
        co_return c.text("sibling");
    }
    ruvia::task<void> ws_inspect(ruvia::context& c) {
        auto& observation = *c.worker_state<std::shared_ptr<udp_tunnel_observation>>();
        auto& ws = c.websocket();
        observation.ws_subprotocol_matched_ = ws.subprotocol() == "chat.v2";
        co_await ws.pong();
        while (auto message = co_await ws.read()) {
            co_await ws.send(std::move(*message));
        }
        observation.ws_peer_close_matched_ =
            ws.peer_close_code() == std::uint16_t{4001} && ws.peer_close_reason() == "client-done";
    }
};
// A raw HTTP/1.1 peer: the response head and any input already read after it.
struct upgraded_connection {
    asio::ip::tcp::socket socket_;
    std::string head_;
    std::string input_;
};

[[nodiscard]] upgraded_connection open_upgrade(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target, std::string_view fields) {
    upgraded_connection connection{asio::ip::tcp::socket(io), {}, {}};
    connection.socket_.connect(endpoint);
    std::string request = "GET ";
    request.append(target).append(" HTTP/1.1\r\nHost: localhost\r\n").append(fields).append("\r\n");
    asio::write(connection.socket_, asio::buffer(request));
    const auto head_size = asio::read_until(connection.socket_, asio::dynamic_buffer(connection.input_), "\r\n\r\n");
    connection.head_ = connection.input_.substr(0, head_size);
    connection.input_.erase(0, head_size);
    return connection;
}

[[nodiscard]] upgraded_connection open_udp_tunnel(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target) {
    return open_upgrade(io, endpoint, target, "Connection: Upgrade\r\nUpgrade: connect-udp\r\nCapsule-Protocol: ?1\r\n");
}

// Appends a capsule; a DATAGRAM capsule (type 0) carries context ID 0 first.
void append_capsule(std::string& output, std::uint64_t type, std::string_view payload) {
    const bool datagram = type == 0;
    std::array<char, 16> header{};
    const auto encoded = ruvia::encode_http_capsule_header(header, type, payload.size() + (datagram ? 1 : 0));
    output.append(header.data(), std::get<std::size_t>(encoded));
    if (datagram) {
        output.push_back('\0');
    }
    output.append(payload);
}

[[nodiscard]] std::uint64_t read_varint(std::string_view& input) {
    const auto length = std::size_t{1} << (static_cast<unsigned char>(input.front()) >> 6);
    std::uint64_t value = static_cast<unsigned char>(input.front()) & 0x3f;
    for (std::size_t index = 1; index != length; ++index) {
        value = (value << 8) | static_cast<unsigned char>(input[index]);
    }
    input.remove_prefix(length);
    return value;
}

[[nodiscard]] bool has_varint(std::string_view input) {
    return !input.empty() && input.size() >= (std::size_t{1} << (static_cast<unsigned char>(input.front()) >> 6));
}

// The payload of the next UDP datagram the server tunnels, or nullopt at EOF.
[[nodiscard]] std::optional<std::string> read_datagram(upgraded_connection& connection) {
    for (;;) {
        std::string_view view = connection.input_;
        if (has_varint(view)) {
            const auto type = read_varint(view);
            if (has_varint(view)) {
                const auto length = read_varint(view);
                if (view.size() >= length) {
                    if (type != 0 || length == 0 || view.front() != '\0') {
                        throw std::runtime_error("unexpected capsule");
                    }
                    std::string payload(view.substr(1, length - 1));
                    connection.input_.erase(0, connection.input_.size() - view.size() + length);
                    return payload;
                }
            }
        }
        std::array<char, 4096> input{};
        std::error_code ec;
        const auto read = connection.socket_.read_some(asio::buffer(input), ec);
        if (ec == asio::error::eof) {
            if (!connection.input_.empty()) {
                throw std::runtime_error("truncated capsule");
            }
            return std::nullopt;
        }
        if (ec) {
            throw std::system_error(ec);
        }
        connection.input_.append(input.data(), read);
    }
}

// One GET on its own connection; returns the whole response.
[[nodiscard]] std::string get_response(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target) {
    auto connection = open_upgrade(io, endpoint, target, "Connection: close\r\n");
    std::error_code ec;
    asio::read(connection.socket_, asio::dynamic_buffer(connection.input_), ec);
    return connection.head_ + connection.input_;
}

// A masked client WebSocket frame (RFC 6455 section 5.3) with a short payload.
[[nodiscard]] std::string client_frame(std::uint8_t opcode, std::string_view payload) {
    constexpr std::array<char, 4> mask{'\x12', '\x34', '\x56', '\x78'};
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    frame.push_back(static_cast<char>(0x80 | payload.size()));
    frame.append(mask.data(), mask.size());
    for (std::size_t index = 0; index != payload.size(); ++index) {
        frame.push_back(static_cast<char>(payload[index] ^ mask[index % mask.size()]));
    }
    return frame;
}

struct server_frame {
    std::uint8_t opcode_{};
    std::string payload_;
};

[[nodiscard]] server_frame read_frame(upgraded_connection& connection) {
    while (connection.input_.size() < 2 ||
           connection.input_.size() < 2 + (static_cast<unsigned char>(connection.input_[1]) & 0x7f)) {
        std::array<char, 1024> input{};
        connection.input_.append(input.data(), connection.socket_.read_some(asio::buffer(input)));
    }
    const auto length = static_cast<std::size_t>(static_cast<unsigned char>(connection.input_[1]) & 0x7f);
    server_frame frame{static_cast<std::uint8_t>(connection.input_[0] & 0x0f), connection.input_.substr(2, length)};
    connection.input_.erase(0, 2 + length);
    return frame;
}

[[nodiscard]] bool eventually(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag;
}
}  // namespace udp_tunnel_test

using udp_tunnel_test::udp_observation;

RUVIA_TEST(http1_udp_tunnel_capsules_echo_preserve_siblings_and_owned_lifetime) {
    using namespace udp_tunnel_test;
    auto& io = ruvia::test::new_test_io_context();
    const auto& observation = udp_observation;
    observation->eof_ = false;
    observation->read_overlap_rejected_ = false;
    observation->write_overlap_rejected_ = false;
    observation->capsule_bound_rejected_ = false;
    observation->failed_ = false;
    ruvia::test::http_server_fixture server(io);
    {
        auto tunnel = open_udp_tunnel(io, server.endpoint(), "/.well-known/masque/udp/target.test/53/");
        RUVIA_CHECK(tunnel.head_.starts_with("HTTP/1.1 101 "));
        RUVIA_CHECK(tunnel.head_.find("\r\ncapsule-protocol: ?1\r\n") != std::string::npos ||
                    tunnel.head_.find("\r\nCapsule-Protocol: ?1\r\n") != std::string::npos);
        for (const auto expected : {std::string_view("ready"), std::string_view("second-ready")}) {
            const auto ready = read_datagram(tunnel);
            RUVIA_CHECK(ready.has_value() && *ready == expected);
        }
        std::string query;
        append_capsule(query, 0, "dns-query");
        asio::write(tunnel.socket_, asio::buffer(query));
        RUVIA_CHECK(get_response(io, server.endpoint(), "/udp-tunnel-sibling").ends_with("\r\n\r\nsibling"));
        tunnel.socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
        const auto echoed = read_datagram(tunnel);
        RUVIA_CHECK(echoed.has_value() && *echoed == "dns-query");
        RUVIA_CHECK(!read_datagram(tunnel));
    }
    RUVIA_CHECK(eventually(observation->eof_));
    RUVIA_CHECK(observation->capsule_bound_rejected_);
    RUVIA_CHECK(observation->read_overlap_rejected_);
    RUVIA_CHECK(observation->write_overlap_rejected_);
    RUVIA_CHECK(!observation->failed_);
    RUVIA_CHECK(get_response(io, server.endpoint(), "/udp-tunnel-sibling").ends_with("\r\n\r\nsibling"));
    {
        auto delayed = open_udp_tunnel(io, server.endpoint(), "/.well-known/masque/udp/cancel.test/53/");
        RUVIA_CHECK(delayed.head_.starts_with("HTTP/1.1 101 "));
        RUVIA_CHECK(!read_datagram(delayed));
    }
    server.finish();
}

// A connect-udp upgrade target is an origin path like any other: dot segments
// are refused before any tunnel route can capture them, and a path with no
// connect-udp route serves its GET route with the Upgrade ignored (RFC 9110 §7.8).
RUVIA_TEST(http1_udp_tunnel_upgrade_routes_only_normalized_tunnel_paths) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    udp_observation->dynamic_invoked_ = false;
    const auto exchange = [&](std::string_view path) {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const auto request = std::string("GET ") + std::string(path) +
                             " HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade, close\r\n"
                             "Upgrade: connect-udp\r\nCapsule-Protocol: ?1\r\n\r\n";
        asio::write(client, asio::buffer(request));
        std::string response;
        std::array<char, 1024> input{};
        std::error_code ec;
        while (!ec) {
            const auto read = client.read_some(asio::buffer(input), ec);
            response.append(input.data(), read);
        }
        return response;
    };
    RUVIA_CHECK(exchange("/udp-tunnel/../53").starts_with("HTTP/1.1 404 "));
    RUVIA_CHECK(exchange("/udp-tunnel/%2E%2E/53").starts_with("HTTP/1.1 404 "));
    RUVIA_CHECK(!udp_observation->dynamic_invoked_);
    const auto sibling = exchange("/udp-tunnel-sibling");
    RUVIA_CHECK(sibling.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(sibling.ends_with("\r\n\r\nsibling"));
    RUVIA_CHECK(exchange("/udp-tunnel/target.test/53").starts_with("HTTP/1.1 101 "));
    RUVIA_CHECK(udp_observation->dynamic_invoked_);
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        constexpr std::string_view h10_request =
            "GET /udp-tunnel/target.test/53 HTTP/1.0\r\nHost: localhost\r\n"
            "Connection: Upgrade, close\r\nUpgrade: connect-udp\r\nCapsule-Protocol: ?1\r\n\r\n";
        asio::write(client, asio::buffer(h10_request));
        std::string response;
        std::array<char, 1024> input{};
        std::error_code ec;
        while (!ec) {
            const auto read = client.read_some(asio::buffer(input), ec);
            response.append(input.data(), read);
        }
        RUVIA_CHECK(response.starts_with("HTTP/1.1 404 "));
    }
    server.finish();
}

// RFC 9110 section 7.8: Upgrade together with Expect: 100-continue gets a
// 100 (Continue) before the 101 (Switching Protocols), for a CONNECT-UDP
// upgrade exactly as for a WebSocket one, even though no content follows.
RUVIA_TEST(http1_udp_tunnel_upgrade_with_continue_expectation_sends_100_before_101) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    constexpr std::string_view request =
        "GET /udp-tunnel/target.test/53 HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade, close\r\n"
        "Upgrade: connect-udp\r\nCapsule-Protocol: ?1\r\nExpect: 100-continue\r\n\r\n";
    asio::write(client, asio::buffer(request));
    std::string response;
    std::array<char, 1024> input{};
    std::error_code ec;
    while (!ec) {
        const auto read = client.read_some(asio::buffer(input), ec);
        response.append(input.data(), read);
    }
    RUVIA_CHECK(response.starts_with("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 101 "));
    client.close();
    server.finish();
}

RUVIA_TEST(server_capsule_and_udp_results_outlive_unread_input_and_connections) {
    using namespace udp_tunnel_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const bool capsule : {true, false}) {
        {
            auto tunnel = open_udp_tunnel(io, server.endpoint(),
                capsule ? "/.well-known/masque/udp/retained-capsule.test/53/"
                        : "/.well-known/masque/udp/retained-datagram.test/53/");
            RUVIA_CHECK(tunnel.head_.starts_with("HTTP/1.1 101 "));
            // One transport write includes a second complete capsule. The
            // handler reads only the first, never advancing to EOF.
            std::string batch;
            for (const char fill : {'r', 's'}) {
                append_capsule(batch, capsule ? 42 : 0, std::string(1024, fill));
            }
            asio::write(tunnel.socket_, asio::buffer(batch));
            tunnel.socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
            RUVIA_CHECK(!read_datagram(tunnel));
        }
        // Wait for actual connection retirement, not just the peer FIN.
        // Worker state must keep the result alive through this barrier.
        server.finish();
        for (const auto& expected : {std::string(1024, 'r'), std::string("empty")}) {
            RUVIA_CHECK(get_response(io, server.endpoint(), "/udp-tunnel-retained").ends_with("\r\n\r\n" + expected));
        }
        server.finish();
    }
}

RUVIA_TEST(server_websocket_exposes_negotiated_subprotocol_and_peer_close) {
    using namespace udp_tunnel_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    udp_observation->ws_subprotocol_matched_ = false;
    udp_observation->ws_peer_close_matched_ = false;
    {
        auto ws = open_upgrade(io, server.endpoint(), "/udp-tunnel-ws",
            "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: chat.v1, chat.v2\r\n");
        RUVIA_CHECK(ws.head_.starts_with("HTTP/1.1 101 "));
        RUVIA_CHECK(ws.head_.find(": chat.v2\r\n") != std::string::npos);
        const auto text = client_frame(0x1, "ping-echo");
        asio::write(ws.socket_, asio::buffer(text));
        auto frame = read_frame(ws);
        if (frame.opcode_ == 0xA) {
            frame = read_frame(ws);
        }
        RUVIA_CHECK(frame.opcode_ == 0x1 && frame.payload_ == "ping-echo");
        const auto close_frame = client_frame(0x8, std::string("\x0f\xa1", 2) + "client-done");
        asio::write(ws.socket_, asio::buffer(close_frame));
        const auto close_reply = read_frame(ws);
        RUVIA_CHECK(close_reply.opcode_ == 0x8);
    }
    RUVIA_CHECK(eventually(udp_observation->ws_subprotocol_matched_));
    RUVIA_CHECK(eventually(udp_observation->ws_peer_close_matched_));
    server.finish();
}
