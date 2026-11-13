#include "ruvia/web/http_server.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"

#include "tcp_port_reservation.h"
#include "test_tls_identity.h"
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
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &preparation});
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
                ruvia::http_client_versions versions_;
                ruvia::http_protocol_version version_;
            };
            std::vector<target> targets{
                {"http://127.0.0.1:" + std::to_string(plain), ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11},
                {"http://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11},
                {"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11},
#if defined(RUVIA_ENABLE_HTTP2)
                {"http://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http1_ = false}, ruvia::http_protocol_version::http2},
                {"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http1_ = false}, ruvia::http_protocol_version::http2},
#endif
#if defined(RUVIA_ENABLE_HTTP3)
                {"https://127.0.0.1:" + std::to_string(udp.endpoint().port()), ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true}, ruvia::http_protocol_version::http3},
#endif
            };
            for (const auto& target : targets) {
                ruvia::http_client_config config{.base_url_ = target.origin_};
                config.versions_ = target.versions_;
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
            EXPECT_TRUE(attachment.loop().worker().accepting());
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
    std::move(root).get();
}

TEST(Ruvia, independent_server_adopts_connected_sockets_for_raw_and_http_services) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor raw(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::acceptor http(io, {asio::ip::address_v4::loopback(), 0});
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server server(attachment.loop(), {.request_stream_ = &answer, .connection_ = &echo});
        co_await server.add_connection_service({.address_ = "127.0.0.1", .http_ = raw.local_endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection});
        co_await server.add_connection_service({.address_ = "127.0.0.1", .http_ = http.local_endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        co_await server.start();
        ruvia::task_scope accepts(server.worker());
        accepts.spawn(adopt_one(server, raw));
        accepts.spawn(adopt_one(server, http));
        std::exception_ptr failure;
        const std::string origin = "http://127.0.0.1:" + std::to_string(http.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = ruvia::http_client_versions{.http2_ = false}});
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
    std::move(root).get();
}

TEST(Ruvia, independent_server_failed_addition_releases_every_prepared_listener) {
    asio::io_context io;
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &preparation});
    ruvia::test::tcp_port_reservation good(io);
    asio::ip::tcp::acceptor busy(io, {asio::ip::address_v4::loopback(), 0});
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server server(attachment.loop(), {.request_stream_ = &answer});
        co_await server.start();
        const auto port = good.endpoint().port();
        bool rejected = false;
        try {
            co_await server.add_listener({.address_ = "127.0.0.1", .http_ = port, .https_ = busy.local_endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream});
        } catch (const std::system_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        co_await server.add_listener({.address_ = "127.0.0.1", .http_ = port, .dispatch_ = ruvia::listener_dispatch::request_stream});
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
    std::move(root).get();
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
        const auto& worker = attachment.loop().worker();
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
        co_await server.add_connection_service({.address_ = "127.0.0.1", .http_ = acceptor.local_endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection});
        co_await server.start();
        server.adopt({.address_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port()}, accepted.release());
        co_await entered.wait();
        attachment.stop();
        co_await server.shutdown();
        EXPECT_EQ(server.stats().active_connections_, 0);
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(token_stopped);
    EXPECT_FALSE(attachment.valid());
}

TEST(Ruvia, independent_server_cold_runtime_addition_owns_configuration) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server server(attachment.loop(), {.request_stream_ = &answer});
        co_await server.start();
        std::string address = "127.0.0.1";
        ruvia::listen_config config{.address_ = address, .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream};
        auto addition = server.add_listener(config);
        address.assign("not-an-address");
        config.http_.reset();
        co_await std::move(addition);
        EXPECT_THROW(server.listen(config), std::logic_error);
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
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
    std::move(root).get();
}

TEST(Ruvia, independent_server_discarded_cold_addition_closes_inherited_socket) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor inherited(io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = inherited.local_endpoint();
    const auto socket = inherited.release();
    ruvia::http_server server(attachment.loop(), {.request_stream_ = &answer});
    {
        auto discarded = server.add_listener({.address_ = "127.0.0.1", .http_ = endpoint.port(), .dispatch_ = ruvia::listener_dispatch::request_stream, .inherited_sockets_ = ruvia::listener_sockets{.http_ = {socket}}});
        asio::ip::tcp::acceptor competing(io);
        competing.open(asio::ip::tcp::v4());
        std::error_code error;
        competing.bind(endpoint, error);
        EXPECT_EQ(error, asio::error::address_in_use);
    }
    asio::ip::tcp::acceptor replacement(io);
    replacement.open(asio::ip::tcp::v4());
    std::error_code error;
    replacement.bind(endpoint, error);
    EXPECT_FALSE(error);
    const auto shutdown = [&]() -> ruvia::task<void> {
        co_await server.shutdown();
        attachment.stop();
    };
    auto completed = attachment.loop().start(shutdown());
    attachment.run();
    std::move(completed).get();
}

TEST(Ruvia, independent_server_cold_shutdown_joins_after_host_destruction) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket peer(io);
    peer.connect(acceptor.local_endpoint());
    auto accepted = acceptor.accept();
    bool retired = false;
    const auto run = [&]() -> ruvia::task<void> {
        const auto worker = attachment.loop().worker();
        ruvia::worker_signal entered(worker);
        auto server = std::make_unique<ruvia::http_server>(attachment.loop(),
            ruvia::http_server_callbacks{.connection_ = [&](ruvia::server_connection& connection) -> ruvia::task<void> {
                struct retirement final {
                    bool& retired_;
                    ~retirement() {
                        retired_ = true;
                    }
                } retire{retired};
                entered.notify();
                try {
                    (void)co_await connection.read();
                } catch (const std::system_error&) {
                }
            }});
        co_await server->add_connection_service({.address_ = "127.0.0.1",
            .http_ = acceptor.local_endpoint().port(),
            .dispatch_ = ruvia::listener_dispatch::connection});
        co_await server->start();
        server->adopt({.address_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port()}, accepted.release());
        co_await entered.wait();
        auto shutdown = server->shutdown();
        server.reset();
        co_await std::move(shutdown);
        EXPECT_TRUE(retired);
        EXPECT_TRUE(attachment.loop().worker().accepting());
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    EXPECT_TRUE(retired);
}

TEST(Ruvia, independent_server_prepared_batch_cancel_and_shutdown_are_owning_transactions) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    ruvia::test::tcp_port_reservation port(io), second(io);
    unsigned calls = 0;
    bool published = false;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
            EXPECT_TRUE(published);
            ++calls;
            co_await answer(stream);
        }});
        co_await server.start();
        const ruvia::listen_config config{.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream};
        const std::array configs{config, ruvia::listen_config{.address_ = "127.0.0.1", .http_ = second.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream}};
        auto cancelled = co_await server.prepare_listeners(configs);
        co_await std::move(cancelled).cancel();
        auto prepared = co_await server.prepare_listeners(configs);
        std::array<asio::ip::tcp::socket, 2> peers{asio::ip::tcp::socket(io), asio::ip::tcp::socket(io)};
        const std::array endpoints{port.endpoint(), second.endpoint()};
        constexpr std::string_view head = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        for (std::size_t index = 0; index != peers.size(); ++index) {
            peers[index].connect(endpoints[index]);
            asio::write(peers[index], asio::buffer(head));
        }
        (void)co_await ruvia::sleep_for(worker, 1ms);
        EXPECT_EQ(calls, 0U);
        published = true;
        co_await std::move(prepared).activate();
        for (auto& peer : peers) {
            std::array<char, 512> bytes{};
            const auto received = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
                asio::async_read(peer, asio::buffer(bytes), asio::transfer_at_least(16), std::move(completion));
            });
            EXPECT_FALSE(received.error_code());
            EXPECT_NE(std::string_view(bytes.data(), received.result()).find(" 200 "), std::string_view::npos);
        }
        EXPECT_EQ(calls, 2U);
        server.remove_listener({.address_ = "127.0.0.1", .port_ = port.endpoint().port()}, {.close_connections_ = true});
        server.remove_listener({.address_ = "127.0.0.1", .port_ = second.endpoint().port()}, {.close_connections_ = true});
        auto pending = co_await server.prepare_listeners(configs);
        co_await server.shutdown();
        bool rejected = false;
        try {
            co_await std::move(pending).activate();
        } catch (const std::logic_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, independent_server_ipv6_only_rejects_ipv4_and_mismatched_inherited_socket) {
    asio::io_context io;
    asio::ip::tcp::acceptor inherited(io);
    std::error_code error;
    inherited.open(asio::ip::tcp::v6(), error);
    if (error == asio::error::address_family_not_supported) {
        GTEST_SKIP() << "IPv6 is unavailable";
    }
    ASSERT_FALSE(error);
    inherited.set_option(asio::ip::v6_only(true));
    inherited.bind({asio::ip::address_v6::loopback(), 0});
    inherited.listen();
    const auto port = inherited.local_endpoint().port();
    const auto socket = inherited.release();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::http_server server(attachment.loop(), {.request_stream_ = &answer});
    EXPECT_THROW((void)server.prepare_listener({.address_ = "::1", .ipv6_only_ = false, .http_ = port, .dispatch_ = ruvia::listener_dispatch::request_stream, .inherited_sockets_ = ruvia::listener_sockets{.http_ = {socket}}}),
        std::invalid_argument);
    asio::ip::tcp::acceptor released(io);
    released.open(asio::ip::tcp::v6());
    released.set_option(asio::ip::v6_only(true));
    EXPECT_NO_THROW(released.bind({asio::ip::address_v6::loopback(), port}));
    const auto run = [&]() -> ruvia::task<void> {
        co_await server.start();
        bool rejected = false;
        try {
            auto invalid = co_await server.prepare_listener({.address_ = "127.0.0.1", .ipv6_only_ = true, .http_ = port, .dispatch_ = ruvia::listener_dispatch::request_stream});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        released.listen();
        auto matching = co_await server.prepare_listener({.address_ = "::1", .ipv6_only_ = true, .http_ = port, .dispatch_ = ruvia::listener_dispatch::request_stream, .inherited_sockets_ = ruvia::listener_sockets{.http_ = {released.release()}}});
        co_await std::move(matching).activate();
        const std::string origin = "http://[::1]:" + std::to_string(port);
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin});
        co_await check_response(client, ruvia::http_protocol_version::http11);
        co_await client.shutdown();
        co_await server.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

#if defined(RUVIA_ENABLE_HTTP3)
TEST(Ruvia, independent_server_ipv6_only_validates_inherited_quic_socket_before_loading_tls) {
    asio::io_context io;
    asio::ip::udp::socket socket(io);
    std::error_code error;
    socket.open(asio::ip::udp::v6(), error);
    if (error == asio::error::address_family_not_supported) {
        GTEST_SKIP() << "IPv6 is unavailable";
    }
    ASSERT_FALSE(error);
    socket.set_option(asio::ip::v6_only(false));
    socket.bind({asio::ip::address_v6::any(), 0});
    const auto endpoint = socket.local_endpoint();
    ruvia::test::tls_identity identity("localhost");
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::http_server server(attachment.loop(), {.request_stream_ = &answer});
    const auto shutdown = [&]() -> ruvia::task<void> {
        co_await server.start();
        bool rejected = false;
        std::exception_ptr failure;
        try {
            auto prepared = co_await server.prepare_listener({.address_ = "::", .ipv6_only_ = true, .https_ = endpoint.port(), .http3_ = endpoint.port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream, .inherited_sockets_ = ruvia::listener_sockets{.http3_ = {socket.release()}}});
        } catch (const std::invalid_argument&) {
            rejected = true;
        } catch (...) {
            failure = std::current_exception();
        }
        EXPECT_TRUE(rejected);
        asio::ip::udp::socket replacement(io);
        replacement.open(asio::ip::udp::v6());
        EXPECT_NO_THROW(replacement.bind(endpoint));
        co_await server.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(shutdown());
    attachment.run();
    std::move(root).get();
}
#endif
