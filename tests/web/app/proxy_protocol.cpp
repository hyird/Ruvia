#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio/read.hpp>
#include <asio/ssl.hpp>
#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_server.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/router.h"

#include "raw_http_peer.h"
#include "tcp_port_reservation.h"
#include "test_tls_identity.h"

namespace proxy_listener_test {
using namespace std::chrono_literals;

struct runtime final {
    asio::io_context io_;
    ruvia::blocking_pool pool_{{.thread_count_ = 1}};
    ruvia::event_loop_attachment attachment_{ruvia::attach_event_loop(io_, {.blocking_pool_ = &pool_})};
    ruvia::event_loop loop_{attachment_.loop()};
    std::jthread thread_{[this] { attachment_.run(); }};
    ~runtime() {
        attachment_.stop();
        thread_.join();
        pool_.join();
    }
};

ruvia::task<void> identify(ruvia::request_stream& stream) {
    const auto remote = stream.conn().remote();
    const auto text = remote.address().to_string();
    std::string body(text.data(), text.size());
    body += ":" + std::to_string(remote.port());
    body += ":" + std::string(stream.conn().client().address());
    body += stream.conn().via_trusted_proxy() ? ":trusted" : ":direct";
    if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
        co_await stream.response().write(body);
        co_await stream.response().end();
    }
}

ruvia::task<void> echo(ruvia::server_connection& connection) {
    while (auto bytes = co_await connection.read()) {
        co_await connection.write(*bytes);
    }
}

std::string head(std::uint16_t port, std::string_view forwarded = {}) {
    return "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nConnection: close\r\n" + std::string(forwarded) + "\r\n";
}

std::string proxy_v2(bool local, std::uint16_t port) {
    std::string result("\r\n\r\n\0\r\nQUIT\n", 12);
    result.push_back(local ? '\x20' : '\x21');
    result.push_back('\x11');
    result.append("\0\x10", 2);  // IPv4 address block and a four-byte TLV.
    const unsigned char address[]{198, 51, 100, 23, 127, 0, 0, 1, 0x12, 0x34,
        static_cast<unsigned char>(port >> 8), static_cast<unsigned char>(port), 0xe0, 0, 1, 42};
    result.append(reinterpret_cast<const char*>(address), sizeof(address));
    return result;
}

ruvia::task<void> start(ruvia::http_server& server, ruvia::listen_config config) {
    server.listen(std::move(config));
    co_await server.start();
}

ruvia::task<void> adopt_read_prefix(ruvia::http_server& server, asio::ip::tcp::acceptor& acceptor,
    std::size_t bytes, std::optional<ruvia::ip_endpoint> remote = {}) {
    auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto handler) {
        acceptor.async_accept(std::move(handler));
    });
    if (accepted.error_code()) {
        throw std::system_error(accepted.error_code());
    }
    std::vector<std::byte> prefix(bytes);
    const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
        asio::async_read(accepted.result(), asio::buffer(prefix), std::move(handler));
    });
    if (read.error_code()) {
        throw std::system_error(read.error_code());
    }
    server.adopt({.address_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port()}, accepted.result().release(),
        {.prefix_ = prefix, .remote_ = remote});
    // The prefix owner dies before the session starts reading it.
}
}  // namespace proxy_listener_test

TEST(Ruvia, proxy_listener_requires_preface_and_uses_source_for_every_peer_fact) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    std::mutex mutex;
    std::vector<ruvia::ip_endpoint> admitted;
    std::vector<std::string> logged;
    ruvia::http_server server(runtime.loop_, {.request_stream_ = &identify,
                                                 .access_ = [&](const ruvia::access_log_record& record) noexcept {
            std::lock_guard lock(mutex);
            logged.emplace_back(record.remote_address()); },
                                                 .connection_admission_ = [&](const ruvia::connection_event& event) noexcept {
            std::lock_guard lock(mutex);
            admitted.push_back(event.remote());
            return true; }});
    const auto setup = [&]() -> ruvia::task<void> {
        ruvia::trusted_proxy_config trusted;
        trusted.cidrs_.emplace_back("203.0.113.0/24");
        server.trusted_proxies(std::move(trusted));
        co_await start(server, {.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .proxy_protocol_ = true});
    };
    runtime.loop_.start(setup()).get();
    const auto request = head(port.endpoint().port(), "X-Forwarded-For: 192.0.2.77\r\n");
    auto response = ruvia::test::exchange(io, port.endpoint(),
        "PROXY TCP4 203.0.113.9 127.0.0.1 4567 " + std::to_string(port.endpoint().port()) + "\r\n" + request);
    EXPECT_NE(response.find("203.0.113.9:4567:192.0.2.77:trusted"), std::string::npos);
    response = ruvia::test::exchange(io, port.endpoint(), proxy_v2(false, port.endpoint().port()) + request);
    EXPECT_NE(response.find("198.51.100.23:4660:198.51.100.23:direct"), std::string::npos);
    response = ruvia::test::exchange(io, port.endpoint(), proxy_v2(true, port.endpoint().port()) + request);
    EXPECT_NE(response.find(":127.0.0.1:direct"), std::string::npos);
    {
        asio::ip::tcp::socket peer(io);
        peer.connect(port.endpoint());
        asio::write(peer, asio::buffer(request));
        std::error_code error;
        EXPECT_TRUE(ruvia::test::read_to_end(peer, error).empty());
        EXPECT_TRUE(error == asio::error::eof || error == asio::error::connection_reset);
    }
    runtime.loop_.start(server.shutdown()).get();
    std::lock_guard lock(mutex);
    ASSERT_EQ(admitted.size(), 3u);
    EXPECT_EQ(admitted[0], ruvia::ip_endpoint(*ruvia::ip_address::parse("203.0.113.9"), 4567));
    EXPECT_EQ(admitted[1], ruvia::ip_endpoint(*ruvia::ip_address::parse("198.51.100.23"), 4660));
    EXPECT_EQ(admitted[2].address(), *ruvia::ip_address::parse("127.0.0.1"));
    EXPECT_EQ(logged, (std::vector<std::string>{"203.0.113.9", "198.51.100.23", "127.0.0.1"}));
}

TEST(Ruvia, proxy_listener_bounds_header_completion_and_rejects_http3) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    ruvia::http_server server(runtime.loop_, {.request_stream_ = &identify},
        {.request_header_timeout_ = 40ms, .header_completion_timeout_ = 60ms});
    runtime.loop_.start(start(server, {.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .proxy_protocol_ = true})).get();
    asio::ip::tcp::socket peer(io);
    peer.connect(port.endpoint());
    asio::write(peer, asio::buffer("PROXY ", 6));
    const auto began = std::chrono::steady_clock::now();
    std::error_code error;
    EXPECT_TRUE(ruvia::test::read_to_end(peer, error).empty());
    EXPECT_TRUE(error == asio::error::eof || error == asio::error::connection_reset);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 2s);
    runtime.loop_.start(server.shutdown()).get();
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::http_server invalid(runtime.loop_, {.request_stream_ = &identify});
    EXPECT_THROW(runtime.loop_.start(start(invalid,
                                         {.address_ = "127.0.0.1", .https_ = port.endpoint().port(), .http3_ = port.endpoint().port(), .proxy_protocol_ = true}))
                     .get(),
        std::invalid_argument);
    runtime.loop_.start(invalid.shutdown()).get();
#endif
}

TEST(Ruvia, adopted_prefix_is_replayed_for_http_raw_and_tls_with_remote_override) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    asio::ip::tcp::acceptor http(runtime.io_, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::acceptor raw(runtime.io_, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::acceptor tls(runtime.io_, {asio::ip::address_v4::loopback(), 0});
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    ruvia::http_server http_server(runtime.loop_, {.request_stream_ = &identify});
    ruvia::http_server raw_server(runtime.loop_, {.connection_ = &echo});
    ruvia::http_server tls_server(runtime.loop_, {.request_stream_ = &identify});
    const auto setup = [&]() -> ruvia::task<void> {
        co_await http_server.add_connection_service({.address_ = "127.0.0.1", .http_ = http.local_endpoint().port()});
        co_await raw_server.add_connection_service({.address_ = "127.0.0.1", .http_ = raw.local_endpoint().port()});
        co_await tls_server.add_connection_service({.address_ = "127.0.0.1", .https_ = tls.local_endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}});
        co_await http_server.start();
        co_await raw_server.start();
        co_await tls_server.start();
    };
    runtime.loop_.start(setup()).get();
    auto adopted = runtime.loop_.start(adopt_read_prefix(http_server, http, 11,
        ruvia::ip_endpoint(*ruvia::ip_address::parse("192.0.2.123"), 444)));
    auto response = ruvia::test::exchange(io, http.local_endpoint(), head(http.local_endpoint().port()));
    std::move(adopted).get();
    EXPECT_NE(response.find("192.0.2.123:444:192.0.2.123:direct"), std::string::npos);
    auto raw_adopted = runtime.loop_.start(adopt_read_prefix(raw_server, raw, 4));
    asio::ip::tcp::socket peer(io);
    peer.connect(raw.local_endpoint());
    asio::write(peer, asio::buffer("prefix-and-tail", 15));
    peer.shutdown(asio::ip::tcp::socket::shutdown_send);
    EXPECT_EQ(ruvia::test::read_to_end(peer), "prefix-and-tail");
    std::move(raw_adopted).get();
    auto tls_adopted = runtime.loop_.start(adopt_read_prefix(tls_server, tls, 7));
    asio::ssl::context context(asio::ssl::context::tls_client);
    context.load_verify_file(identity.ca_file_.string());
    asio::ssl::stream<asio::ip::tcp::socket> secure(io, context);
    secure.set_verify_mode(asio::ssl::verify_peer);
    secure.lowest_layer().connect(tls.local_endpoint());
    secure.handshake(asio::ssl::stream_base::client);
    asio::write(secure, asio::buffer(head(tls.local_endpoint().port())));
    response = ruvia::test::read_to_end(secure);
    EXPECT_NE(response.find("127.0.0.1"), std::string::npos);
    std::move(tls_adopted).get();
    runtime.loop_.start(http_server.shutdown()).get();
    runtime.loop_.start(raw_server.shutdown()).get();
    runtime.loop_.start(tls_server.shutdown()).get();
}

TEST(Ruvia, standalone_dispatch_rejects_missing_ambiguous_and_mismatched_callbacks) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    ruvia::http_server absent(runtime.loop_, {});
    ruvia::http_server ambiguous(runtime.loop_, {.request_stream_ = &identify, .connection_ = &echo});
    ruvia::http_server mismatch(runtime.loop_, {.request_stream_ = &identify});
    for (auto* server : {&absent, &ambiguous}) {
        EXPECT_THROW(runtime.loop_.start(start(*server, {.address_ = "127.0.0.1", .http_ = port.endpoint().port()})).get(), std::invalid_argument);
        runtime.loop_.start(server->shutdown()).get();
    }
    EXPECT_THROW(runtime.loop_.start(start(mismatch, {.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection})).get(), std::invalid_argument);
    runtime.loop_.start(mismatch.shutdown()).get();
}

TEST(Ruvia, proxy_source_is_the_router_rate_limit_key) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    std::unique_ptr<ruvia::http_server> server;
    const auto setup = [&]() -> ruvia::task<void> {
        auto routes = std::make_shared<ruvia::router>(runtime.loop_.handle());
        routes->use<ruvia::rate_limit<1, 60000>>();
        routes->add("GET", "/", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text("allowed");
        });
        routes->freeze();
        server = std::make_unique<ruvia::http_server>(runtime.loop_, ruvia::http_server_callbacks{
                                                                         .request_stream_ = [routes](ruvia::request_stream& stream) { return routes->dispatch(stream); }});
        co_await start(*server, {.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .proxy_protocol_ = true});
    };
    runtime.loop_.start(setup()).get();
    const auto request = [&](std::string_view source) {
        return ruvia::test::exchange(io, port.endpoint(), "PROXY TCP4 " + std::string(source) + " 127.0.0.1 4567 " + std::to_string(port.endpoint().port()) + "\r\n" + head(port.endpoint().port()));
    };
    EXPECT_TRUE(request("198.51.100.10").starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(request("198.51.100.10").starts_with("HTTP/1.1 429"));
    EXPECT_TRUE(request("198.51.100.11").starts_with("HTTP/1.1 200"));
    runtime.loop_.start(server->shutdown()).get();
}

TEST(Ruvia, proxy_preface_precedes_tls_client_hello_and_preserves_effective_source) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    ruvia::http_server server(runtime.loop_, {.request_stream_ = &identify});
    runtime.loop_.start(start(server, {.address_ = "127.0.0.1", .https_ = port.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .proxy_protocol_ = true})).get();
    asio::ssl::context context(asio::ssl::context::tls_client);
    context.load_verify_file(identity.ca_file_.string());
    asio::ssl::stream<asio::ip::tcp::socket> peer(io, context);
    peer.set_verify_mode(asio::ssl::verify_peer);
    peer.next_layer().connect(port.endpoint());
    const std::string preface = "PROXY TCP6 2001:db8::123 ::1 9876 " + std::to_string(port.endpoint().port()) + "\r\n";
    asio::write(peer.next_layer(), asio::buffer(preface));
    peer.handshake(asio::ssl::stream_base::client);
    asio::write(peer, asio::buffer(head(port.endpoint().port())));
    const auto response = ruvia::test::read_to_end(peer);
    EXPECT_NE(response.find("2001:db8::123:9876:2001:db8::123:direct"), std::string::npos);
    runtime.loop_.start(server.shutdown()).get();
}
