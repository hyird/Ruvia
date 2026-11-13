#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#if !defined(_WIN32)
#include <fcntl.h>

#include <cerrno>
#endif

#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/read.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/http_server.h"

#include "raw_http_peer.h"
#include "tcp_port_reservation.h"
#include "test_tls_identity.h"
#include "udp_port_reservation.h"
#if defined(RUVIA_ENABLE_HTTP2)
#include "http2_wire_fixture.h"
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3_test_client.h"
#endif

namespace server_group_test {
using namespace std::chrono_literals;

struct external_loops final {
    asio::io_context first_, second_;
    ruvia::blocking_pool pool_{{.thread_count_ = 2}};
    ruvia::event_loop_attachment first_attachment_{ruvia::attach_event_loop(first_, {.blocking_pool_ = &pool_})};
    ruvia::event_loop_attachment second_attachment_{ruvia::attach_event_loop(second_, {.blocking_pool_ = &pool_})};
    std::array<ruvia::event_loop, 2> loops_{first_attachment_.loop(), second_attachment_.loop()};
    std::jthread first_thread_, second_thread_;

    explicit external_loops(bool drive = true) {
        if (drive) {
            start();
        }
    }
    void start() {
        first_thread_ = std::jthread([this] { first_attachment_.run(); });
        second_thread_ = std::jthread([this] { second_attachment_.run(); });
        const auto ready = []() -> ruvia::task<void> { co_return; };
        loops_[0].start(ready()).get();
        loops_[1].start(ready()).get();
    }
    ~external_loops() {
        first_attachment_.stop();
        second_attachment_.stop();
        if (first_thread_.joinable()) {
            first_thread_.join();
        }
        if (second_thread_.joinable()) {
            second_thread_.join();
        }
        pool_.join();
    }
};

struct held_work final {
    std::shared_ptr<std::counting_semaphore<>> gate_{std::make_shared<std::counting_semaphore<>>(0)};
    std::ptrdiff_t count_;

    explicit held_work(std::ptrdiff_t count = 1)
        : count_(count) {}
    ~held_work() {
        release();
    }
    void release() {
        if (gate_) {
            gate_->release(count_);
            gate_.reset();
        }
    }
};

ruvia::task<void> answer(ruvia::request_stream& stream) {
    std::string body = std::to_string(stream.worker().id()) + ":" + std::to_string(stream.conn().id());
    if (stream.request().path() == "/proxy") {
        body = std::string(stream.conn().client().address());
        body += stream.conn().via_trusted_proxy() ? ":proxy:" : ":direct:";
        body += stream.conn().scheme() == ruvia::http_scheme::https ? "https" : "http";
    }
    if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
        co_await stream.response().write(body);
        co_await stream.response().end();
    }
}

std::string request(std::uint16_t port, std::string_view path = "/", std::string_view forwarded = {}) {
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    socket.connect({asio::ip::address_v4::loopback(), port});
    const std::string head = "GET " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nConnection: close\r\n" + std::string(forwarded) + "\r\n";
    asio::write(socket, asio::buffer(head));
    std::string result;
    std::array<char, 1024> bytes;
    asio::error_code error;
    for (;;) {
        const auto count = socket.read_some(asio::buffer(bytes), error);
        result.append(bytes.data(), count);
        if (error == asio::error::eof) {
            break;
        }
        if (error) {
            throw std::system_error(error);
        }
    }
    EXPECT_NE(result.find(" 200 "), std::string::npos);
    return result;
}

ruvia::listen_config plain(std::uint16_t port) {
    return {.address_ = "127.0.0.1", .http_ = port, .dispatch_ = ruvia::listener_dispatch::request_stream};
}

ruvia::listen_config secure(std::uint16_t tcp, std::uint16_t udp, const ruvia::test::tls_identity& identity) {
    ruvia::listen_config config{.address_ = "127.0.0.1", .https_ = tcp, .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
    if (udp != 0) {
        config.http3_ = udp;
    }
#else
    (void)udp;
#endif
    return config;
}

template <typename stream_type>
std::string request_on(stream_type& stream, std::string_view path = "/proxy") {
    const std::string head = "GET " + std::string(path) +
                             " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    asio::write(stream, asio::buffer(head));
    const auto response = ruvia::test::read_to_end(stream);
    return std::string(response);
}

std::string tls_request(std::uint16_t port, const ruvia::test::tls_identity& identity) {
    asio::io_context io;
    asio::ssl::context context(asio::ssl::context::tls_client);
    context.load_verify_file(identity.ca_file_.string());
    context.set_verify_mode(asio::ssl::verify_peer);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, context);
    stream.next_layer().connect({asio::ip::address_v4::loopback(), port});
    stream.handshake(asio::ssl::stream_base::client);
    return request_on(stream);
}

ruvia::task<void> raw_transport_answer(ruvia::server_connection& connection) {
    std::string head;
    while (head.find("\r\n\r\n") == std::string::npos) {
        const auto bytes = co_await connection.read();
        if (!bytes) {
            co_return;
        }
        head.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    }
    const std::string_view body = connection.tls() != nullptr ? "tls" : "plain";
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + std::string(body);
    co_await connection.write(std::as_bytes(std::span(response.data(), response.size())));
    co_await connection.finish();
}
}  // namespace server_group_test

using namespace server_group_test;

TEST(Ruvia, independent_group_trusted_proxy_replacement_is_atomic_and_defaults_to_direct) {
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation trusted_port(ports), direct_port(ports);
    ruvia::http_server_group trusted(runtime.loops_, {.request_stream_ = &answer});
    ruvia::http_server_group direct(runtime.loops_, {.request_stream_ = &answer});
    trusted.listen(plain(trusted_port.endpoint().port()));
    direct.listen(plain(direct_port.endpoint().port()));
    ruvia::trusted_proxy_config previous;
    previous.cidrs_.emplace_back("0.0.0.0/0");
    trusted.trusted_proxies(std::move(previous));
    ruvia::trusted_proxy_config config;
    config.cidrs_.emplace_back("127.0.0.0/8");
    config.cidrs_.emplace_back("10.0.0.0/8");
    config.trust_x_forwarded_proto_ = true;
    trusted.trusted_proxies(config);
    config.cidrs_.emplace_back("127.0.0.1/33");
    EXPECT_THROW(trusted.trusted_proxies(config), std::invalid_argument);
    runtime.loops_[0].start(trusted.start(runtime.loops_[0].handle())).get();
    runtime.loops_[0].start(direct.start(runtime.loops_[0].handle())).get();
    const std::string forwarded = "X-Forwarded-For: 203.0.113.7, 198.51.100.9, 10.1.2.3\r\nX-Forwarded-Proto: http, https, http\r\n";
    EXPECT_NE(request(trusted_port.endpoint().port(), "/proxy", forwarded).find("198.51.100.9:proxy:https"), std::string::npos);
    EXPECT_NE(request(direct_port.endpoint().port(), "/proxy", forwarded).find("127.0.0.1:direct:http"), std::string::npos);
    EXPECT_THROW(trusted.trusted_proxies({}), std::logic_error);
    runtime.loops_[0].start(trusted.shutdown(runtime.loops_[0].handle())).get();
    runtime.loops_[0].start(direct.shutdown(runtime.loops_[0].handle())).get();
    EXPECT_TRUE(runtime.loops_[0].accepting());
    EXPECT_TRUE(runtime.loops_[1].accepting());
}

TEST(Ruvia, independent_group_stages_configuration_before_external_loops_are_driven) {
    external_loops runtime(false);
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    group.listen(plain(port.endpoint().port()));
    group.trusted_proxies({});
    runtime.start();
    runtime.loops_[0].start(group.start(runtime.loops_[0].handle())).get();
    EXPECT_NE(request(port.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].handle())).get();
}

TEST(Ruvia, independent_single_server_installs_trusted_proxy_configuration_before_start) {
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server server(runtime.loops_[0], {.request_stream_ = &answer});
    const auto start = [&]() -> ruvia::task<void> {
        ruvia::trusted_proxy_config config;
        config.cidrs_.emplace_back("127.0.0.0/8");
        config.trust_x_forwarded_proto_ = true;
        server.trusted_proxies(config);
        config.cidrs_.emplace_back("invalid");
        EXPECT_THROW(server.trusted_proxies(config), std::invalid_argument);
        server.listen(plain(port.endpoint().port()));
        co_await server.start();
        EXPECT_THROW(server.trusted_proxies({}), std::logic_error);
    };
    runtime.loops_[0].start(start()).get();
    EXPECT_NE(request(port.endpoint().port(), "/proxy", "X-Forwarded-For: 198.51.100.4\r\nX-Forwarded-Proto: https\r\n").find("198.51.100.4:proxy:https"), std::string::npos);
    runtime.loops_[0].start(server.shutdown()).get();
}

TEST(Ruvia, independent_group_adopts_connected_sockets_on_the_explicit_worker) {
    external_loops runtime;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto port = acceptor.local_endpoint().port();
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    runtime.loops_[0].start(group.add_connection_service(runtime.loops_[0].handle(), plain(port))).get();
    runtime.loops_[0].start(group.start(runtime.loops_[0].handle())).get();
    for (std::size_t index = 0; index != group.worker_count(); ++index) {
        asio::ip::tcp::socket peer(io);
        peer.connect(acceptor.local_endpoint());
        auto accepted = acceptor.accept();
        runtime.loops_[0].start(group.adopt(runtime.loops_[0].handle(), index, {.address_ = "127.0.0.1", .port_ = port}, accepted.release())).get();
        const std::string head = "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nConnection: close\r\n\r\n";
        asio::write(peer, asio::buffer(head));
        std::string response;
        std::array<char, 1024> bytes;
        asio::error_code error;
        for (;;) {
            const auto count = peer.read_some(asio::buffer(bytes), error);
            response.append(bytes.data(), count);
            if (error == asio::error::eof) {
                break;
            }
            if (error) {
                throw std::system_error(error);
            }
        }
        EXPECT_NE(response.find(" 200 "), std::string::npos);
        EXPECT_NE(response.find(std::to_string(group.worker(index).id()) + ":"), std::string::npos);
    }
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].handle())).get();
}

TEST(Ruvia, independent_group_prepares_all_and_rolls_back_failed_dynamic_addition) {
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation first(ports), good(ports);
    asio::ip::tcp::acceptor busy(ports, {asio::ip::address_v4::loopback(), 0});
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    group.listen(plain(first.endpoint().port()));
    runtime.loops_[0].start(group.start(runtime.loops_[0].handle())).get();
    auto bad = secure(busy.local_endpoint().port(), 0, identity);
    bad.http_ = good.endpoint().port();
    bad.http3_.reset();
    EXPECT_THROW(runtime.loops_[0].start(group.add_listener(runtime.loops_[0].handle(), std::move(bad))).get(), std::system_error);
    EXPECT_NE(request(first.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(group.add_listener(runtime.loops_[0].handle(), plain(good.endpoint().port()))).get();
    EXPECT_NE(request(good.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(group.remove_listener(runtime.loops_[0].handle(), {.address_ = "127.0.0.1", .port_ = good.endpoint().port()})).get();
    runtime.loops_[0].start(group.add_listener(runtime.loops_[0].handle(), plain(good.endpoint().port()))).get();
    EXPECT_NE(request(good.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].handle())).get();
}

TEST(Ruvia, independent_group_start_failure_preserves_external_unrelated_work) {
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation good(ports);
    asio::ip::tcp::acceptor busy(ports, {asio::ip::address_v4::loopback(), 0});
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    group.listen(plain(good.endpoint().port()));
    group.listen(plain(busy.local_endpoint().port()));
    EXPECT_THROW(runtime.loops_[0].start(group.start(runtime.loops_[0].handle())).get(), std::system_error);
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].handle())).get();
    const auto alive = []() -> ruvia::task<int> { co_return 42; };
    EXPECT_EQ(runtime.loops_[0].start(alive()).get(), 42);
    EXPECT_EQ(runtime.loops_[1].start(alive()).get(), 42);
    ruvia::http_server_group replacement(runtime.loops_, {.request_stream_ = &answer});
    replacement.listen(plain(good.endpoint().port()));
    runtime.loops_[0].start(replacement.start(runtime.loops_[0].handle())).get();
    EXPECT_NE(request(good.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(replacement.shutdown(runtime.loops_[0].handle())).get();
}

TEST(Ruvia, independent_group_accepts_member_worker_controls_and_coordinates_loop_retirement) {
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    const auto member_controls = [&]() -> ruvia::task<void> {
        const auto& caller = runtime.loops_[0].handle();
        co_await group.start(caller);
        co_await group.add_listener(caller, plain(port.endpoint().port()));
        EXPECT_FALSE(co_await group.close_connection(caller, 0));
        co_await group.remove_listener(caller, {.address_ = "127.0.0.1", .port_ = port.endpoint().port()});
        co_await group.shutdown(caller);
    };
    runtime.loops_[0].start(member_controls()).get();
    ruvia::http_server_group retiring(runtime.loops_, {.request_stream_ = &answer});
    ruvia::stop_source unrelated_stop;
    std::promise<void> sleeping, resumed;
    auto sleeping_result = sleeping.get_future();
    auto resumed_result = resumed.get_future();
    const auto unrelated = [&]() -> ruvia::task<ruvia::timer_sleep_result> {
        sleeping.set_value();
        const auto result = co_await ruvia::sleep_for(runtime.loops_[1].handle(), 1h, unrelated_stop.token());
        resumed.set_value();
        co_return result;
    };
    auto unrelated_root = runtime.loops_[1].start(unrelated());
    sleeping_result.get();
    runtime.loops_[0].start(retiring.start(runtime.loops_[0].handle())).get();
    runtime.first_attachment_.stop();
    auto finished = std::async(std::launch::async, [&] {
        runtime.loops_[1].start(retiring.shutdown(runtime.loops_[1].handle())).get();
    });
    ASSERT_EQ(finished.wait_for(5s), std::future_status::ready);
    finished.get();
    EXPECT_TRUE(runtime.loops_[1].accepting());
    EXPECT_EQ(retiring.stats().active_connections_, std::size_t{0});
    EXPECT_EQ(resumed_result.wait_for(0s), std::future_status::timeout);
    unrelated_stop.request_stop();
    EXPECT_EQ(std::move(unrelated_root).get(), ruvia::timer_sleep_result::stop_requested);
}

#if !defined(_WIN32)
TEST(Ruvia, independent_group_duplicates_in_worker_order_and_rolls_back_invalid_inheritance) {
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group original(runtime.loops_, {.request_stream_ = &answer});
    original.listen(plain(port.endpoint().port()));
    runtime.loops_[0].start(original.start(runtime.loops_[0].handle())).get();
    auto inherited = runtime.loops_[0].start(original.duplicate_listener_sockets(runtime.loops_[0].handle(), {.address_ = "127.0.0.1", .port_ = port.endpoint().port()})).get();
    auto valid = runtime.loops_[0].start(original.duplicate_listener_sockets(runtime.loops_[0].handle(), {.address_ = "127.0.0.1", .port_ = port.endpoint().port()})).get();
    ASSERT_EQ(inherited.http_.size(), std::size_t{2});
    runtime.loops_[0].start(original.shutdown(runtime.loops_[0].handle())).get();
    ruvia::http_server_group replacement(runtime.loops_, {.request_stream_ = &answer});
    auto invalid = plain(port.endpoint().port());
    invalid.inherited_sockets_ = inherited;
    invalid.inherited_sockets_->http_.pop_back();
    const auto extra = inherited.http_.back();
    inherited.http_.pop_back();
    // The malformed config consumes its one socket; close the deliberately
    // omitted duplicate through the public native socket owner as well.
    ruvia::http_server_group rejected(runtime.loops_, {.request_stream_ = &answer});
    rejected.listen(std::move(invalid));
    EXPECT_THROW(runtime.loops_[0].start(rejected.start(runtime.loops_[0].handle())).get(), std::invalid_argument);
    runtime.loops_[0].start(rejected.shutdown(runtime.loops_[0].handle())).get();
    asio::ip::tcp::acceptor omitted(ports);
    omitted.assign(asio::ip::tcp::v4(), extra);
    omitted.close();
    auto adopted = plain(port.endpoint().port());
    adopted.inherited_sockets_ = std::move(valid);
    replacement.listen(std::move(adopted));
    runtime.loops_[0].start(replacement.start(runtime.loops_[0].handle())).get();
    EXPECT_NE(request(port.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(replacement.shutdown(runtime.loops_[0].handle())).get();
}
#endif

#if defined(RUVIA_ENABLE_HTTP3)
TEST(Ruvia, independent_group_shared_http3_endpoint_survives_repeated_migration_rebind_and_listener_replacement) {
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation tcp(ports);
    ruvia::test::udp_port_reservation udp(ports);
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    group.listen(secure(tcp.endpoint().port(), udp.endpoint().port(), identity));
    runtime.loops_[0].start(group.start(runtime.loops_[0].handle())).get();
    ruvia::test::http3_test_client client({asio::ip::address_v4::loopback(), udp.endpoint().port()}, identity.ca_file_, "localhost", 5s);
    auto previous = client.start_request("GET", "/worker");
    const auto initial = client.finish_response(previous);
    ASSERT_EQ(initial.status_, 200);
    ASSERT_FALSE(initial.body_.empty());
    const ruvia::test::tls_identity rotated("localhost", "DNS:localhost,IP:127.0.0.1");
    runtime.loops_[0].start(group.replace_tls_identities(runtime.loops_[0].handle(), {.address_ = "127.0.0.1", .port_ = tcp.endpoint().port()},
                                {.identity_ = {.certificate_chain_file_ = rotated.ca_file_, .private_key_file_ = rotated.private_key_file()}}))
        .get();
    {
        ruvia::test::http3_test_client renewed({asio::ip::address_v4::loopback(), udp.endpoint().port()}, rotated.ca_file_, "localhost", 5s);
        EXPECT_EQ(renewed.request("GET", "/worker").status_, 200);
    }
    for (int generation = 0; generation != 3; ++generation) {
        for (const bool migrate : {true, false}) {
            for (int change = 0; change != 8; ++change) {
                if (migrate) {
                    client.migrate();
                } else {
                    client.rebind();
                }
                const auto stream = client.start_request("GET", "/worker");
                EXPECT_EQ(stream, previous + 4);
                const auto response = client.finish_response(stream);
                EXPECT_EQ(response.status_, 200);
                EXPECT_EQ(response.body_, initial.body_);
                previous = stream;
            }
        }
        runtime.loops_[0].start(group.remove_listener(runtime.loops_[0].handle(), {.address_ = "127.0.0.1", .port_ = tcp.endpoint().port()})).get();
        runtime.loops_[0].start(group.add_listener(runtime.loops_[0].handle(), secure(tcp.endpoint().port(), udp.endpoint().port(), identity))).get();
        ruvia::test::http3_test_client new_client({asio::ip::address_v4::loopback(), udp.endpoint().port()}, identity.ca_file_, "localhost", 5s);
        EXPECT_EQ(new_client.request("GET", "/worker").status_, 200);
        EXPECT_EQ(client.request("GET", "/worker").body_, initial.body_);
        previous += 4;
    }
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].handle())).get();
    EXPECT_EQ(group.stats().active_connections_, std::size_t{0});
}
#endif

TEST(Ruvia, independent_group_mixed_listener_preserves_transport_during_rotation_readd_and_inheritance) {
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto port = reservation.endpoint().port();
    const ruvia::listener_endpoint target{.address_ = "127.0.0.1", .port_ = port};
    const ruvia::test::tls_identity first("mixed-group-first", "DNS:localhost");
    const ruvia::test::tls_identity second("mixed-group-second", "DNS:localhost");
    auto config = secure(port, 0, first);
    config.http_ = port;
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    runtime.loops_[0].start(group.start(runtime.loops_[0].handle())).get();
    runtime.loops_[0].start(group.add_listener(runtime.loops_[0].handle(), config)).get();
    auto plaintext = std::async(std::launch::async, [port] { return request(port, "/proxy"); });
    EXPECT_NE(tls_request(port, first).find("127.0.0.1:direct:https"), std::string::npos);
    EXPECT_NE(plaintext.get().find("127.0.0.1:direct:http"), std::string::npos);
    runtime.loops_[0].start(group.replace_tls_identities(runtime.loops_[0].handle(), target, secure(port, 0, second).tls_)).get();
    EXPECT_NE(tls_request(port, second).find("127.0.0.1:direct:https"), std::string::npos);
    runtime.loops_[0].start(group.remove_listener(runtime.loops_[0].handle(), target)).get();
    asio::ip::tcp::socket refused(io);
    asio::error_code error;
    refused.connect(reservation.endpoint(), error);
    EXPECT_EQ(error, asio::error::connection_refused);
    config.tls_ = secure(port, 0, second).tls_;
    runtime.loops_[0].start(group.add_listener(runtime.loops_[0].handle(), config)).get();
    EXPECT_NE(request(port, "/proxy").find("127.0.0.1:direct:http"), std::string::npos);
    EXPECT_NE(tls_request(port, second).find("127.0.0.1:direct:https"), std::string::npos);
#if !defined(_WIN32)
    auto inherited = runtime.loops_[0].start(group.duplicate_listener_sockets(runtime.loops_[0].handle(), target)).get();
    EXPECT_TRUE(inherited.http_.empty());
    EXPECT_EQ(inherited.https_.size(), 2U);
#endif
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].handle())).get();
    ruvia::http_server_group successor(runtime.loops_, {.request_stream_ = &answer});
#if !defined(_WIN32)
    config.inherited_sockets_ = std::move(inherited);
#endif
    successor.listen(std::move(config));
    runtime.loops_[0].start(successor.start(runtime.loops_[0].handle())).get();
    EXPECT_NE(request(port, "/proxy").find("127.0.0.1:direct:http"), std::string::npos);
    EXPECT_NE(tls_request(port, second).find("127.0.0.1:direct:https"), std::string::npos);
    runtime.loops_[0].start(successor.shutdown(runtime.loops_[0].handle())).get();
}

TEST(Ruvia, mixed_raw_connection_services_detect_transport_after_single_and_group_adoption) {
    external_loops runtime;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto port = acceptor.local_endpoint().port();
    const ruvia::listener_endpoint target{.address_ = "127.0.0.1", .port_ = port};
    const ruvia::test::tls_identity identity("mixed-raw", "DNS:localhost");
    auto config = secure(port, 0, identity);
    config.http_ = port;
    config.dispatch_ = ruvia::listener_dispatch::connection;
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.load_verify_file(identity.ca_file_.string());
    client.set_verify_mode(asio::ssl::verify_peer);

    ruvia::http_server single(runtime.loops_[0], {.connection_ = &raw_transport_answer});
    const auto start_single = [&]() -> ruvia::task<void> {
        co_await single.add_connection_service(config);
        co_await single.start();
    };
    runtime.loops_[0].start(start_single()).get();
    for (const bool secured : {false, true}) {
        asio::ip::tcp::socket peer(io);
        peer.connect(acceptor.local_endpoint());
        auto accepted = acceptor.accept();
        const auto adopt = [&]() -> ruvia::task<void> {
            single.adopt(target, accepted.release());
            co_return;
        };
        runtime.loops_[0].start(adopt()).get();
        if (secured) {
            asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(peer), client);
            stream.handshake(asio::ssl::stream_base::client);
            EXPECT_TRUE(request_on(stream).ends_with("tls"));
        } else {
            EXPECT_TRUE(request_on(peer).ends_with("plain"));
        }
    }
    runtime.loops_[0].start(single.shutdown()).get();

    ruvia::http_server_group group(runtime.loops_, {.connection_ = &raw_transport_answer});
    runtime.loops_[0].start(group.add_connection_service(runtime.loops_[0].handle(), config)).get();
    runtime.loops_[0].start(group.start(runtime.loops_[0].handle())).get();
    for (std::size_t worker = 0; worker < group.worker_count(); ++worker) {
        for (const bool secured : {false, true}) {
            asio::ip::tcp::socket peer(io);
            peer.connect(acceptor.local_endpoint());
            auto accepted = acceptor.accept();
            runtime.loops_[0].start(group.adopt(runtime.loops_[0].handle(), worker, target, accepted.release())).get();
            if (secured) {
                asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(peer), client);
                stream.handshake(asio::ssl::stream_base::client);
                EXPECT_TRUE(request_on(stream).ends_with("tls"));
            } else {
                EXPECT_TRUE(request_on(peer).ends_with("plain"));
            }
        }
    }
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].handle())).get();
}

TEST(Ruvia, independent_group_cross_worker_add_preserves_caller_timers_and_cold_address_inputs) {
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    runtime.loops_[0].start(group.start(runtime.loops_[0].handle())).get();

    auto addition = [&] {
        std::string address = "127.0.0.1";
        auto config = plain(port.endpoint().port());
        config.address_ = address;
        auto pending = group.add_listener(runtime.loops_[0].handle(), std::move(config));
        address.assign(address.size(), 'x');
        return pending;
    }();
    held_work held;
    std::promise<void> blocked;
    auto blocked_result = blocked.get_future();
    ASSERT_TRUE(runtime.loops_[1].post([gate = held.gate_, &blocked] {
                                     blocked.set_value();
                                     gate->acquire();
                                 })
            .accepted());
    blocked_result.get();
    auto operation = runtime.loops_[0].start(std::move(addition));
    std::promise<void> ticked;
    auto tick = ticked.get_future();
    const auto timer = [&]() -> ruvia::task<void> {
        (void)co_await ruvia::sleep_for(runtime.loops_[0].handle(), 10ms);
        ticked.set_value();
    };
    auto timer_root = runtime.loops_[0].start(timer());
    const auto progress = tick.wait_for(2s);
    held.release();
    EXPECT_EQ(progress, std::future_status::ready);
    std::move(timer_root).get();
    std::move(operation).get();
    EXPECT_NE(request(port.endpoint().port()).find(" 200 "), std::string::npos);

    auto removal = [&] {
        std::string address = "127.0.0.1";
        auto pending = group.remove_listener(runtime.loops_[0].handle(),
            {.address_ = address, .port_ = port.endpoint().port()});
        address.assign(address.size(), 'x');
        return pending;
    }();
    runtime.loops_[0].start(std::move(removal)).get();
    asio::ip::tcp::socket refused(ports);
    asio::error_code error;
    refused.connect(port.endpoint(), error);
    EXPECT_EQ(error, asio::error::connection_refused);
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].handle())).get();
}

TEST(Ruvia, independent_group_tls_preparation_keeps_caller_live_and_close_cancels_publication) {
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    const ruvia::test::tls_identity identity("group-cancel", "DNS:localhost");
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    runtime.loops_[0].start(group.start(runtime.loops_[0].handle())).get();
    std::counting_semaphore<> occupied{0};
    held_work held(2);
    for (int index = 0; index != 2; ++index) {
        ASSERT_EQ(runtime.pool_.submit([gate = held.gate_, &occupied] {
            occupied.release();
            gate->acquire();
        }),
            ruvia::blocking_submit_status::accepted);
    }
    occupied.acquire();
    occupied.acquire();
    auto operation = runtime.loops_[0].start(group.add_listener(runtime.loops_[0].handle(),
        secure(port.endpoint().port(), 0, identity)));
    std::promise<void> ticked;
    auto tick = ticked.get_future();
    const auto timer = [&]() -> ruvia::task<void> {
        (void)co_await ruvia::sleep_for(runtime.loops_[0].handle(), 10ms);
        ticked.set_value();
    };
    auto timer_root = runtime.loops_[0].start(timer());
    const auto progress = tick.wait_for(2s);
    const auto queued = runtime.pool_.stats().queued_;
    group.close();
    held.release();
    EXPECT_EQ(progress, std::future_status::ready);
    EXPECT_GT(queued, 0U);
    std::move(timer_root).get();
    EXPECT_ANY_THROW(std::move(operation).get());
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].handle())).get();
    asio::ip::tcp::socket refused(ports);
    asio::error_code error;
    refused.connect(port.endpoint(), error);
    EXPECT_EQ(error, asio::error::connection_refused);
    EXPECT_TRUE(runtime.loops_[0].accepting());
    EXPECT_TRUE(runtime.loops_[1].accepting());
}

TEST(Ruvia, independent_group_cold_adoption_owns_socket_and_endpoint_before_execution) {
    external_loops runtime;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto port = acceptor.local_endpoint().port();
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    runtime.loops_[0].start(group.add_connection_service(runtime.loops_[0].handle(), plain(port))).get();
    runtime.loops_[0].start(group.start(runtime.loops_[0].handle())).get();

    asio::ip::tcp::socket discarded(io);
    discarded.connect(acceptor.local_endpoint());
    {
        auto accepted = acceptor.accept();
        auto cold = group.adopt(runtime.loops_[0].handle(), 1,
            {.address_ = "127.0.0.1", .port_ = port}, accepted.release());
        (void)cold;
    }
    discarded.non_blocking(true);
    asio::error_code error;
    std::array<char, 1> byte;
    discarded.read_some(asio::buffer(byte), error);
    EXPECT_TRUE(error == asio::error::eof || error == asio::error::connection_reset);

    asio::ip::tcp::socket peer(io);
    peer.connect(acceptor.local_endpoint());
    auto accepted = acceptor.accept();
    auto adoption = [&] {
        std::string address = "127.0.0.1";
        auto pending = group.adopt(runtime.loops_[0].handle(), 1,
            {.address_ = address, .port_ = port}, accepted.release());
        address.assign(address.size(), 'x');
        return pending;
    }();
    runtime.loops_[0].start(std::move(adoption)).get();
    const auto response = request_on(peer, "/");
    EXPECT_NE(response.find(" 200 "), std::string::npos);
    EXPECT_NE(response.find(std::to_string(group.worker(1).id()) + ":"), std::string::npos);
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].handle())).get();
}

#if !defined(_WIN32)
TEST(Ruvia, independent_group_discarded_cold_addition_closes_all_inherited_listener_sockets) {
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group original(runtime.loops_, {.request_stream_ = &answer});
    original.listen(plain(port.endpoint().port()));
    runtime.loops_[0].start(original.start(runtime.loops_[0].handle())).get();
    auto sockets = runtime.loops_[0].start(original.duplicate_listener_sockets(runtime.loops_[0].handle(),
                                               {.address_ = "127.0.0.1", .port_ = port.endpoint().port()}))
                       .get();
    ASSERT_EQ(sockets.http_.size(), 2U);
    const auto descriptors = sockets.http_;
    ruvia::http_server_group replacement(runtime.loops_, {.request_stream_ = &answer});
    runtime.loops_[0].start(replacement.start(runtime.loops_[0].handle())).get();
    {
        auto config = plain(port.endpoint().port());
        config.inherited_sockets_ = std::move(sockets);
        auto cold = replacement.add_listener(runtime.loops_[0].handle(), std::move(config));
        (void)cold;
    }
    for (const auto descriptor : descriptors) {
        errno = 0;
        EXPECT_EQ(::fcntl(descriptor, F_GETFD), -1);
        EXPECT_EQ(errno, EBADF);
    }
    EXPECT_NE(request(port.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(replacement.shutdown(runtime.loops_[0].handle())).get();
    runtime.loops_[0].start(original.shutdown(runtime.loops_[0].handle())).get();
}
#endif

TEST(Ruvia, independent_group_member_retirement_cancels_pending_cross_worker_addition) {
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    runtime.loops_[0].start(group.start(runtime.loops_[0].handle())).get();
    held_work held;
    std::promise<void> blocked;
    auto blocked_result = blocked.get_future();
    ASSERT_TRUE(runtime.loops_[1].post([gate = held.gate_, &blocked] {
                                     blocked.set_value();
                                     gate->acquire();
                                 })
            .accepted());
    blocked_result.get();
    auto operation = runtime.loops_[0].start(group.add_listener(runtime.loops_[0].handle(),
        plain(port.endpoint().port())));
    std::promise<void> submitted;
    auto submitted_result = submitted.get_future();
    const auto observe = [&]() -> ruvia::task<void> {
        (void)co_await ruvia::sleep_for(runtime.loops_[0].handle(), 10ms);
        submitted.set_value();
    };
    auto observed = runtime.loops_[0].start(observe());
    const auto progress = submitted_result.wait_for(2s);
    runtime.second_attachment_.stop();
    held.release();
    EXPECT_EQ(progress, std::future_status::ready);
    std::move(observed).get();
    EXPECT_ANY_THROW(std::move(operation).get());
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].handle())).get();
    EXPECT_EQ(group.stats().active_connections_, 0U);
    EXPECT_TRUE(runtime.loops_[0].accepting());
    asio::ip::tcp::socket refused(ports);
    asio::error_code error;
    refused.connect(port.endpoint(), error);
    EXPECT_EQ(error, asio::error::connection_refused);
}

TEST(Ruvia, independent_group_prepared_batch_defers_handlers_and_rolls_back_abandoned_commit) {
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io), second(io), abandoned(io), abandoned_second(io);
    std::atomic<unsigned> calls{};
    std::atomic<bool> published{};
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
        EXPECT_TRUE(published.load());
        ++calls;
        co_await answer(stream);
    }});
    const auto caller = runtime.loops_[0].handle();
    runtime.loops_[0].start(group.start(caller)).get();
    auto preparation = [&] {
        std::string address = "127.0.0.1";
        std::array configs{plain(port.endpoint().port()), plain(second.endpoint().port())};
        for (auto& config : configs) {
            config.address_ = address;
        }
        auto cold = group.prepare_listeners(caller, configs);
        address.assign(address.size(), 'x');
        return cold;
    }();
    auto prepared = runtime.loops_[0].start(std::move(preparation)).get();
    std::array<asio::ip::tcp::socket, 2> queued{asio::ip::tcp::socket(io), asio::ip::tcp::socket(io)};
    const std::array endpoints{port.endpoint(), second.endpoint()};
    constexpr std::string_view head = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    for (std::size_t index = 0; index != queued.size(); ++index) {
        queued[index].connect(endpoints[index]);
        asio::write(queued[index], asio::buffer(head));
    }
    const auto barrier = []() -> ruvia::task<void> { co_return; };
    for (const auto& loop : runtime.loops_) {
        loop.start(barrier()).get();
    }
    EXPECT_EQ(calls.load(), 0U);
    published.store(true);
    runtime.loops_[0].start(std::move(prepared).activate()).get();
    for (auto& peer : queued) {
        EXPECT_NE(ruvia::test::read_to_end(peer).find(" 200 "), std::string::npos);
    }
    EXPECT_EQ(calls.load(), 2U);
    const std::array abandoned_configs{plain(abandoned.endpoint().port()), plain(abandoned_second.endpoint().port())};
    {
        auto pending = runtime.loops_[0].start(group.prepare_listeners(caller, abandoned_configs)).get();
        auto cold_commit = std::move(pending).activate();
    }
    // The next transaction also joins the abandoned transaction's rollback.
    auto replacement = runtime.loops_[0].start(group.prepare_listeners(caller, abandoned_configs)).get();
    runtime.loops_[0].start(std::move(replacement).cancel()).get();
    auto stopped = runtime.loops_[0].start(group.prepare_listeners(caller, abandoned_configs)).get();
    runtime.loops_[0].start(group.shutdown(caller)).get();
    EXPECT_THROW(runtime.loops_[0].start(std::move(stopped).activate()).get(), std::logic_error);
}

TEST(Ruvia, independent_group_prepared_batch_second_bind_failure_rolls_back_without_interrupting_old_listener) {
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation original(io), first(io);
    asio::ip::tcp::acceptor blocker(io, {asio::ip::address_v4::loopback(), 0});
    const auto blocked = blocker.local_endpoint();
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    group.listen(plain(original.endpoint().port()));
    const auto caller = runtime.loops_[0].handle();
    runtime.loops_[0].start(group.start(caller)).get();
    const std::array configs{plain(first.endpoint().port()), plain(blocked.port())};
    EXPECT_THROW(runtime.loops_[0].start(group.prepare_listeners(caller, configs)).get(), std::system_error);
    EXPECT_NE(request(original.endpoint().port()).find(" 200 "), std::string::npos);
    asio::ip::tcp::socket refused(io);
    asio::error_code error;
    refused.connect(first.endpoint(), error);
    EXPECT_EQ(error, asio::error::connection_refused);
    blocker.close();
    auto prepared = runtime.loops_[0].start(group.prepare_listeners(caller, configs)).get();
    runtime.loops_[0].start(std::move(prepared).activate()).get();
    EXPECT_NE(request(first.endpoint().port()).find(" 200 "), std::string::npos);
    EXPECT_NE(request(blocked.port()).find(" 200 "), std::string::npos);
    EXPECT_NE(request(original.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(group.shutdown(caller)).get();
}

TEST(Ruvia, independent_group_force_remove_uses_listener_identity_not_reused_endpoint) {
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation first(io), second(io);
    const auto config = [](std::uint16_t port) {
        return ruvia::listen_config{.address_ = "127.0.0.1", .http_ = port, .dispatch_ = ruvia::listener_dispatch::connection};
    };
    ruvia::http_server_group group(runtime.loops_, {.connection_ = [](ruvia::server_connection& connection) -> ruvia::task<void> {
        while (auto bytes = co_await connection.read()) {
            co_await connection.write(*bytes);
        }
    }});
    group.listen(config(first.endpoint().port()));
    group.listen(config(second.endpoint().port()));
    const auto caller = runtime.loops_[0].handle();
    runtime.loops_[0].start(group.start(caller)).get();
    const auto echo_byte = [](asio::ip::tcp::socket& socket) {
        const char sent = 'x';
        char received = 0;
        asio::write(socket, asio::buffer(&sent, 1));
        asio::read(socket, asio::buffer(&received, 1));
        EXPECT_EQ(sent, received);
    };
    asio::ip::tcp::socket old(io), unaffected(io), replacement(io);
    old.connect(first.endpoint());
    unaffected.connect(second.endpoint());
    echo_byte(old);
    echo_byte(unaffected);
    const ruvia::listener_endpoint endpoint{.address_ = "127.0.0.1", .port_ = first.endpoint().port()};
    runtime.loops_[0].start(group.remove_listener(caller, endpoint)).get();
    runtime.loops_[0].start(group.add_listener(caller, config(first.endpoint().port()))).get();
    replacement.connect(first.endpoint());
    echo_byte(replacement);
    runtime.loops_[0].start(group.remove_listener(caller, endpoint, {.close_connections_ = true})).get();
    char byte = 0;
    std::error_code error;
    EXPECT_EQ(replacement.read_some(asio::buffer(&byte, 1), error), 0U);
    EXPECT_TRUE(error == asio::error::eof || error == asio::error::connection_reset);
    echo_byte(old);
    echo_byte(unaffected);
    runtime.loops_[0].start(group.shutdown(caller)).get();
    EXPECT_EQ(group.stats().active_connections_, 0U);
}

TEST(Ruvia, independent_group_ipv6_only_controls_tcp_and_quic_socket_families) {
    external_loops runtime;
    asio::io_context io;
    asio::ip::tcp::acceptor available(io);
    std::error_code error;
    available.open(asio::ip::tcp::v6(), error);
    if (error == asio::error::address_family_not_supported) {
        GTEST_SKIP() << "IPv6 is unavailable";
    }
    ASSERT_FALSE(error);
    available.close();
    const ruvia::test::tls_identity identity("ipv6-listener", "DNS:localhost,IP:::1");
    for (const bool only : {true, false}) {
        ruvia::test::tcp_port_reservation tcp(io);
        ruvia::test::udp_port_reservation udp(io);
        auto config = secure(tcp.endpoint().port(), udp.endpoint().port(), identity);
        config.address_ = "::";
        config.ipv6_only_ = only;
        ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
        group.listen(config);
        const auto caller = runtime.loops_[0].handle();
        runtime.loops_[0].start(group.start(caller)).get();
        asio::ssl::context tls(asio::ssl::context::tls_client);
        tls.load_verify_file(identity.ca_file_.string());
        tls.set_verify_mode(asio::ssl::verify_peer);
        asio::ssl::stream<asio::ip::tcp::socket> ipv6(io, tls);
        ipv6.next_layer().connect({asio::ip::address_v6::loopback(), tcp.endpoint().port()});
        ipv6.handshake(asio::ssl::stream_base::client);
        EXPECT_NE(request_on(ipv6).find(" 200 "), std::string::npos);
        asio::ip::tcp::socket ipv4(io);
        ipv4.connect(tcp.endpoint(), error);
        EXPECT_EQ(static_cast<bool>(error), only);
#if defined(RUVIA_ENABLE_HTTP3)
        ruvia::test::http3_test_client quic({asio::ip::address_v6::loopback(), udp.endpoint().port()}, identity.ca_file_, "localhost", 5s);
        EXPECT_EQ(quic.request("GET", "/worker").status_, 200);
#endif
#if !defined(_WIN32)
        auto duplicates = runtime.loops_[0].start(group.duplicate_listener_sockets(caller, {.address_ = "::", .port_ = tcp.endpoint().port()})).get();
        for (const auto socket : duplicates.https_) {
            asio::ip::tcp::acceptor owner(io);
            owner.assign(asio::ip::tcp::v6(), socket);
            asio::ip::v6_only value;
            owner.get_option(value);
            EXPECT_EQ(value.value(), only);
        }
        for (const auto socket : duplicates.http3_) {
            asio::ip::udp::socket owner(io);
            owner.assign(asio::ip::udp::v6(), socket);
            asio::ip::v6_only value;
            owner.get_option(value);
            EXPECT_EQ(value.value(), only);
        }
#endif
        runtime.loops_[0].start(group.shutdown(caller)).get();
    }
}

#if defined(RUVIA_ENABLE_HTTP3)
TEST(Ruvia, independent_group_force_remove_closes_tls_and_quic_but_keeps_other_listener) {
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation secure_port(io), other(io);
    ruvia::test::udp_port_reservation udp(io);
    const ruvia::test::tls_identity identity("forced-listener", "DNS:localhost");
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    group.listen(secure(secure_port.endpoint().port(), udp.endpoint().port(), identity));
    group.listen(plain(other.endpoint().port()));
    const auto caller = runtime.loops_[0].handle();
    runtime.loops_[0].start(group.start(caller)).get();
    asio::ssl::context tls(asio::ssl::context::tls_client);
    tls.load_verify_file(identity.ca_file_.string());
    tls.set_verify_mode(asio::ssl::verify_peer);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, tls);
    stream.next_layer().connect(secure_port.endpoint());
    stream.handshake(asio::ssl::stream_base::client);
    ruvia::test::http3_test_client quic(udp.endpoint(), identity.ca_file_, "localhost", 5s);
    EXPECT_EQ(quic.request("GET", "/worker").status_, 200);
    runtime.loops_[0].start(group.remove_listener(caller, {.address_ = "127.0.0.1", .port_ = secure_port.endpoint().port()}, {.close_connections_ = true})).get();
    char byte = 0;
    std::error_code error;
    EXPECT_EQ(stream.read_some(asio::buffer(&byte, 1), error), 0U);
    EXPECT_TRUE(error);
    EXPECT_ANY_THROW((void)quic.request("GET", "/worker"));
    EXPECT_NE(request(other.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(group.shutdown(caller)).get();
}
#endif

TEST(Ruvia, independent_group_force_remove_from_its_own_handler_does_not_join_itself) {
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    std::binary_semaphore removed{0};
    ruvia::http_server_group* owner = nullptr;
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
        co_await owner->remove_listener(stream.worker(),
            {.address_ = "127.0.0.1", .port_ = port.endpoint().port()}, {.close_connections_ = true});
        removed.release();
    }});
    owner = &group;
    group.listen(plain(port.endpoint().port()));
    const auto caller = runtime.loops_[0].handle();
    runtime.loops_[0].start(group.start(caller)).get();
    asio::ip::tcp::socket peer(io);
    peer.connect(port.endpoint());
    constexpr std::string_view head = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    asio::write(peer, asio::buffer(head));
    EXPECT_TRUE(removed.try_acquire_for(5s));
    runtime.loops_[0].start(group.shutdown(caller)).get();
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, independent_group_force_remove_closes_http2_connection_not_only_streams) {
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io), other(io);
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = &answer});
    group.listen(plain(port.endpoint().port()));
    group.listen(plain(other.endpoint().port()));
    const auto caller = runtime.loops_[0].handle();
    runtime.loops_[0].start(group.start(caller)).get();
    asio::ip::tcp::socket peer(io);
    peer.connect(port.endpoint());
    std::pmr::string preface("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", ruvia::process_resource());
    ruvia::test::append_frame(preface, ruvia::http2_frame_type::settings, 0, 0, {});
    asio::write(peer, asio::buffer(preface));
    const auto settings = ruvia::test::read_frame(peer, std::chrono::steady_clock::now() + 5s);
    EXPECT_EQ(settings.header_.type_, static_cast<std::uint8_t>(ruvia::http2_frame_type::settings));
    runtime.loops_[0].start(group.remove_listener(caller, {.address_ = "127.0.0.1", .port_ = port.endpoint().port()}, {.close_connections_ = true})).get();
    std::array<char, 1024> bytes{};
    std::error_code error;
    while (peer.read_some(asio::buffer(bytes), error) != 0) {
    }
    EXPECT_TRUE(error == asio::error::eof || error == asio::error::connection_reset);
    EXPECT_NE(request(other.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(group.shutdown(caller)).get();
}
#endif
