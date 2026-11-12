#include "ruvia/web/http_server.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"

#include "tcp_port_reservation.h"
#include "test_tls_identity.h"
#include "udp_port_reservation.h"

namespace standalone_server_test {
using namespace std::chrono_literals;

// These registrations must never be consulted or sealed by an independent host.
class conflicting_routes final {
public:
    RUVIA_ROUTES_BEGIN(conflicting_routes)
    RUVIA_GET("/unused/:first", first);
    RUVIA_GET("/unused/:second", second);
    RUVIA_ROUTES_END
    ruvia::task<ruvia::http_response> first(ruvia::context& context) {
        co_return context.text("first");
    }
    ruvia::task<ruvia::http_response> second(ruvia::context& context) {
        co_return context.text("second");
    }
};

ruvia::task<void> answer(ruvia::request_stream& stream) {
    std::string body;
    while (auto chunk = co_await stream.body().text()) {
        body.append(*chunk);
    }
    if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
        co_await stream.response().write(body.empty() ? std::string_view("independent") : std::string_view(body));
        co_await stream.response().end();
    }
}

ruvia::task<void> check_response(ruvia::http_client& client, ruvia::http_protocol_version protocol) {
    auto response = co_await client.client().post("/echo").body("owned-loop").send();
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    EXPECT_EQ(response.protocol_version(), protocol);
    const auto content = co_await response.text();
    EXPECT_EQ(content.text(), "owned-loop");
}

ruvia::task<void> adopt_one(ruvia::http_server& server, asio::ip::tcp::acceptor& acceptor) {
    auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto completion) {
        acceptor.async_accept(std::move(completion));
    });
    if (accepted.error_code()) {
        throw std::system_error(accepted.error_code());
    }
    server.adopt({.address_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port()}, accepted.result().release());
}

ruvia::task<void> echo(ruvia::server_connection& connection) {
    while (auto chunk = co_await connection.read()) {
        co_await connection.write(*chunk);
    }
    co_await connection.finish();
}
}  // namespace standalone_server_test

using namespace standalone_server_test;

TEST(Ruvia, independent_servers_share_an_external_loop_without_routes_or_shared_shutdown) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation first(io), second(io), secure(io);
    ruvia::test::udp_port_reservation udp(io);
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server server(attachment.loop(), {.request_stream_ = &answer});
        ruvia::http_server other(attachment.loop(), {.request_stream_ = &answer});
        const auto plain = first.endpoint().port();
        const auto another = second.endpoint().port();
        server.listen({.address_ = "127.0.0.1", .http_ = plain, .dispatch_ = ruvia::listener_dispatch::request_stream});
        ruvia::listen_config tls{.address_ = "127.0.0.1", .http_ = secure.endpoint().port(), .https_ = secure.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
        tls.http3_ = udp.endpoint().port();
#endif
        server.listen(std::move(tls));
        other.listen({.address_ = "127.0.0.1", .http_ = another, .dispatch_ = ruvia::listener_dispatch::request_stream});
        std::exception_ptr failure;
        try {
            co_await server.start();
            co_await other.start();
            struct target {
                std::string origin_;
                ruvia::http_client_protocol protocol_;
                ruvia::http_protocol_version version_;
            };
            std::vector<target> targets{
                {"http://127.0.0.1:" + std::to_string(plain), ruvia::http_client_protocol::http1_only, ruvia::http_protocol_version::http11},
                {"http://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_protocol::http1_only, ruvia::http_protocol_version::http11},
                {"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_protocol::http1_only, ruvia::http_protocol_version::http11},
#if defined(RUVIA_ENABLE_HTTP2)
                {"http://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_protocol::http2_only, ruvia::http_protocol_version::http2},
                {"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_protocol::http2_only, ruvia::http_protocol_version::http2},
#endif
#if defined(RUVIA_ENABLE_HTTP3)
                {"https://127.0.0.1:" + std::to_string(udp.endpoint().port()), ruvia::http_client_protocol::http3_only, ruvia::http_protocol_version::http3},
#endif
            };
            for (const auto& target : targets) {
                ruvia::http_client_config config{.base_url_ = target.origin_};
                config.protocol_ = target.protocol_;
                config.tls_.ca_file_ = identity.ca_file_;
                ruvia::http_client client(attachment.loop(), config);
                std::exception_ptr client_failure;
                try {
                    co_await check_response(client, target.version_);
                } catch (...) {
                    client_failure = std::current_exception();
                }
                co_await client.shutdown();
                if (client_failure) std::rethrow_exception(client_failure);
            }
            co_await server.shutdown();
            EXPECT_TRUE(attachment.loop().accepting());
            const auto delay = co_await ruvia::sleep_for(other.worker(), 1ms);
            EXPECT_EQ(delay, ruvia::timer_sleep_result::elapsed);
            const std::string origin = "http://127.0.0.1:" + std::to_string(another);
            ruvia::http_client client(attachment.loop(), {.base_url_ = origin});
            try {
                co_await check_response(client, ruvia::http_protocol_version::http11);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        } catch (...) {
            failure = std::current_exception();
        }
        co_await server.shutdown();
        co_await other.shutdown();
        attachment.stop();
        if (failure) std::rethrow_exception(failure);
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

TEST(Ruvia, independent_server_adopts_connected_sockets_for_raw_and_http_services) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor raw(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::acceptor http(io, {asio::ip::address_v4::loopback(), 0});
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server server(attachment.loop(), {.request_stream_ = &answer, .connection_ = &echo});
        server.add_connection_service({.address_ = "127.0.0.1", .http_ = raw.local_endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection});
        server.add_connection_service({.address_ = "127.0.0.1", .http_ = http.local_endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        co_await server.start();
        ruvia::task_scope accepts(server.worker());
        accepts.spawn(adopt_one(server, raw));
        accepts.spawn(adopt_one(server, http));
        std::exception_ptr failure;
        const std::string origin = "http://127.0.0.1:" + std::to_string(http.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .protocol_ = ruvia::http_client_protocol::http1_only});
        asio::ip::tcp::socket peer(io);
        try {
            auto connected = co_await ruvia::async_asio([&](auto completion) { peer.async_connect(raw.local_endpoint(), std::move(completion)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const std::string message = "native ownership";
            auto sent = co_await ruvia::async_asio<std::size_t>([&](auto completion) { asio::async_write(peer, asio::buffer(message), std::move(completion)); });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
            std::array<char, 16> received{};
            auto read = co_await ruvia::async_asio<std::size_t>([&](auto completion) { asio::async_read(peer, asio::buffer(received), asio::transfer_exactly(message.size()), std::move(completion)); });
            if (read.error_code()) {
                throw std::system_error(read.error_code());
            }
            EXPECT_EQ(std::string_view(received.data(), read.result()), message);
            co_await check_response(client, ruvia::http_protocol_version::http11);
        } catch (...) {
            failure = std::current_exception();
        }
        raw.cancel();
        http.cancel();
        co_await accepts.join();
        co_await client.shutdown();
        co_await server.shutdown();
        EXPECT_EQ(server.stats().active_connections_, 0);
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

TEST(Ruvia, independent_server_failed_addition_releases_every_prepared_listener) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation good(io);
    asio::ip::tcp::acceptor busy(io, {asio::ip::address_v4::loopback(), 0});
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server server(attachment.loop(), {.request_stream_ = &answer});
        co_await server.start();
        const auto port = good.endpoint().port();
        bool rejected = false;
        try {
            server.listen({.address_ = "127.0.0.1", .http_ = port, .https_ = busy.local_endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream});
        } catch (const std::system_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        server.listen({.address_ = "127.0.0.1", .http_ = port, .dispatch_ = ruvia::listener_dispatch::request_stream});
        const std::string origin = "http://127.0.0.1:" + std::to_string(port);
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin});
        std::exception_ptr failure;
        try {
            co_await check_response(client, ruvia::http_protocol_version::http11);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        co_await server.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

TEST(Ruvia, independent_server_loop_retirement_joins_pending_adopted_connection) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket peer(io);
    peer.connect(acceptor.local_endpoint());
    auto accepted = acceptor.accept();
    bool cancelled = false;
    bool token_stopped = false;
    const auto run = [&]() -> ruvia::task<void> {
        const auto& worker = attachment.loop().handle();
        ruvia::worker_signal entered(worker);
        ruvia::http_server server(attachment.loop(), {.connection_ = [&](ruvia::server_connection& connection) -> ruvia::task<void> {
            entered.notify();
            try {
                (void)co_await connection.read();
            } catch (const std::system_error&) {
                cancelled = true;
            }
            token_stopped = connection.stop_token().stop_requested();
        }});
        server.add_connection_service({.address_ = "127.0.0.1", .http_ = acceptor.local_endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection});
        co_await server.start();
        server.adopt({.address_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port()}, accepted.release());
        co_await entered.wait();
        attachment.stop();
        co_await server.shutdown();
        EXPECT_EQ(server.stats().active_connections_, 0);
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(token_stopped);
    EXPECT_FALSE(attachment.valid());
}
