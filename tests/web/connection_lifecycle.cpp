#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"

#include "http_server_fixture.h"
#if defined(RUVIA_ENABLE_HTTP2)
#include "http_server_fixture.h"
#endif

#if defined(RUVIA_ENABLE_HTTP3)
#include "http_server_fixture.h"
#endif

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <variant>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/read_until.hpp>
#include <asio/socket_base.hpp>
#include <asio/ssl/error.hpp>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/objects.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/http/http_client.h"
#include "ruvia/web/middleware.h"

#if defined(RUVIA_ENABLE_HTTP3)
#include <asio/ip/udp.hpp>

#include "http_server_fixture.h"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
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
#include <asio/ssl.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/middleware.h"

#include "http_server_fixture.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include "ruvia/http/http3_connection.h"

#include "http_server_fixture.h"
#endif
#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/http2_connection.h"

#include "http_server_fixture.h"
#endif
#include <asio/error.hpp>
namespace connection_lifecycle_test {
using namespace std::chrono_literals;

template <typename value_type>
concept endpoint_address_borrowable = requires(value_type&& value) {
    std::forward<value_type>(value).address();
};
template <typename value_type>
concept connection_endpoints_borrowable = requires(value_type&& value) {
    std::forward<value_type>(value).local();
    std::forward<value_type>(value).remote();
};
static_assert(endpoint_address_borrowable<const ruvia::ip_endpoint&>);
static_assert(endpoint_address_borrowable<ruvia::ip_endpoint>);
static_assert(connection_endpoints_borrowable<const ruvia::connection_event&>);
static_assert(!connection_endpoints_borrowable<ruvia::connection_event>);

struct loops final {
    asio::io_context first_, second_;
    ruvia::blocking_pool pool_{{.thread_count_ = 2}};
    ruvia::event_loop_attachment first_attachment_{ruvia::attach_event_loop(first_, {.blocking_pool_ = &pool_})};
    ruvia::event_loop_attachment second_attachment_{ruvia::attach_event_loop(second_, {.blocking_pool_ = &pool_})};
    std::array<ruvia::event_loop, 2> handles_{first_attachment_.loop(), second_attachment_.loop()};
    std::jthread first_thread_{[this] { first_attachment_.run(); }};
    std::jthread second_thread_{[this] { second_attachment_.run(); }};
    ~loops() {
        first_attachment_.stop();
        second_attachment_.stop();
        first_thread_.join();
        second_thread_.join();
        pool_.join();
    }
};

ruvia::listen_config raw(std::uint16_t port) {
    return {.address_ = "127.0.0.1", .http_ = port, .dispatch_ = ruvia::listener_dispatch::connection};
}

ruvia::listen_config secure(std::string address, std::uint16_t tcp,
    const ruvia::test::tls_identity& identity, std::uint16_t udp = 0) {
    ruvia::listen_config result{.address_ = address, .https_ = tcp, .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
    if (udp != 0) {
        result.http3_ = udp;
    }
#else
    (void)udp;
#endif
    return result;
}

bool closed(asio::ip::tcp::socket& socket) {
    socket.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::array<char, 1024> bytes;
    while (std::chrono::steady_clock::now() < deadline) {
        asio::error_code error;
        socket.read_some(asio::buffer(bytes), error);
        if (error && error != asio::error::would_block && error != asio::error::try_again) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

struct payload final {
    std::atomic<unsigned>& destroyed_;
    explicit payload(std::atomic<unsigned>& destroyed)
        : destroyed_(destroyed) {}
    ~payload() {
        ++destroyed_;
    }
};

}  // namespace connection_lifecycle_test

TEST(Ruvia, connection_admission_rejects_before_tcp_tls_or_quic_handshake) {
    using namespace connection_lifecycle_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation plain(io), tls(io);
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    std::atomic<unsigned> admissions{}, quic_admissions{}, opens{}, closes{}, hellos{}, handlers{};
    ruvia::http_server_group server(runtime.handles_, [&](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{.request_stream_ = [&](ruvia::request_stream&) -> ruvia::task<void> { ++handlers; co_return; },
                                                                                          .connection_ = [&](ruvia::server_connection&) -> ruvia::task<void> { ++handlers; co_return; },
                                                                                          .tls_client_hello_ = [&](const ruvia::tls_client_hello_record&) noexcept { ++hellos; },
                                                                                          .connection_admission_ = [&](const ruvia::connection_event& event) noexcept {
            EXPECT_NE(event.connection_id(), 0u);
            EXPECT_EQ(event.local().address(), *ruvia::ip_address::parse("127.0.0.1"));
            EXPECT_EQ(event.remote().address(), *ruvia::ip_address::parse("127.0.0.1"));
            EXPECT_NE(event.remote().port(), 0);
            ++admissions;
            if (event.quic()){ ++quic_admissions;
}
            return false; },
                                                                                          .connection_open_ = [&](const ruvia::connection_event&) -> std::shared_ptr<void> { ++opens; return {}; },
                                                                                          .connection_close_ = [&](const ruvia::connection_event&, const std::shared_ptr<void>&, const ruvia::connection_close_info&) noexcept { ++closes; }}; });
    server.listen(raw(plain.endpoint().port()));
    std::uint16_t udp_port{};
#if defined(RUVIA_ENABLE_HTTP3)
    asio::ip::udp::socket udp_picker(io, {asio::ip::address_v4::loopback(), 0});
    udp_port = udp_picker.local_endpoint().port();
    udp_picker.close();
#endif
    server.listen(secure("127.0.0.1", tls.endpoint().port(), identity, udp_port));
    runtime.handles_[0].start(server.start(runtime.handles_[0].worker())).get();
    asio::ip::tcp::socket peer(io);
    peer.connect(plain.endpoint());
    EXPECT_TRUE(closed(peer));
    asio::ssl::context tls_context(asio::ssl::context::tls_client);
    asio::ssl::stream<asio::ip::tcp::socket> encrypted(io, tls_context);
    encrypted.next_layer().connect(tls.endpoint());
    asio::error_code handshake_error;
    encrypted.handshake(asio::ssl::stream_base::client, handshake_error);
    EXPECT_TRUE(handshake_error);
#if defined(RUVIA_ENABLE_HTTP3)
    EXPECT_THROW((ruvia::test::http3_test_client({asio::ip::address_v4::loopback(), udp_port},
                     identity.ca_file_, "localhost", 300ms)),
        std::runtime_error);
    EXPECT_GT(quic_admissions.load(), 0u);
#endif
    runtime.handles_[0].start(server.shutdown(runtime.handles_[0].worker())).get();
    EXPECT_GE(admissions.load(), 2u);
    EXPECT_EQ(opens.load(), 0u);
    EXPECT_EQ(closes.load(), 0u);
    EXPECT_EQ(hellos.load(), 0u);
    EXPECT_EQ(handlers.load(), 0u);
}

TEST(Ruvia, connection_registry_closes_adopted_and_accepted_ids_and_retains_owned_state) {
    using namespace connection_lifecycle_test;
    loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation listening(io);
    asio::ip::tcp::acceptor adoptor(io, {asio::ip::address_v4::loopback(), 0});
    std::atomic<unsigned> destroyed{}, open_count{}, close_count{}, failures{};
    std::atomic<std::uint64_t> active_id{};
    std::counting_semaphore<> entered{0}, ended{0};
    std::shared_ptr<void> retained;
    std::atomic<bool> throw_open{};
    ruvia::http_server_group server(runtime.handles_, [&](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{.connection_ = [&](ruvia::server_connection& connection) -> ruvia::task<void> {
                                                                                                                              EXPECT_EQ(connection.local().address().to_string(), "127.0.0.1");
                                                                                                                              EXPECT_NE(connection.local().port(), 0);
                                                                                                                              EXPECT_NE(connection.connection_state(), nullptr);
                                                                                                                              retained = connection.connection_state();
                                                                                                                              active_id = connection.connection_id();
                                                                                                                              entered.release();
                                                                                                                              try {
                                                                                                                                  (void)co_await connection.read();
                                                                                                                              } catch (const std::system_error&) {
                                                                                                                              }
                                                                                                                          },
                                                                                          .connection_failure_ = [&](const ruvia::connection_failure_record&) noexcept { ++failures; },
                                                                                          .connection_open_ = [&](const ruvia::connection_event&) -> std::shared_ptr<void> {
                                                                                              ++open_count;
                                                                                              if (throw_open.load()) {
                                                                                                  throw std::runtime_error("open factory failed");
                                                                                              }
                                                                                              return std::make_shared<payload>(destroyed);
                                                                                          },
                                                                                          .connection_close_ = [&](const ruvia::connection_event& event, const std::shared_ptr<void>& state, const ruvia::connection_close_info&) noexcept {
            EXPECT_NE(event.connection_id(), 0u);
            EXPECT_EQ(state == nullptr, throw_open.load());
            ++close_count;
            ended.release(); }}; });
    server.listen(raw(listening.endpoint().port()));
    runtime.handles_[0].start(server.add_connection_service(runtime.handles_[0].worker(), raw(adoptor.local_endpoint().port()))).get();
    runtime.handles_[0].start(server.start(runtime.handles_[0].worker())).get();
    std::uint64_t previous{};
    for (std::size_t index = 0; index != server.worker_count() + 1; ++index) {
        asio::ip::tcp::socket peer(io);
        if (index == server.worker_count()) {
            peer.connect(listening.endpoint());
        } else {
            peer.connect(adoptor.local_endpoint());
            auto adopted = adoptor.accept();
            runtime.handles_[0].start(server.adopt(runtime.handles_[0].worker(), index, {.address_ = "127.0.0.1", .port_ = adoptor.local_endpoint().port()}, adopted.release())).get();
        }
        ASSERT_TRUE(entered.try_acquire_for(5s));
        const auto id = active_id.load();
        EXPECT_NE(id, previous);
        EXPECT_FALSE(runtime.handles_[0].start(server.close_connection(runtime.handles_[0].worker(), previous)).get());
        EXPECT_TRUE(runtime.handles_[0].start(server.close_connection(runtime.handles_[0].worker(), id)).get());
        EXPECT_TRUE(closed(peer));
        ASSERT_TRUE(ended.try_acquire_for(5s));
        EXPECT_FALSE(runtime.handles_[0].start(server.close_connection(runtime.handles_[0].worker(), id)).get());
        previous = id;
        EXPECT_NE(retained, nullptr);
        retained.reset();
    }
    throw_open = true;
    asio::ip::tcp::socket failed(io);
    failed.connect(listening.endpoint());
    EXPECT_TRUE(closed(failed));
    ASSERT_TRUE(ended.try_acquire_for(5s));
    runtime.handles_[0].start(server.shutdown(runtime.handles_[0].worker())).get();
    EXPECT_EQ(open_count.load(), close_count.load());
    EXPECT_EQ(destroyed.load(), server.worker_count() + 1);
    EXPECT_EQ(failures.load(), 1u);
}

TEST(Ruvia, single_owner_connection_close_event_can_reenter_retired_id_control) {
    using namespace connection_lifecycle_test;
    loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    std::atomic<std::uint64_t> id{};
    std::binary_semaphore opened{0}, ended{0};
    ruvia::http_server* owner{};
    ruvia::http_server server(runtime.handles_[0], {.connection_ = [&](ruvia::server_connection& connection) -> ruvia::task<void> {
                                                        EXPECT_NE(connection.connection_state(), nullptr);
                                                        opened.release();
                                                        try {
                                                            (void)co_await connection.read();
                                                        } catch (const std::system_error&) {
                                                        }
                                                    },
                                                       .connection_open_ = [&](const ruvia::connection_event& event) -> std::shared_ptr<void> {
                                                           id = event.connection_id();
                                                           return std::make_shared<int>(42);
                                                       },
                                                       .connection_close_ = [&](const ruvia::connection_event& event, const std::shared_ptr<void>& state, const ruvia::connection_close_info&) noexcept {
            EXPECT_FALSE(owner->close_connection(event.connection_id()));
            EXPECT_EQ(*std::static_pointer_cast<int>(state), 42);
            ended.release(); }});
    owner = &server;
    const auto start = [&]() -> ruvia::task<void> { server.listen(raw(port.endpoint().port())); co_await server.start(); };
    runtime.handles_[0].start(start()).get();
    asio::ip::tcp::socket peer(io);
    peer.connect(port.endpoint());
    ASSERT_TRUE(opened.try_acquire_for(5s));
    const auto close = [&]() -> ruvia::task<void> { EXPECT_TRUE(server.close_connection(id.load())); co_return; };
    runtime.handles_[0].start(close()).get();
    EXPECT_TRUE(closed(peer));
    ASSERT_TRUE(ended.try_acquire_for(5s));
    runtime.handles_[0].start(server.shutdown()).get();
}

#if defined(RUVIA_ENABLE_HTTP3)
TEST(Ruvia, quic_wildcard_connections_report_destination_and_reply_from_that_address) {
    using namespace connection_lifecycle_test;
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1,IP:127.0.0.2,IP:::1");
    for (const bool v6 : {false, true}) {
        loops runtime;
        asio::io_context io;
        const auto protocol = v6 ? asio::ip::udp::v6() : asio::ip::udp::v4();
        asio::ip::udp::socket udp_picker(io, protocol);
        udp_picker.bind({v6 ? asio::ip::address(asio::ip::address_v6::loopback()) : asio::ip::address(asio::ip::address_v4::loopback()), 0});
        const auto udp_port = udp_picker.local_endpoint().port();
        udp_picker.close();
        asio::ip::tcp::acceptor tcp_picker(io, v6 ? asio::ip::tcp::v6() : asio::ip::tcp::v4());
        tcp_picker.bind({v6 ? asio::ip::address(asio::ip::address_v6::loopback()) : asio::ip::address(asio::ip::address_v4::loopback()), 0});
        const auto tcp_port = tcp_picker.local_endpoint().port();
        tcp_picker.close();
        std::atomic<unsigned> admitted{}, opened{}, closed_count{};
        ruvia::http_server_group server(runtime.handles_, [&](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
                                                                                                                                  EXPECT_EQ(stream.conn().local().port(), udp_port);
                                                                                                                                  EXPECT_EQ(*std::static_pointer_cast<int>(stream.connection_state()), 7);
                                                                                                                                  if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
                                                                                                                                      co_await stream.response().write(stream.conn().local().address().to_string());
                                                                                                                                      co_await stream.response().end();
                                                                                                                                  }
                                                                                                                              },
                                                                                              .connection_admission_ = [&](const ruvia::connection_event& event) noexcept {
                EXPECT_TRUE(event.quic());
                EXPECT_FALSE(event.local().address() == ruvia::ip_address{});
                EXPECT_EQ(event.local().port(), udp_port);
                ++admitted;
                return true; },
                                                                                              .connection_open_ = [&](const ruvia::connection_event&) -> std::shared_ptr<void> { ++opened; return std::make_shared<int>(7); },
                                                                                              .connection_close_ = [&](const ruvia::connection_event&, const std::shared_ptr<void>&, const ruvia::connection_close_info&) noexcept { ++closed_count; }}; });
        server.listen(secure(v6 ? "::" : "0.0.0.0", tcp_port, identity, udp_port));
        runtime.handles_[0].start(server.start(runtime.handles_[0].worker())).get();
        const std::vector<std::string> destinations = v6 ? std::vector<std::string>{"::1"} : std::vector<std::string>{"127.0.0.1", "127.0.0.2"};
        for (const auto& destination : destinations) {
            ruvia::test::http3_test_client client({asio::ip::make_address(destination), udp_port}, identity.ca_file_, "localhost", 5s);
            const auto response = client.request("GET", "/");
            EXPECT_EQ(response.status_, 200);
            EXPECT_EQ(std::string_view(response.body_), std::string_view(destination));
            client.migrate();
            const auto migrated = client.request("GET", "/migrated");
            EXPECT_EQ(migrated.status_, 200);
            EXPECT_EQ(std::string_view(migrated.body_), std::string_view(destination));
            client.close();
        }
        runtime.handles_[0].start(server.shutdown(runtime.handles_[0].worker())).get();
        EXPECT_EQ(admitted.load(), destinations.size());
        EXPECT_EQ(opened.load(), destinations.size());
        EXPECT_EQ(closed_count.load(), opened.load());
    }
}
#endif

TEST(Ruvia, accepted_tls_handshake_failure_and_shutdown_each_close_once) {
    using namespace connection_lifecycle_test;
    loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost");
    std::atomic<unsigned> opens{}, closes{};
    std::counting_semaphore<> opened{0}, ended{0};
    ruvia::http_server_group server(runtime.handles_, [&](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{.request_stream_ = [](ruvia::request_stream&) -> ruvia::task<void> { co_return; },
                                                                                          .connection_open_ = [&](const ruvia::connection_event&) -> std::shared_ptr<void> {
                                                                                              ++opens;
                                                                                              opened.release();
                                                                                              return {};
                                                                                          },
                                                                                          .connection_close_ = [&](const ruvia::connection_event&, const std::shared_ptr<void>& state, const ruvia::connection_close_info&) noexcept {
            EXPECT_EQ(state, nullptr); ++closes; ended.release(); }}; });
    server.listen(secure("127.0.0.1", port.endpoint().port(), identity));
    runtime.handles_[0].start(server.start(runtime.handles_[0].worker())).get();
    asio::ip::tcp::socket malformed(io);
    malformed.connect(port.endpoint());
    ASSERT_TRUE(opened.try_acquire_for(5s));
    const std::string garbage = "not a TLS record";
    asio::write(malformed, asio::buffer(garbage));
    EXPECT_TRUE(closed(malformed));
    ASSERT_TRUE(ended.try_acquire_for(5s));
    asio::ip::tcp::socket pending(io);
    pending.connect(port.endpoint());
    ASSERT_TRUE(opened.try_acquire_for(5s));
    runtime.handles_[0].start(server.shutdown(runtime.handles_[0].worker())).get();
    EXPECT_TRUE(closed(pending));
    ASSERT_TRUE(ended.try_acquire_for(5s));
    EXPECT_EQ(opens.load(), 2u);
    EXPECT_EQ(closes.load(), 2u);
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, request_stream_abort_connection_wakes_every_http2_body_reader) {
    using namespace connection_lifecycle_test;
    loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    std::counting_semaphore<> entered{0}, left{0};
    std::atomic<unsigned> opens{}, closes{}, failures{};
    ruvia::http_server_group server(runtime.handles_, [&](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
                                                                                                                              EXPECT_EQ(stream.conn().local().port(), port.endpoint().port());
                                                                                                                              if (stream.request().path() == "/abort") {
                                                                                                                                  stream.abort_connection();
                                                                                                                                  co_return;
                                                                                                                              }
                                                                                                                              entered.release();
                                                                                                                              try {
                                                                                                                                  (void)co_await stream.body().text();
                                                                                                                              } catch (const std::system_error&) {
                                                                                                                              }
                                                                                                                              left.release();
                                                                                                                          },
                                                                                          .connection_failure_ = [&](const ruvia::connection_failure_record&) noexcept { ++failures; },
                                                                                          .connection_open_ = [&](const ruvia::connection_event&) -> std::shared_ptr<void> { ++opens; return {}; },
                                                                                          .connection_close_ = [&](const ruvia::connection_event&, const std::shared_ptr<void>&, const ruvia::connection_close_info&) noexcept { ++closes; }}; });
    server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
    runtime.handles_[0].start(server.start(runtime.handles_[0].worker())).get();
    asio::ip::tcp::socket peer(io);
    peer.connect(port.endpoint());
    std::string wire("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    const std::string waiting = std::string("\x83\x86\x04\x05", 4) + "/wait" + std::string("\x01\x09", 2) + "localhost";
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x04, 1, waiting);
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x04, 3, waiting);
    asio::write(peer, asio::buffer(wire));
    ASSERT_TRUE(entered.try_acquire_for(5s));
    ASSERT_TRUE(entered.try_acquire_for(5s));
    wire.clear();
    const std::string aborting = std::string("\x82\x86\x04\x06", 4) + "/abort" + std::string("\x01\x09", 2) + "localhost";
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x05, 5, aborting);
    asio::write(peer, asio::buffer(wire));
    EXPECT_TRUE(closed(peer));
    EXPECT_TRUE(left.try_acquire_for(5s));
    EXPECT_TRUE(left.try_acquire_for(5s));
    runtime.handles_[0].start(server.shutdown(runtime.handles_[0].worker())).get();
    EXPECT_EQ(opens.load(), 1u);
    EXPECT_EQ(closes.load(), 1u);
    EXPECT_EQ(failures.load(), 0u);
}
#endif

#if defined(RUVIA_ENABLE_HTTP3)
TEST(Ruvia, request_stream_abort_connection_wakes_every_http3_body_reader) {
    using namespace connection_lifecycle_test;
    loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation tcp(io);
    asio::ip::udp::socket udp_picker(io, {asio::ip::address_v4::loopback(), 0});
    const auto udp_port = udp_picker.local_endpoint().port();
    udp_picker.close();
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost");
    std::counting_semaphore<> entered{0}, left{0};
    std::atomic<unsigned> opens{}, closes{}, failures{};
    ruvia::http_server_group server(runtime.handles_, [&](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
                                                                                                                              EXPECT_EQ(stream.conn().local().port(), udp_port);
                                                                                                                              if (stream.request().path() == "/abort") {
                                                                                                                                  stream.abort_connection();
                                                                                                                                  co_return;
                                                                                                                              }
                                                                                                                              entered.release();
                                                                                                                              try {
                                                                                                                                  (void)co_await stream.body().text();
                                                                                                                              } catch (const std::system_error&) {
                                                                                                                              }
                                                                                                                              left.release();
                                                                                                                          },
                                                                                          .connection_failure_ = [&](const ruvia::connection_failure_record&) noexcept { ++failures; },
                                                                                          .connection_open_ = [&](const ruvia::connection_event&) -> std::shared_ptr<void> { ++opens; return {}; },
                                                                                          .connection_close_ = [&](const ruvia::connection_event&, const std::shared_ptr<void>&, const ruvia::connection_close_info&) noexcept { ++closes; }}; });
    server.listen(secure("127.0.0.1", tcp.endpoint().port(), identity, udp_port));
    runtime.handles_[0].start(server.start(runtime.handles_[0].worker())).get();
    ruvia::test::http3_test_client client({asio::ip::address_v4::loopback(), udp_port}, identity.ca_file_, "localhost", 5s);
    (void)client.start_streaming_request("POST", "/wait");
    (void)client.start_streaming_request("POST", "/wait");
    // QUIC pacing is pumped while waiting for server output.
    unsigned active{};
    ASSERT_TRUE(client.wait_until([&] {
        while (entered.try_acquire()) {
            ++active;
        }
        return active == 2;
    },
        5s));
    (void)client.start_request("GET", "/abort");
    EXPECT_TRUE(client.wait_until([&] { return client.closed(); }, 5s));
    EXPECT_TRUE(left.try_acquire_for(5s));
    EXPECT_TRUE(left.try_acquire_for(5s));
    runtime.handles_[0].start(server.shutdown(runtime.handles_[0].worker())).get();
    EXPECT_EQ(opens.load(), 1u);
    EXPECT_EQ(closes.load(), 1u);
    EXPECT_EQ(failures.load(), 0u);
}
#endif

namespace connection_lifecycle_test {
class lifecycle_controller final {
public:
    RUVIA_ROUTES_BEGIN(lifecycle_controller)
    RUVIA_GET("/lifecycle/info", info);
    RUVIA_GET("/lifecycle/virtual", virtual_request);
    RUVIA_GET("/lifecycle/abort", abort);
    RUVIA_ROUTES_END
    ruvia::task<ruvia::http_response> info(ruvia::context& context) {
        const auto state = std::static_pointer_cast<int>(context.connection_state());
        co_return context.text(std::string(context.conn().local().address().to_string()) + ":" +
                               std::to_string(context.conn().local().port()) + ":" + std::to_string(state ? *state : 0));
    }
    ruvia::task<ruvia::http_response> virtual_request(ruvia::context& context) {
        const auto response = co_await context.dispatch({.method_ = "GET", .target_ = "/lifecycle/info"});
        co_return context.text(response.body());
    }
    ruvia::task<ruvia::http_response> abort(ruvia::context& context) {
        context.abort_connection();
        co_return context.text("must not reach the peer");
    }
};
}  // namespace connection_lifecycle_test

TEST(Ruvia, application_hooks_connection_id_control_context_abort_and_virtual_local_are_real) {
    using namespace connection_lifecycle_test;
    loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    std::binary_semaphore ready{0};
    std::counting_semaphore<> opened{0}, ended{0};
    std::atomic<unsigned> admissions{}, opens{}, closes{};
    std::atomic<std::uint64_t> id{};
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port()})
        .on_start(nullptr)
        .on_start([&] { ready.release(); })
        .on_connection_admission([&](const ruvia::connection_event&) noexcept { ++admissions; return true; })
        .on_connection_open([&](const ruvia::connection_event& event) -> std::shared_ptr<void> {
            id = event.connection_id();
            ++opens;
            opened.release();
            return std::make_shared<int>(19);
        })
        .on_connection_close([&](const ruvia::connection_event&, const std::shared_ptr<void>& state, const ruvia::connection_close_info&) noexcept {
            EXPECT_EQ(*std::static_pointer_cast<int>(state), 19);
            ++closes;
            ended.release();
        });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(ready.try_acquire_for(10s));
    asio::ip::tcp::socket pending(io);
    pending.connect(port.endpoint());
    ASSERT_TRUE(opened.try_acquire_for(5s));
    EXPECT_TRUE(runtime.handles_[0].start(app.close_connection(runtime.handles_[0].worker(), id.load())).get());
    EXPECT_TRUE(closed(pending));
    ASSERT_TRUE(ended.try_acquire_for(5s));
    EXPECT_FALSE(runtime.handles_[0].start(app.close_connection(runtime.handles_[0].worker(), id.load())).get());
    for (const std::string_view path : {"/lifecycle/info", "/lifecycle/virtual", "/lifecycle/abort"}) {
        asio::ip::tcp::socket peer(io);
        peer.connect(port.endpoint());
        const std::string request = "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        asio::write(peer, asio::buffer(request));
        std::string response;
        std::array<char, 1024> bytes;
        asio::error_code error;
        while (!error) {
            const auto count = peer.read_some(asio::buffer(bytes), error);
            response.append(bytes.data(), count);
        }
        if (path == "/lifecycle/abort") {
            EXPECT_TRUE(response.empty());
        } else {
            EXPECT_NE(response.find("127.0.0.1:" + std::to_string(port.endpoint().port()) + ":19"), std::string::npos);
        }
        ASSERT_TRUE(opened.try_acquire_for(5s));
        ASSERT_TRUE(ended.try_acquire_for(5s));
    }
    app.stop();
    runner.join();
    EXPECT_EQ(admissions.load(), 4u);
    EXPECT_EQ(opens.load(), closes.load());
    app.on_start(nullptr).on_connection_admission(nullptr).on_connection_open(nullptr).on_connection_close(nullptr);
}

TEST(Ruvia, request_stream_abort_connection_prevents_pipelining_and_preserves_callback_failures) {
    using namespace connection_lifecycle_test;
    for (const bool fail : {false, true}) {
        SCOPED_TRACE(fail);
        loops runtime;
        asio::io_context io;
        ruvia::test::tcp_port_reservation port(io);
        std::atomic<unsigned> dispatched{}, closes{}, failures{};
        ruvia::http_server_group server(runtime.handles_, [&](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
                                                                                                                                  ++dispatched;
                                                                                                                                  EXPECT_EQ(stream.connection_state(), nullptr);
                                                                                                                                  EXPECT_EQ(stream.conn().local().address().to_string(), "127.0.0.1");
                                                                                                                                  EXPECT_EQ(stream.conn().local().port(), port.endpoint().port());
                                                                                                                                  stream.abort_connection();
                                                                                                                                  if (fail) {
                                                                                                                                      throw std::runtime_error("callback failed after abort");
                                                                                                                                  }
                                                                                                                                  co_return;
                                                                                                                              },
                                                                                              .connection_failure_ = [&](const ruvia::connection_failure_record&) noexcept { ++failures; },
                                                                                              .connection_close_ = [&](const ruvia::connection_event&, const std::shared_ptr<void>& state, const ruvia::connection_close_info&) noexcept {
            EXPECT_EQ(state, nullptr);
            ++closes; }}; });
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        runtime.handles_[0].start(server.start(runtime.handles_[0].worker())).get();
        asio::ip::tcp::socket peer(io);
        peer.connect(port.endpoint());
        const std::string requests =
            "GET /abort HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "GET /must-not-dispatch HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(peer, asio::buffer(requests));
        EXPECT_TRUE(closed(peer));
        runtime.handles_[0].start(server.shutdown(runtime.handles_[0].worker())).get();
        EXPECT_EQ(dispatched.load(), 1u);
        EXPECT_EQ(closes.load(), 1u);
        EXPECT_EQ(failures.load(), fail ? 1u : 0u);
    }
}

TEST(Ruvia, group_cold_shutdown_joins_connection_callbacks_after_facade_destruction) {
    using namespace connection_lifecycle_test;
    loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    std::binary_semaphore entered{0};
    std::atomic<unsigned> opens{}, closes{}, destroyed{};
    auto server = std::make_unique<ruvia::http_server_group>(runtime.handles_,
        [&](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{
                                            .connection_ = [&](ruvia::server_connection& connection) -> ruvia::task<void> {
                                                EXPECT_NE(connection.connection_state(), nullptr);
                                                entered.release();
                                                try {
                                                    (void)co_await connection.read();
                                                } catch (const std::system_error&) {
                                                }
                                            },
                                            .connection_open_ = [&](const ruvia::connection_event&) -> std::shared_ptr<void> {
                                                ++opens;
                                                return std::make_shared<payload>(destroyed);
                                            },
                                            .connection_close_ = [&](const ruvia::connection_event&, const std::shared_ptr<void>& state, const ruvia::connection_close_info&) noexcept {
                EXPECT_NE(state, nullptr);
                ++closes; }}; });
    server->listen(raw(port.endpoint().port()));
    runtime.handles_[0].start(server->start(runtime.handles_[0].worker())).get();
    asio::ip::tcp::socket peer(io);
    peer.connect(port.endpoint());
    ASSERT_TRUE(entered.try_acquire_for(5s));
    auto shutdown = server->shutdown(runtime.handles_[0].worker());
    server.reset();
    runtime.handles_[0].start(std::move(shutdown)).get();
    EXPECT_TRUE(closed(peer));
    EXPECT_EQ(opens.load(), 1U);
    EXPECT_EQ(closes.load(), 1U);
    EXPECT_EQ(destroyed.load(), 1U);
    EXPECT_TRUE(runtime.handles_[0].worker().accepting());
    EXPECT_TRUE(runtime.handles_[1].worker().accepting());
}

namespace lifecycle_test {

using namespace std::chrono_literals;

struct shared_state final {
    std::counting_semaphore<16> started_{0};
    std::atomic<unsigned> stop_hooks_{};
    std::atomic<bool> slow_entered_{};
    std::atomic<bool> release_slow_{};
    std::atomic<bool> slow_saw_stop_{};
    std::atomic<bool> stuck_entered_{};
    std::atomic<bool> stuck_cancelled_{};
    std::atomic<bool> upgrade_held_{};
    std::atomic<bool> upgrade_handler_ran_{};
    std::atomic<bool> socket_open_{};
    std::atomic<bool> tunnel_open_{};
};

shared_state& state() {
    static shared_state value;
    return value;
}

// Holds an upgrade inside its route chain until stop, polling on the worker
// so the worker keeps serving lifecycle controls meanwhile.
class held_upgrade final {
public:
    ruvia::task<void> invoke(ruvia::context& context, ruvia::next& next_value) {
        auto& state_value = state();
        state_value.upgrade_held_ = true;
        const auto stop = context.stop_token();
        while (!stop.stop_requested()) {
            (void)co_await ruvia::sleep_for(context.worker(), 1ms, stop);
        }
        co_await next_value();
    }
};

class app_lifecycle_controller final {
public:
    RUVIA_ROUTES_BEGIN(app_lifecycle_controller)
    RUVIA_GET("/fast", fast);
    RUVIA_GET("/slow", slow);
    RUVIA_GET("/stuck", stuck);
    RUVIA_GET_WS("/held-socket", held_socket, held_upgrade);
    RUVIA_GET_WS("/socket", open_socket);
    RUVIA_CONNECT("lifecycle-target.test:443", echo_tunnel);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> fast(ruvia::context& context) {
        co_return context.text(std::string_view("fast"));
    }

    // Finishes once the test releases it or the worker stops, polling on the
    // worker so the worker keeps serving lifecycle controls meanwhile.
    ruvia::task<ruvia::http_response> slow(ruvia::context& context) {
        auto& state_value = state();
        state_value.slow_entered_ = true;
        const auto stop = context.stop_token();
        while (!state_value.release_slow_.load() && !stop.stop_requested()) {
            (void)co_await ruvia::sleep_for(context.worker(), 1ms, stop);
        }
        state_value.slow_saw_stop_ = stop.stop_requested();
        co_return context.text(std::string_view("slow-done"));
    }

    // Ends only on its stop token.
    ruvia::task<ruvia::http_response> stuck(ruvia::context& context) {
        auto& state_value = state();
        state_value.stuck_entered_ = true;
        const auto result = co_await ruvia::sleep_for(context.worker(), 1h, context.stop_token());
        state_value.stuck_cancelled_ = result == ruvia::timer_sleep_result::stop_requested;
        co_return context.text(std::string_view("stuck-done"));
    }

    // Lives until the peer ends the session.
    ruvia::task<void> held_socket(ruvia::context& context) {
        state().upgrade_handler_ran_ = true;
        auto& websocket_value = context.websocket();
        try {
            while (co_await websocket_value.read()) {
            }
        } catch (...) {
        }
    }

    // Reads until the session ends.
    ruvia::task<void> open_socket(ruvia::context& context) {
        auto& websocket_value = context.websocket();
        state().socket_open_ = true;
        try {
            while (co_await websocket_value.read()) {
            }
        } catch (...) {
        }
    }

    // Echoes until the peer ends its send direction.
    ruvia::task<void> echo_tunnel(ruvia::context& context) {
        auto& tunnel_value = context.tunnel();
        state().tunnel_open_ = true;
        while (auto bytes = co_await tunnel_value.read()) {
            co_await tunnel_value.write(*bytes);
        }
        co_await tunnel_value.finish();
    }
};

// Hooks accumulate on the process-wide application, so every case shares one set.
void install_hooks() {
    static std::once_flag once;
    std::call_once(once, [] {
        ruvia::app()
            .on_start([] { state().started_.release(); })
            .on_stop([] { state().stop_hooks_.fetch_add(1); });
    });
}

using ruvia::test::app_run;

// Appends received bytes until `done` holds or the peer ends the stream.
// Returns true when the peer ended it.
template <typename predicate_type>
bool read_until(asio::ip::tcp::socket& socket, std::string& received, predicate_type&& done,
    std::chrono::milliseconds limit = 5s) {
    socket.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::array<char, 1024> buffer;
    while (!done(received) && std::chrono::steady_clock::now() < deadline) {
        asio::error_code error;
        const auto size = socket.read_some(asio::buffer(buffer), error);
        received.append(buffer.data(), size);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (error) {
            return true;
        }
    }
    return false;
}

bool read_to_end(asio::ip::tcp::socket& socket, std::string& received, std::chrono::milliseconds limit = 5s) {
    return read_until(socket, received, [](const std::string&) { return false; }, limit);
}

// The bytes after the response head, or empty while the head is incomplete.
[[nodiscard]] std::string_view after_head(std::string_view received) {
    const auto head_end = received.find("\r\n\r\n");
    return head_end == std::string_view::npos ? std::string_view{} : received.substr(head_end + 4);
}

void send(asio::ip::tcp::socket& socket, std::string_view bytes) {
    asio::write(socket, asio::buffer(bytes));
}

void send_websocket_upgrade(asio::ip::tcp::socket& socket, std::string_view path) {
    send(socket, "GET ");
    send(socket, path);
    send(socket,
        " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
}

}  // namespace lifecycle_test

TEST(Ruvia, app_multi_worker_restarts_after_idle_connections_and_pending_accepts) {
    using namespace lifecycle_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    install_hooks();
    auto& app = ruvia::app();
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        app.server({.worker_count_ = 2, .connection_scan_interval_ = 5ms, .max_connections_per_worker_ = 64});
        app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
        app_run run(app);
        EXPECT_TRUE(state().started_.try_acquire_for(3s));
        EXPECT_EQ(app.workers().size(), std::size_t{2});
        std::vector<asio::ip::tcp::socket> idle{};
        if (cycle != 0) {
            // Connecting without sending data must not hold up later accepts.
            for (unsigned i = 0; i < 16; ++i) {
                idle.emplace_back(io);
                idle.back().connect(endpoint);
            }
            for (unsigned i = 0; i < 24; ++i) {
                asio::ip::tcp::socket aborted(io);
                aborted.connect(endpoint);
                aborted.set_option(asio::socket_base::linger(true, 0));
                aborted.close();
            }
            std::vector<asio::ip::tcp::socket> requests{};
            for (unsigned i = 0; i < 24; ++i) {
                requests.emplace_back(io);
                requests.back().connect(endpoint);
                send(requests.back(), "GET /fast HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
            }
            for (auto& socket : requests) {
                std::string response{};
                EXPECT_TRUE(read_to_end(socket, response));
                EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
                EXPECT_TRUE(response.ends_with("fast"));
            }
        }
        // Cycle zero stops with no client; later cycles also retire connected
        // peers that never sent a request. Rebinding the port exercises reuse.
        app.stop();
        run.join();
        EXPECT_TRUE(run.failure() == nullptr);
        for (auto& socket : idle) {
            std::string response{};
            EXPECT_TRUE(read_to_end(socket, response));
            EXPECT_TRUE(response.empty());
        }
        EXPECT_TRUE(app.workers().empty());
        EXPECT_EQ(app.stats().active_connections_, std::size_t{0});
        asio::ip::tcp::socket stopped(io);
        asio::error_code error;
        stopped.connect(endpoint, error);
        EXPECT_TRUE(error == asio::error::connection_refused);
    }
}

TEST(Ruvia, app_unsigned_max_timeouts_preserve_response_delivery) {
    using namespace lifecycle_test;
    install_hooks();
    auto& app = ruvia::app();
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    const auto huge = std::chrono::duration<std::uint64_t, std::milli>{
        (std::numeric_limits<std::uint64_t>::max)()};
    app.server({.worker_count_ = 1,
        .idle_timeout_ = huge,
        .connection_scan_interval_ = huge,
        .request_header_timeout_ = huge,
        .request_body_timeout_ = huge,
        .header_completion_timeout_ = huge,
        .body_completion_timeout_ = huge,
        .write_timeout_ = huge});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    app_run run(app);
    ASSERT_TRUE(state().started_.try_acquire_for(3s));
    asio::ip::tcp::socket client(io);
    client.connect(endpoint);
    send(client, "GET /fast HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    std::string response{};
    EXPECT_TRUE(read_to_end(client, response));
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(response.ends_with("fast"));
    client.close();
    app.stop();
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
}

// A stop disconnects directly, with no drain phase: an idle keep-alive
// connection, a connection still sending its request head and one with a
// request in flight all close at once without a response, the in-flight
// handler observes its stop token, and the stop hooks run once.
TEST(Ruvia, app_stop_closes_every_connection_at_once) {
    using namespace lifecycle_test;
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.slow_entered_ = false;
    state_value.release_slow_ = false;
    state_value.slow_saw_stop_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    const auto stop_hooks = state_value.stop_hooks_.load();

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));

    // An idle keep-alive connection, after one served request.
    asio::ip::tcp::socket idle(io);
    idle.connect(endpoint);
    send(idle, "GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
    std::string idle_response{};
    EXPECT_TRUE(!read_until(idle, idle_response, [](const std::string& value) { return value.ends_with("fast"); }));
    EXPECT_TRUE(idle_response.starts_with("HTTP/1.1 200 "));

    asio::ip::tcp::socket partial(io);
    partial.connect(endpoint);
    send(partial, "GET /fa");

    asio::ip::tcp::socket busy(io);
    busy.connect(endpoint);
    send(busy, "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n");
    ruvia::test::wait_until([&] { return state_value.slow_entered_.load(); }, 3s);
    ruvia::test::wait_until([&] { return app.stats().active_connections_ == 3; }, 3s);

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    for (auto* socket : {&idle, &partial, &busy}) {
        std::string rest{};
        EXPECT_TRUE(read_to_end(*socket, rest, 3s));
        EXPECT_TRUE(rest.empty());
    }
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
    EXPECT_TRUE(std::chrono::steady_clock::now() - stop_started < 3s);
    EXPECT_TRUE(state_value.slow_saw_stop_.load());
    EXPECT_EQ(state_value.stop_hooks_.load(), stop_hooks + 1);
    EXPECT_EQ(app.stats().active_connections_, std::size_t{0});
}

// A request that never ends by itself does not hold the stop: its connection
// closes without a response and its handler observes the stop token.
TEST(Ruvia, app_stop_cancels_a_request_that_never_ends) {
    using namespace lifecycle_test;
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.stuck_entered_ = false;
    state_value.stuck_cancelled_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    send(socket, "GET /stuck HTTP/1.1\r\nHost: localhost\r\n\r\n");
    ruvia::test::wait_until([&] { return state_value.stuck_entered_.load(); }, 3s);

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string response{};
    EXPECT_TRUE(read_to_end(socket, response, 3s));
    EXPECT_TRUE(response.empty());
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
    EXPECT_TRUE(std::chrono::steady_clock::now() - stop_started < 3s);
    EXPECT_TRUE(state_value.stuck_cancelled_.load());
}

// An upgrade still inside its route chain when the stop begins is never
// established: its connection closes without a 101 and the session handler
// never runs.
TEST(Ruvia, app_stop_closes_an_upgrade_still_in_its_route_chain) {
    using namespace lifecycle_test;
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.upgrade_held_ = false;
    state_value.upgrade_handler_ran_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));
    asio::ip::tcp::socket upgrading(io);
    upgrading.connect(endpoint);
    send_websocket_upgrade(upgrading, "/held-socket");
    ruvia::test::wait_until([&] { return state_value.upgrade_held_.load(); }, 3s);

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string received{};
    EXPECT_TRUE(read_to_end(upgrading, received, 3s));
    EXPECT_TRUE(received.empty());
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
    EXPECT_TRUE(!state_value.upgrade_handler_ran_.load());
    EXPECT_TRUE(std::chrono::steady_clock::now() - stop_started < 3s);
}

// An established WebSocket session is closed at once, without a Close
// handshake: nothing follows the 101 response before the connection ends.
TEST(Ruvia, app_stop_closes_websockets_without_close_handshake) {
    using namespace lifecycle_test;
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.socket_open_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));
    asio::ip::tcp::socket client(io);
    client.connect(endpoint);
    send_websocket_upgrade(client, "/socket");
    std::string received{};
    EXPECT_TRUE(!read_until(client, received, [](const std::string& value) { return value.find("\r\n\r\n") != std::string::npos; }));
    EXPECT_TRUE(received.starts_with("HTTP/1.1 101 "));
    ruvia::test::wait_until([&] { return state_value.socket_open_.load(); }, 3s);

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    EXPECT_TRUE(read_to_end(client, received, 3s));
    EXPECT_TRUE(after_head(received).empty());
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
    EXPECT_TRUE(std::chrono::steady_clock::now() - stop_started < 3s);
}

// A tunnel has no in-band close signal and is not waited for: the stop
// closes it at once, while its peer still holds it open.
TEST(Ruvia, app_stop_closes_tunnels_at_once) {
    using namespace lifecycle_test;
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.tunnel_open_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));
    asio::ip::tcp::socket tunnel(io);
    tunnel.connect(endpoint);
    send(tunnel, "CONNECT lifecycle-target.test:443 HTTP/1.1\r\nHost: lifecycle-target.test:443\r\n\r\n");
    std::string head{};
    EXPECT_TRUE(!read_until(tunnel, head, [](const std::string& value) { return value.ends_with("\r\n\r\n"); }));
    EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
    ruvia::test::wait_until([&] { return state_value.tunnel_open_.load(); }, 3s);
    send(tunnel, "ping");
    std::string echoed{};
    EXPECT_TRUE(!read_until(tunnel, echoed, [](const std::string& value) { return value.size() >= 4; }));
    EXPECT_EQ(echoed, std::string("ping"));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string rest{};
    EXPECT_TRUE(read_to_end(tunnel, rest, 3s));
    EXPECT_TRUE(rest.empty());
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
    EXPECT_TRUE(std::chrono::steady_clock::now() - stop_started < 3s);
}

// At max_connections_per_worker_, a new connection takes the slot of the
// connection idle longest between requests, whose socket closes; only when no
// connection is idle is the new one refused.
TEST(Ruvia, app_connection_cap_evicts_the_oldest_idle_connection) {
    using namespace lifecycle_test;
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.stuck_entered_ = false;
    state_value.slow_entered_ = false;
    state_value.release_slow_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .max_connections_per_worker_ = 2});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));
    const auto served = [](asio::ip::tcp::socket& socket) {
        send(socket, "GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
        std::string response{};
        const bool ended =
            read_until(socket, response, [](const std::string& value) { return value.ends_with("fast"); });
        return !ended && response.starts_with("HTTP/1.1 200 ");
    };
    asio::ip::tcp::socket oldest(io);
    oldest.connect(endpoint);
    EXPECT_TRUE(served(oldest));
    asio::ip::tcp::socket newer(io);
    newer.connect(endpoint);
    EXPECT_TRUE(served(newer));
    ruvia::test::wait_until([&] { return app.stats().active_connections_ == 2; }, 3s);

    asio::ip::tcp::socket admitted(io);
    admitted.connect(endpoint);
    EXPECT_TRUE(served(admitted));
    std::string evicted{};
    EXPECT_TRUE(read_to_end(oldest, evicted));
    EXPECT_TRUE(evicted.empty());
    EXPECT_TRUE(served(newer));
    EXPECT_EQ(app.stats().connections_refused_, std::size_t{0});

    // With a request in flight on both, no connection is idle.
    send(newer, "GET /stuck HTTP/1.1\r\nHost: localhost\r\n\r\n");
    ruvia::test::wait_until([&] { return state_value.stuck_entered_.load(); }, 3s);
    send(admitted, "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n");
    ruvia::test::wait_until([&] { return state_value.slow_entered_.load(); }, 3s);
    asio::ip::tcp::socket refused(io);
    refused.connect(endpoint);
    std::string refused_bytes{};
    EXPECT_TRUE(read_to_end(refused, refused_bytes));
    EXPECT_TRUE(refused_bytes.empty());
    EXPECT_EQ(app.stats().connections_refused_, std::size_t{1});

    app.stop();
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
}

// Connection timeouts never fire early because of the scanner's coarse clock,
// even when the scan interval exceeds the timeout, and they still fire.
TEST(Ruvia, app_connection_timeouts_honor_coarse_scan_clock) {
    using namespace lifecycle_test;
    install_hooks();
    auto& app = ruvia::app();
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    constexpr auto header_timeout = 200ms;
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 300ms, .request_header_timeout_ = header_timeout});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state().started_.try_acquire_for(3s));

    // A head trickling in every 50 ms stays active across several scans.
    asio::ip::tcp::socket active(io);
    active.connect(endpoint);
    send(active, "GET /fast HTTP/1.1\r\nHost: localhost\r\nX-Pad: ");
    for (int i = 0; i < 24; ++i) {
        std::this_thread::sleep_for(50ms);
        send(active, "a");
    }
    send(active, "\r\n\r\n");
    std::string response{};
    EXPECT_TRUE(!read_until(active, response, [](const std::string& value) { return value.ends_with("fast"); }));
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));

    // A connection that sends nothing is closed, but not before its timeout.
    const auto connected = std::chrono::steady_clock::now();
    asio::ip::tcp::socket silent(io);
    silent.connect(endpoint);
    std::string silent_bytes{};
    EXPECT_TRUE(read_to_end(silent, silent_bytes, 5s));
    EXPECT_TRUE(std::chrono::steady_clock::now() - connected >= header_timeout);
    EXPECT_TRUE(silent_bytes.empty());

    app.stop();
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
}

#ifdef RUVIA_ENABLE_REDIS
// A stop while a worker capability is still connecting cancels startup: run()
// returns normally, and no worker handle was published before readiness.
TEST(Ruvia, app_stop_during_capability_connect_returns_normally) {
    using namespace lifecycle_test;
    install_hooks();
    auto& app = ruvia::app();
    asio::io_context io;
    // Accepts the Redis connection and never answers its AUTH.
    asio::ip::tcp::acceptor silent_redis(io, {asio::ip::address_v4::loopback(), 0});
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    app.redis({.config_ = {.port_ = silent_redis.local_endpoint().port(),
                   .password_ = "secret",
                   .pool_size_per_worker_ = 1,
                   .connect_timeout_ = std::nullopt}});

    std::optional<app_run> run(std::in_place, app);
    silent_redis.non_blocking(true);
    asio::ip::tcp::socket peer(io);
    ruvia::test::wait_until([&] {
        asio::error_code error;
        silent_redis.accept(peer, error);
        return !error;
    },
        3s);
    std::string auth{};
    EXPECT_TRUE(!read_until(peer, auth, [](const std::string& value) { return !value.empty(); }));
    EXPECT_TRUE(!auth.empty());
    EXPECT_TRUE(app.workers().empty());
    EXPECT_TRUE(!app.worker_for(std::uint64_t{0}).valid());

    app.stop();
    run->join();
    EXPECT_TRUE(run->failure() == nullptr);
    run.reset();
    app.redis(nullptr);
}
#endif

namespace tls_paths_test {

using namespace std::chrono_literals;

class tls_path_controller final {
public:
    RUVIA_ROUTES_BEGIN(tls_path_controller)
    RUVIA_GET("/tls", secure);
    RUVIA_GET("/tls-peer", peer_identity);
    RUVIA_GET("/tls-truncated", truncated);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> secure(ruvia::context& context) {
        co_return context.text(std::string_view("secure"));
    }

    ruvia::task<ruvia::http_response> peer_identity(ruvia::context& context) {
        const auto connection = context.conn();
        const auto subject = connection.tls()->client_certificate_subject();
        co_return context.text(!subject           ? std::string_view("absent")
                               : subject->empty() ? std::string_view("present-empty")
                                                  : *subject);
    }

    // Commits the response, then fails before the body is complete.
    ruvia::task<void> truncated(ruvia::context& context) {
        if (co_await context.stream_text().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await context.stream().write("partial");
        throw std::runtime_error("upstream failed");
    }
};

// A directory whose name no legacy ANSI code page can represent: CJK,
// Cyrillic and a character outside the Basic Multilingual Plane.
class non_ascii_directory final {
public:
    non_ascii_directory()
        : directory_("ruvia-tls-\xe8\xaf\x81\xe4\xb9\xa6-\xd1\x82\xd0\xb5\xd1\x81\xd1\x82-\xf0\x9f\x94\x92") {}

    non_ascii_directory(const non_ascii_directory&) = delete;
    non_ascii_directory& operator=(const non_ascii_directory&) = delete;

    [[nodiscard]] std::filesystem::path file(std::u8string_view name) const {
        return directory_.path() / std::filesystem::path(name);
    }

private:
    ruvia::test::scoped_temp_directory directory_;
};

// Written through the native path API, independent of the server's loader.
void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("cannot write TLS test file");
    }
}

std::string encrypted_private_key(ruvia::test::tls_identity& identity, std::string_view password) {
    const std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
    if (!output || ruvia::test::write_tls_private_key(output.get(),
                       SSL_CTX_get0_privatekey(identity.context_.native_handle()), password, true) != 1) {
        throw std::runtime_error("cannot encrypt TLS test private key");
    }
    char* data = nullptr;
    const auto size = BIO_get_mem_data(output.get(), &data);
    if (size <= 0 || data == nullptr) {
        throw std::runtime_error("cannot read encrypted TLS test private key");
    }
    return std::string(data, static_cast<std::size_t>(size));
}

// A verified identity may have no Subject attributes (RFC 5280 section 4.1.2.6).
// A separate, conforming CA avoids relying on direct trust of an empty-subject
// leaf. The existing named test identity remains separately trusted.
asio::ssl::context empty_subject_client(ruvia::test::tls_identity& issuer,
    const std::filesystem::path& trust_bundle) {
    auto* const key = SSL_CTX_get0_privatekey(issuer.context_.native_handle());
    const auto authority = std::unique_ptr<X509, decltype(&X509_free)>(X509_new(), X509_free);
    const auto authority_name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    if (!authority || !authority_name || X509_set_version(authority.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(authority.get()), 3) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(authority.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(authority.get()), 86400) ||
        X509_set_pubkey(authority.get(), key) != 1 ||
        X509_NAME_add_entry_by_txt(authority_name.get(), "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("empty-subject-client-ca"), -1, -1, 0) != 1 ||
        X509_set_subject_name(authority.get(), authority_name.get()) != 1 ||
        X509_set_issuer_name(authority.get(), authority_name.get()) != 1) {
        throw std::runtime_error("cannot create empty-subject client CA");
    }
    X509V3_CTX authority_context{};
    X509V3_set_ctx(&authority_context, authority.get(), authority.get(), nullptr, nullptr, 0);
    const auto constraints = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>(
        X509V3_EXT_nconf_nid(nullptr, &authority_context, NID_basic_constraints, "critical,CA:TRUE"),
        X509_EXTENSION_free);
    if (!constraints || X509_add_ext(authority.get(), constraints.get(), -1) != 1 ||
        ruvia::test::sign_tls_certificate(authority.get(), key) <= 0) {
        throw std::runtime_error("cannot sign empty-subject client CA");
    }
    const auto certificate = std::unique_ptr<X509, decltype(&X509_free)>(X509_new(), X509_free);
    const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    if (!certificate || !subject || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 2) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) ||
        X509_set_pubkey(certificate.get(), key) != 1 ||
        X509_set_subject_name(certificate.get(), subject.get()) != 1 ||
        X509_set_issuer_name(certificate.get(), authority_name.get()) != 1) {
        throw std::runtime_error("cannot create empty-subject client identity");
    }
    X509V3_CTX extension_context{};
    X509V3_set_ctx(&extension_context, authority.get(), certificate.get(), nullptr, nullptr, 0);
    const auto alternative_name = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>(
        X509V3_EXT_nconf_nid(nullptr, &extension_context, NID_subject_alt_name,
            "critical,URI:urn:ruvia:test:client"),
        X509_EXTENSION_free);
    if (!alternative_name || X509_add_ext(certificate.get(), alternative_name.get(), -1) != 1 ||
        ruvia::test::sign_tls_certificate(certificate.get(), key) <= 0) {
        throw std::runtime_error("cannot sign empty-subject client identity");
    }
    asio::ssl::context client(asio::ssl::context::tls_client);
    if (SSL_CTX_use_certificate(client.native_handle(), certificate.get()) != 1 ||
        SSL_CTX_use_PrivateKey(client.native_handle(), key) != 1) {
        throw std::runtime_error("cannot load empty-subject client identity");
    }
    const auto encoded = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), BIO_free);
    if (!encoded || PEM_write_bio_X509(encoded.get(), authority.get()) != 1) {
        throw std::runtime_error("cannot encode empty-subject client certificate");
    }
    char* bytes = nullptr;
    const auto size = BIO_get_mem_data(encoded.get(), &bytes);
    std::ofstream output(trust_bundle, std::ios::binary | std::ios::app);
    if (size <= 0 || bytes == nullptr || !output.write(bytes, size)) {
        throw std::runtime_error("cannot trust empty-subject client certificate");
    }
    return client;
}

template <typename fn_type>
bool throws_system_error(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::system_error&) {
        return true;
    }
}

// The peer deliberately permits legacy algorithms so TLS 1.0 rejection and
// TLS 1.1 success exercise the listener, not the test client's defaults.
void configure_client_version(asio::ssl::context& context, int version) {
    if (version == 0) {
        return;
    }
    if (SSL_CTX_set_min_proto_version(context.native_handle(), version) != 1 ||
        SSL_CTX_set_max_proto_version(context.native_handle(), version) != 1) {
        throw std::runtime_error("cannot configure the TLS test client version");
    }
    if (version <= TLS1_1_VERSION) {
        SSL_CTX_set_security_level(context.native_handle(), 0);
    }
}

// Verifies the server chain against the test CA; `host` is both the SNI name
// and, when `verify_host` is set, the name the certificate must match.
std::string https_get(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    const std::filesystem::path& ca_file, const char* host, bool verify_host,
    std::string_view target = "/tls", SSL_CTX* client_identity = nullptr, int version = 0) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.load_verify_file(ca_file.string());
    client.set_verify_mode(asio::ssl::verify_peer);
    configure_client_version(client, version);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (client_identity != nullptr &&
        (SSL_use_certificate(stream.native_handle(), SSL_CTX_get0_certificate(client_identity)) != 1 ||
            SSL_use_PrivateKey(stream.native_handle(), SSL_CTX_get0_privatekey(client_identity)) != 1)) {
        throw std::runtime_error("cannot present TLS test client identity");
    }
    if (SSL_set_tlsext_host_name(stream.native_handle(), host) != 1 ||
        (verify_host && SSL_set1_host(stream.native_handle(), host) != 1)) {
        return std::string{};
    }
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    const auto request = std::string("GET ") + std::string(target) + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (!error) {
        asio::write(stream, asio::buffer(request), error);
    }
    std::string response{};
    std::array<char, 1024> buffer;
    while (!error) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    return response;
}

bool serves_secure_body(const std::string& response, std::string_view expected = "secure") {
    const auto body_start = response.find("\r\n\r\n");
    return response.starts_with("HTTP/1.1 200 ") && body_start != std::string::npos &&
           std::string_view(response).substr(body_start + 4) == expected;
}

// RFC 9112 §9.8: an HTTP/1.0 response body is delimited by the close, so a
// stream that fails after its head must end without close_notify; a clean TLS
// close would make the truncated body read as complete.
bool truncated_close_delimited_stream_lacks_close_notify(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    constexpr std::string_view request = "GET /tls-truncated HTTP/1.0\r\nHost: localhost\r\n\r\n";
    if (!error) {
        asio::write(stream, asio::buffer(request), error);
    }
    if (error) {
        return false;
    }
    std::string response{};
    std::array<char, 1024> buffer;
    while (!error) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    return response.starts_with("HTTP/1.1 200 ") && response.ends_with("\r\n\r\npartial") &&
           error == asio::ssl::error::stream_truncated;
}

struct alpn_handshake final {
    asio::error_code error_;
    std::string selected_{};
};

// Handshakes offering `protocols` (ALPN wire format) and reports the protocol
// the server selected.
alpn_handshake negotiate_alpn(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    std::string_view protocols, int version = 0) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    configure_client_version(client, version);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (SSL_set_alpn_protos(stream.native_handle(), reinterpret_cast<const unsigned char*>(protocols.data()),
            static_cast<unsigned int>(protocols.size())) != 0) {
        throw std::runtime_error("cannot offer ALPN protocols");
    }
    alpn_handshake result;
    stream.next_layer().connect(endpoint, result.error_);
    if (!result.error_) {
        stream.handshake(asio::ssl::stream_base::client, result.error_);
    }
    if (!result.error_) {
        const unsigned char* selected = nullptr;
        unsigned int size = 0;
        SSL_get0_alpn_selected(stream.native_handle(), &selected, &size);
        result.selected_.assign(reinterpret_cast<const char*>(selected), size);
    }
    return result;
}

// RFC 7301 §3.2: a client offering no protocol the server speaks gets a fatal
// no_application_protocol alert rather than a connection without ALPN.
bool refuses_with_no_application_protocol(const alpn_handshake& handshake) {
    return handshake.error_.category() == asio::error::get_ssl_category() &&
           ERR_GET_REASON(static_cast<unsigned long>(handshake.error_.value())) == SSL_R_TLSV1_ALERT_NO_APPLICATION_PROTOCOL;
}

bool bounded_tls_connection_ends(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, int version) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    configure_client_version(client, version);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    std::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (error) {
        return false;
    }
    stream.handshake(asio::ssl::stream_base::client, error);
    if (error) {
        return false;
    }
    unsigned completed = 0;
    for (; completed != 64; ++completed) {
        if (version == TLS1_3_VERSION && completed % 4 == 0 &&
            SSL_key_update(stream.native_handle(), SSL_KEY_UPDATE_REQUESTED) != 1) {
            return false;
        }
        error.clear();
        constexpr std::string_view request = "GET /tls HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(stream, asio::buffer(request), error);
        if (error) {
            return completed != 0;
        }
        bool done = false;
        std::string response{};
        asio::async_read_until(stream, asio::dynamic_buffer(response), "\r\n\r\nsecure",
            [&](const std::error_code& result, std::size_t) { error = result; done = true; });
        io.restart();
        io.run_for(1s);
        if (!done) {
            stream.next_layer().close();
            io.restart();
            io.run();
            return false;
        }
        if (error) {
            return completed != 0;
        }
    }
    return false;
}

// A client context that authenticates nothing; tests choose version and offer.
asio::ssl::context cipher_client(const char* cipher_suites, int version) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    configure_client_version(client, version);
    if (SSL_CTX_set_cipher_list(client.native_handle(), cipher_suites) != 1) {
        throw std::runtime_error("cannot configure the TLS test client ciphers");
    }
    return client;
}

// The suite selected from the client's offer; empty when the handshake fails.
std::string selected_cipher(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    const char* offer, int version = TLS1_2_VERSION, const char* host = "localhost") {
    auto client = cipher_client(offer, version);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (SSL_set_tlsext_host_name(stream.native_handle(), host) != 1) {
        throw std::runtime_error("cannot configure the TLS test SNI");
    }
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    if (error) {
        return std::string{};
    }
    return std::string(SSL_get_cipher_name(stream.native_handle()));
}

// 在指定 TLS 版本下建立会话，再以第二个 SNI 尝试恢复。
bool resumes_tls_session_under(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    int version, const char* first, const char* second) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    configure_client_version(client, version);
    std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> session(nullptr, SSL_SESSION_free);
    for (const char* name : {first, second}) {
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
        if (SSL_set_tlsext_host_name(stream.native_handle(), name) != 1 ||
            (session && SSL_set_session(stream.native_handle(), session.get()) != 1)) {
            throw std::runtime_error("cannot prepare the TLS resumption client");
        }
        asio::error_code error;
        stream.next_layer().connect(endpoint, error);
        if (!error) {
            stream.handshake(asio::ssl::stream_base::client, error);
        }
        if (error) {
            throw std::system_error(error, "TLS resumption handshake failed");
        }
        if (session) {
            return SSL_session_reused(stream.native_handle()) == 1;
        }
        if (version == TLS1_3_VERSION) {
            // TLS 1.3 的 ticket 在握手后发送；读取实际响应以处理 NewSessionTicket。
            const auto request = std::string("GET /tls HTTP/1.1\r\nHost: ") + name + "\r\nConnection: close\r\n\r\n";
            asio::write(stream, asio::buffer(request), error);
            std::array<char, 1024> buffer;
            while (!error) {
                (void)stream.read_some(asio::buffer(buffer), error);
            }
            if (error != asio::error::eof) {
                throw std::system_error(error, "TLS session ticket read failed");
            }
        }
        session.reset(SSL_get1_session(stream.native_handle()));
        if (!session || SSL_SESSION_is_resumable(session.get()) != 1) {
            throw std::runtime_error("TLS session is not resumable");
        }
        // A session whose SSL is freed without a local shutdown is marked
        // unresumable; record a clean local close instead.
        SSL_set_shutdown(stream.native_handle(), SSL_SENT_SHUTDOWN);
    }
    return false;
}

}  // namespace tls_paths_test

// Every TLS file (certificate chain, encrypted private key, client CA bundle
// and the SNI identity) lives under a path that no ANSI code page can encode.
// Startup prepares the staged credentials off-worker and cleanses its password
// copy; the same listener must serve HTTPS on a second run without listen().
// Each run also negotiates ALPN: any HTTP version the server speaks when
// offered (RFC 7301 §3.2), a fatal alert otherwise. TLS 1.1 negotiates RSA/ECDSA
// ECDHE+AES-CBC and HTTP/1.x only; TLS 1.0 is refused. TLS 1.2 stays ECDHE+AEAD
// in the server's order (RFC 9325 §4.1-4.2.1). Every supported version enforces
// the outgoing record budget and isolates resumed sessions by server_name.
TEST(Ruvia, https_serves_credentials_from_non_ascii_paths_across_runs) {
    using namespace tls_paths_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    // The SNI identity reuses this certificate, so it must also name sni.test
    // (RFC 9110 §7.4: other hosts get 421).
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,DNS:sni.test");
    ruvia::test::tls_identity elliptic_identity("ecdsa.test", "DNS:ecdsa.test", true);
    const non_ascii_directory directory;
    const auto certificate = directory.file(u8"\u8bc1\u4e66.pem");
    const auto private_key = directory.file(u8"\u79c1\u94a5-\U0001F511.pem");
    const auto client_ca = directory.file(u8"\u4fe1\u4efb-\u0434\u043e\u0432\u0435\u0440\u0438\u0435.pem");
    const std::string password{"correct horse battery staple"};
    std::filesystem::copy_file(identity.ca_file_, certificate);
    std::filesystem::copy_file(identity.ca_file_, client_ca);
    write_file(private_key, encrypted_private_key(identity, password));
    auto empty_client = empty_subject_client(identity, client_ca);

    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();

    auto& app = ruvia::app();
    const auto listener = [&](std::string_view key_password, std::filesystem::path chain) {
        return ruvia::listen_config{
            .address_ = "127.0.0.1",
            .https_ = endpoint.port(),
            .tls_ =
                {
                    .identity_ =
                        {
                            .certificate_chain_file_ = std::move(chain),
                            .private_key_file_ = private_key,
                            .private_key_password_ = std::string(key_password),
                        },
                    .client_certificates_ = {.ca_file_ = client_ca},
                    .sni_ = {
                        {
                            .host_ = "sni.test",
                            .identity_ =
                                {
                                    .certificate_chain_file_ = certificate,
                                    .private_key_file_ = private_key,
                                    .private_key_password_ = password,
                                },
                        },
                        {
                            .host_ = "ecdsa.test",
                            .identity_ =
                                {
                                    .certificate_chain_file_ = elliptic_identity.ca_file_,
                                    .private_key_file_ = elliptic_identity.private_key_file(),
                                },
                        },
                    },
                    .max_outgoing_records_ = 32,
                },
        };
    };
    // Invalid credentials never replace the application's staged listeners.
    app.server({.worker_count_ = 1});
    EXPECT_TRUE(throws_system_error([&] { app.listen(listener("wrong password", certificate)); }));
    EXPECT_TRUE(throws_system_error([&] { app.listen(listener(password, directory.file(u8"\u7f3a\u5931.pem"))); }));

    static std::binary_semaphore started{0};
    app.server({.worker_count_ = 1}).listen(listener(password, certificate)).on_start([] {
        started.release();
    });
    for (int round = 0; round < 2; ++round) {
        ruvia::test::app_run runner(app);
        const bool ready = started.try_acquire_for(10s);
        EXPECT_TRUE(ready);
        if (ready) {
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "localhost", true)));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "sni.test", false)));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "localhost", true,
                "/tls", nullptr, TLS1_1_VERSION)));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, elliptic_identity.ca_file_, "ecdsa.test", true,
                "/tls", nullptr, TLS1_1_VERSION)));
            EXPECT_TRUE(selected_cipher(io, endpoint, "ECDHE-RSA-AES128-SHA", TLS1_VERSION).empty());
            for (const char* cipher : {"ECDHE-RSA-AES128-SHA", "ECDHE-RSA-AES256-SHA"}) {
                EXPECT_EQ(selected_cipher(io, endpoint, cipher, TLS1_1_VERSION),
                    std::string(cipher));
            }
            for (const char* cipher : {"ECDHE-ECDSA-AES128-SHA", "ECDHE-ECDSA-AES256-SHA"}) {
                EXPECT_EQ(selected_cipher(io, endpoint, cipher, TLS1_1_VERSION, "ecdsa.test"),
                    std::string(cipher));
            }
            const auto legacy_alpn = negotiate_alpn(io, endpoint, "\x02h2\x08http/1.1", TLS1_1_VERSION);
            EXPECT_FALSE(legacy_alpn.error_);
            EXPECT_EQ(legacy_alpn.selected_, std::string("http/1.1"));
            EXPECT_TRUE(refuses_with_no_application_protocol(negotiate_alpn(io, endpoint, "\x02h2", TLS1_1_VERSION)));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "localhost", true,
                                               "/tls-peer"),
                "absent"));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "localhost", true,
                                               "/tls-peer", identity.context_.native_handle()),
                "CN=localhost"));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "localhost", true,
                                               "/tls-peer", empty_client.native_handle()),
                "present-empty"));
            EXPECT_TRUE(refuses_with_no_application_protocol(negotiate_alpn(io, endpoint, "\x04h2xx")));
            EXPECT_EQ(negotiate_alpn(io, endpoint, "\x02h2").selected_, std::string("h2"));
            const auto negotiated = negotiate_alpn(io, endpoint, "\x02h2\x08http/1.1");
            EXPECT_TRUE(!negotiated.error_);
            EXPECT_EQ(negotiated.selected_, std::string("h2"));
            EXPECT_TRUE(truncated_close_delimited_stream_lacks_close_notify(io, endpoint));
            EXPECT_EQ(negotiate_alpn(io, endpoint, "\x08http/1.0").selected_, std::string("http/1.0"));
            EXPECT_EQ(negotiate_alpn(io, endpoint, "\x08http/1.0\x08http/1.1").selected_, std::string("http/1.1"));
            EXPECT_TRUE(selected_cipher(io, endpoint, "AES128-SHA").empty());
            EXPECT_TRUE(selected_cipher(io, endpoint, "ECDHE-RSA-AES128-SHA").empty());
            EXPECT_TRUE(selected_cipher(io, endpoint, "ECDHE-ECDSA-AES128-SHA", TLS1_2_VERSION, "ecdsa.test").empty());
            EXPECT_EQ(selected_cipher(io, endpoint, "AES128-SHA:ECDHE-RSA-AES128-GCM-SHA256"),
                std::string("ECDHE-RSA-AES128-GCM-SHA256"));
            EXPECT_EQ(selected_cipher(io, endpoint, "ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES128-GCM-SHA256"),
                std::string("ECDHE-RSA-AES128-GCM-SHA256"));
            EXPECT_EQ(selected_cipher(io, endpoint, "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256"),
                std::string("ECDHE-RSA-AES128-GCM-SHA256"));
            for (const int version : {TLS1_1_VERSION, TLS1_2_VERSION, TLS1_3_VERSION}) {
                EXPECT_TRUE(bounded_tls_connection_ends(io, endpoint, version));
                EXPECT_TRUE(resumes_tls_session_under(io, endpoint, version, "localhost", "localhost"));
                EXPECT_TRUE(resumes_tls_session_under(io, endpoint, version, "sni.test", "SNI.test"));
                EXPECT_TRUE(!resumes_tls_session_under(io, endpoint, version, "localhost", "sni.test"));
                EXPECT_TRUE(!resumes_tls_session_under(io, endpoint, version, "localhost", "unknown.test"));
            }
        }
        app.stop();
        runner.join();
    }
}

namespace tls_identities_test {

using namespace std::chrono_literals;
using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;

std::binary_semaphore& started() {
    static std::binary_semaphore value{0};
    return value;
}

// The HTTPS port a route tries to replace identities on.
std::atomic<std::uint16_t>& replaced_port() {
    static std::atomic<std::uint16_t> value{0};
    return value;
}

ruvia::tls_config& worker_replacement() {
    static ruvia::tls_config value;
    return value;
}

// invalid_argument is a logic_error; the contract tells them apart.
template <typename fn_type>
std::string failure_of(fn_type&& fn) {
    try {
        fn();
        return "none";
    } catch (const std::invalid_argument&) {
        return "invalid_argument";
    } catch (const std::system_error&) {
        return "system_error";
    } catch (const std::logic_error&) {
        return "logic_error";
    }
}

class identity_controller final {
public:
    RUVIA_ROUTES_BEGIN(identity_controller)
    RUVIA_GET("/hello", hello);
    RUVIA_GET("/replace", replace);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> hello(ruvia::context& context) {
        co_return context.text(std::string_view("hello"));
    }

    // Runtime controls suspend the member worker instead of waiting on it.
    ruvia::task<ruvia::http_response> replace(ruvia::context& context) {
        try {
            co_await ruvia::app().replace_tls_identities(context.worker(),
                {.address_ = "127.0.0.1", .port_ = replaced_port().load()}, worker_replacement());
            co_return context.text(std::string_view("none"));
        } catch (const std::invalid_argument&) {
            co_return context.text(std::string_view("invalid_argument"));
        }
    }
};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    if (!input) {
        throw std::runtime_error("cannot read TLS test PEM");
    }
    return std::string(text.view());
}

ruvia::tls_identity_config file_identity(const ruvia::test::tls_identity& identity) {
    return {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()};
}

ruvia::tls_identity_config pem_identity(const ruvia::test::tls_identity& identity) {
    return {.certificate_chain_pem_ = read_text(identity.ca_file_),
        .private_key_pem_ = read_text(identity.private_key_file())};
}

// Authenticates nothing: tests read the certificate they were served.
asio::ssl::context tls_client(int version = 0) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    if (version != 0 && (SSL_CTX_set_min_proto_version(client.native_handle(), version) != 1 ||
                            SSL_CTX_set_max_proto_version(client.native_handle(), version) != 1)) {
        throw std::runtime_error("cannot configure the TLS test client version");
    }
    return client;
}

struct client_io_result final {
    asio::error_code error_;
    std::size_t size_{};
};

// Client contexts contain only fixture I/O. A deadline closes the transport,
// then drains the cancelled completion before its borrowed state can retire.
template <typename initiation_type>
client_io_result client_io(asio::ip::tcp::socket& socket, std::chrono::steady_clock::time_point deadline,
    initiation_type&& initiate) {
    auto& io = static_cast<asio::io_context&>(socket.get_executor().context());
    client_io_result result;
    bool done = false;
    initiate([&](const asio::error_code& error, auto... sizes) {
        result.error_ = error;
        ((result.size_ = sizes), ...);
        done = true;
    });
    io.restart();
    io.run_until(deadline);
    if (!done) {
        asio::error_code ignored;
        socket.close(ignored);
        io.restart();
        io.run();
        result.error_ = asio::error::timed_out;
    }
    return result;
}

// A completed handshake offering `server_name` (no SNI when null) and, when
// given, resuming `session`.
std::unique_ptr<tls_stream> connect_tls(asio::io_context& io, asio::ssl::context& client,
    const asio::ip::tcp::endpoint& endpoint, const char* server_name, SSL_SESSION* session = nullptr) {
    auto stream = std::make_unique<tls_stream>(io, client);
    // OpenSSL consumes TLS 1.3 tickets even when the attempted handshake fails.
    // Each attempt owns a copy, never the retained resumption snapshot.
    const std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> restored(
        session ? SSL_SESSION_dup(session) : nullptr, SSL_SESSION_free);
    if ((server_name != nullptr && SSL_set_tlsext_host_name(stream->native_handle(), server_name) != 1) ||
        (session != nullptr && (!restored || SSL_set_session(stream->native_handle(), restored.get()) != 1))) {
        throw std::runtime_error("cannot configure the TLS test connection");
    }
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    auto result = client_io(stream->next_layer(), deadline,
        [&](auto completion) { stream->next_layer().async_connect(endpoint, std::move(completion)); });
    if (!result.error_) {
        result = client_io(stream->next_layer(), deadline,
            [&](auto completion) { stream->async_handshake(asio::ssl::stream_base::client, std::move(completion)); });
    }
    if (result.error_) {
        throw std::system_error(result.error_, "TLS test connection failed");
    }
    return stream;
}

// The subject CN of the certificate the connection was served.
std::string peer_common_name(SSL* ssl) {
    const std::unique_ptr<X509, decltype(&X509_free)> peer(SSL_get1_peer_certificate(ssl), X509_free);
    if (!peer) {
        return {};
    }
    std::array<char, 256> name{};
    const auto size = X509_NAME_get_text_by_NID(
        X509_get_subject_name(peer.get()), NID_commonName, name.data(), static_cast<int>(name.size()));
    return size < 0 ? std::string() : std::string(name.data(), static_cast<std::size_t>(size));
}

// The CN a fresh handshake is served; empty when it fails.
std::string served_name(asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint,
    const char* server_name) {
    try {
        return peer_common_name(connect_tls(io, client, endpoint, server_name)->native_handle());
    } catch (const std::exception&) {
        return {};
    }
}

// One keep-alive GET /hello on an established connection.
bool serves_hello(tls_stream& stream) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    std::array<char, 1024> request;
    auto prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "localhost"}), {.method_ = "GET", .target_ = "/hello"}, request);
    auto& exchange = std::get<ruvia::prepared_http1_client_request>(prepared);
    ruvia::http1_client_response_parser parser(std::move(exchange).take_exchange_state());
    const auto written = client_io(stream.next_layer(), deadline,
        [&](auto completion) { asio::async_write(stream, asio::buffer(exchange.head()), std::move(completion)); });
    if (written.error_) {
        return false;
    }
    std::optional<ruvia::http1_client_response_body_decoder> decoder;
    std::string response, body;
    std::array<char, 1024> bytes, scratch;
    bool successful = false, eof = false;
    for (;;) {
        if (!decoder) {
            auto parsed = parser.parse(response);
            if (parsed.failure()) {
                return false;
            }
            if (const auto* head = parsed.parsed()) {
                if (head->plan().informational()) {
                    response.erase(0, head->consumed_bytes());
                    continue;
                }
                successful = head->head().status() == ruvia::http_status::ok;
                decoder.emplace(head->plan());
                response.erase(0, head->consumed_bytes());
            }
        }
        if (decoder) {
            const auto decoded = eof ? decoder->finish_input(response, scratch) : decoder->decode(response, scratch);
            if (const auto* output = decoded.output()) {
                body.append(output->bytes());
            }
            response.erase(0, decoded.consumed_bytes());
            if (decoded.complete()) {
                return successful && body == "hello";
            }
            if (decoded.protocol_failure() || decoded.decoder_failure()) {
                return false;
            }
            if (!decoded.need_input()) {
                continue;
            }
        }
        if (eof) {
            return false;
        }
        const auto received = client_io(stream.next_layer(), deadline,
            [&](auto completion) { stream.async_read_some(asio::buffer(bytes), std::move(completion)); });
        response.append(bytes.data(), received.size_);
        if (received.error_) {
            if (received.error_ != asio::error::eof) {
                return false;
            }
            eof = true;
        }
    }
}

bool serves_hello(asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint) {
    try {
        const auto stream = connect_tls(io, client, endpoint, "localhost");
        return serves_hello(*stream);
    } catch (const std::system_error&) {
        return false;
    }
}

template <typename stream_type>
std::string response_to_end(stream_type& stream, asio::ip::tcp::socket& socket,
    std::chrono::steady_clock::time_point deadline) {
    std::string response;
    std::array<char, 1024> buffer;
    for (;;) {
        const auto result = client_io(socket, deadline,
            [&](auto completion) { stream.async_read_some(asio::buffer(buffer), std::move(completion)); });
        response.append(buffer.data(), result.size_);
        if (result.error_ == asio::error::eof) {
            return response;
        }
        if (result.error_) {
            throw std::system_error(result.error_, "TLS fixture response failed");
        }
    }
}

// The complete response to GET /hello for `host` over a fresh connection.
std::string https_response(asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint,
    const char* server_name, std::string_view host) {
    const auto stream = connect_tls(io, client, endpoint, server_name);
    const auto request = "GET /hello HTTP/1.1\r\nHost: " + std::string(host) + "\r\nConnection: close\r\n\r\n";
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    const auto result = client_io(stream->next_layer(), deadline,
        [&](auto completion) { asio::async_write(*stream, asio::buffer(request), std::move(completion)); });
    if (result.error_) {
        throw std::system_error(result.error_, "TLS fixture request failed");
    }
    return response_to_end(*stream, stream->next_layer(), deadline);
}

std::string plain_body(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target) {
    asio::ip::tcp::socket socket(io);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    const auto connected = client_io(socket, deadline,
        [&](auto completion) { socket.async_connect(endpoint, std::move(completion)); });
    if (connected.error_) {
        throw std::system_error(connected.error_, "plain fixture connection failed");
    }
    const auto request = "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    const auto written = client_io(socket, deadline,
        [&](auto completion) { asio::async_write(socket, asio::buffer(request), std::move(completion)); });
    if (written.error_) {
        throw std::system_error(written.error_, "plain fixture request failed");
    }
    const auto response = response_to_end(socket, socket, deadline);
    const auto body = response.find("\r\n\r\n");
    return body == std::string::npos ? std::string() : response.substr(body + 4);
}

// A session of a completed exchange, kept resumable.
std::shared_ptr<SSL_SESSION> resumable_session(
    asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint) {
    const auto stream = connect_tls(io, client, endpoint, "localhost");
    // TLS 1.3 tickets follow the handshake; reading a response processes them.
    if (!serves_hello(*stream)) {
        throw std::runtime_error("TLS session exchange failed");
    }
    std::shared_ptr<SSL_SESSION> session(SSL_SESSION_dup(SSL_get0_session(stream->native_handle())), SSL_SESSION_free);
    if (!session || SSL_SESSION_is_resumable(session.get()) != 1) {
        throw std::runtime_error("TLS session is not resumable");
    }
    return session;
}

bool resumes(asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint,
    const std::shared_ptr<SSL_SESSION>& session) {
    const auto stream = connect_tls(io, client, endpoint, "localhost", session.get());
    return SSL_session_reused(stream->native_handle()) == 1 && serves_hello(*stream);
}

#if defined(RUVIA_ENABLE_HTTP3)
bool serves_hello(ruvia::test::http3_test_client& client) {
    const auto response = client.request("GET", "/hello");
    return response.status_ == 200 && response.body_ == "hello";
}

// Whether a fresh HTTP/3 connection verifying the server against `ca_file`
// for `server_name` (an IP literal sends no SNI) serves GET /hello.
bool http3_serves(std::uint16_t port, const std::filesystem::path& ca_file, const std::string& server_name) {
    try {
        ruvia::test::http3_test_client client({asio::ip::address_v4::loopback(), port}, ca_file, server_name, 3s);
        return serves_hello(client);
    } catch (const std::exception&) {
        return false;
    }
}
#endif

struct external_runtime final {
    ruvia::blocking_pool pool_{{.thread_count_ = 1, .queue_capacity_ = 64}};
    asio::io_context first_io_, second_io_;
    ruvia::event_loop_attachment first_{ruvia::attach_event_loop(first_io_, {.blocking_pool_ = &pool_})};
    ruvia::event_loop_attachment second_{ruvia::attach_event_loop(second_io_, {.blocking_pool_ = &pool_})};
    std::array<ruvia::event_loop, 2> loops_{first_.loop(), second_.loop()};
    std::jthread first_thread_{[this] { first_.run(); }};
    std::jthread second_thread_{[this] { second_.run(); }};
    ~external_runtime() {
        first_.stop();
        second_.stop();
    }
};

struct held_pool_job final {
    struct state final {
        std::binary_semaphore entered_{0}, release_{0};
    };
    std::shared_ptr<state> state_{std::make_shared<state>()};
    bool released_{false};

    explicit held_pool_job(ruvia::blocking_pool& pool) {
        if (pool.submit([state = state_] {
                state->entered_.release();
                state->release_.acquire();
            }) != ruvia::blocking_submit_status::accepted) {
            throw std::runtime_error("cannot occupy TLS preparation pool");
        }
        state_->entered_.acquire();
    }
    void release() {
        if (!std::exchange(released_, true)) {
            state_->release_.release();
        }
    }
    ~held_pool_job() {
        release();
    }
};

// Exercise the public operation itself, not a substitute blocking callable.
// Even a regression that blocks the worker is released before joining tasks.
std::exception_ptr queued_preparation(external_runtime& runtime, ruvia::task<void> operation) {
    const auto caller = runtime.loops_[0];
    held_pool_job occupied(runtime.pool_);
    std::atomic_bool completed{false};
    const auto consume = [&]() -> ruvia::task<void> {
        try {
            co_await std::move(operation);
        } catch (...) {
            completed = true;
            throw;
        }
        completed = true;
    };
    auto pending = caller.start(consume());
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (runtime.pool_.stats().queued_ == 0 && !completed &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(runtime.pool_.stats().queued_, 1u);
    std::binary_semaphore task_ran{0}, timer_ran{0};
    const auto task = [&]() -> ruvia::task<void> {
        task_ran.release();
        co_return;
    };
    const auto timer = [&]() -> ruvia::task<void> {
        EXPECT_EQ(co_await ruvia::sleep_for(caller.worker(), 10ms), ruvia::timer_sleep_result::elapsed);
        timer_ran.release();
    };
    auto task_result = caller.start(task());
    auto timer_result = caller.start(timer());
    EXPECT_TRUE(task_ran.try_acquire_for(2s));
    EXPECT_TRUE(timer_ran.try_acquire_for(2s));
    EXPECT_FALSE(completed.load());
    occupied.release();
    std::move(task_result).get();
    std::move(timer_result).get();
    try {
        std::move(pending).get();
        return {};
    } catch (...) {
        return std::current_exception();
    }
}

struct selector_retirement_action final {
    std::function<void()> action_;
    std::exception_ptr& failure_;

    ~selector_retirement_action() {
        try {
            action_();
        } catch (...) {
            failure_ = std::current_exception();
        }
    }
};

std::shared_ptr<const ruvia::tls_certificate_selector> retiring_selector(
    std::function<void()> action, std::exception_ptr& failure) {
    auto retirement = std::make_unique<selector_retirement_action>(std::move(action), failure);
    return std::make_shared<const ruvia::tls_certificate_selector>(
        [retirement = std::move(retirement)](const ruvia::tls_certificate_selection&)
            -> std::optional<ruvia::tls_certificate_id> { return 1; });
}

}  // namespace tls_identities_test

// One HTTPS listener on the wildcard address chooses each handshake's identity
// by precedence: an exact SNI host, a wildcard SNI host covering exactly the
// left-most label (ASCII case-insensitively), the address the client
// connected to, else the primary identity. Identities load from files and
// from in-memory PEM alike, and RFC 9110 §7.4 checks each request against the
// certificate its own connection was served.
TEST(Ruvia, https_selects_identity_by_exact_and_wildcard_sni_then_local_address) {
    using namespace tls_identities_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    const ruvia::test::tls_identity primary("primary", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity exact("exact", "DNS:api.example.test");
    const ruvia::test::tls_identity wildcard("wildcard", "DNS:*.example.test");
    const ruvia::test::tls_identity local("local", "IP:127.0.0.2");
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto port = reservation.endpoint().port();
    const asio::ip::tcp::endpoint first(asio::ip::make_address("127.0.0.1"), port);
    const asio::ip::tcp::endpoint second(asio::ip::make_address("127.0.0.2"), port);

    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({
            .address_ = "0.0.0.0",
            .https_ = port,
            .tls_ =
                {
                    .identity_ = pem_identity(primary),
                    .sni_ = {{.host_ = "*.example.test", .identity_ = pem_identity(wildcard)},
                        {.host_ = "api.example.test", .identity_ = file_identity(exact)}},
                    .local_addresses_ = {{.address_ = "127.0.0.2", .identity_ = file_identity(local)}},
                },
        })
        .on_start(nullptr)
        .on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));

    for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        auto client = tls_client(version);
        EXPECT_EQ(served_name(io, client, first, nullptr), "primary");
        EXPECT_EQ(served_name(io, client, second, nullptr), "local");
        EXPECT_EQ(served_name(io, client, first, "api.example.test"), "exact");
        EXPECT_EQ(served_name(io, client, second, "API.Example.Test"), "exact");
        EXPECT_EQ(served_name(io, client, first, "www.example.test"), "wildcard");
        EXPECT_EQ(served_name(io, client, second, "WWW.example.TEST"), "wildcard");
        EXPECT_EQ(served_name(io, client, first, "example.test"), "primary");
        EXPECT_EQ(served_name(io, client, first, "a.b.example.test"), "primary");
        EXPECT_EQ(served_name(io, client, second, "a.b.example.test"), "local");
        EXPECT_EQ(served_name(io, client, second, "other.test"), "local");
    }
    auto client = tls_client();
    EXPECT_TRUE(https_response(io, client, first, "www.example.test", "www.example.test").starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(https_response(io, client, first, "www.example.test", "localhost").starts_with("HTTP/1.1 421 "));
    EXPECT_TRUE(https_response(io, client, second, nullptr, "127.0.0.2").starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(https_response(io, client, second, nullptr, "localhost").starts_with("HTTP/1.1 421 "));
    EXPECT_TRUE(https_response(io, client, first, nullptr, "localhost").starts_with("HTTP/1.1 200 "));
    app.stop();
    runner.join();
}

// replace_tls_identities() swaps a running listener's identities for every
// later handshake over TCP and HTTP/3, while established connections keep
// theirs and sessions keep resuming across the swap. Invalid input changes
// nothing; stopped applications cannot replace, but member workers can await
// replacement; the listener keeps the last set for its next run.
TEST(Ruvia, https_identities_are_replaced_at_runtime_for_new_handshakes) {
    using namespace tls_identities_test;
    const ruvia::test::tls_identity first("first", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity second("second", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity exact("exact", "DNS:api.example.test");
    const ruvia::test::tls_identity local("local", "DNS:localhost,IP:127.0.0.1");
    external_runtime runtime;
    const auto caller = runtime.loops_[0];
    asio::io_context io;
    ruvia::test::tcp_port_reservation http_reservation(io);
    ruvia::test::tcp_port_reservation https_reservation(io);
    const auto http_endpoint = http_reservation.endpoint();
    const auto https_endpoint = https_reservation.endpoint();
    replaced_port() = https_endpoint.port();
    worker_replacement() = {.identity_ = file_identity(first)};
    const ruvia::listener_endpoint https{.address_ = "127.0.0.1", .port_ = https_endpoint.port()};
    ruvia::listen_config listener{
        .address_ = "127.0.0.1",
        .http_ = http_endpoint.port(),
        .https_ = https_endpoint.port(),
        .tls_ = {.identity_ = file_identity(first)},
    };
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation udp_reservation(io);
    const auto http3_port = udp_reservation.endpoint().port();
    listener.http3_ = http3_port;
#endif
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2}).listen(listener).on_start(nullptr).on_start([] { started().release(); });
    EXPECT_EQ(failure_of([&] { caller.start(app.replace_tls_identities(caller.worker(), https, {.identity_ = file_identity(second)})).get(); }),
        "logic_error");

    auto client = tls_client();
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started().try_acquire_for(10s));
        const auto established = connect_tls(io, client, https_endpoint, "localhost");
        ASSERT_TRUE(serves_hello(*established));
        EXPECT_EQ(peer_common_name(established->native_handle()), "first");
        auto tls12 = tls_client(TLS1_2_VERSION);
        auto tls13 = tls_client(TLS1_3_VERSION);
        const auto tls12_session = resumable_session(io, tls12, https_endpoint);
        const auto tls13_session = resumable_session(io, tls13, https_endpoint);
#if defined(RUVIA_ENABLE_HTTP3)
        const auto loopback = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), http3_port);
        ruvia::test::http3_test_client established_http3(loopback, first.ca_file_, "localhost", 5s);
        ASSERT_TRUE(serves_hello(established_http3));
        const auto http3_ticket = [&] {
            ruvia::test::http3_test_client ticket_client(loopback, first.ca_file_, "localhost", 5s);
            return ticket_client.capture_resumption();
        }();
#endif

        EXPECT_EQ(plain_body(io, http_endpoint, "/replace"), "none");
        const auto replace = [&](const ruvia::listener_endpoint& endpoint, ruvia::tls_config config) {
            return failure_of([&] { caller.start(app.replace_tls_identities(caller.worker(), endpoint, std::move(config))).get(); });
        };
        const auto replacement = [&] { return ruvia::tls_config{.identity_ = file_identity(second)}; };
        EXPECT_EQ(replace({.address_ = "127.0.0.2", .port_ = https_endpoint.port()}, replacement()), "invalid_argument");
        EXPECT_EQ(replace({.address_ = "localhost", .port_ = https_endpoint.port()}, replacement()), "invalid_argument");
        EXPECT_EQ(replace({.address_ = "127.0.0.1", .port_ = http_endpoint.port()}, replacement()), "invalid_argument");
        EXPECT_EQ(replace(https, {.identity_ = file_identity(second), .max_outgoing_records_ = 32}), "invalid_argument");
        EXPECT_EQ(replace(https, {.identity_ = file_identity(second), .client_certificates_ = {.ca_file_ = first.ca_file_}}),
            "invalid_argument");
        EXPECT_EQ(replace(https, {.identity_ = file_identity(second), .sni_ = {{.host_ = "*", .identity_ = file_identity(exact)}}}),
            "invalid_argument");
        EXPECT_EQ(replace(https, {.identity_ = {.certificate_chain_pem_ = read_text(second.ca_file_)}}), "invalid_argument");
        EXPECT_EQ(replace(https, {.identity_ = {.certificate_chain_file_ = first.ca_file_.parent_path() / "missing.pem",
                                      .private_key_file_ = first.private_key_file()}}),
            "system_error");
        EXPECT_EQ(replace(https, {.identity_ = {.certificate_chain_pem_ = read_text(second.ca_file_),
                                      .private_key_pem_ = read_text(first.private_key_file())}}),
            "system_error");
        EXPECT_EQ(served_name(io, client, https_endpoint, "localhost"), "first");
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_TRUE(http3_serves(http3_port, first.ca_file_, "localhost"));
#endif

        EXPECT_EQ(replace(https,
                      {.identity_ = pem_identity(second),
                          .sni_ = {{.host_ = "api.example.test", .identity_ = file_identity(exact)}}}),
            "none");
        EXPECT_EQ(served_name(io, client, https_endpoint, "localhost"), "second");
        EXPECT_EQ(served_name(io, client, https_endpoint, nullptr), "second");
        EXPECT_EQ(served_name(io, client, https_endpoint, "api.example.test"), "exact");
        EXPECT_TRUE(serves_hello(*established));
        EXPECT_EQ(peer_common_name(established->native_handle()), "first");
        EXPECT_TRUE(resumes(io, tls12, https_endpoint, tls12_session));
        EXPECT_TRUE(resumes(io, tls13, https_endpoint, tls13_session));
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_TRUE(serves_hello(established_http3));
        EXPECT_TRUE(http3_serves(http3_port, second.ca_file_, "localhost"));
        EXPECT_FALSE(http3_serves(http3_port, first.ca_file_, "localhost"));
        {
            ruvia::test::http3_test_client resumed(loopback, first.ca_file_, "localhost", 5s, &http3_ticket);
            EXPECT_TRUE(resumed.resumed());
            EXPECT_TRUE(serves_hello(resumed));
        }
#endif

        // A connection without SNI (HTTP/3 to an IP literal) arrives at
        // 127.0.0.1, which now names its own identity.
        EXPECT_EQ(replace(https,
                      {.identity_ = file_identity(second),
                          .local_addresses_ = {{.address_ = "127.0.0.1", .identity_ = pem_identity(local)}}}),
            "none");
        EXPECT_EQ(served_name(io, client, https_endpoint, nullptr), "local");
        EXPECT_EQ(served_name(io, client, https_endpoint, "localhost"), "local");
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_TRUE(http3_serves(http3_port, local.ca_file_, "127.0.0.1"));
#endif
        app.stop();
        runner.join();
    }
    EXPECT_EQ(failure_of([&] { caller.start(app.replace_tls_identities(caller.worker(), https, {.identity_ = file_identity(second)})).get(); }),
        "logic_error");

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    EXPECT_EQ(served_name(io, client, https_endpoint, nullptr), "local");
    app.stop();
    runner.join();
}

TEST(Ruvia, certificate_library_parses_only_changed_identity_and_failed_updates_are_atomic) {
    using namespace tls_identities_test;
    ruvia::test::tls_identity first("first"), second("second");
    external_runtime runtime;
    const auto caller = runtime.loops_[0];
    ruvia::tls_certificate_store store;
    caller.start(store.insert_or_assign(caller.worker(), 1, file_identity(first))).get();
    caller.start(store.insert_or_assign(caller.worker(), 2, pem_identity(second))).get();
    EXPECT_EQ(store.size(), 2u);
    // Existing identities are already parsed, not re-read during any mutation.
    std::filesystem::remove(first.private_key_file());
    caller.start(store.insert_or_assign(caller.worker(), 2, pem_identity(second))).get();
    EXPECT_TRUE(store.contains(1));
    EXPECT_THROW(caller.start(store.insert_or_assign(caller.worker(), 1, file_identity(first))).get(), std::system_error);
    EXPECT_THROW(caller.start(store.insert_or_assign(caller.worker(), 0, pem_identity(second))).get(), std::invalid_argument);
    EXPECT_EQ(store.size(), 2u);
    std::array<std::jthread, 4> writers;
    for (std::size_t i = 0; i < writers.size(); ++i) {
        writers[i] = std::jthread([&, id = i + 10] {
            for (int n = 0; n < 8; ++n) {
                caller.start(store.insert_or_assign(caller.worker(), id, pem_identity(second))).get();
                EXPECT_TRUE(store.contains(id));
                EXPECT_TRUE(caller.start(store.erase(caller.worker(), id)).get());
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    EXPECT_EQ(store.size(), 2u);
    EXPECT_TRUE(caller.start(store.erase(caller.worker(), 1)).get());
    EXPECT_FALSE(caller.start(store.erase(caller.worker(), 1)).get());
}

TEST(Ruvia, certificate_library_selector_uses_capabilities_rejects_and_preserves_inflight_snapshots) {
    using namespace tls_identities_test;
    const ruvia::test::tls_identity rsa("rsa", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity replacement("replacement", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity ec("ec", "DNS:localhost,DNS:deny.test,DNS:unknown.test,DNS:throw.test,IP:127.0.0.1", true);
    const ruvia::test::tls_identity next_ec("next-ec", "DNS:localhost,DNS:deny.test,DNS:unknown.test,DNS:throw.test,IP:127.0.0.1", true);
    external_runtime runtime;
    const auto caller = runtime.loops_[0];
    const auto store = std::make_shared<ruvia::tls_certificate_store>();
    caller.start(store->insert_or_assign(caller.worker(), 1, pem_identity(rsa))).get();
    caller.start(store->insert_or_assign(caller.worker(), 2, pem_identity(ec))).get();
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    std::binary_semaphore selecting{0}, release{0};
    std::atomic<unsigned int> invalid_metadata{0}, calls{0};
    const auto selector = std::make_shared<const ruvia::tls_certificate_selector>(
        [&](const ruvia::tls_certificate_selection& input) -> std::optional<ruvia::tls_certificate_id> {
            ++calls;
            if (input.connection_id_ == 0 || input.remote_address_ != *ruvia::ip_address::parse("127.0.0.1") ||
                input.remote_port_ == 0 || !input.local_address_ || *input.local_address_ != *ruvia::ip_address::parse("127.0.0.1") ||
                input.local_port_ == 0 || input.cipher_suites_.empty()) {
                ++invalid_metadata;
            }
            if (input.server_name_ == "deny.test") {
                return std::nullopt;
            }
            if (input.server_name_ == "unknown.test") {
                return 999;
            }
            if (input.server_name_ == "throw.test") {
                throw std::runtime_error("policy failed");
            }
            if (input.server_name_ == "hold.test") {
                selecting.release();
                if (!release.try_acquire_for(10s)) {
                    throw std::runtime_error("TLS snapshot selection was not released");
                }
                return 1;
            }
            return input.offers_signature_algorithm(0x0403) ? 2 : 1;
        });
    ruvia::listen_config listener{
        .address_ = "127.0.0.1", .https_ = endpoint.port(), .tls_ = {.certificate_store_ = store, .select_certificate_ = selector}};
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation udp(io);
    listener.http3_ = udp.endpoint().port();
#endif
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2}).listen(listener).on_start(nullptr).on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        auto client = tls_client(version);
        ASSERT_EQ(SSL_CTX_set1_sigalgs_list(client.native_handle(), "rsa_pss_rsae_sha256"), 1);
        EXPECT_EQ(served_name(io, client, endpoint, "localhost"), "rsa");
        auto ec_client = tls_client(version);
        ASSERT_EQ(SSL_CTX_set1_sigalgs_list(ec_client.native_handle(), "ecdsa_secp256r1_sha256"), 1);
        EXPECT_EQ(served_name(io, ec_client, endpoint, "localhost"), "ec");
        EXPECT_TRUE(served_name(io, client, endpoint, "deny.test").empty());
        EXPECT_TRUE(served_name(io, client, endpoint, "unknown.test").empty());
        EXPECT_TRUE(served_name(io, client, endpoint, "throw.test").empty());
        EXPECT_TRUE(https_response(io, ec_client, endpoint, "localhost", "other.test").starts_with("HTTP/1.1 421 "));
    }
    auto rsa12 = tls_client(TLS1_2_VERSION), rsa13 = tls_client(TLS1_3_VERSION);
    ASSERT_EQ(SSL_CTX_set1_sigalgs_list(rsa12.native_handle(), "rsa_pss_rsae_sha256"), 1);
    ASSERT_EQ(SSL_CTX_set1_sigalgs_list(rsa13.native_handle(), "rsa_pss_rsae_sha256"), 1);
    const auto session12 = resumable_session(io, rsa12, endpoint);
    const auto session13 = resumable_session(io, rsa13, endpoint);
    const auto established = connect_tls(io, rsa13, endpoint, "localhost");
    auto held = std::async(std::launch::async, [&] {
        asio::io_context held_io;
        auto client = tls_client();
        return served_name(held_io, client, endpoint, "hold.test");
    });
    const bool entered = selecting.try_acquire_for(10s);
    EXPECT_TRUE(entered);
    caller.start(store->insert_or_assign(caller.worker(), 1, pem_identity(replacement))).get();
    EXPECT_TRUE(caller.start(store->erase(caller.worker(), 1)).get());
    release.release();
    EXPECT_EQ(held.get(), "rsa");
    EXPECT_TRUE(served_name(io, rsa13, endpoint, "localhost").empty());
    EXPECT_THROW(connect_tls(io, rsa13, endpoint, "localhost", session13.get()), std::system_error);
    EXPECT_TRUE(serves_hello(*established));
    EXPECT_EQ(peer_common_name(established->native_handle()), "rsa");
    caller.start(store->insert_or_assign(caller.worker(), 1, pem_identity(replacement))).get();
    EXPECT_EQ(served_name(io, rsa13, endpoint, "localhost"), "replacement");
    EXPECT_TRUE(resumes(io, rsa12, endpoint, session12));
    EXPECT_TRUE(resumes(io, rsa13, endpoint, session13));
#if defined(RUVIA_ENABLE_HTTP3)
    const asio::ip::udp::endpoint h3_endpoint(asio::ip::address_v4::loopback(), udp.endpoint().port());
    ruvia::test::http3_test_client h3(h3_endpoint, ec.ca_file_, "localhost", 5s);
    EXPECT_TRUE(serves_hello(h3));
    const auto ticket = [&] {
        ruvia::test::http3_test_client client(h3_endpoint, ec.ca_file_, "localhost", 5s);
        return client.capture_resumption();
    }();
    caller.start(store->insert_or_assign(caller.worker(), 2, pem_identity(next_ec))).get();
    EXPECT_TRUE(serves_hello(h3));
    EXPECT_TRUE(http3_serves(udp.endpoint().port(), next_ec.ca_file_, "localhost"));
    {
        ruvia::test::http3_test_client resumed(h3_endpoint, ec.ca_file_, "localhost", 5s, &ticket);
        EXPECT_TRUE(resumed.resumed());
        EXPECT_TRUE(serves_hello(resumed));
    }
    EXPECT_FALSE(http3_serves(udp.endpoint().port(), next_ec.ca_file_, "deny.test"));
    EXPECT_FALSE(http3_serves(udp.endpoint().port(), next_ec.ca_file_, "unknown.test"));
    EXPECT_FALSE(http3_serves(udp.endpoint().port(), next_ec.ca_file_, "throw.test"));
#endif
    EXPECT_EQ(invalid_metadata.load(), 0u);
    EXPECT_GT(calls.load(), 10u);
    app.stop();
    runner.join();
}

TEST(Ruvia, certificate_library_is_shared_by_independent_single_and_multiworker_hosts) {
    using namespace tls_identities_test;
    const ruvia::test::tls_identity first("first", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity second("second", "DNS:localhost,IP:127.0.0.1");
    const auto store = std::make_shared<ruvia::tls_certificate_store>();
    external_runtime runtime;
    auto& loops = runtime.loops_;
    loops[0].start(store->insert_or_assign(loops[0].worker(), 1, file_identity(first))).get();
    asio::io_context client_io;
    // Ensure the external loops are driven before a cross-worker operation.
    const auto ready = []() -> ruvia::task<void> { co_return; };
    loops[0].start(ready()).get();
    loops[1].start(ready()).get();
    ruvia::test::tcp_port_reservation single_port(client_io), group_port(client_io), mtls_port(client_io), limited_port(client_io);
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation single_udp(client_io), group_udp(client_io);
#endif
    const auto answer = [](ruvia::request_stream& stream) -> ruvia::task<void> {
        if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
            co_await stream.response().write("hello");
            co_await stream.response().end();
        }
    };
    const auto selector = std::make_shared<const ruvia::tls_certificate_selector>(
        [](const ruvia::tls_certificate_selection& input) -> std::optional<ruvia::tls_certificate_id> {
            if (input.connection_id_ == 0 || input.remote_port_ == 0 || input.local_port_ == 0) {
                return std::nullopt;
            }
            return 1;
        });
    const auto configuration = [&](std::uint16_t port) {
        return ruvia::listen_config{
            .address_ = "127.0.0.1", .https_ = port, .tls_ = {.certificate_store_ = store, .select_certificate_ = selector}, .dispatch_ = ruvia::listener_dispatch::request_stream};
    };
    ruvia::http_server single(loops[0], {.request_stream_ = answer});
    ruvia::http_server_group group(loops, [&](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{.request_stream_ = answer}; });
    auto group_configuration = configuration(group_port.endpoint().port());
#if defined(RUVIA_ENABLE_HTTP3)
    group_configuration.http3_ = group_udp.endpoint().port();
#endif
    group.listen(group_configuration);
    const auto configure_single = [&]() -> ruvia::task<void> {
        auto single_configuration = configuration(single_port.endpoint().port());
#if defined(RUVIA_ENABLE_HTTP3)
        single_configuration.http3_ = single_udp.endpoint().port();
#endif
        single.listen(single_configuration);
        auto mtls = configuration(mtls_port.endpoint().port());
        mtls.tls_.select_certificate_.reset();
        mtls.tls_.default_certificate_ = 1;
        mtls.tls_.client_certificates_ = {
            .ca_file_ = first.ca_file_, .requirement_ = ruvia::tls_client_certificate_requirement::required};
        single.listen(mtls);
        auto limited = configuration(limited_port.endpoint().port());
        limited.tls_.max_outgoing_records_ = 1;
        single.listen(limited);
        co_await single.start();
    };
    loops[0].start(configure_single()).get();
    loops[0].start(group.start(loops[0].worker())).get();
    std::array clients{tls_client(TLS1_2_VERSION), tls_client(TLS1_3_VERSION)};
    std::array authenticated{tls_client(TLS1_2_VERSION), tls_client(TLS1_3_VERSION)};
    std::array<std::shared_ptr<SSL_SESSION>, 2> single_tickets, group_tickets;
    for (std::size_t i = 0; i < clients.size(); ++i) {
        SCOPED_TRACE(i == 0 ? "TLS 1.2" : "TLS 1.3");
        EXPECT_EQ(served_name(client_io, clients[i], single_port.endpoint(), "localhost"), "first");
        EXPECT_EQ(served_name(client_io, clients[i], group_port.endpoint(), "localhost"), "first");
        single_tickets[i] = resumable_session(client_io, clients[i], single_port.endpoint());
        group_tickets[i] = resumable_session(client_io, clients[i], group_port.endpoint());
        EXPECT_FALSE(serves_hello(client_io, clients[i], mtls_port.endpoint()));
        EXPECT_FALSE(serves_hello(client_io, clients[i], limited_port.endpoint()));
        authenticated[i].use_certificate_chain_file(first.ca_file_.string());
        authenticated[i].use_private_key_file(first.private_key_file().string(), asio::ssl::context::pem);
        EXPECT_EQ(served_name(client_io, authenticated[i], mtls_port.endpoint(), "localhost"), "first");
        EXPECT_TRUE(serves_hello(client_io, authenticated[i], mtls_port.endpoint()));
    }
#if defined(RUVIA_ENABLE_HTTP3)
    EXPECT_TRUE(http3_serves(single_udp.endpoint().port(), first.ca_file_, "localhost"));
    EXPECT_TRUE(http3_serves(group_udp.endpoint().port(), first.ca_file_, "localhost"));
#endif
    loops[0].start(store->insert_or_assign(loops[0].worker(), 1, pem_identity(second))).get();
    for (std::size_t i = 0; i < clients.size(); ++i) {
        SCOPED_TRACE(i == 0 ? "TLS 1.2" : "TLS 1.3");
        EXPECT_EQ(served_name(client_io, clients[i], single_port.endpoint(), "localhost"), "second");
        EXPECT_EQ(served_name(client_io, clients[i], group_port.endpoint(), "localhost"), "second");
        EXPECT_EQ(served_name(client_io, authenticated[i], mtls_port.endpoint(), "localhost"), "second");
        EXPECT_TRUE(serves_hello(client_io, authenticated[i], mtls_port.endpoint()));
        EXPECT_TRUE(resumes(client_io, clients[i], single_port.endpoint(), single_tickets[i]));
        EXPECT_TRUE(resumes(client_io, clients[i], group_port.endpoint(), group_tickets[i]));
    }
#if defined(RUVIA_ENABLE_HTTP3)
    EXPECT_TRUE(http3_serves(single_udp.endpoint().port(), second.ca_file_, "localhost"));
    EXPECT_TRUE(http3_serves(group_udp.endpoint().port(), second.ca_file_, "localhost"));
#endif
    EXPECT_TRUE(loops[0].start(store->erase(loops[0].worker(), 1)).get());
    for (auto& client : clients) {
        EXPECT_TRUE(served_name(client_io, client, single_port.endpoint(), "localhost").empty());
        EXPECT_TRUE(served_name(client_io, client, group_port.endpoint(), "localhost").empty());
    }
    loops[0].start(group.shutdown(loops[0].worker())).get();
    loops[0].start(single.shutdown()).get();
}

TEST(Ruvia, certificate_selector_retirement_can_schedule_async_application_replacement) {
    using namespace tls_identities_test;
    const ruvia::test::tls_identity first("first", "DNS:localhost");
    const ruvia::test::tls_identity second("second", "DNS:localhost");
    const ruvia::test::tls_identity third("third", "DNS:localhost");
    external_runtime runtime;
    const auto caller = runtime.loops_[0];
    const auto store = std::make_shared<ruvia::tls_certificate_store>();
    caller.start(store->insert_or_assign(caller.worker(), 1, pem_identity(first))).get();
    caller.start(store->insert_or_assign(caller.worker(), 2, pem_identity(second))).get();
    caller.start(store->insert_or_assign(caller.worker(), 3, pem_identity(third))).get();
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    const ruvia::listener_endpoint endpoint{.address_ = "127.0.0.1", .port_ = port.endpoint().port()};
    std::exception_ptr retirement_failure;
    std::promise<ruvia::root_task<void>> retirement;
    auto retired = retirement.get_future();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .https_ = port.endpoint().port(), .tls_ = {.certificate_store_ = store, .select_certificate_ = retiring_selector([&] {
                                                                                         try {
                                                                                             EXPECT_TRUE(store->contains(1));
                                                                                             retirement.set_value(caller.start(app.replace_tls_identities(
                                                                                                 caller.worker(), endpoint, {.certificate_store_ = store, .default_certificate_ = 3})));
                                                                                         } catch (...) {
                                                                                             retirement.set_exception(std::current_exception());
                                                                                         }
                                                                                     },
                                                                                                                      retirement_failure)}})
        .on_start(nullptr)
        .on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    auto client = tls_client();
    EXPECT_EQ(served_name(io, client, port.endpoint(), "localhost"), "first");
    caller.start(app.replace_tls_identities(
                     caller.worker(), endpoint, {.certificate_store_ = store, .default_certificate_ = 2}))
        .get();
    ASSERT_EQ(retired.wait_for(3s), std::future_status::ready);
    retired.get().get();
    EXPECT_FALSE(static_cast<bool>(retirement_failure));
    EXPECT_EQ(served_name(io, client, port.endpoint(), "localhost"), "third");
    app.stop();
    runner.join();
}

TEST(Ruvia, certificate_selector_retirement_can_schedule_async_member_host_controls) {
    using namespace tls_identities_test;
    const ruvia::test::tls_identity first("first", "DNS:localhost");
    const ruvia::test::tls_identity second("second", "DNS:localhost");
    const ruvia::test::tls_identity third("third", "DNS:localhost");
    external_runtime runtime;
    const auto caller = runtime.loops_[0];
    const auto store = std::make_shared<ruvia::tls_certificate_store>();
    caller.start(store->insert_or_assign(caller.worker(), 1, pem_identity(first))).get();
    caller.start(store->insert_or_assign(caller.worker(), 2, pem_identity(second))).get();
    caller.start(store->insert_or_assign(caller.worker(), 3, pem_identity(third))).get();
    asio::io_context io;
    ruvia::test::tcp_port_reservation single_port(io), group_port(io);
    const ruvia::listener_endpoint single_endpoint{.address_ = "127.0.0.1", .port_ = single_port.endpoint().port()};
    const ruvia::listener_endpoint group_endpoint{.address_ = "127.0.0.1", .port_ = group_port.endpoint().port()};
    const auto answer = [](ruvia::request_stream& stream) -> ruvia::task<void> {
        if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
            co_await stream.response().write("hello");
            co_await stream.response().end();
        }
    };
    std::exception_ptr single_failure, group_failure;
    std::promise<ruvia::root_task<void>> single_retirement, group_retirement;
    auto single_retired = single_retirement.get_future();
    auto group_retired = group_retirement.get_future();
    ruvia::http_server single(caller, {.request_stream_ = answer});
    ruvia::http_server_group group(runtime.loops_, [&](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{.request_stream_ = answer}; });
    const auto start_single = [&]() -> ruvia::task<void> {
        single.listen({.address_ = "127.0.0.1", .https_ = single_port.endpoint().port(), .tls_ = {.certificate_store_ = store, .select_certificate_ = retiring_selector([&] {
                                                                                                      try {
                                                                                                          EXPECT_TRUE(store->contains(1));
                                                                                                          single_retirement.set_value(caller.start(single.replace_tls_identities(
                                                                                                              single_endpoint, {.certificate_store_ = store, .default_certificate_ = 3})));
                                                                                                      } catch (...) {
                                                                                                          single_retirement.set_exception(std::current_exception());
                                                                                                      }
                                                                                                  },
                                                                                                                                   single_failure)},
            .dispatch_ = ruvia::listener_dispatch::request_stream});
        co_await single.start();
    };
    caller.start(start_single()).get();
    group.listen({.address_ = "127.0.0.1", .https_ = group_port.endpoint().port(), .tls_ = {.certificate_store_ = store, .select_certificate_ = retiring_selector([&] {
                                                                                                try {
                                                                                                    EXPECT_TRUE(store->contains(1));
                                                                                                    group_retirement.set_value(caller.start(group.replace_tls_identities(
                                                                                                        caller.worker(), group_endpoint, {.certificate_store_ = store, .default_certificate_ = 3})));
                                                                                                } catch (...) {
                                                                                                    group_retirement.set_exception(std::current_exception());
                                                                                                }
                                                                                            },
                                                                                                                             group_failure)},
        .dispatch_ = ruvia::listener_dispatch::request_stream});
    caller.start(group.start(caller.worker())).get();
    auto client = tls_client();
    EXPECT_EQ(served_name(io, client, single_port.endpoint(), "localhost"), "first");
    EXPECT_EQ(served_name(io, client, group_port.endpoint(), "localhost"), "first");
    caller.start(single.replace_tls_identities(
                     single_endpoint, {.certificate_store_ = store, .default_certificate_ = 2}))
        .get();
    ASSERT_EQ(single_retired.wait_for(3s), std::future_status::ready);
    single_retired.get().get();
    caller.start(group.replace_tls_identities(
                     caller.worker(), group_endpoint, {.certificate_store_ = store, .default_certificate_ = 2}))
        .get();
    ASSERT_EQ(group_retired.wait_for(3s), std::future_status::ready);
    group_retired.get().get();
    EXPECT_FALSE(static_cast<bool>(single_failure));
    EXPECT_FALSE(static_cast<bool>(group_failure));
    EXPECT_EQ(served_name(io, client, single_port.endpoint(), "localhost"), "third");
    EXPECT_EQ(served_name(io, client, group_port.endpoint(), "localhost"), "third");
    caller.start(group.shutdown(caller.worker())).get();
    caller.start(single.shutdown()).get();
}

TEST(Ruvia, public_certificate_preparation_leaves_caller_worker_responsive_and_publishes_atomically) {
    using namespace tls_identities_test;
    const ruvia::test::tls_identity first("first", "DNS:localhost");
    const ruvia::test::tls_identity second("second", "DNS:localhost");
    external_runtime runtime;
    const auto caller = runtime.loops_[0];
    asio::io_context io;
    ruvia::test::tcp_port_reservation single_port(io), group_port(io), app_port(io);
    const ruvia::listener_endpoint single_endpoint{.address_ = "127.0.0.1", .port_ = single_port.endpoint().port()};
    const ruvia::listener_endpoint group_endpoint{.address_ = "127.0.0.1", .port_ = group_port.endpoint().port()};
    const ruvia::listener_endpoint app_endpoint{.address_ = "127.0.0.1", .port_ = app_port.endpoint().port()};
    const auto store = std::make_shared<ruvia::tls_certificate_store>();
    auto initial = [&] {
        auto identity = file_identity(first);
        return store->insert_or_assign(caller.worker(), 1, std::move(identity));
    }();
    EXPECT_FALSE(queued_preparation(runtime, std::move(initial)));
    const auto answer = [](ruvia::request_stream& stream) -> ruvia::task<void> {
        if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
            co_await stream.response().write("hello");
            co_await stream.response().end();
        }
    };
    ruvia::http_server single(caller, {.request_stream_ = answer});
    ruvia::http_server_group group(runtime.loops_, [&](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{.request_stream_ = answer}; });
    const auto start_single = [&]() -> ruvia::task<void> {
        single.listen({.address_ = "127.0.0.1", .https_ = single_port.endpoint().port(), .tls_ = {.certificate_store_ = store, .default_certificate_ = 1}, .dispatch_ = ruvia::listener_dispatch::request_stream});
        co_await single.start();
    };
    caller.start(start_single()).get();
    group.listen({.address_ = "127.0.0.1", .https_ = group_port.endpoint().port(), .tls_ = {.identity_ = file_identity(first)}, .dispatch_ = ruvia::listener_dispatch::request_stream});
    EXPECT_FALSE(queued_preparation(runtime, group.start(caller.worker())));
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .https_ = app_port.endpoint().port(), .tls_ = {.identity_ = file_identity(first)}})
        .on_start(nullptr)
        .on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    auto client = tls_client();
    EXPECT_EQ(served_name(io, client, single_port.endpoint(), "localhost"), "first");
    // Input strings and endpoints retire before these cold tasks are started.
    auto update = [&] {
        auto identity = pem_identity(second);
        return store->insert_or_assign(caller.worker(), 1, std::move(identity));
    }();
    EXPECT_FALSE(queued_preparation(runtime, std::move(update)));
    EXPECT_EQ(served_name(io, client, single_port.endpoint(), "localhost"), "second");
    const auto invalid = [&] {
        return ruvia::tls_identity_config{.certificate_chain_pem_ = read_text(first.ca_file_),
            .private_key_pem_ = read_text(second.private_key_file())};
    };
    const auto expect_bad_identity = [&](ruvia::task<void> operation) {
        const auto failure = queued_preparation(runtime, std::move(operation));
        ASSERT_TRUE(failure);
        EXPECT_THROW(std::rethrow_exception(failure), std::system_error);
    };
    expect_bad_identity(store->insert_or_assign(caller.worker(), 1, invalid()));
    EXPECT_EQ(served_name(io, client, single_port.endpoint(), "localhost"), "second");
    auto replace_single = [&] {
        auto endpoint = single_endpoint;
        ruvia::tls_config config{.identity_ = file_identity(first)};
        return single.replace_tls_identities(endpoint, std::move(config));
    }();
    EXPECT_FALSE(queued_preparation(runtime, std::move(replace_single)));
    EXPECT_EQ(served_name(io, client, single_port.endpoint(), "localhost"), "first");
    expect_bad_identity(single.replace_tls_identities(single_endpoint, {.identity_ = invalid()}));
    EXPECT_EQ(served_name(io, client, single_port.endpoint(), "localhost"), "first");
    auto replace_group = [&] {
        auto endpoint = group_endpoint;
        ruvia::tls_config config{.identity_ = pem_identity(second)};
        return group.replace_tls_identities(caller.worker(), endpoint, std::move(config));
    }();
    EXPECT_FALSE(queued_preparation(runtime, std::move(replace_group)));
    EXPECT_EQ(served_name(io, client, group_port.endpoint(), "localhost"), "second");
    expect_bad_identity(group.replace_tls_identities(caller.worker(), group_endpoint, {.identity_ = invalid()}));
    EXPECT_EQ(served_name(io, client, group_port.endpoint(), "localhost"), "second");
    auto replace_app = [&] {
        auto endpoint = app_endpoint;
        ruvia::tls_config config{.identity_ = file_identity(second)};
        return app.replace_tls_identities(caller.worker(), endpoint, std::move(config));
    }();
    EXPECT_FALSE(queued_preparation(runtime, std::move(replace_app)));
    EXPECT_EQ(served_name(io, client, app_port.endpoint(), "localhost"), "second");
    expect_bad_identity(app.replace_tls_identities(caller.worker(), app_endpoint, {.identity_ = invalid()}));
    EXPECT_EQ(served_name(io, client, app_port.endpoint(), "localhost"), "second");
    app.stop();
    runner.join();
    caller.start(group.shutdown(caller.worker())).get();
    caller.start(single.shutdown()).get();
}

namespace accept_ownership_test {
using namespace std::chrono_literals;

struct observations final {
    std::binary_semaphore started_{0};
    std::atomic<unsigned> live_{};
    std::atomic<unsigned> destroyed_{};
    std::atomic<unsigned> wrong_owner_{};
};

struct worker_state final {
    explicit worker_state(std::shared_ptr<observations> observed)
        : observed_(std::move(observed)),
          owner_(std::this_thread::get_id()) {
        ++observed_->live_;
    }
    worker_state(worker_state&&) noexcept = default;
    worker_state(const worker_state&) = delete;
    ~worker_state() {
        if (observed_) {
            if (owner_ != std::this_thread::get_id()) {
                ++observed_->wrong_owner_;
            }
            --observed_->live_;
            ++observed_->destroyed_;
        }
    }
    void check(const ruvia::context& context) const {
        if (owner_ != std::this_thread::get_id() || !context.worker().is_current()) {
            ++observed_->wrong_owner_;
            throw std::logic_error("connection resumed on another worker");
        }
    }
    std::shared_ptr<observations> observed_;
    std::thread::id owner_;
};

class routes final {
public:
    RUVIA_ROUTES_BEGIN(routes)
    RUVIA_POST("/accept-owner", echo, ruvia::stream_body);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> echo(ruvia::context& context) {
        auto& owner = context.worker_state<worker_state>();
        owner.check(context);
        const auto worker_id = context.worker().id();
        std::string body;
        auto& reader = context.req().body_reader();
        while (const auto chunk = co_await reader.text()) {
            owner.check(context);
            body.append(*chunk);
        }
        owner.check(context);
        (void)co_await ruvia::sleep_for(context.worker(), 1ms, context.stop_token());
        owner.check(context);
        context.draft().header("X-Connection-Worker", std::to_string(worker_id));
        co_return context.text(std::move(body));
    }
};

std::string_view header(std::string_view head, std::string_view name) {
    while (!head.empty()) {
        const auto end = head.find("\r\n");
        const auto line = head.substr(0, end);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos &&
            ruvia::http_ascii_equals_ignore_case(line.substr(0, colon), name)) {
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            return value;
        }
        if (end == std::string_view::npos) {
            break;
        }
        head.remove_prefix(end + 2);
    }
    return {};
}

template <typename stream_type>
std::string exchange(stream_type& stream, std::string_view expected) {
    const auto request = std::move(std::string("POST /accept-owner HTTP/1.1\r\nHost: localhost\r\nContent-Length: ").append(std::to_string(expected.size()))) + "\r\n\r\n";
    asio::write(stream, asio::buffer(request));
    asio::write(stream, asio::buffer(expected.substr(0, expected.size() / 2)));
    asio::write(stream, asio::buffer(expected.substr(expected.size() / 2)));
    std::string response{};
    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        asio::error_code error;
        const auto count = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), count);
        if (error && error != asio::error::would_block && error != asio::error::try_again) {
            throw std::system_error(error);
        }
        const auto head_end = response.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            const auto head = std::string_view(response).substr(0, head_end);
            const auto length_text = header(head, "Content-Length");
            if (length_text.empty()) {
                throw std::runtime_error("missing response content length");
            }
            std::size_t length{};
            const auto parsed = std::from_chars(length_text.data(), length_text.data() + length_text.size(), length);
            if (parsed.ec != std::errc{} || parsed.ptr != length_text.data() + length_text.size() || length != expected.size()) {
                throw std::runtime_error("missing response content length");
            }
            if (response.size() >= head_end + 4 + length) {
                if (!response.starts_with("HTTP/1.1 200 ") ||
                    std::string_view(response).substr(head_end + 4) != expected) {
                    throw std::runtime_error("incorrect affinity echo response");
                }
                const auto worker = header(head, "X-Connection-Worker");
                if (worker.empty()) {
                    throw std::runtime_error("missing connection owner identity");
                }
                return std::string(worker);
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("connection ownership response timed out");
        }
        if (count == 0) {
            std::this_thread::sleep_for(1ms);
        }
    }
}

template <typename stream_type>
void check_connection(stream_type& stream) {
    const auto first = exchange(stream, "first split body");
    for (unsigned request = 0; request != 3; ++request) {
        EXPECT_EQ(exchange(stream, "next split body"), first);
    }
}

}  // namespace accept_ownership_test

TEST(Ruvia, app_multi_listener_tcp_tls_preserves_connection_worker_and_joins_pending_accepts) {
    using namespace accept_ownership_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    using namespace accept_ownership_test;
    auto observed = std::make_shared<observations>();
    auto& app = ruvia::app();
    app.use_worker_state<worker_state>([observed] { return worker_state(observed); });
    app.on_start([observed] { observed->started_.release(); });
    ruvia::test::tls_identity identity("localhost");
    const auto key = identity.private_key_file();
    asio::io_context io;
    ruvia::test::tcp_port_reservation http_reservation(io);
    ruvia::test::tcp_port_reservation https_reservation(io);
    const auto http = http_reservation.endpoint();
    const auto https = https_reservation.endpoint();
    const auto configure = [&] {
        app.server({.worker_count_ = 3, .connection_scan_interval_ = 5ms});
        app.listen({.address_ = "127.0.0.1", .http_ = http.port(), .https_ = https.port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = key}}});
    };
    // A pending stop must retire even a graph whose worker threads never ran.
    configure();
    app.stop();
    app.run();
    EXPECT_TRUE(app.workers().empty());
    EXPECT_EQ(observed->live_.load(), 0U);
    const auto cold_destroyed = observed->destroyed_.load();
    for (unsigned cycle = 0; cycle != 3; ++cycle) {
        configure();
        ruvia::test::app_run run(app);
        if (!observed->started_.try_acquire_for(5s)) {
            // join 在此传播启动失败的原始异常。
            app.stop();
            run.join();
            throw std::runtime_error("ownership test app did not start");
        }
        EXPECT_EQ(observed->live_.load(), 3U);
        std::vector<asio::ip::tcp::socket> idle{};
        if (cycle != 0) {
            for (unsigned connection = 0; connection != 12; ++connection) {
                idle.emplace_back(io);
                idle.back().connect(connection % 2 == 0 ? http : https);
            }
            for (unsigned connection = 0; connection != 12; ++connection) {
                asio::ip::tcp::socket aborted(io);
                aborted.connect(connection % 2 == 0 ? http : https);
                aborted.set_option(asio::socket_base::linger(true, 0));
                aborted.close();
            }
            for (unsigned connection = 0; connection != 6; ++connection) {
                asio::ip::tcp::socket plain(io);
                plain.connect(http);
                plain.non_blocking(true);
                check_connection(plain);
                asio::ssl::context tls(asio::ssl::context::tls_client);
                tls.load_verify_file(identity.ca_file_.string());
                tls.set_verify_mode(asio::ssl::verify_peer);
                tls.set_verify_callback(asio::ssl::host_name_verification("localhost"));
                asio::ssl::stream<asio::ip::tcp::socket> secure(io, tls);
                secure.next_layer().connect(https);
                if (SSL_set_tlsext_host_name(secure.native_handle(), "localhost") != 1) {
                    throw std::runtime_error("cannot set ownership test TLS server name");
                }
                secure.handshake(asio::ssl::stream_base::client);
                secure.next_layer().non_blocking(true);
                check_connection(secure);
            }
        }
        app.stop();
        run.join();
        EXPECT_EQ(observed->live_.load(), 0U);
        EXPECT_EQ(observed->destroyed_.load(), cold_destroyed + (cycle + 1) * 3U);
        EXPECT_EQ(observed->wrong_owner_.load(), 0U);
        EXPECT_TRUE(app.workers().empty());
        for (const auto& endpoint : {http, https}) {
            asio::ip::tcp::socket closed(io);
            asio::error_code error;
            closed.connect(endpoint, error);
            EXPECT_TRUE(error == asio::error::connection_refused);
        }
    }
}

namespace startup_test {

using namespace std::chrono_literals;

enum class startup_outcome { fail,
    stop_during_worker_startup,
    stop_in_hook,
    stop_from_caller,
    serve };

struct startup_state final {
    startup_outcome outcome_{};
    std::atomic<unsigned> live_workers_{};
    std::atomic<unsigned> destroyed_workers_{};
    std::atomic<unsigned> requests_{};
    std::atomic<bool> initialized_{};
    std::atomic<bool> startup_job_cancelled_{};
    std::atomic<bool> worker_startup_blocked_{};
    std::binary_semaphore hook_entered_{0};
    std::binary_semaphore release_hook_{0};
    unsigned second_hooks_{};
    unsigned stop_hooks_{};
    std::thread::id start_thread_{};
    std::thread::id stop_thread_{};
    std::atomic<bool> websocket_message_{};
    std::atomic<bool> websocket_end_{};
    std::atomic<bool> websocket_failed_{};
};

struct worker_state final {
    explicit worker_state(std::shared_ptr<startup_state> owner_value)
        : state_(std::move(owner_value)) {
        state_->live_workers_.fetch_add(1);
    }
    ~worker_state() {
        state_->live_workers_.fetch_sub(1);
        state_->destroyed_workers_.fetch_add(1);
    }
    std::shared_ptr<startup_state> state_;
};

class startup_controller final {
public:
    RUVIA_ROUTES_BEGIN(startup_controller)
    RUVIA_GET("/ready", ready);
    RUVIA_GET("/connection", connection);
    RUVIA_POST("/limited", limited, ruvia::body_limit<16>);
    RUVIA_GET_WS("/startup-socket", read_websocket);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> ready(ruvia::context& context) {
        auto& state_value = *context.worker_state<worker_state>().state_;
        state_value.requests_.fetch_add(1);
        co_return context.text(std::string_view(state_value.initialized_.load() ? "initialized" : "premature"));
    }

    ruvia::task<ruvia::http_response> limited(ruvia::context& context) {
        co_return context.text(std::string_view("accepted"));
    }

    ruvia::task<ruvia::http_response> connection(ruvia::context& context) {
        const auto info = context.conn();
        std::string result = info.client().address().to_string();
        result += info.via_trusted_proxy() ? ":proxy" : ":direct";
        result += info.scheme() == ruvia::http_scheme::https ? ":https" : ":http";
        co_return context.text(std::string_view(result));
    }

    ruvia::task<void> read_websocket(ruvia::context& context) {
        auto& state_value = *context.worker_state<worker_state>().state_;
        auto& websocket_value = context.websocket();
        try {
            const auto first = co_await websocket_value.read();
            state_value.websocket_message_ = first.has_value() && first->payload() == std::string_view("hi");
            const auto end = co_await websocket_value.read();
            state_value.websocket_end_ = !end.has_value();
        } catch (...) {
            state_value.websocket_failed_ = true;
        }
    }
};

// Startup hooks may block independently of application::stop().
class startup_run final {
public:
    explicit startup_run(ruvia::application& app, startup_state& state_value)
        : app_(app),
          state_(state_value),
          run_(app) {}

    ~startup_run() {
        if (!joined_) {
            app_.stop();
            state_.release_hook_.release();
        }
    }
    void stop_and_join() {
        app_.stop();
        state_.release_hook_.release();
        join();
    }
    void join() {
        joined_ = true;
        try {
            run_.join();
        } catch (...) {
            failure_ = std::current_exception();
        }
    }
    [[nodiscard]] std::thread::id id() const noexcept {
        return run_.id();
    }
    std::exception_ptr failure_;

private:
    ruvia::application& app_;
    startup_state& state_;
    ruvia::test::app_run run_;
    bool joined_{};
};

std::string read_response(asio::ip::tcp::socket& socket) {
    std::string response{};
    std::array<char, 1024> buffer;
    const auto deadline_value = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline_value) {
        asio::error_code error;
        const auto size = socket.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
        if (response.find("initialized") != std::string::npos ||
            (error && error != asio::error::would_block && error != asio::error::try_again)) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    return response;
}

bool forwarded_connection_matches(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    std::string_view forwarding, std::string_view expected) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    const std::string request = "GET /connection HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" +
                                std::string(forwarding) + "\r\n";
    asio::write(socket, asio::buffer(request));
    socket.non_blocking(true);
    const auto response = read_response(socket);
    const auto body_start = response.find("\r\n\r\n");
    return response.starts_with("HTTP/1.1 200 ") && body_start != std::string::npos &&
           std::string_view(response).substr(body_start + 4) == expected;
}

// The listener's ticket keys are shared by every worker, so each resumption
// attempt succeeds regardless of which worker accepts it. Offering the session
// must never fail a handshake.
bool resume_tls_session(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> session(nullptr, SSL_SESSION_free);
    for (int attempt = 0; attempt < 6; ++attempt) {
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
        asio::error_code error;
        stream.next_layer().connect(endpoint, error);
        if (!error && session && SSL_set_session(stream.native_handle(), session.get()) != 1) {
            return false;
        }
        if (!error) {
            stream.handshake(asio::ssl::stream_base::client, error);
        }
        if (error) {
            return false;
        }
        if (!session) {
            // TLS 1.3 tickets precede the response, so the first response
            // bytes mean they were processed. Capture the session before the
            // server can close the connection.
            constexpr std::string_view request = "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n";
            asio::write(stream, asio::buffer(request), error);
            std::array<char, 1024> buffer;
            if (error || stream.read_some(asio::buffer(buffer), error) == 0 || error) {
                return false;
            }
            session.reset(SSL_get1_session(stream.native_handle()));
            if (!session || SSL_SESSION_is_resumable(session.get()) != 1) {
                return false;
            }
        } else if (SSL_session_reused(stream.native_handle()) != 1) {
            return false;
        }
        // OpenSSL marks the session of an SSL freed without a local shutdown
        // as unresumable; record a clean local close so later attempts can offer it.
        SSL_set_shutdown(stream.native_handle(), SSL_SENT_SHUTDOWN);
    }
    return true;
}

using ruvia::test::read_to_end;

// RFC 8446 §6.1: closing after a Connection: close response sends close_notify,
// so the client observes a clean TLS end instead of a truncated stream.
bool tls_close_sends_close_notify(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    constexpr std::string_view request = "GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (!error) {
        asio::write(stream, asio::buffer(request), error);
    }
    if (error) {
        return false;
    }
    asio::error_code end;
    const auto response = read_to_end(stream, end);
    return response.starts_with("HTTP/1.1 404 ") && end == asio::error::eof &&
           (SSL_get_shutdown(stream.native_handle()) & SSL_RECEIVED_SHUTDOWN) != 0;
}

[[nodiscard]] std::size_t count_occurrences(std::string_view text, std::string_view marker) {
    std::size_t count = 0;
    for (auto position = text.find(marker); position != std::string_view::npos;
        position = text.find(marker, position + marker.size())) {
        ++count;
    }
    return count;
}

// An idle TLS connection waits for its next request like a plain one. Two
// heads larger than that wait's first read arrive in one TLS record, so most
// of their plaintext is already decrypted inside the engine; both are still
// answered, and so is a request sent after the connection went idle.
bool tls_keep_alive_serves_decrypted_and_later_requests(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    const auto request = "GET /missing HTTP/1.1\r\nHost: localhost\r\nX-Padding: " + std::string(1024, 'p') + "\r\n\r\n";
    if (!error) {
        asio::write(stream, asio::buffer(std::string(request) + request), error);
    }
    std::string received{};
    std::array<char, 4096> buffer;
    while (!error && count_occurrences(received, "HTTP/1.1 404 ") < 2) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        received.append(buffer.data(), size);
    }
    if (error) {
        return false;
    }
    std::this_thread::sleep_for(20ms);
    constexpr std::string_view last = "GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    asio::write(stream, asio::buffer(last), error);
    if (error) {
        return false;
    }
    asio::error_code end;
    received += read_to_end(stream, end);
    return end == asio::error::eof && count_occurrences(received, "HTTP/1.1 404 ") == 3;
}

// Reject an oversized Expect request before its body is sent. The client must
// receive the final status, not 100 Continue, and the unread body precludes reuse.
bool rejected_expect_upload_closes_without_body(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ip::tcp::socket socket(io);
    asio::error_code error;
    socket.connect(endpoint, error);
    constexpr std::string_view request =
        "POST /limited HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 1048576\r\nExpect: 100-continue\r\n\r\n";
    if (!error) {
        asio::write(socket, asio::buffer(request), error);
    }
    if (error) {
        return false;
    }
    asio::error_code end;
    const auto response = read_to_end(socket, end);
    return response.starts_with("HTTP/1.1 413 ") && response.find("\r\n\r\n") != std::string::npos &&
           response.find("HTTP/1.1 100 ") == std::string::npos && end == asio::error::eof;
}

// A TLS peer that drops TCP without close_notify ends the server websocket
// read side exactly like an orderly close instead of failing the read.
bool tls_websocket_truncation_ends_read(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, const startup_state& state_value) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    constexpr std::string_view upgrade =
        "GET /startup-socket HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    if (!error) {
        asio::write(stream, asio::buffer(upgrade), error);
    }
    std::string head{};
    char byte{};
    while (!error && !head.ends_with("\r\n\r\n")) {
        if (stream.read_some(asio::buffer(&byte, 1), error) == 1) {
            head.push_back(byte);
        }
    }
    if (error || !head.starts_with("HTTP/1.1 101 ")) {
        return false;
    }
    // One masked text frame (zero masking key), then a TCP FIN without close_notify.
    constexpr std::array<char, 8> frame{'\x81', '\x82', '\0', '\0', '\0', '\0', 'h', 'i'};
    asio::write(stream, asio::buffer(frame), error);
    if (error) {
        return false;
    }
    stream.next_layer().shutdown(asio::ip::tcp::socket::shutdown_send, error);
    const auto deadline_value = std::chrono::steady_clock::now() + 3s;
    while (!state_value.websocket_end_.load() && !state_value.websocket_failed_.load() &&
           std::chrono::steady_clock::now() < deadline_value) {
        std::this_thread::sleep_for(1ms);
    }
    return state_value.websocket_message_.load() && state_value.websocket_end_.load() &&
           !state_value.websocket_failed_.load();
}

}  // namespace startup_test

TEST(Ruvia, app_trusted_proxy_config_accepts_ipv6_prefix_lengths) {
    using namespace startup_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    auto& app = ruvia::app();
    for (const auto cidr : {"127.0.0.0/8", "::ffff:127.0.0.0/104", "::ffff:127.0.0.1/128", "::ffff:127.0.0.0/96", "::ffff:127.0.0.0/0", "2001:db8::/32"}) {
        bool accepted = true;
        try {
            app.trusted_proxies({.cidrs_ = {cidr}});
        } catch (const std::invalid_argument&) {
            accepted = false;
        }
        EXPECT_TRUE(accepted);
    }
    for (const auto cidr : {"127.0.0.0/33", "::ffff:127.0.0.0/129", "::ffff:127.0.0.0/", "::ffff:127.0.0.0/-1"}) {
        bool rejected = false;
        try {
            app.trusted_proxies({.cidrs_ = {cidr}});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    app.trusted_proxies({});
}

// A CIDR without '/' is exactly one IP literal, so trusted proxy configuration
// exposes the strict literal parser: complete IPv4/IPv6 text only, no ports,
// brackets, padding, partial IPv4 or invalid IPv6 zones.
TEST(Ruvia, app_trusted_proxy_config_accepts_only_strict_ip_literals) {
    using namespace startup_test;
    auto& app = ruvia::app();
    for (const std::string_view literal : {"192.0.2.1", "0.0.0.0", "255.255.255.255",
             "2001:0db8:1234:5678:90ab:cdef:1234:5678", "fe80::1%12", "::ffff:192.0.2.1"}) {
        bool accepted = true;
        try {
            app.trusted_proxies({.cidrs_ = {std::string(literal)}});
        } catch (const std::invalid_argument&) {
            accepted = false;
        }
        EXPECT_TRUE(accepted);
    }
    for (const std::string_view literal : {std::string_view{}, std::string_view("not an address"),
             std::string_view("192.0.2.1\0suffix", 16), std::string_view("192.0.2.1suffix"),
             std::string_view("192.0.2.1:80"), std::string_view("[::1]:80"), std::string_view("[::1]"),
             std::string_view("192.0.2"), std::string_view("01.2.3.4"), std::string_view("192.0.2.256"),
             std::string_view("192.0.2.1."), std::string_view(".192.0.2.1"), std::string_view("192.0.2.1%1"),
             std::string_view("fe80::1%"), std::string_view("fe80::1%1%2"), std::string_view("::1 "),
             std::string_view("1"), std::string_view("::ffff:01.2.3.4"), std::string_view("::ffff:192.0.2"),
             std::string_view("fe80::1%12:80"), std::string_view("fe80::1%12 "), std::string_view("fe80::1%1]:80"),
             std::string_view("fe80::1%0"), std::string_view("fe80::1%01"), std::string_view("fe80::1%-1"),
             std::string_view("fe80::1%+1"), std::string_view("fe80::1%12abc"), std::string_view("fe80::1%4294967296"),
             std::string_view("fe80::1%99999999999999999999"),
             std::string_view("fe80::1%nonexistent_ruvia_zone_id")}) {
        bool rejected = false;
        try {
            app.trusted_proxies({.cidrs_ = {std::string(literal)}});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    app.trusted_proxies({});
}

// application is a process singleton with sealed controller registration. This functional
// test runs in its own executable and exercises retries against the same config.
TEST(Ruvia, app_startup_barrier_orders_hooks_and_rolls_back) {
    using namespace startup_test;
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.private_key_file();
    auto state_value = std::make_shared<startup_state>();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2, .worker_queue_capacity_ = 16, .max_connections_per_worker_ = 4})
        .trusted_proxies({.cidrs_ = {"::ffff:127.0.0.0/104"}, .trust_x_forwarded_proto_ = true})
        .blocking_pool(nullptr)
        .use_worker_state<worker_state>([state_value] {
            if (state_value->outcome_ == startup_outcome::stop_during_worker_startup &&
                !state_value->worker_startup_blocked_.exchange(true)) {
                // Hold one worker inside startup while the lifecycle caller waits for it.
                state_value->hook_entered_.release();
                state_value->release_hook_.acquire();
            }
            return state_value;
        });

    // Separate configurations cannot claim one endpoint; sharing HTTP and
    // HTTPS is explicit within a single listen_config.
    bool duplicate_rejected = false;
    try {
        app.listen({
            {.address_ = "127.0.0.1", .http_ = 12345, .https_ = 12345, .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}}},
            {.address_ = "127.0.0.1", .http_ = 12345},
        });
    } catch (const std::invalid_argument&) {
        duplicate_rejected = true;
    }
    EXPECT_TRUE(duplicate_rejected);

    for (const bool occupy_http : {true, false}) {
        asio::io_context io;
        asio::ip::tcp::acceptor occupied(io);
        occupied.open(asio::ip::tcp::v4());
#if defined(_WIN32)
        // The server intentionally uses SO_REUSEADDR on Windows. Only an
        // exclusive external listener makes this conflict deterministic.
        const BOOL exclusive = TRUE;
        if (::setsockopt(occupied.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
            throw std::system_error(::WSAGetLastError(), asio::system_category());
        }
#endif
        occupied.bind({asio::ip::address_v4::loopback(), 0});
        occupied.listen();
        ruvia::test::tcp_port_reservation reservation(io);
        const auto available_endpoint = reservation.endpoint();
        const auto occupied_port = occupied.local_endpoint().port();
        app.listen({.address_ = "127.0.0.1",
            .http_ = occupy_http ? occupied_port : available_endpoint.port(),
            .https_ = occupy_http ? available_endpoint.port() : occupied_port,
            .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}}});
        auto failed_run = std::async(std::launch::async, [&app] {
            try {
                app.run();
                return std::exception_ptr{};
            } catch (...) {
                return std::current_exception();
            }
        });
        const bool returned = failed_run.wait_for(3s) == std::future_status::ready;
        EXPECT_TRUE(returned);
        if (!returned) {
            app.stop();
        }
        EXPECT_TRUE(failed_run.get() != nullptr);
        EXPECT_TRUE(app.workers().empty());
        EXPECT_EQ(state_value->live_workers_.load(), 0U);
        EXPECT_EQ(state_value->requests_.load(), 0U);
        EXPECT_EQ(app.stats().active_connections_, std::size_t{0});
        // Failure of either service must also retire the other service's
        // listeners, including ones prepared by a different worker.
        asio::ip::tcp::socket socket(io);
        asio::error_code error;
        socket.connect(available_endpoint, error);
        EXPECT_TRUE(error == asio::error::connection_refused);
    }

    app.on_start([state_value, &app] {
        state_value->start_thread_ = std::this_thread::get_id();
        auto workers = app.workers();
        if (workers.size() != 2 || state_value->live_workers_.load() != 2) {
            throw std::runtime_error("start hook ran before worker initialization");
        }
        for (auto& worker : workers) {
            struct job_result final {
                std::binary_semaphore completed_{0};
                bool valid_{};
            };
            auto result_value = std::make_shared<job_result>();
            const auto posted = worker.post([state_value, result_value](ruvia::web_worker_context& context_value) -> ruvia::task<void> {
                result_value->valid_ = context_value.worker().is_current() &&
                                       context_value.worker_state<worker_state>().state_ == state_value;
                result_value->completed_.release();
                co_return;
            });
            if (!posted.accepted() || !result_value->completed_.try_acquire_for(3s) || !result_value->valid_) {
                throw std::runtime_error("ready worker could not execute a startup job");
            }
        }
        if (state_value->outcome_ == startup_outcome::stop_from_caller) {
            std::promise<void> completion;
            auto done = completion.get_future();
            const auto posted = workers.front().post([state_value, completion = std::move(completion)](ruvia::web_worker_context& context_value) mutable -> ruvia::task<void> {
                state_value->hook_entered_.release();
                const auto result_value = co_await ruvia::sleep_for(context_value.worker(), 1h, context_value.stop_token());
                state_value->startup_job_cancelled_.store(result_value == ruvia::timer_sleep_result::stop_requested);
                completion.set_value();
            });
            if (!posted.accepted()) {
                throw std::runtime_error("ready worker rejected startup initialization");
            }
            // The lifecycle caller can wait for cancellable worker initialization.
            // application::stop() must wake it without requiring this caller to join first.
            done.get();
            return;
        }
        state_value->hook_entered_.release();
        state_value->release_hook_.acquire();
        if (state_value->outcome_ == startup_outcome::fail) {
            throw std::runtime_error("startup hook failed");
        }
        if (state_value->outcome_ == startup_outcome::stop_in_hook) {
            app.stop();
        }
    });
    app.on_start([state_value] {
        ++state_value->second_hooks_;
        state_value->initialized_.store(true);
    });
    app.on_stop([state_value] {
        ++state_value->stop_hooks_;
        state_value->stop_thread_ = std::this_thread::get_id();
    });

    for (const auto outcome : {startup_outcome::fail, startup_outcome::stop_during_worker_startup,
             startup_outcome::stop_in_hook, startup_outcome::stop_from_caller, startup_outcome::serve}) {
        state_value->outcome_ = outcome;
        state_value->initialized_.store(false);
        state_value->startup_job_cancelled_.store(false);
        state_value->worker_startup_blocked_.store(false);
        state_value->requests_.store(0);
        const auto second_hooks = state_value->second_hooks_;
        const auto stop_hooks = state_value->stop_hooks_;
        const auto destroyed_workers = state_value->destroyed_workers_.load();
        asio::io_context io;
        ruvia::test::tcp_port_reservation reservation(io);
        ruvia::test::tcp_port_reservation tls_reservation(io);
        const auto endpoint = reservation.endpoint();
        const auto tls_endpoint = tls_reservation.endpoint();
        app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port(), .https_ = tls_endpoint.port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}, .client_certificates_ = {.ca_file_ = identity.ca_file_}}});
        startup_run run(app, *state_value);
        const auto owner_thread = run.id();
        const bool entered = state_value->hook_entered_.try_acquire_for(3s);
        EXPECT_TRUE(entered);
        if (!entered) {
            // Join before inspecting an exception produced by the startup thread.
            run.stop_and_join();
            if (run.failure_) {
                std::rethrow_exception(run.failure_);
            }
            return;
        }
        if (outcome == startup_outcome::stop_during_worker_startup) {
            // A stop request that cancels worker startup is not a startup
            // failure: run() returns normally and leaves no partial service.
            std::this_thread::sleep_for(25ms);
            // No worker handle escapes before every worker is ready.
            EXPECT_TRUE(app.workers().empty());
            app.stop();
            state_value->release_hook_.release();
            run.join();
            EXPECT_TRUE(run.failure_ == nullptr);
            EXPECT_EQ(state_value->stop_hooks_, stop_hooks + 1);
            EXPECT_EQ(state_value->live_workers_.load(), 0U);
            EXPECT_EQ(state_value->destroyed_workers_.load(), destroyed_workers + 2);
            EXPECT_EQ(state_value->second_hooks_, second_hooks);
            EXPECT_EQ(state_value->requests_.load(), 0U);
            continue;
        }
        EXPECT_TRUE(state_value->start_thread_ == owner_thread);

        // A completed TCP handshake can queue in the kernel backlog, but neither
        // an accept nor a request dispatch is allowed before hooks finish.
        asio::ip::tcp::socket socket(io);
        socket.connect(endpoint);
        constexpr std::string_view request = "GET /ready HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        asio::write(socket, asio::buffer(request));
        socket.non_blocking(true);
        std::this_thread::sleep_for(25ms);
        std::array<char, 256> premature;
        asio::error_code error;
        EXPECT_EQ(socket.read_some(asio::buffer(premature), error), std::size_t{0});
        EXPECT_TRUE(error == asio::error::would_block || error == asio::error::try_again);
        EXPECT_EQ(state_value->requests_.load(), 0U);
        EXPECT_EQ(app.stats().active_connections_, std::size_t{0});
        auto escaped_worker = app.workers().front();
        if (outcome == startup_outcome::stop_from_caller) {
            app.stop();
        } else {
            state_value->release_hook_.release();
        }
        if (outcome == startup_outcome::serve) {
            EXPECT_TRUE(read_response(socket).find("initialized") != std::string::npos);
            // A final response lingers only until the peer ends its input
            // (RFC 9112 §9.6); the peer's close then releases server capacity.
            socket.close();
            const auto close_deadline = std::chrono::steady_clock::now() + 1s;
            while (app.stats().active_connections_ != 0 &&
                   std::chrono::steady_clock::now() < close_deadline) {
                std::this_thread::sleep_for(1ms);
            }
            EXPECT_EQ(app.stats().active_connections_, std::size_t{0});
            EXPECT_TRUE(forwarded_connection_matches(io, endpoint, "", "127.0.0.1:direct:http"));
            EXPECT_TRUE(forwarded_connection_matches(io, endpoint,
                "X-Forwarded-For: 198.51.100.7, 127.3.4.5\r\nX-Forwarded-Proto: https, http\r\n",
                "198.51.100.7:proxy:https"));
            EXPECT_TRUE(forwarded_connection_matches(io, endpoint,
                "X-Forwarded-For: 192.0.2.1, 128.3.4.5\r\nX-Forwarded-Proto: https, http\r\n",
                "128.3.4.5:proxy:http"));
            // RFC 7239: the rightmost untrusted element is the client hop even
            // when it names no address; a client-written `for` left of it is
            // never believed, and the trusted hop's proto still applies.
            EXPECT_TRUE(forwarded_connection_matches(io, endpoint,
                "Forwarded: for=203.0.113.9\r\nForwarded: proto=https\r\n", "127.0.0.1:proxy:https"));
            EXPECT_TRUE(forwarded_connection_matches(io, endpoint,
                "Forwarded: for=203.0.113.9, for=unknown;proto=https\r\n", "127.0.0.1:proxy:https"));
            EXPECT_TRUE(forwarded_connection_matches(io, endpoint,
                "Forwarded: for=203.0.113.9;proto=http, for=_hidden;proto=https, for=127.0.0.2\r\n",
                "127.0.0.2:proxy:https"));
            EXPECT_TRUE(forwarded_connection_matches(io, endpoint,
                "Forwarded: for=192.0.2.1;proto=https, for=\"[2001:db8::1]:4711\";proto=http, for=127.0.0.2\r\n",
                "2001:db8::1:proxy:http"));
            EXPECT_TRUE(forwarded_connection_matches(io, endpoint,
                "X-Forwarded-For: 198.51.100.7, unknown, 127.3.4.5\r\nX-Forwarded-Proto: http, https, http\r\n",
                "127.3.4.5:proxy:https"));
            // Optional client-certificate verification must not turn session
            // resumption into a fatal handshake error, and every resumption
            // succeeds whichever of the two workers accepts it.
            EXPECT_TRUE(resume_tls_session(io, tls_endpoint));
            EXPECT_TRUE(tls_close_sends_close_notify(io, tls_endpoint));
            EXPECT_TRUE(tls_keep_alive_serves_decrypted_and_later_requests(io, tls_endpoint));
            EXPECT_TRUE(rejected_expect_upload_closes_without_body(io, endpoint));
            EXPECT_TRUE(tls_websocket_truncation_ends_read(io, tls_endpoint, *state_value));
            app.stop();
        }
        run.join();
        EXPECT_TRUE(state_value->stop_thread_ == owner_thread);
        EXPECT_EQ(state_value->stop_hooks_, stop_hooks + 1);
        EXPECT_EQ(state_value->live_workers_.load(), 0U);
        EXPECT_EQ(state_value->destroyed_workers_.load(), destroyed_workers + 2);
        EXPECT_TRUE(!escaped_worker.valid());
        EXPECT_TRUE(!escaped_worker.accepting());
        EXPECT_EQ(state_value->second_hooks_, second_hooks + (outcome == startup_outcome::serve ? 1 : 0));
        EXPECT_EQ(state_value->requests_.load(), outcome == startup_outcome::serve ? 1U : 0U);
        EXPECT_EQ(state_value->startup_job_cancelled_.load(), outcome == startup_outcome::stop_from_caller);
        if (outcome == startup_outcome::fail) {
            EXPECT_TRUE(run.failure_ != nullptr);
            if (run.failure_) {
                try {
                    std::rethrow_exception(run.failure_);
                } catch (const std::runtime_error& failure) {
                    EXPECT_EQ(std::string(failure.what()), std::string("startup hook failed"));
                }
            }
        } else {
            EXPECT_TRUE(run.failure_ == nullptr);
        }
    }

    // A stop() that wins the race against run() is kept for that run, which
    // returns without starting; the latch is consumed, so the next run starts.
    {
        state_value->outcome_ = startup_outcome::serve;
        const auto second_hooks = state_value->second_hooks_;
        const auto stop_hooks = state_value->stop_hooks_;
        asio::io_context io;
        ruvia::test::tcp_port_reservation reservation(io);
        const auto endpoint = reservation.endpoint();
        app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
        app.stop();
        auto latched = std::async(std::launch::async, [&app] { app.run(); });
        const bool returned = latched.wait_for(3s) == std::future_status::ready;
        EXPECT_TRUE(returned);
        if (!returned) {
            app.stop();
            state_value->release_hook_.release();
        }
        latched.get();
        EXPECT_EQ(state_value->second_hooks_, second_hooks);
        EXPECT_EQ(state_value->stop_hooks_, stop_hooks + 1);
        EXPECT_EQ(state_value->live_workers_.load(), 0U);

        startup_run run(app, *state_value);
        const bool entered = state_value->hook_entered_.try_acquire_for(3s);
        EXPECT_TRUE(entered);
        if (!entered) {
            run.stop_and_join();
            if (run.failure_) {
                std::rethrow_exception(run.failure_);
            }
            return;
        }
        app.stop();
        state_value->release_hook_.release();
        run.join();
        EXPECT_TRUE(run.failure_ == nullptr);
        EXPECT_EQ(state_value->second_hooks_, second_hooks);
        EXPECT_EQ(state_value->stop_hooks_, stop_hooks + 2);
    }
    app.trusted_proxies({});
}

namespace request_stream_test {

using namespace std::chrono_literals;

std::atomic<int>& middleware_runs() {
    static std::atomic<int> value{0};
    return value;
}

std::atomic<int>& error_handler_runs() {
    static std::atomic<int> value{0};
    return value;
}

// App-wide error handler: no request stream failure may reach it.
ruvia::task<ruvia::http_response> answer_error(ruvia::context& context, ruvia::http_error_info) {
    ++error_handler_runs();
    auto response = context.text("handled");
    response.status(ruvia::http_status::service_unavailable);
    co_return response;
}

// App-wide middleware: it must run for routed requests only.
class marking_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& context, ruvia::next& next_value) {
        ++middleware_runs();
        context.draft().header("x-middleware", "ran");
        co_await next_value();
    }
};

std::atomic<bool> refusal_read{false};
std::atomic<bool> refusal_returned{false};
std::atomic<unsigned> proxy_chunks_written{0};
std::atomic<unsigned> reset_chunks_written{0};
std::atomic<unsigned> reset_finished{0};
constexpr unsigned proxy_chunk_count = 512;
constexpr std::string_view reset_failure = "streamed refusal cancelled by peer";

ruvia::task<void> await_refusal_reader(const ruvia::worker_ref& worker, ruvia::stop_token stop) {
    for (int attempt = 0; attempt != 5000 && !refusal_read.load() && !stop.stop_requested(); ++attempt) {
        (void)co_await ruvia::sleep_for(worker, 1ms, stop);
    }
    EXPECT_TRUE(refusal_read.load());
}

// A refusal handler sets refusal_returned only after its response ended on
// the wire, which the client can observe first. Each exchange waits for its
// own handler to return, so a late store cannot leak into the next exchange.
ruvia::task<void> await_refusal_return(const ruvia::worker_ref& worker, const std::string& label) {
    for (int attempt = 0; attempt != 5000 && !refusal_returned.load(); ++attempt) {
        (void)co_await ruvia::sleep_for(worker, 1ms);
    }
    EXPECT_TRUE(refusal_returned.load()) << label;
}

class ping_controller final {
public:
    RUVIA_ROUTES_BEGIN(ping_controller)
    RUVIA_GET("/ping", ping);
    RUVIA_GET("/refusal-source", refusal_source);
    RUVIA_ROUTES_END

    // Large enough for the application's compression policy.
    ruvia::task<ruvia::http_response> ping(ruvia::context& context) {
        std::string body;
        for (int index = 0; index < 512; ++index) {
            body.append("pong");
        }
        co_return context.text(body);
    }

    ruvia::task<void> refusal_source(ruvia::context& context) {
        context.draft().status(ruvia::http_status::forbidden);
        (void)co_await context.stream().commit();
        co_await context.stream().write("denied");
        co_await await_refusal_reader(context.worker(), context.stop_token());
        const std::string chunk(64 * 1024, 'r');
        for (unsigned index = 0; index != proxy_chunk_count; ++index) {
            co_await context.stream().write(std::string_view(chunk));
            ++proxy_chunks_written;
        }
        co_await context.stream().end();
    }
};

std::binary_semaphore& started() {
    static std::binary_semaphore value{0};
    return value;
}

std::mutex& failures_mutex() {
    static std::mutex value;
    return value;
}

std::vector<std::string>& failures() {
    static std::vector<std::string> value;
    return value;
}

void record_failure(const ruvia::connection_failure_record& record) noexcept {
    std::string message;
    try {
        std::rethrow_exception(record.exception());
    } catch (const std::exception& error) {
        message = error.what();
    } catch (...) {
    }
    std::lock_guard lock(failures_mutex());
    failures().push_back(std::move(message));
}

[[nodiscard]] std::size_t failures_with(std::string_view message) {
    std::lock_guard lock(failures_mutex());
    return static_cast<std::size_t>(std::count(failures().begin(), failures().end(), message));
}

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

constexpr std::string_view refused_authority = "deny.test:443";
constexpr std::string_view failure_message = "request stream failed before commit";
constexpr std::string_view silent_message = "request stream callback completed without a response";
constexpr std::string_view abort_message = "request stream failed after commit";

struct runtime_state final {
    unsigned requests_{};
};

std::binary_semaphore blocking_entered{0};
std::binary_semaphore blocking_release{0};
std::binary_semaphore blocking_cancelled{0};

// Echoes the tunnel until the peer ends its direction, then ends its own.
ruvia::task<void> echo_tunnel(ruvia::http_tunnel& tunnel) {
    while (auto chunk = co_await tunnel.read()) {
        co_await tunnel.write(*chunk);
    }
    co_await tunnel.finish();
}

// One callback serves every protocol: it reads the whole request body, then
// streams it back with the byte count as a trailer; CONNECT and HTTP/1.1
// Upgrade become echo tunnels, except a CONNECT to refused_authority. A few
// paths fail before or after the response is committed.
ruvia::task<void> serve(ruvia::request_stream& stream) {
    const auto& request = stream.request();
    const auto initial_body = stream.body_state();
    if (request.path().starts_with("/state-")) {
        const auto path = request.path();
        if (request.protocol_version() == ruvia::http_protocol_version::http11) {
            EXPECT_FALSE(initial_body.initial_end_stream_.has_value());
            EXPECT_EQ(initial_body.framing_, path == "/state-zero"      ? ruvia::request_body_framing::known_length
                                             : path == "/state-chunked" ? ruvia::request_body_framing::chunked
                                                                        : ruvia::request_body_framing::none);
        } else {
            EXPECT_EQ(initial_body.initial_end_stream_, path == "/state-none");
            EXPECT_EQ(initial_body.framing_, path == "/state-none" ? ruvia::request_body_framing::none
                                                                   : ruvia::request_body_framing::multiplexed);
        }
        EXPECT_EQ(initial_body.content_length_, path == "/state-zero" ? std::optional<std::uint64_t>(0) : std::nullopt);
    }
    EXPECT_FALSE(stream.conn().local().address() == ruvia::ip_address{});
    EXPECT_NE(stream.conn().local().port(), 0);
    if (request.path() == "/runtime-cancel") {
        while (co_await stream.body().read()) {
        }
        (void)co_await stream.response().commit();
        const auto result = co_await stream.run_blocking([] {
            blocking_entered.release();
            blocking_release.acquire();
        });
        if (result.status() == ruvia::blocking_status::cancelled) {
            blocking_cancelled.release();
        }
        co_return;
    }
    if (request.path() == "/runtime") {
        EXPECT_TRUE(stream.worker().is_current());
        EXPECT_EQ(co_await ruvia::sleep_for(stream.worker(), 1ms, stream.stop_token()), ruvia::timer_sleep_result::elapsed);
        auto result = co_await stream.run_blocking([] { return 42; });
        EXPECT_EQ(std::move(result).take_value(), 42);
        const auto count = ++stream.worker_state<runtime_state>().requests_;
        stream.draft().header("x-runtime-count", std::to_string(count));
        stream.draft().header("x-worker", std::to_string(stream.worker().id()));
        auto upstream = co_await stream.client().get("/ping").send();
        auto body = co_await upstream.text();
        EXPECT_EQ(body.text().size(), 2048);
        EXPECT_THROW(static_cast<void>(stream.run_blocking([] { return 1; }, {.timeout_ = 0ms})), std::invalid_argument);
        (void)co_await stream.response().commit();
        co_await stream.response().write(std::string_view("runtime-ready"));
        co_await stream.response().end();
        co_return;
    }
    if (stream.tunnel_requested()) {
        if (request.target() == "reset-deny.test:443") {
            stream.draft().status(ruvia::http_status::forbidden);
            (void)co_await stream.response().commit();
            const std::string chunk(64 * 1024, 'c');
            try {
                while (!stream.stop_token().stop_requested()) {
                    co_await stream.response().write(std::string_view(chunk));
                    ++reset_chunks_written;
                }
            } catch (...) {
                EXPECT_TRUE(stream.stop_token().stop_requested());
            }
            ++reset_finished;
            throw std::runtime_error(std::string(reset_failure));
        }
        if (request.target() == refused_authority || request.path() == "/deny-stream" ||
            request.target() == "pipe-deny.test:443") {
            stream.draft().status(ruvia::http_status::forbidden);
            stream.draft().header("x-denied", "yes");
            EXPECT_THROW((void)stream.body(), std::logic_error);
            if (request.target() == "pipe-deny.test:443") {
                auto upstream = co_await stream.client().get("/refusal-source").header("accept-encoding", "identity").send();
                EXPECT_EQ(upstream.status(), ruvia::http_status::forbidden);
                co_await upstream.pipe_to(stream);
                refusal_returned = true;
                co_return;
            }
            (void)co_await stream.response().commit();
            bool exclusive = false;
            try {
                (void)co_await stream.accept_tunnel();
            } catch (const std::logic_error&) {
                exclusive = true;
            }
            EXPECT_TRUE(exclusive);
            co_await stream.response().write(std::string_view("denied"));
            co_await await_refusal_reader(stream.worker(), stream.stop_token());
            if (request.protocol_version() == ruvia::http_protocol_version::http11) {
                co_await stream.response().end();
            } else {
                const std::array fields{ruvia::http_header_view{"x-denial-finished", "yes"}};
                co_await stream.response().end(fields);
            }
            refusal_returned = true;
            co_return;
        }
        const bool capsules = request.path() == "/capsules";
        const bool udp = request.protocol() == "connect-udp" ||
                         request.path().starts_with("/.well-known/masque/udp/");
        if (capsules || udp) {
            auto* tunnel = co_await stream.accept_tunnel(request.known_method() == ruvia::http_known_method::connect
                                                             ? ruvia::http_status::ok
                                                             : ruvia::http_status::switching_protocols,
                {.datagrams_ = capsules});
            if (capsules) {
                auto channel = tunnel->capsules();
                while (auto capsule = co_await channel.read()) {
                    co_await channel.write(capsule->type(), capsule->payload());
                }
                co_await channel.finish();
            } else {
                auto channel = tunnel->udp();
                while (auto datagram = co_await channel.read()) {
                    co_await channel.send(datagram->payload());
                }
                co_await channel.finish();
            }
            co_return;
        }
        bool invalid_datagrams = false;
        if (request.known_method() == ruvia::http_known_method::connect && request.protocol().empty()) {
            try {
                (void)co_await stream.accept_tunnel(ruvia::http_status::ok, {.datagrams_ = true});
            } catch (const std::invalid_argument&) {
                invalid_datagrams = true;
            }
            EXPECT_TRUE(invalid_datagrams);
        }
        auto* tunnel = co_await stream.accept_tunnel(request.known_method() == ruvia::http_known_method::connect
                                                         ? ruvia::http_status::ok
                                                         : ruvia::http_status::switching_protocols);
        co_await echo_tunnel(*tunnel);
        co_return;
    }
    if (request.path() == "/fail") {
        throw std::runtime_error(std::string(failure_message));
    }
    if (request.path() == "/silent") {
        co_return;
    }
    if (request.path() == "/conflict") {
        throw ruvia::http_error({.status_ = ruvia::http_status::conflict, .message_ = "conflict"});
    }
    if (request.path() == "/abort") {
        (void)co_await stream.response().commit();
        co_await stream.response().write(std::string_view("partial"));
        throw std::runtime_error(std::string(abort_message));
    }
    if (request.path() == "/push") {
        const bool accepted = stream.push({.scheme_ = request.scheme(), .authority_ = request.authority(), .path_ = "/pushed"});
        stream.draft().header("x-push", accepted ? "yes" : "no");
    }
    if (request.path() == "/pushed") {
        (void)co_await stream.response().commit();
        co_await stream.response().write(std::string_view("pushed-body"));
        co_await stream.response().end();
        co_return;
    }
    if (request.path() == "/advertise") {
        const std::string origin = "https://" + std::string(request.authority());
        const std::array<std::string_view, 1> origins{origin};
        stream.advertise_origins(origins);
        if (request.protocol_version() == ruvia::http_protocol_version::http2) {
            stream.advertise_alternative_service("h2=\":8443\"; ma=60");
        }
        stream.draft().header("x-advertised-origin", origin);
    }
    if (request.path() == "/upload-trailer") {
        EXPECT_TRUE(stream.trailers().empty());
        const std::array fields{ruvia::http_header_view{"link", "</style.css>; rel=preload"}};
        co_await stream.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, fields));
    }
    bool not_a_tunnel = false;
    try {
        (void)co_await stream.accept_tunnel();
    } catch (const std::logic_error&) {
        not_a_tunnel = true;
    }
    std::string body;
    while (auto chunk = co_await stream.body().read()) {
        body.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
    }
    EXPECT_EQ(stream.body_state().framing_, initial_body.framing_);
    EXPECT_EQ(stream.body_state().content_length_, initial_body.content_length_);
    EXPECT_EQ(stream.body_state().initial_end_stream_, initial_body.initial_end_stream_);
    if (request.path() == "/upload-trailer") {
        stream.draft().header("x-upload-checksum", stream.trailer("x-upload-checksum").value_or("missing"));
        stream.draft().header("x-upload-trailers", std::to_string(stream.trailers().size()));
    }
    stream.draft().header("x-early-data", stream.early_data() ? "yes" : "no");
    stream.draft().header("x-connection-id", std::to_string(stream.conn().connection_id()));
    stream.draft().header("x-not-a-tunnel", not_a_tunnel ? "yes" : "no");
    stream.draft().header("content-type", "text/plain");
    if (co_await stream.response().commit() != ruvia::stream_commit_result::body) {
        co_return;
    }
    constexpr std::size_t chunk_bytes = 64 * 1024;
    for (std::size_t offset = 0; offset < body.size(); offset += chunk_bytes) {
        co_await stream.response().write(bytes_of(std::string_view(body).substr(offset, chunk_bytes)));
    }
    const auto length = std::to_string(body.size());
    const std::array trailers{ruvia::http_header_view{"x-body-bytes", length}};
    co_await stream.response().end(trailers);
}

[[nodiscard]] std::string large_payload() {
    // Larger than every HTTP/2 and HTTP/3 flow-control window.
    std::string payload(1536 * 1024, '\0');
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>('a' + index % 26);
    }
    return payload;
}

ruvia::task<void> exercise(ruvia::http_client_ref client, ruvia::http_protocol_version version,
    std::string label, ruvia::worker_ref worker) {
    const auto payload = large_payload();
    {
        auto response = co_await client.post("/echo")
                            .header("origin", "https://site.test")
                            .header("accept-encoding", "identity")
                            .body(payload)
                            .send();
        EXPECT_EQ(response.status().value(), 200) << label;
        EXPECT_TRUE(response.protocol_version() == version) << label;
        EXPECT_FALSE(response.header("x-middleware").has_value()) << label;
        EXPECT_EQ(response.header("access-control-allow-origin").value_or(""), "*") << label;
        EXPECT_FALSE(response.header("content-encoding").has_value()) << label;
        EXPECT_EQ(response.header("x-not-a-tunnel").value_or(""), "yes") << label;
        const auto id = response.header("x-connection-id");
        EXPECT_TRUE(id.has_value() && !id->empty() && *id != "0") << label;
        std::string echoed;
        while (auto chunk = co_await response.stream().text()) {
            echoed.append(*chunk);
        }
        EXPECT_EQ(echoed.size(), payload.size()) << label;
        EXPECT_TRUE(echoed == payload) << label;
        EXPECT_EQ(response.trailer("x-body-bytes").value_or(""), std::to_string(payload.size())) << label;
        EXPECT_EQ(response.header("x-early-data").value_or(""), "no") << label;
    }
    {
        auto exchange = co_await client.post("/upload-trailer").header("trailer", "x-upload-checksum").upload({});
        co_await exchange.body().write("uploaded:");
        co_await exchange.body().write("content");
        const std::array fields{ruvia::http_header_view{"x-upload-checksum", "verified"}};
        co_await exchange.body().end(fields);
        auto response = co_await exchange.response();
        EXPECT_EQ(response.status(), ruvia::http_status::ok) << label;
        const auto interim = response.informational_responses();
        EXPECT_EQ(interim.size(), 1) << label;
        if (!interim.empty()) {
            EXPECT_EQ(interim.front().status(), ruvia::http_status::early_hints) << label;
            EXPECT_FALSE(interim.front().headers().empty()) << label;
            if (!interim.front().headers().empty()) {
                EXPECT_EQ(interim.front().headers().front().value(), "</style.css>; rel=preload") << label;
            }
        }
        EXPECT_EQ(response.header("x-upload-checksum").value_or(""), "verified") << label;
        EXPECT_EQ(response.header("x-upload-trailers").value_or(""), "1") << label;
        const auto content = co_await response.text();
        EXPECT_EQ(content.text(), "uploaded:content") << label;
    }
    {
        unsigned previous = 0;
        std::string previous_worker;
        for (int repeat = 0; repeat != 2; ++repeat) {
            auto response = co_await client.get("/runtime").send();
            const auto count = static_cast<unsigned>(std::stoul(std::string(response.header("x-runtime-count").value_or("0"))));
            const std::string current_worker(response.header("x-worker").value_or(""));
            if (repeat != 0) {
                EXPECT_EQ(count, previous + 1) << label;
                EXPECT_EQ(current_worker, previous_worker) << label;
            }
            previous = count;
            previous_worker = current_worker;
            const auto content = co_await response.text();
            EXPECT_EQ(content.text(), "runtime-ready") << label;
        }
    }
    if (version == ruvia::http_protocol_version::http2) {
        auto response = co_await client.get("/push").send();
        EXPECT_EQ(response.header("x-push").value_or(""), "yes") << label;
        (void)co_await response.text();
        auto pushed = client.next_push();
        for (int attempt = 0; !pushed && attempt != 5000; ++attempt) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
            pushed = client.next_push();
        }
        EXPECT_TRUE(pushed.has_value()) << label;
        if (pushed) {
            EXPECT_EQ(pushed->request().path_, "/pushed") << label;
            auto pushed_response = co_await pushed->response();
            const auto body = co_await pushed_response.text();
            EXPECT_EQ(body.text(), "pushed-body") << label;
        }
    }
    if (version != ruvia::http_protocol_version::http11 && client.scheme() == ruvia::http_scheme::https) {
        auto response = co_await client.get("/advertise").send();
        const std::string origin(response.header("x-advertised-origin").value_or(""));
        (void)co_await response.text();
        bool origins_seen = false;
        bool alternative_seen = false;
        for (int attempt = 0; attempt != 5000 &&
                              (!origins_seen || (version == ruvia::http_protocol_version::http2 && !alternative_seen));
            ++attempt) {
            while (auto advertisement = client.next_advertisement()) {
                if (const auto* origins = advertisement->origins()) {
                    EXPECT_EQ(origins->origins_.size(), 1) << label;
                    if (!origins->origins_.empty()) {
                        EXPECT_EQ(std::string_view(origins->origins_.front()), std::string_view(origin)) << label;
                    }
                    origins_seen = true;
                }
                if (const auto* alternative = advertisement->alternative_service()) {
                    EXPECT_EQ(alternative->field_value_, "h2=\":8443\"; ma=60") << label;
                    alternative_seen = true;
                }
            }
            if (!origins_seen || (version == ruvia::http_protocol_version::http2 && !alternative_seen)) {
                (void)co_await ruvia::sleep_for(worker, 1ms);
            }
        }
        EXPECT_TRUE(origins_seen) << label;
        if (version == ruvia::http_protocol_version::http2) {
            EXPECT_TRUE(alternative_seen) << label;
        }
    }
    {
        auto opened = co_await client.udp("/.well-known/masque/udp/echo.test/53/").open();
        auto* tunnel = opened.tunnel();
        EXPECT_TRUE(tunnel != nullptr) << label;
        if (tunnel != nullptr) {
            auto channel = std::move(*tunnel).udp();
            for (const auto message : {std::string_view{}, std::string_view("reliable-udp")}) {
                co_await channel.send(message);
                auto packet = co_await channel.read();
                EXPECT_TRUE(packet.has_value()) << label;
                if (packet.has_value()) {
                    EXPECT_EQ(packet->text(), message) << label;
                }
            }
            co_await channel.finish();
            EXPECT_FALSE((co_await channel.read()).has_value()) << label;
        }
    }
    if (version != ruvia::http_protocol_version::http11) {
        auto opened = co_await client.connect("localhost").protocol("echo-capsules").target("/capsules").open();
        auto* tunnel = opened.tunnel();
        EXPECT_TRUE(tunnel != nullptr) << label;
        if (tunnel != nullptr) {
            auto channel = std::move(*tunnel).capsules();
            co_await channel.write(17, "capsule-payload");
            auto capsule = co_await channel.read();
            EXPECT_TRUE(capsule.has_value()) << label;
            if (capsule) {
                EXPECT_EQ(capsule->type(), 17) << label;
                EXPECT_EQ(capsule->payload(), "capsule-payload") << label;
            }
            co_await channel.finish();
            EXPECT_FALSE((co_await channel.read()).has_value()) << label;
        }
    }
    for (const auto& [path, status] : {std::pair<std::string_view, unsigned>{"/fail", 500},
             std::pair<std::string_view, unsigned>{"/silent", 500},
             std::pair<std::string_view, unsigned>{"/conflict", 409}}) {
        auto response = co_await client.get(path).send();
        EXPECT_EQ(response.status().value(), status) << label << ' ' << path;
        (void)co_await response.text();
    }
    {
        // A failure after commit aborts the response the client is reading.
        bool aborted = false;
        try {
            auto response = co_await client.get("/abort").send();
            while (co_await response.stream().text()) {
            }
        } catch (const ruvia::http_client_error&) {
            aborted = true;
        }
        EXPECT_TRUE(aborted) << label;
    }
    {
        auto opened = co_await client.connect("echo.test:443").open();
        auto* tunnel = opened.tunnel();
        EXPECT_TRUE(tunnel != nullptr) << label;
        if (tunnel != nullptr) {
            EXPECT_EQ(tunnel->status().value(), 200) << label;
            const std::string message = "tunnel bytes over " + label;
            co_await tunnel->write(message);
            co_await tunnel->finish();
            std::string echoed;
            while (auto chunk = co_await tunnel->text()) {
                echoed.append(*chunk);
            }
            EXPECT_EQ(echoed, message) << label;
        }
    }
    for (const bool proxy : {false, true}) {
        refusal_read = false;
        refusal_returned = false;
        proxy_chunks_written = 0;
        auto refused = co_await client.connect(proxy ? "pipe-deny.test:443" : refused_authority).open();
        auto* response = refused.response();
        EXPECT_TRUE(response != nullptr) << label;
        if (response != nullptr) {
            EXPECT_EQ(response->status().value(), 403) << label;
            EXPECT_EQ(response->header("x-denied").value_or(""), "yes") << label;
            auto first = co_await response->stream().text();
            EXPECT_TRUE(first.has_value()) << label;
            EXPECT_EQ(first.value_or(""), "denied") << label;
            std::size_t body_bytes = first ? first->size() : 0;
            EXPECT_FALSE(refusal_returned.load()) << label;
            refusal_read = true;
            if (proxy) {
                (void)co_await ruvia::sleep_for(worker, 50ms);
                EXPECT_LT(proxy_chunks_written.load(), proxy_chunk_count) << label;
            }
            while (auto chunk = co_await response->stream().text()) {
                body_bytes += chunk->size();
            }
            EXPECT_EQ(body_bytes, proxy ? 6 + proxy_chunk_count * 64 * 1024 : 6) << label;
            if (!proxy && version != ruvia::http_protocol_version::http11) {
                EXPECT_EQ(response->trailer("x-denial-finished").value_or(""), "yes") << label;
            }
            co_await await_refusal_return(worker, label);
        }
    }
    if (version != ruvia::http_protocol_version::http11) {
        refusal_read = false;
        refusal_returned = false;
        auto refused = co_await client.connect("localhost").protocol("echo").target("/deny-stream").open();
        auto* response = refused.response();
        EXPECT_TRUE(response != nullptr) << label;
        if (response != nullptr) {
            EXPECT_EQ(response->status(), ruvia::http_status::forbidden) << label;
            const auto first = co_await response->stream().text();
            EXPECT_EQ(first.value_or(""), "denied") << label;
            EXPECT_FALSE(refusal_returned.load()) << label;
            refusal_read = true;
            EXPECT_FALSE((co_await response->stream().text()).has_value()) << label;
            EXPECT_EQ(response->trailer("x-denial-finished").value_or(""), "yes") << label;
            co_await await_refusal_return(worker, label);
        }
    }
    const auto previous_reset = reset_finished.load();
    reset_chunks_written = 0;
    {
        auto refused = co_await client.connect("reset-deny.test:443").open();
        auto* response = refused.response();
        EXPECT_TRUE(response != nullptr) << label;
        if (response != nullptr) {
            EXPECT_TRUE((co_await response->stream().text()).has_value()) << label;
            (void)co_await ruvia::sleep_for(worker, 50ms);
            EXPECT_LT(reset_chunks_written.load(), 512) << label;
        }
        // Destruction abandons a live response: HTTP/1 closes its connection,
        // while HTTP/2 and HTTP/3 reset only this request stream.
    }
    for (int attempt = 0; attempt != 5000 && reset_finished.load() == previous_reset; ++attempt) {
        (void)co_await ruvia::sleep_for(worker, 1ms);
    }
    EXPECT_EQ(reset_finished.load(), previous_reset + 1) << label;
}

struct client_target final {
    std::string base_url_;
    ruvia::http_client_versions versions_;
    ruvia::http_protocol_version version_;
    std::string label_;
};

// Each target gets its own client; a failure ends the run, but the client
// that met it still shuts down on its loop.
ruvia::task<void> exercise_all(ruvia::event_loop loop, std::vector<client_target> targets,
    std::filesystem::path ca_file, std::exception_ptr& failure) {
    for (const auto& target : targets) {
        std::optional<ruvia::http_client> owner;
        try {
            ruvia::http_client_config config{.base_url_ = target.base_url_};
            config.versions_ = target.versions_;
            config.connect_timeout_ = 5s;
            config.request_timeout_ = 20s;
            config.max_response_bytes_ = 64 * 1024;
            config.push_.enabled_ = true;
            config.advertisements_.receive_origins_ = target.base_url_.starts_with("https://");
            config.advertisements_.receive_alternative_services_ = true;
            config.tls_.ca_file_ = ca_file;
            owner.emplace(loop, config);
            co_await exercise(owner->client(), target.version_, target.label_, loop.worker());
        } catch (...) {
            failure = std::current_exception();
        }
        if (owner.has_value()) {
            co_await owner->shutdown();
        }
        if (failure != nullptr) {
            co_return;
        }
    }
}

// Reads one HTTP/1 head and whatever followed it.
[[nodiscard]] std::string read_head(asio::ip::tcp::socket& socket, std::string& rest) {
    std::string buffer;
    const auto head_end = asio::read_until(socket, asio::dynamic_buffer(buffer), "\r\n\r\n");
    rest = buffer.substr(head_end);
    return buffer.substr(0, head_end);
}

[[nodiscard]] std::string lowercase(std::string_view source) {
    std::string text(source);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        return static_cast<char>(value >= 'A' && value <= 'Z' ? value - 'A' + 'a' : value);
    });
    return text;
}

}  // namespace request_stream_test

// A request stream listener needs on_request_stream at run(); ALPN lists stay
// reserved for connection listeners.
TEST(Ruvia, request_stream_configuration_is_validated) {
    using namespace request_stream_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto port = reservation.endpoint().port();
    auto& app = ruvia::app();
    EXPECT_THROW(app.listen(ruvia::listen_config{.address_ = "127.0.0.1",
                     .http_ = port,
                     .tls_ = {.alpn_ = {"h2"}},
                     .dispatch_ = ruvia::listener_dispatch::request_stream}),
        std::invalid_argument);
    app.server({.worker_count_ = 1})
        .listen(ruvia::listen_config{
            .address_ = "127.0.0.1", .http_ = port, .dispatch_ = ruvia::listener_dispatch::request_stream})
        .on_start(nullptr)
        .on_request_stream(nullptr);
    EXPECT_THROW(app.run(), std::invalid_argument);
}

// One callback serves HTTP/1.1, cleartext prior-knowledge HTTP/2, HTTP/1.1
// and HTTP/2 over TLS, and HTTP/3: a streamed body larger than every
// flow-control window echoed back with a trailer, CONNECT tunnels, a refused
// CONNECT, failures before commit, which answer 500 and reach
// on_connection_failure, an http_error, which answers its own status
// unreported, and a failure after commit, which aborts the response and is
// reported. An HTTP/1.1 Upgrade becomes a tunnel after 101. The host's
// server_config CORS and compression policies cover both listeners; the routes
// listener's middleware and on_error never touch a request stream.
TEST(Ruvia, request_stream_listeners_serve_every_protocol_beside_routes) {
    using namespace request_stream_test;
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation plain_reservation(io);
    ruvia::test::tcp_port_reservation tls_reservation(io);
    ruvia::test::tcp_port_reservation routes_reservation(io);
    const auto plain_endpoint = plain_reservation.endpoint();
    const auto tls_port = tls_reservation.endpoint().port();
    const auto routes_endpoint = routes_reservation.endpoint();
    ruvia::listen_config stream_listener{.address_ = "127.0.0.1",
        .http_ = plain_endpoint.port(),
        .https_ = tls_port,
        .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_,
                     .private_key_file_ = identity.private_key_file()}},
        .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation http3_reservation(io);
    const auto http3_port = http3_reservation.endpoint().port();
    stream_listener.http3_ = http3_port;
    stream_listener.http3_early_data_ = true;
#endif
    const ruvia::listen_config routes_listener{.address_ = "127.0.0.1", .http_ = routes_endpoint.port()};
    const auto registered_client_url = "http://127.0.0.1:" + std::to_string(routes_endpoint.port());

    {
        std::lock_guard lock(failures_mutex());
        failures().clear();
    }
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2, .compression_ = ruvia::compression_config{}, .cors_ = ruvia::http_cors_policy_config{}})
        .listen({stream_listener, routes_listener})
        .blocking_pool({.thread_count_ = 2})
        .use_worker_state<runtime_state>()
        .client({.base_url_ = registered_client_url})
        .use<marking_middleware>()
        .on_error(&answer_error)
        .on_start(nullptr)
        .on_start([] { started().release(); })
        .on_connection_failure(&record_failure)
        .on_request_stream(&serve);

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    EXPECT_THROW(app.on_request_stream(nullptr), std::logic_error);

    const auto plain_url = "http://127.0.0.1:" + std::to_string(plain_endpoint.port());
    const auto tls_url = "https://localhost:" + std::to_string(tls_port);
    std::vector<client_target> targets{
        {plain_url, ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11, "HTTP/1.1"},
        {tls_url, ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11, "HTTP/1.1 over TLS"},
    };
#if defined(RUVIA_ENABLE_HTTP2)
    targets.push_back(
        {plain_url, ruvia::http_client_versions{.http1_ = false}, ruvia::http_protocol_version::http2, "h2c"});
    targets.push_back({tls_url, ruvia::http_client_versions{.http1_ = false}, ruvia::http_protocol_version::http2, "h2"});
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    targets.push_back({"https://localhost:" + std::to_string(http3_port), ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true},
        ruvia::http_protocol_version::http3, "HTTP/3"});
#endif
    {
        // "localhost" is looked up on the pool, which outlives the loop.
        ruvia::blocking_pool lookups({.thread_count_ = 1});
        auto& client_io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(client_io, {.blocking_pool_ = &lookups});
        std::exception_ptr failure;
        auto run = [&]() -> ruvia::task<void> {
            co_await exercise_all(attachment.loop(), targets, identity.ca_file_, failure);
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        std::move(root).get();
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
    }

#if defined(RUVIA_ENABLE_HTTP3)
    {
        const asio::ip::udp::endpoint endpoint(asio::ip::address_v4::loopback(), http3_port);
        ruvia::test::http3_test_client initial(endpoint, identity.ca_file_, "localhost", 10s);
        EXPECT_EQ(initial.request("GET", "/early").header("x-early-data").value_or(std::string{}), "no");
        for (const auto path : {"/state-none", "/state-zero", "/state-unknown"}) {
            const bool ended = std::string_view(path) == "/state-none";
            const auto stream = ended ? initial.start_request("POST", path)
                                      : initial.start_streaming_request("POST", path,
                                            std::string_view(path) == "/state-zero"
                                                ? std::optional<ruvia::http_content_length>(0)
                                                : std::nullopt);
            if (!ended) {
                initial.send_stream_data(stream, bytes_of(std::string_view(path) == "/state-zero" ? "" : "x"), true);
            }
            EXPECT_EQ(initial.finish_response(stream).status_, 200);
        }
        const auto ticket = initial.capture_resumption();
        ASSERT_TRUE(ticket.permits_early_data());
        ruvia::test::http3_test_client resumed(endpoint, identity.ca_file_, "localhost", 10s, &ticket, true);
        const auto early = resumed.start_early_request("GET", "/early");
        const auto response = resumed.finish_response(early);
        EXPECT_EQ(response.status_, 200);
        EXPECT_EQ(response.header("x-early-data").value_or(std::string{}), "yes");
        EXPECT_TRUE(resumed.early_data_accepted());

        constexpr std::array fields{ruvia::http_header_view{"capsule-protocol", "?1"}};
        const ruvia::http3_extended_connect_request_head_view udp{
            .protocol_ = "connect-udp", .authority_ = "localhost", .target_ = "/.well-known/masque/udp/echo.test/53/", .headers_ = fields};
        const auto stream = initial.start_tunnel(udp);
        EXPECT_EQ(initial.wait_for_response_head(stream).status_, 200);
        const std::string native("\0native", 7);
        initial.send_datagram(stream, bytes_of(native));
        EXPECT_EQ(std::string_view(initial.receive_datagram(stream)), std::string_view(native));
        initial.send_stream_data(stream, {}, true);
        EXPECT_FALSE(initial.finish_response(stream).reset_error_.has_value());

        // Disabling H3 DATAGRAM forces reliable DATAGRAM capsules.
        ruvia::test::http3_test_client reliable(endpoint, identity.ca_file_, "localhost", 10s, nullptr, false, false);
        const auto fallback = reliable.start_tunnel(udp);
        EXPECT_EQ(reliable.wait_for_response_head(fallback).status_, 200);
        const std::string capsule("\0\x09\0reliable", 11);
        reliable.send_stream_data(fallback, bytes_of(capsule), true);
        const auto echoed = reliable.finish_response(fallback);
        EXPECT_EQ(std::string_view(echoed.body_), std::string_view(capsule));
        EXPECT_FALSE(echoed.reset_error_.has_value());
    }
#endif
    for (const auto path : {"/state-none", "/state-zero", "/state-chunked"}) {
        asio::ip::tcp::socket socket(io);
        socket.connect(plain_endpoint);
        std::string wire = std::string("POST ") + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
        if (std::string_view(path) == "/state-zero") {
            wire += "Content-Length: 0\r\n";
        }
        if (std::string_view(path) == "/state-chunked") {
            wire += "Transfer-Encoding: chunked\r\n";
        }
        wire += "\r\n";
        if (std::string_view(path) == "/state-chunked") {
            wire += "0\r\n\r\n";
        }
        asio::write(socket, asio::buffer(wire));
        EXPECT_TRUE(ruvia::test::read_to_end(socket).starts_with("HTTP/1.1 200 "));
    }
#if defined(RUVIA_ENABLE_HTTP2)
    {
        asio::ip::tcp::socket socket(io);
        socket.connect(plain_endpoint);
        std::string wire(ruvia::http2_client_preface);
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        std::uint32_t id = 1;
        for (const auto path : {"/state-none", "/state-zero", "/state-unknown"}) {
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "POST");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", path);
            if (std::string_view(path) == "/state-zero") {
                ruvia::hpack_encoder::encode_header(block, "content-length", "0");
            }
            const bool ended = std::string_view(path) == "/state-none";
            ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, ended ? 5 : 4, id, block);
            if (!ended) {
                ruvia::test::append_frame(wire, ruvia::http2_frame_type::data, 1, id,
                    std::string_view(path) == "/state-zero" ? "" : "x");
            }
            id += 2;
        }
        // 首部与终结DATA同一次socket写入；初始HEADERS状态不能被后者覆盖。
        asio::write(socket, asio::buffer(wire));
        unsigned ended = 0;
        while (ended != 3) {
            const auto frame = ruvia::test::read_frame(socket, std::chrono::steady_clock::now() + 5s);
            EXPECT_NE(frame.header_.type_, static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
            EXPECT_NE(frame.header_.type_, static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
            if (frame.header_.stream_id_ != 0 &&
                (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers) || frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) &&
                (frame.header_.flags_ & 1) != 0) {
                ++ended;
            }
        }
    }
#endif

    // A rejected HTTP/1 Upgrade must flush before the callback can finish,
    // while retaining chunked framing and the close-after-response policy.
    {
        refusal_read = false;
        refusal_returned = false;
        asio::ip::tcp::socket socket(io);
        socket.connect(plain_endpoint);
        asio::write(socket, asio::buffer(std::string_view("GET /deny-stream HTTP/1.1\r\nHost: localhost\r\n"
                                                          "Connection: Upgrade\r\nUpgrade: echo/1\r\n\r\npending")));
        std::string rest;
        const auto head = lowercase(read_head(socket, rest));
        EXPECT_TRUE(head.starts_with("http/1.1 403 ")) << head;
        while (rest.find("denied") == std::string::npos) {
            std::array<char, 128> bytes;
            rest.append(bytes.data(), socket.read_some(asio::buffer(bytes)));
        }
        EXPECT_FALSE(refusal_returned.load());
        refusal_read = true;
        rest += ruvia::test::read_to_end(socket);
        EXPECT_NE(rest.find("6\r\ndenied\r\n"), std::string::npos);
        EXPECT_TRUE(rest.ends_with("0\r\n\r\n"));
    }

    // HTTP/1 Upgrade negotiates a custom Capsule Protocol independently of UDP.
    {
        asio::ip::tcp::socket socket(io);
        socket.connect(plain_endpoint);
        asio::write(socket, asio::buffer(std::string_view("GET /capsules HTTP/1.1\r\nHost: localhost\r\n"
                                                          "Connection: Upgrade\r\nUpgrade: echo-capsules\r\n\r\n")));
        std::string rest;
        const auto head = lowercase(read_head(socket, rest));
        EXPECT_TRUE(head.starts_with("http/1.1 101 ")) << head;
        EXPECT_NE(head.find("\r\ncapsule-protocol: ?1\r\n"), std::string::npos);
        const std::string capsule("\x11\x07payload", 9);
        asio::write(socket, asio::buffer(capsule));
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        rest += ruvia::test::read_to_end(socket);
        EXPECT_EQ(rest, capsule);
    }

    // Cancellation ends the request's blocking wait before its callable ends.
    {
        asio::ip::tcp::socket socket(io);
        socket.connect(plain_endpoint);
        asio::write(socket, asio::buffer(std::string_view("GET /runtime-cancel HTTP/1.1\r\nHost: localhost\r\n\r\n")));
        EXPECT_TRUE(blocking_entered.try_acquire_for(10s));
        socket.close();
        EXPECT_TRUE(blocking_cancelled.try_acquire_for(10s));
        blocking_release.release();
    }

    // HTTP/1.1 Upgrade: the bytes behind the head already belong to the
    // tunnel; the 101 names the offered protocol.
    {
        asio::ip::tcp::socket socket(io);
        socket.connect(plain_endpoint);
        asio::write(socket, asio::buffer(std::string_view("GET /chat HTTP/1.1\r\nHost: localhost\r\n"
                                                          "Connection: Upgrade\r\nUpgrade: echo/1\r\n\r\nhello")));
        std::string rest;
        const auto head = lowercase(read_head(socket, rest));
        EXPECT_TRUE(head.starts_with("http/1.1 101 switching protocols\r\n")) << head;
        EXPECT_NE(head.find("\r\nupgrade: echo/1\r\n"), std::string::npos) << head;
        EXPECT_NE(head.find("\r\nconnection: upgrade\r\n"), std::string::npos) << head;
        if (rest.size() < 5) {
            std::string more(5 - rest.size(), '\0');
            asio::read(socket, asio::buffer(more));
            rest += more;
        }
        EXPECT_EQ(rest, "hello");
        asio::write(socket, asio::buffer(std::string_view("world")));
        std::string echoed(5, '\0');
        asio::read(socket, asio::buffer(echoed));
        EXPECT_EQ(echoed, "world");
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        EXPECT_EQ(ruvia::test::read_to_end(socket), "");
    }

    const auto pong = lowercase(ruvia::test::exchange(io, routes_endpoint,
        "GET /ping HTTP/1.1\r\nHost: localhost\r\nOrigin: https://site.test\r\nAccept-Encoding: gzip\r\n"
        "Connection: close\r\n\r\n"));
    const auto pong_head = pong.substr(0, pong.find("\r\n\r\n") + 2);
    EXPECT_TRUE(pong_head.starts_with("http/1.1 200 ")) << pong_head;
    EXPECT_NE(pong_head.find("\r\nx-middleware: ran\r\n"), std::string::npos) << pong_head;
    EXPECT_NE(pong_head.find("\r\naccess-control-allow-origin: *\r\n"), std::string::npos) << pong_head;
    EXPECT_NE(pong_head.find("\r\ncontent-encoding: gzip\r\n"), std::string::npos) << pong_head;

    // An http_error answers with its status and is not reported.
    ruvia::test::wait_until(
        [&targets] {
            return failures_with(failure_message) + failures_with(silent_message) + failures_with(abort_message) +
                       failures_with(reset_failure) ==
                   4 * targets.size();
        },
        10s);
    app.stop();
    runner.join();
    app.on_connection_failure(nullptr);
    EXPECT_EQ(middleware_runs().load(), 3 * targets.size() + 1);
    EXPECT_EQ(error_handler_runs().load(), 0);
    EXPECT_EQ(failures_with(failure_message), targets.size());
    EXPECT_EQ(failures_with(silent_message), targets.size());
    EXPECT_EQ(failures_with(abort_message), targets.size());
    EXPECT_EQ(failures_with(reset_failure), targets.size());
    std::lock_guard lock(failures_mutex());
    EXPECT_EQ(failures().size(), 4 * targets.size());
}

TEST(Ruvia, standalone_streamed_tunnel_refusals_propagate_upstream_disconnect) {
    using namespace request_stream_test;
    asio::io_context io;
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &preparation});
    ruvia::test::tcp_port_reservation source_port(io), plain_port(io), tls_port(io);
    ruvia::test::udp_port_reservation udp_port(io);
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    bool release_source = false;
    unsigned source_aborted = 0;
    unsigned forwarding_retired = 0;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server source(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& request) -> ruvia::task<void> {
            request.draft().status(ruvia::http_status::forbidden);
            (void)co_await request.response().commit();
            co_await request.response().write("denied");
            for (int attempt = 0; attempt != 5000 && !release_source; ++attempt) {
                (void)co_await ruvia::sleep_for(request.worker(), 1ms, request.stop_token());
            }
            EXPECT_TRUE(release_source);
            ++source_aborted;
            request.abort_connection();
        }});
        source.listen({.address_ = "127.0.0.1", .http_ = source_port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        const auto upstream_origin = "http://127.0.0.1:" + std::to_string(source_port.endpoint().port());
        ruvia::http_client upstream(attachment.loop(),
            {.base_url_ = upstream_origin,
                .versions_ = ruvia::http_client_versions{.http2_ = false}});
        ruvia::http_server proxy(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& request) -> ruvia::task<void> {
            struct retirement final {
                unsigned& count_;
                ~retirement() {
                    ++count_;
                }
            } retired{forwarding_retired};
            auto response = co_await upstream.client().get("/source").send();
            request.draft().status(response.status());
            const auto committed = co_await request.response().commit();
            EXPECT_EQ(committed, ruvia::stream_commit_result::body);
            if (committed != ruvia::stream_commit_result::body) {
                co_return;
            }
            co_await response.stream().pipe_to(request.response());
        }});
        proxy.listen({.address_ = "127.0.0.1", .http_ = plain_port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        ruvia::listen_config secure{.address_ = "127.0.0.1", .https_ = tls_port.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
        secure.http3_ = udp_port.endpoint().port();
#endif
        proxy.listen(std::move(secure));
        std::exception_ptr failure;
        try {
            co_await source.start();
            co_await proxy.start();
            std::vector<client_target> targets{{"http://127.0.0.1:" + std::to_string(plain_port.endpoint().port()),
                ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11, "HTTP/1"}};
#if defined(RUVIA_ENABLE_HTTP2)
            targets.push_back({"https://127.0.0.1:" + std::to_string(tls_port.endpoint().port()),
                ruvia::http_client_versions{.http1_ = false}, ruvia::http_protocol_version::http2, "HTTP/2"});
#endif
#if defined(RUVIA_ENABLE_HTTP3)
            targets.push_back({"https://127.0.0.1:" + std::to_string(udp_port.endpoint().port()),
                ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true}, ruvia::http_protocol_version::http3, "HTTP/3"});
#endif
            for (const auto& target : targets) {
                release_source = false;
                const auto prior_retired = forwarding_retired;
                ruvia::http_client_config config{.base_url_ = target.base_url_, .versions_ = target.versions_};
                config.tls_.ca_file_ = identity.ca_file_;
                config.request_timeout_ = 10s;
                ruvia::http_client client(attachment.loop(), config);
                std::exception_ptr exchange_failure;
                try {
                    auto refused = co_await client.client().connect("upstream-denied.test:443").open();
                    auto* response = refused.response();
                    EXPECT_NE(response, nullptr) << target.label_;
                    if (response != nullptr) {
                        EXPECT_EQ(response->status(), ruvia::http_status::forbidden) << target.label_;
                        EXPECT_EQ((co_await response->stream().text()).value_or(""), "denied") << target.label_;
                        EXPECT_EQ(forwarding_retired, prior_retired) << target.label_;
                        release_source = true;
                        bool truncated = false;
                        try {
                            while (co_await response->stream().text()) {
                            }
                        } catch (const ruvia::http_client_error&) {
                            truncated = true;
                        }
                        EXPECT_TRUE(truncated) << target.label_;
                    }
                } catch (...) {
                    release_source = true;
                    exchange_failure = std::current_exception();
                }
                co_await client.shutdown();
                if (exchange_failure) std::rethrow_exception(exchange_failure);
            }
            EXPECT_EQ(source_aborted, targets.size());
            EXPECT_EQ(forwarding_retired, targets.size());
        } catch (...) {
            failure = std::current_exception();
        }
        co_await proxy.shutdown();
        co_await source.shutdown();
        co_await upstream.shutdown();
        EXPECT_EQ(proxy.stats().active_connections_, 0U);
        EXPECT_EQ(source.stats().active_connections_, 0U);
        attachment.stop();
        if (failure) std::rethrow_exception(failure);
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, request_stream_body_wait_cancellation_and_reset_are_isolated) {
    using namespace request_stream_test;
    asio::io_context io;
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &preparation});
    ruvia::test::tcp_port_reservation plain(io), secure(io);
    ruvia::test::udp_port_reservation udp(io);
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    const auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::stop_source> stop;
        ruvia::request_stream* pending = nullptr;
        std::uint64_t pending_connection = 0;
        unsigned cancelled = 0;
        unsigned reset_failures = 0;
        bool throw_after_reset = false;
        bool reset_requested = false;
        bool resume_body = false;
        bool survivor_pending = false;
        std::optional<ruvia::task<std::optional<std::span<const std::byte>>>> cold;
        const auto worker = attachment.loop().worker();
        ruvia::worker_signal retirement(worker);
        std::uint64_t retiring_connection = 0;
        bool connection_retired = false;
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& request) -> ruvia::task<void> {
                                                          const auto initial = request.body_state();
                                                          const bool multiplexed = request.request().protocol_version() == ruvia::http_protocol_version::http2 ||
                                                                                   request.request().protocol_version() == ruvia::http_protocol_version::http3;
                                                          if (request.request().path() == "/survivor") {
                                                              survivor_pending = true;
                                                              std::string received;
                                                              while (auto chunk = co_await request.body().text()) {
                                                                  received.append(*chunk);
                                                              }
                                                              EXPECT_EQ(received, "alive");
                                                          } else if (request.request().path() == "/pending") {
                                                              EXPECT_EQ(initial.framing_, multiplexed ? ruvia::request_body_framing::multiplexed : ruvia::request_body_framing::chunked);
                                                              EXPECT_FALSE(initial.content_length_.has_value());
                                                              EXPECT_EQ(initial.initial_end_stream_, multiplexed ? std::optional(false) : std::nullopt);
                                                              pending = &request;
                                                              pending_connection = request.conn().connection_id();
                                                              try {
                                                                  (void)co_await request.body().read({.stop_token_ = stop->token()});
                                                                  ADD_FAILURE() << "read unexpectedly completed without content or cancellation";
                                                              } catch (const std::system_error& error) {
                                                                  if (!reset_requested) {
                                                                      EXPECT_EQ(error.code(), std::errc::operation_canceled);
                                                                      ++cancelled;
                                                                  }
                                                              } catch (...) {
                                                                  if (!reset_requested) {
                                                                      throw;
                                                                  }
                                                              }
                                                              pending = nullptr;
                                                              EXPECT_EQ(request.body_state().framing_, initial.framing_);
                                                              EXPECT_EQ(request.body_state().initial_end_stream_, initial.initial_end_stream_);
                                                              if (reset_requested) {
                                                                  if (throw_after_reset) {
                                                                      throw std::runtime_error("failure after explicit stream reset");
                                                                  }
                                                                  co_return;
                                                              }
                                                              if (resume_body) {
                                                                  std::string received;
                                                                  while (auto chunk = co_await request.body().text()) {
                                                                      received.append(*chunk);
                                                                  }
                                                                  EXPECT_EQ(received, "resumed");
                                                              }
                                                              request.draft().status(ruvia::http_status::forbidden);
                                                          } else {
                                                              EXPECT_EQ(initial.initial_end_stream_, multiplexed ? std::optional(true) : std::nullopt);
                                                              if (request.request().path() == "/cold") {
                                                                  cold.emplace(request.body().read());
                                                              }
                                                          }
                                                          request.draft().header("x-connection-id", std::to_string(request.conn().connection_id()));
                                                          (void)co_await request.response().commit();
                                                          co_await request.response().end();
                                                      },
                                                         .connection_failure_ = [&](const ruvia::connection_failure_record& record) noexcept {
                try {
                    std::rethrow_exception(record.exception());
                } catch (const std::runtime_error& error) {
                    EXPECT_EQ(std::string_view(error.what()), "failure after explicit stream reset");
                    ++reset_failures;
                } },
                                                         .connection_close_ = [&](const ruvia::connection_event& event, const std::shared_ptr<void>&, const ruvia::connection_close_info&) noexcept {
                if (event.connection_id() == retiring_connection) {
                    connection_retired = true;
                    retirement.notify();
                } }});
        server.listen({.address_ = "127.0.0.1", .http_ = plain.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        ruvia::listen_config tls{.address_ = "127.0.0.1", .https_ = secure.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
        tls.http3_ = udp.endpoint().port();
#endif
        server.listen(std::move(tls));
        std::exception_ptr failure;
        try {
            co_await server.start();
            std::vector<client_target> targets{
                {"http://127.0.0.1:" + std::to_string(plain.endpoint().port()), ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11, "HTTP/1"},
                {"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11, "TLS HTTP/1"}};
#if defined(RUVIA_ENABLE_HTTP2)
            targets.push_back({"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http1_ = false}, ruvia::http_protocol_version::http2, "HTTP/2"});
#endif
#if defined(RUVIA_ENABLE_HTTP3)
            targets.push_back({"https://127.0.0.1:" + std::to_string(udp.endpoint().port()), ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true}, ruvia::http_protocol_version::http3, "HTTP/3"});
#endif
            for (const auto& target : targets) {
                ruvia::http_client_config config{.base_url_ = target.base_url_, .versions_ = target.versions_};
                config.tls_.ca_file_ = identity.ca_file_;
                config.request_timeout_ = 5s;
                ruvia::http_client client(attachment.loop(), config);
                std::exception_ptr exchange_failure;
                try {
                    const bool multiplexed = target.version_ != ruvia::http_protocol_version::http11;
                    for (unsigned mode = 0; mode != 3; ++mode) {
                        const bool reset = mode == 2;
                        reset_requested = false;
                        resume_body = mode == 1;
                        stop.emplace();
                        throw_after_reset = false;
                        auto exchange = co_await client.client().post("/pending").upload({});
                        for (unsigned count = 0; pending == nullptr && count != 5000; ++count) {
                            (void)co_await ruvia::sleep_for(client.worker(), 1ms);
                        }
                        if (pending == nullptr) throw std::runtime_error("pending read did not start");
                        const auto connection = pending_connection;
                        std::optional<ruvia::http_client_exchange> survivor;
                        if (multiplexed) {
                            survivor_pending = false;
                            survivor.emplace(co_await client.client().post("/survivor").upload({}));
                            for (unsigned count = 0; !survivor_pending && count != 5000; ++count) {
                                (void)co_await ruvia::sleep_for(client.worker(), 1ms);
                            }
                            EXPECT_TRUE(survivor_pending);
                            EXPECT_NE(pending, nullptr);
                        }
                        if (reset) {
                            reset_requested = true;
                            pending->reset();
                        } else {
                            // 跨线程取消必须经控制通道转投，而不是等下一个网络字节。
                            std::thread cancel([&] { stop->request_stop(); });
                            cancel.join();
                        }
                        if (resume_body) {
                            co_await exchange.body().write("resumed");
                            co_await exchange.body().end();
                        }
                        bool reset_seen = false;
                        try {
                            auto response = co_await exchange.response();
                            EXPECT_FALSE(reset);
                            EXPECT_EQ(response.status(), ruvia::http_status::forbidden);
                            (void)co_await response.text();
                        } catch (const ruvia::http_client_error&) {
                            reset_seen = true;
                        }
                        EXPECT_EQ(reset_seen, reset);
                        if (survivor) {
                            co_await survivor->body().write("alive");
                            co_await survivor->body().end();
                            auto response = co_await survivor->response();
                            EXPECT_EQ(response.header("x-connection-id").value_or(""), std::to_string(connection));
                            (void)co_await response.text();
                        }
                        auto next = co_await client.client().get("/other").send();
                        const auto next_id = std::string(next.header("x-connection-id").value_or(""));
                        // An HTTP/1 body left incomplete closes its connection; a resumed one keeps it.
                        EXPECT_EQ(next_id == std::to_string(connection), multiplexed || resume_body);
                        (void)co_await next.text();
                    }
                    if (multiplexed) {
                        stop.emplace();
                        throw_after_reset = true;
                        auto exchange = co_await client.client().post("/pending").upload({});
                        for (unsigned count = 0; pending == nullptr && count != 5000; ++count) {
                            (void)co_await ruvia::sleep_for(client.worker(), 1ms);
                        }
                        if (pending == nullptr) throw std::runtime_error("reset exception read did not start");
                        reset_requested = true;
#if defined(RUVIA_ENABLE_HTTP2)
                        if (target.version_ == ruvia::http_protocol_version::http2) {
                            EXPECT_THROW(pending->reset(ruvia::http3_error_code::request_cancelled), std::invalid_argument);
                            pending->reset(ruvia::http2_error_code::internal_error);
                        }
#endif
#if defined(RUVIA_ENABLE_HTTP3)
                        if (target.version_ == ruvia::http_protocol_version::http3) {
                            EXPECT_THROW(pending->reset(static_cast<ruvia::http2_error_code>(8)), std::invalid_argument);
                            pending->reset(ruvia::http3_error_code::internal_error);
                        }
#endif
                        try {
                            (void)co_await exchange.response();
                            ADD_FAILURE() << "reset produced a response";
                        } catch (const ruvia::http_client_error&) {
                        }
                    }
                    {
                        auto response = co_await client.client().get("/cold").send();
                        (void)co_await response.text();
                        // 收到响应EOF不意味着服务端已完成协程/正文facade的清理。
                        // 等待真实连接退休通知，确定冷任务的宿主已销毁，不靠延时。
                        retiring_connection = std::stoull(std::string(response.header("x-connection-id").value_or("0")));
                        connection_retired = false;
                        if (server.close_connection(retiring_connection)) {
                            while (!connection_retired) {
                                co_await retirement.wait();
                            }
                        }
                        bool expired = false;
                        try {
                            (void)co_await std::move(*cold);
                        } catch (const std::logic_error&) {
                            expired = true;
                        }
                        cold.reset();
                        EXPECT_TRUE(expired);
                    }
                } catch (...) {
                    exchange_failure = std::current_exception();
                }
                co_await client.shutdown();
                if (exchange_failure) std::rethrow_exception(exchange_failure);
            }
            EXPECT_EQ(cancelled, 2 * targets.size());
            EXPECT_EQ(reset_failures, targets.size() - 2);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await server.shutdown();
        attachment.stop();
        if (failure) std::rethrow_exception(failure);
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

// Each operation's operation_options bound only that wait, on every protocol.
// A body read that times out or is stopped from another thread keeps the
// bytes already received and reading resumes; a response write that times out
// or is stopped resets the request; a stop requested before commit, write,
// end or inform starts sends nothing; non-positive timeouts are rejected.
TEST(Ruvia, request_stream_operation_options_bound_each_wait) {
    using namespace request_stream_test;
    asio::io_context io;
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &preparation});
    ruvia::test::tcp_port_reservation plain(io), secure(io);
    ruvia::test::udp_port_reservation udp(io);
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    const auto run = [&]() -> ruvia::task<void> {
        unsigned phase = 0;
        unsigned read_interruptions = 0;
        unsigned write_interruptions = 0;
        unsigned rejected = 0;
        unsigned prestopped = 0;
        const auto stop_from_another_thread = [](ruvia::stop_source& stop) {
            std::thread([&stop] { stop.request_stop(); }).join();
        };
        const auto expect_interrupted = [](std::exception_ptr failure, std::errc expected) {
            if (failure == nullptr) {
                return false;
            }
            try {
                std::rethrow_exception(failure);
            } catch (const std::system_error& error) {
                return error.code() == expected;
            } catch (...) {
            }
            return false;
        };
        const auto handle = [&](ruvia::request_stream& request) -> ruvia::task<void> {
            const auto path = request.request().path();
            auto& response = request.response();
            if (path == "/read") {
                auto& body = request.body();
                try {
                    (void)body.read({.timeout_ = 0ms});
                } catch (const std::invalid_argument&) {
                    ++rejected;
                }
                try {
                    (void)body.text({.timeout_ = -1ms});
                } catch (const std::invalid_argument&) {
                    ++rejected;
                }
                std::exception_ptr failure;
                try {
                    (void)co_await body.read({.timeout_ = 50ms});
                } catch (...) {
                    failure = std::current_exception();
                }
                read_interruptions += expect_interrupted(failure, std::errc::timed_out);
                phase = 1;
                std::string received;
                while (received.size() < 3) {
                    const auto chunk = co_await body.text();
                    if (!chunk) {
                        break;
                    }
                    received.append(*chunk);
                }
                ruvia::stop_source stop;
                ruvia::task_scope reads(request.worker());
                const auto stopped_read = [&]() -> ruvia::task<void> {
                    try {
                        (void)co_await body.read({.stop_token_ = stop.token()});
                    } catch (...) {
                        failure = std::current_exception();
                    }
                };
                failure = nullptr;
                reads.spawn(stopped_read());
                (void)co_await ruvia::sleep_for(request.worker(), 50ms);
                stop_from_another_thread(stop);
                co_await reads.join();
                read_interruptions += expect_interrupted(failure, std::errc::operation_canceled);
                phase = 2;
                while (const auto chunk = co_await body.text()) {
                    received.append(*chunk);
                }
                EXPECT_EQ((co_await response.commit()), ruvia::stream_commit_result::body);
                co_await response.write(received);
                co_await response.end();
                co_return;
            }
            if (path == "/prestopped") {
                ruvia::stop_source stop;
                stop.request_stop();
                const ruvia::http_interim_response_head hints(ruvia::http_status::early_hints);
                for (const auto attempt : {0, 1, 2, 3}) {
                    try {
                        switch (attempt) {
                            case 0:
                                (void)response.commit({.timeout_ = 0ms});
                                break;
                            case 1:
                                (void)response.write(std::string_view("x"), {.timeout_ = 0ms});
                                break;
                            case 2:
                                (void)response.end({}, {.timeout_ = -1ms});
                                break;
                            default:
                                (void)request.inform(hints, {.timeout_ = 0ms});
                                break;
                        }
                    } catch (const std::invalid_argument&) {
                        ++rejected;
                    }
                }
                const ruvia::operation_options stopped{.stop_token_ = stop.token()};
                std::exception_ptr failure;
                try {
                    co_await request.inform(hints, stopped);
                } catch (...) {
                    failure = std::current_exception();
                }
                prestopped += expect_interrupted(failure, std::errc::operation_canceled);
                try {
                    (void)co_await response.commit(stopped);
                } catch (...) {
                    failure = std::current_exception();
                }
                prestopped += expect_interrupted(failure, std::errc::operation_canceled);
                EXPECT_EQ((co_await response.commit()), ruvia::stream_commit_result::body);
                EXPECT_EQ((co_await response.sleep(10s, stop.token())), ruvia::timer_sleep_result::stop_requested);
                try {
                    co_await response.write(std::string_view("must-not-be-sent"), stopped);
                } catch (...) {
                    failure = std::current_exception();
                }
                prestopped += expect_interrupted(failure, std::errc::operation_canceled);
                co_await response.write(std::string_view("ok"));
                try {
                    co_await response.end({}, stopped);
                } catch (...) {
                    failure = std::current_exception();
                }
                prestopped += expect_interrupted(failure, std::errc::operation_canceled);
                EXPECT_FALSE(response.aborted());
                co_await response.end();
                co_return;
            }
            // /write/timeout and /write/stop: the client reads nothing, so the
            // write stalls once flow control or the socket buffers are full.
            EXPECT_EQ((co_await response.commit()), ruvia::stream_commit_result::body);
            const std::string payload(64 * 1024 * 1024, 'x');
            std::exception_ptr failure;
            const bool timeout = path == "/write/timeout";
            ruvia::stop_source stop;
            ruvia::task_scope writes(request.worker());
            const auto stalled_write = [&]() -> ruvia::task<void> {
                try {
                    co_await response.write(std::string_view(payload),
                        timeout ? ruvia::operation_options{.timeout_ = 200ms} : ruvia::operation_options{.stop_token_ = stop.token()});
                } catch (...) {
                    failure = std::current_exception();
                }
            };
            writes.spawn(stalled_write());
            if (!timeout) {
                (void)co_await ruvia::sleep_for(request.worker(), 200ms);
                stop_from_another_thread(stop);
            }
            co_await writes.join();
            write_interruptions += expect_interrupted(failure, timeout ? std::errc::timed_out : std::errc::operation_canceled);
            EXPECT_TRUE(response.aborted());
            phase = 3;
        };
        ruvia::http_server server(attachment.loop(), {.request_stream_ = handle});
        server.listen({.address_ = "127.0.0.1", .http_ = plain.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        ruvia::listen_config tls{.address_ = "127.0.0.1", .https_ = secure.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
        tls.http3_ = udp.endpoint().port();
#endif
        server.listen(std::move(tls));
        std::exception_ptr failure;
        try {
            co_await server.start();
            std::vector<client_target> targets{
                {"http://127.0.0.1:" + std::to_string(plain.endpoint().port()), ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11, "HTTP/1"},
                {"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11, "TLS HTTP/1"}};
#if defined(RUVIA_ENABLE_HTTP2)
            targets.push_back({"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http1_ = false}, ruvia::http_protocol_version::http2, "HTTP/2"});
#endif
#if defined(RUVIA_ENABLE_HTTP3)
            targets.push_back({"https://127.0.0.1:" + std::to_string(udp.endpoint().port()), ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true}, ruvia::http_protocol_version::http3, "HTTP/3"});
#endif
            for (const auto& target : targets) {
                ruvia::http_client_config config{.base_url_ = target.base_url_, .versions_ = target.versions_};
                config.tls_.ca_file_ = identity.ca_file_;
                config.request_timeout_ = 10s;
                ruvia::http_client client(attachment.loop(), config);
                std::exception_ptr exchange_failure;
                const auto await_phase = [&](unsigned expected) -> ruvia::task<void> {
                    for (unsigned count = 0; phase != expected && count != 5000; ++count) {
                        (void)co_await ruvia::sleep_for(client.worker(), 1ms);
                    }
                    EXPECT_EQ(phase, expected) << target.label_;
                };
                try {
                    phase = 0;
                    auto exchange = co_await client.client().post("/read").upload({});
                    co_await await_phase(1);
                    co_await exchange.body().write("abc");
                    co_await await_phase(2);
                    co_await exchange.body().write("def");
                    co_await exchange.body().end();
                    auto read_response = co_await exchange.response();
                    EXPECT_EQ(read_response.status(), ruvia::http_status::ok) << target.label_;
                    const auto read_bytes = co_await read_response.text();
                    EXPECT_EQ(read_bytes.text(), "abcdef") << target.label_;

                    auto prestopped_response = co_await client.client().get("/prestopped").send();
                    EXPECT_EQ(prestopped_response.status(), ruvia::http_status::ok) << target.label_;
                    EXPECT_TRUE(prestopped_response.informational_responses().empty()) << target.label_;
                    const auto prestopped_bytes = co_await prestopped_response.text();
                    EXPECT_EQ(prestopped_bytes.text(), "ok") << target.label_;

                    for (const std::string_view write_path : {"/write/timeout", "/write/stop"}) {
                        phase = 0;
                        auto stalled = co_await client.client().get(write_path).send();
                        EXPECT_EQ(stalled.status(), ruvia::http_status::ok) << target.label_;
                        co_await await_phase(3);
                        bool truncated = false;
                        try {
                            for (;;) {
                                if (!(co_await stalled.stream().read())) {
                                    break;
                                }
                            }
                        } catch (const ruvia::http_client_error&) {
                            truncated = true;
                        }
                        EXPECT_TRUE(truncated) << target.label_ << " " << write_path;
                    }
                } catch (...) {
                    exchange_failure = std::current_exception();
                }
                co_await client.shutdown();
                if (exchange_failure) std::rethrow_exception(exchange_failure);
            }
            EXPECT_EQ(read_interruptions, 2 * targets.size());
            EXPECT_EQ(write_interruptions, 2 * targets.size());
            EXPECT_EQ(rejected, 6 * targets.size());
            EXPECT_EQ(prestopped, 4 * targets.size());
        } catch (...) {
            failure = std::current_exception();
        }
        co_await server.shutdown();
        attachment.stop();
        if (failure) std::rethrow_exception(failure);
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

// The same per-operation contract on established tunnels and their Capsule
// and HTTP Datagram adapters. A tunnel or capsule read that times out or is
// stopped keeps unread input, including a partly received capsule; a stop
// requested before a write, send or finish starts sends nothing; a stalled
// tunnel write that times out or is stopped aborts the tunnel.
TEST(Ruvia, tunnel_operation_options_bound_each_wait) {
    using namespace request_stream_test;
    asio::io_context io;
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &preparation});
    ruvia::test::tcp_port_reservation plain(io), secure(io);
    ruvia::test::udp_port_reservation udp(io);
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    const auto run = [&]() -> ruvia::task<void> {
        unsigned phase = 0;
        unsigned rejected = 0;
        unsigned read_interruptions = 0;
        unsigned prestopped = 0;
        unsigned write_interruptions = 0;
        const auto interrupted = [](std::exception_ptr failure, std::errc expected) {
            if (failure == nullptr) {
                return false;
            }
            try {
                std::rethrow_exception(failure);
            } catch (const std::system_error& error) {
                return error.code() == expected;
            } catch (...) {
            }
            return false;
        };
        // Runs `operation` and reports whether it failed with `expected`.
        const auto fails_with = [interrupted](ruvia::task<void> operation, std::errc expected) -> ruvia::task<bool> {
            try {
                co_await std::move(operation);
            } catch (...) {
                co_return interrupted(std::current_exception(), expected);
            }
            co_return false;
        };
        const auto stopped_options = [](ruvia::stop_source& stop) {
            stop.request_stop();
            return ruvia::operation_options{.stop_token_ = stop.token()};
        };
        // A read stopped from another thread while it waits.
        const auto stop_pending_read = [&](const ruvia::worker_ref& worker, auto read) -> ruvia::task<void> {
            ruvia::stop_source stop;
            std::exception_ptr failure;
            ruvia::task_scope reads(worker);
            const auto pending = [&]() -> ruvia::task<void> {
                try {
                    (void)co_await read({.stop_token_ = stop.token()});
                } catch (...) {
                    failure = std::current_exception();
                }
            };
            reads.spawn(pending());
            (void)co_await ruvia::sleep_for(worker, 50ms);
            std::thread([&stop] { stop.request_stop(); }).join();
            co_await reads.join();
            read_interruptions += interrupted(failure, std::errc::operation_canceled);
        };
        const auto handle = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
            const auto& request = stream.request();
            const auto& worker = stream.worker();
            if (request.path() == "/op-capsules") {
                auto* tunnel = co_await stream.accept_tunnel(ruvia::http_status::ok, {.datagrams_ = true});
                auto channel = tunnel->capsules();
                try {
                    (void)channel.read({.timeout_ = 0ms});
                } catch (const std::invalid_argument&) {
                    ++rejected;
                }
                try {
                    (void)co_await channel.read({.timeout_ = 50ms});
                } catch (...) {
                    read_interruptions += interrupted(std::current_exception(), std::errc::timed_out);
                }
                phase = 4;
                const auto capsule = co_await channel.read();
                EXPECT_TRUE(capsule.has_value());
                co_await stop_pending_read(worker, [&](ruvia::operation_options operation) { return channel.read(std::move(operation)); });
                ruvia::stop_source stop;
                prestopped += co_await fails_with(channel.write(99, "must-not-be-sent", stopped_options(stop)), std::errc::operation_canceled);
                if (capsule) {
                    co_await channel.write(capsule->type(), capsule->payload());
                }
                co_await channel.finish();
                co_return;
            }
            if (request.protocol() == "connect-udp" || request.path().starts_with("/.well-known/masque/udp/")) {
                auto* tunnel = co_await stream.accept_tunnel(request.known_method() == ruvia::http_known_method::connect
                                                                 ? ruvia::http_status::ok
                                                                 : ruvia::http_status::switching_protocols);
                auto channel = tunnel->udp();
                try {
                    (void)channel.send(std::string_view("x"), {.timeout_ = 0ms});
                } catch (const std::invalid_argument&) {
                    ++rejected;
                }
                try {
                    (void)co_await channel.read({.timeout_ = 50ms});
                } catch (...) {
                    read_interruptions += interrupted(std::current_exception(), std::errc::timed_out);
                }
                phase = 6;
                const auto datagram = co_await channel.read();
                EXPECT_TRUE(datagram.has_value());
                ruvia::stop_source stop;
                prestopped += co_await fails_with(channel.send(std::string_view("must-not-be-sent"), stopped_options(stop)), std::errc::operation_canceled);
                if (datagram) {
                    co_await channel.send(datagram->payload());
                }
                co_await channel.finish();
                co_return;
            }
            auto* tunnel = co_await stream.accept_tunnel();
            if (request.target() == "read.test:443") {
                for (const auto attempt : {0, 1, 2}) {
                    try {
                        if (attempt == 0) {
                            (void)tunnel->read({.timeout_ = 0ms});
                        } else if (attempt == 1) {
                            (void)tunnel->write(std::string_view("x"), {.timeout_ = -1ms});
                        } else {
                            (void)tunnel->finish({.timeout_ = 0ms});
                        }
                    } catch (const std::invalid_argument&) {
                        ++rejected;
                    }
                }
                try {
                    (void)co_await tunnel->read({.timeout_ = 50ms});
                } catch (...) {
                    read_interruptions += interrupted(std::current_exception(), std::errc::timed_out);
                }
                phase = 1;
                std::string received;
                while (received.size() < 3) {
                    const auto chunk = co_await tunnel->read();
                    if (!chunk) {
                        break;
                    }
                    received.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
                }
                co_await stop_pending_read(worker, [&](ruvia::operation_options operation) { return tunnel->read(std::move(operation)); });
                ruvia::stop_source stop;
                prestopped += co_await fails_with(tunnel->write(std::string_view("must-not-be-sent"), stopped_options(stop)), std::errc::operation_canceled);
                prestopped += co_await fails_with(tunnel->finish(stopped_options(stop)), std::errc::operation_canceled);
                phase = 2;
                while (const auto chunk = co_await tunnel->read()) {
                    received.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
                }
                co_await tunnel->write(std::string_view(received));
                co_await tunnel->finish();
                co_return;
            }
            // write-timeout.test / write-stop.test: the peer reads nothing.
            const bool timeout = request.target() == "write-timeout.test:443";
            const std::string payload(64 * 1024 * 1024, 'x');
            ruvia::stop_source stop;
            std::exception_ptr failure;
            ruvia::task_scope writes(worker);
            const auto stalled_write = [&]() -> ruvia::task<void> {
                try {
                    co_await tunnel->write(std::string_view(payload),
                        timeout ? ruvia::operation_options{.timeout_ = 200ms} : ruvia::operation_options{.stop_token_ = stop.token()});
                } catch (...) {
                    failure = std::current_exception();
                }
            };
            writes.spawn(stalled_write());
            if (!timeout) {
                (void)co_await ruvia::sleep_for(worker, 200ms);
                std::thread([&stop] { stop.request_stop(); }).join();
            }
            co_await writes.join();
            write_interruptions += interrupted(failure, timeout ? std::errc::timed_out : std::errc::operation_canceled);
            bool aborted = false;
            try {
                co_await tunnel->write(std::string_view("after"));
            } catch (const std::exception&) {
                aborted = true;
            }
            EXPECT_TRUE(aborted);
            phase = 3;
        };
        ruvia::http_server server(attachment.loop(), {.request_stream_ = handle});
        server.listen({.address_ = "127.0.0.1", .http_ = plain.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        ruvia::listen_config tls{.address_ = "127.0.0.1", .https_ = secure.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
        tls.http3_ = udp.endpoint().port();
#endif
        server.listen(std::move(tls));
        std::exception_ptr failure;
        std::size_t multiplexed_targets = 0;
        std::size_t target_count = 0;
        try {
            co_await server.start();
            std::vector<client_target> targets{
                {"http://127.0.0.1:" + std::to_string(plain.endpoint().port()), ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11, "HTTP/1"},
                {"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11, "TLS HTTP/1"}};
#if defined(RUVIA_ENABLE_HTTP2)
            targets.push_back({"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http1_ = false}, ruvia::http_protocol_version::http2, "HTTP/2"});
#endif
#if defined(RUVIA_ENABLE_HTTP3)
            targets.push_back({"https://127.0.0.1:" + std::to_string(udp.endpoint().port()), ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true}, ruvia::http_protocol_version::http3, "HTTP/3"});
#endif
            target_count = targets.size();
            for (const auto& target : targets) {
                const bool multiplexed = target.version_ != ruvia::http_protocol_version::http11;
                multiplexed_targets += multiplexed;
                ruvia::http_client_config config{.base_url_ = target.base_url_, .versions_ = target.versions_};
                config.tls_.ca_file_ = identity.ca_file_;
                config.request_timeout_ = 10s;
                ruvia::http_client client(attachment.loop(), config);
                std::exception_ptr exchange_failure;
                const auto await_phase = [&](unsigned expected) -> ruvia::task<void> {
                    for (unsigned count = 0; phase != expected && count != 5000; ++count) {
                        (void)co_await ruvia::sleep_for(client.worker(), 1ms);
                    }
                    EXPECT_EQ(phase, expected) << target.label_;
                };
                try {
                    {
                        phase = 0;
                        auto opened = co_await client.client().connect("read.test:443").open();
                        auto* tunnel = opened.tunnel();
                        EXPECT_TRUE(tunnel != nullptr) << target.label_;
                        if (tunnel != nullptr) {
                            co_await await_phase(1);
                            co_await tunnel->write("abc");
                            co_await await_phase(2);
                            co_await tunnel->write("def");
                            co_await tunnel->finish();
                            std::string echoed;
                            while (auto chunk = co_await tunnel->text()) {
                                echoed.append(*chunk);
                            }
                            EXPECT_EQ(echoed, "abcdef") << target.label_;
                        }
                    }
                    for (const std::string_view authority : {"write-timeout.test:443", "write-stop.test:443"}) {
                        phase = 0;
                        auto opened = co_await client.client().connect(authority).open();
                        EXPECT_TRUE(opened.tunnel() != nullptr) << target.label_;
                        co_await await_phase(3);
                        if (auto* tunnel = opened.tunnel()) {
                            tunnel->abort();
                        }
                    }
                    {
                        phase = 0;
                        auto opened = co_await client.client().udp("/.well-known/masque/udp/echo.test/53/").open();
                        auto* tunnel = opened.tunnel();
                        EXPECT_TRUE(tunnel != nullptr) << target.label_;
                        if (tunnel != nullptr) {
                            auto channel = std::move(*tunnel).udp();
                            // The client adapter keeps the contract in the client's error family.
                            const auto client_failure = [](auto operation) -> ruvia::task<std::optional<std::error_code>> {
                                try {
                                    (void)co_await std::move(operation);
                                } catch (const ruvia::http_client_error& error) {
                                    co_return error.code();
                                }
                                co_return std::nullopt;
                            };
                            EXPECT_THROW((void)channel.read({.timeout_ = 0ms}), std::invalid_argument);
                            EXPECT_TRUE((co_await client_failure(channel.read({.timeout_ = 30ms}))) == std::make_error_code(std::errc::timed_out)) << target.label_;
                            ruvia::stop_source stop;
                            stop.request_stop();
                            EXPECT_TRUE((co_await client_failure(channel.send(std::string_view("lost"), {.stop_token_ = stop.token()}))) ==
                                        std::make_error_code(std::errc::operation_canceled))
                                << target.label_;
                            co_await await_phase(6);
                            co_await channel.send(std::string_view("dgram"));
                            const auto echoed = co_await channel.read();
                            EXPECT_TRUE(echoed.has_value()) << target.label_;
                            if (echoed) {
                                EXPECT_EQ(echoed->text(), "dgram") << target.label_;
                            }
                            EXPECT_FALSE((co_await channel.read()).has_value()) << target.label_;
                            co_await channel.finish();
                        }
                    }
                    if (multiplexed) {
                        phase = 0;
                        auto opened = co_await client.client().connect("localhost").protocol("op-capsules").target("/op-capsules").open();
                        auto* tunnel = opened.tunnel();
                        EXPECT_TRUE(tunnel != nullptr) << target.label_;
                        if (tunnel != nullptr) {
                            // A capsule of type 17 with a 15-byte payload, sent in two
                            // writes so a read can time out holding its first half.
                            co_await tunnel->write(std::string_view(
                                "\x11\x0f"
                                "capsule"));
                            co_await await_phase(4);
                            co_await tunnel->write(std::string_view("-payload"));
                            std::string echoed;
                            while (auto chunk = co_await tunnel->text()) {
                                echoed.append(*chunk);
                            }
                            co_await tunnel->finish();
                            EXPECT_EQ(echoed, std::string_view("\x11\x0f"
                                                               "capsule-payload"))
                                << target.label_;
                        }
                    }
                } catch (...) {
                    exchange_failure = std::current_exception();
                }
                co_await client.shutdown();
                if (exchange_failure) std::rethrow_exception(exchange_failure);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await server.shutdown();
        attachment.stop();
        if (failure) std::rethrow_exception(failure);
        // Per target: tunnel 3 + udp 1 rejections, plus capsules 1 when multiplexed.
        EXPECT_EQ(rejected, 4 * target_count + multiplexed_targets);
        // Tunnel timeout and stop, udp timeout; capsule timeout and stop.
        EXPECT_EQ(read_interruptions, 3 * target_count + 2 * multiplexed_targets);
        EXPECT_EQ(prestopped, 3 * target_count + multiplexed_targets);
        EXPECT_EQ(write_interruptions, 2 * target_count);
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

// pipe_to(response_stream_writer&) is one client operation bounding both its
// upstream reads and its downstream writes. A stalled upstream read that
// times out or is stopped keeps the unread body, so a later forward completes
// it; a stalled downstream write that times out resets the downstream request
// and reports the client's timeout; an already-stopped token forwards nothing.
TEST(Ruvia, response_pipe_bounds_upstream_reads_and_downstream_writes) {
    using namespace request_stream_test;
    asio::io_context io;
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &preparation});
    ruvia::test::tcp_port_reservation plain(io), secure(io);
    ruvia::test::udp_port_reservation udp(io);
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    const auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client> upstream;
        unsigned phase = 0;
        std::vector<std::optional<std::error_code>> outcomes;
        const auto failure_of = [](ruvia::task<void> forward) -> ruvia::task<std::optional<std::error_code>> {
            try {
                co_await std::move(forward);
            } catch (const ruvia::http_client_error& error) {
                co_return error.code();
            }
            co_return std::nullopt;
        };
        const auto handle = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
            const auto path = stream.request().path();
            auto& output = stream.response();
            if (path == "/stalled") {
                (void)co_await output.commit();
                co_await output.write(std::string_view("first-"));
                (void)co_await output.sleep(300ms);
                co_await output.write(std::string_view("second"));
                co_await output.end();
                co_return;
            }
            if (path == "/large") {
                (void)co_await output.commit();
                const std::string chunk(64 * 1024, 'x');
                for (unsigned index = 0; index != 1024; ++index) {
                    co_await output.write(std::string_view(chunk));
                }
                co_await output.end();
                co_return;
            }
            const bool downstream = path == "/proxy/downstream-timeout";
            auto source = co_await upstream->client().get(downstream ? "/large" : "/stalled").send();
            (void)co_await output.commit();
            ruvia::stop_source stop;
            ruvia::operation_options operation{.timeout_ = downstream ? 300ms : 100ms};
            if (path == "/proxy/upstream-stop") {
                operation = {.stop_token_ = stop.token()};
            } else if (path == "/proxy/prestopped") {
                stop.request_stop();
                operation = {.stop_token_ = stop.token()};
            }
            EXPECT_THROW((void)source.stream().pipe_to(output, {.timeout_ = 0ms}), std::invalid_argument);
            ruvia::task_scope stopper(stream.worker());
            const auto stop_later = [&]() -> ruvia::task<void> {
                (void)co_await ruvia::sleep_for(stream.worker(), 50ms);
                std::thread([&stop] { stop.request_stop(); }).join();
            };
            if (path == "/proxy/upstream-stop") {
                stopper.spawn(stop_later());
            }
            outcomes.push_back(co_await failure_of(source.stream().pipe_to(output, std::move(operation))));
            co_await stopper.join();
            if (downstream) {
                EXPECT_TRUE(output.aborted());
                phase = 1;
                co_return;
            }
            co_await source.stream().pipe_to(output);
            co_await output.end();
        };
        ruvia::http_server server(attachment.loop(), {.request_stream_ = handle});
        server.listen({.address_ = "127.0.0.1", .http_ = plain.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        ruvia::listen_config tls{.address_ = "127.0.0.1", .https_ = secure.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
        tls.http3_ = udp.endpoint().port();
#endif
        server.listen(std::move(tls));
        std::exception_ptr failure;
        std::size_t target_count = 0;
        try {
            co_await server.start();
            const std::string upstream_url = "http://127.0.0.1:" + std::to_string(plain.endpoint().port());
            upstream.emplace(attachment.loop(), ruvia::http_client_config{.base_url_ = upstream_url, .versions_ = ruvia::http_client_versions{.http2_ = false}});
            std::vector<client_target> targets{
                {"http://127.0.0.1:" + std::to_string(plain.endpoint().port()), ruvia::http_client_versions{.http2_ = false}, ruvia::http_protocol_version::http11, "HTTP/1"}};
#if defined(RUVIA_ENABLE_HTTP2)
            targets.push_back({"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http1_ = false}, ruvia::http_protocol_version::http2, "HTTP/2"});
#endif
#if defined(RUVIA_ENABLE_HTTP3)
            targets.push_back({"https://127.0.0.1:" + std::to_string(udp.endpoint().port()), ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true}, ruvia::http_protocol_version::http3, "HTTP/3"});
#endif
            target_count = targets.size();
            for (const auto& target : targets) {
                ruvia::http_client_config config{.base_url_ = target.base_url_, .versions_ = target.versions_};
                config.tls_.ca_file_ = identity.ca_file_;
                config.request_timeout_ = 10s;
                ruvia::http_client client(attachment.loop(), config);
                std::exception_ptr exchange_failure;
                try {
                    for (const std::string_view path : {"/proxy/upstream-timeout", "/proxy/upstream-stop", "/proxy/prestopped"}) {
                        auto response = co_await client.client().get(path).send();
                        const auto body = co_await response.text();
                        EXPECT_EQ(body.text(), "first-second") << target.label_ << ' ' << path;
                    }
                    phase = 0;
                    auto stalled = co_await client.client().get("/proxy/downstream-timeout").send();
                    for (unsigned count = 0; phase != 1 && count != 5000; ++count) {
                        (void)co_await ruvia::sleep_for(client.worker(), 1ms);
                    }
                    EXPECT_EQ(phase, 1U) << target.label_;
                    bool truncated = false;
                    try {
                        while (co_await stalled.stream().read()) {
                        }
                    } catch (const ruvia::http_client_error&) {
                        truncated = true;
                    }
                    EXPECT_TRUE(truncated) << target.label_;
                } catch (...) {
                    exchange_failure = std::current_exception();
                }
                co_await client.shutdown();
                if (exchange_failure) std::rethrow_exception(exchange_failure);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        if (upstream) {
            co_await upstream->shutdown();
        }
        co_await server.shutdown();
        attachment.stop();
        if (failure) std::rethrow_exception(failure);
        EXPECT_EQ(outcomes.size(), 4 * target_count);
        for (std::size_t index = 0; index + 3 < outcomes.size(); index += 4) {
            EXPECT_TRUE(outcomes[index] == std::make_error_code(std::errc::timed_out));
            EXPECT_TRUE(outcomes[index + 1] == std::make_error_code(std::errc::operation_canceled));
            EXPECT_TRUE(outcomes[index + 2] == std::make_error_code(std::errc::operation_canceled));
            EXPECT_TRUE(outcomes[index + 3] == std::make_error_code(std::errc::timed_out));
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, response_pipe_to_request_stream_forwards_status_headers_body_and_trailers) {
    using namespace request_stream_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        const auto origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::http_client upstream(attachment.loop(), {.base_url_ = origin, .versions_ = {.http2_ = false}});
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& request) -> ruvia::task<void> {
            if (request.request().path() == "/source") {
                request.draft().status(ruvia::http_status::created);
                request.draft().header("x-end-to-end", "retained");
                request.draft().header("connection", "x-private");
                request.draft().header("x-private", "removed");
                (void)co_await request.response().commit();
                co_await request.response().write("forwarded-body");
                const std::array trailers{ruvia::http_header_view{"x-checksum", "complete"}};
                co_await request.response().end(trailers);
            } else {
                auto response = co_await upstream.client().get("/source").send();
                co_await response.pipe_to(request);
            }
        }});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        ruvia::http_client downstream(attachment.loop(), {.base_url_ = origin, .versions_ = {.http2_ = false}});
        std::exception_ptr failure;
        try {
            co_await server.start();
            auto response = co_await downstream.client().get("/proxy").send();
            EXPECT_EQ(response.status(), ruvia::http_status::created);
            EXPECT_EQ(response.header("x-end-to-end").value_or(""), "retained");
            EXPECT_FALSE(response.header("x-private").has_value());
            const auto body = co_await response.text();
            EXPECT_EQ(body.text(), "forwarded-body");
            EXPECT_EQ(response.trailers().size(), 1U);
            if (!response.trailers().empty()) {
                EXPECT_EQ(response.trailers().front().name(), "x-checksum");
                EXPECT_EQ(response.trailers().front().value(), "complete");
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await downstream.shutdown();
        co_await upstream.shutdown();
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

// Multiple client connections share one application rate-limit budget.
// This executable owns its application, so its rate limit and short request
// phase timeouts cannot leak into other suites.

namespace server_admission_test {

constexpr std::size_t admitted_requests = 4;
constexpr auto phase_timeout = std::chrono::milliseconds(200);

[[nodiscard]] ruvia::server_config admission_server_config() {
    return {.worker_count_ = 1,
        .idle_timeout_ = phase_timeout,
        .connection_scan_interval_ = std::chrono::milliseconds(5),
        .request_header_timeout_ = phase_timeout,
        .request_body_timeout_ = phase_timeout};
}

// Error-handler calls of the request in flight; the server worker writes, the
// client coroutine reads after the response arrives.
std::atomic<int> error_handler_calls{0};

ruvia::task<ruvia::http_response> counted_error(ruvia::context& c, ruvia::http_error_info error) {
    ++error_handler_calls;
    c.draft().status(error.status());
    co_return c.text("rejected");
}

void configure_admission_app() {
    ruvia::app().rate_limit(ruvia::rate_limit_config{
        .rule_ = {.max_requests_ = admitted_requests, .window_ = std::chrono::minutes(10)}});
    ruvia::app().trusted_proxies({.cidrs_ = {"127.0.0.1/32"}});
    ruvia::app().on_error(nullptr).on_error(&counted_error);
}

class admission_routes final {
    RUVIA_ROUTES_BEGIN(admission_routes)
    RUVIA_GET("/plain", plain);
    RUVIA_POST("/upload", upload);
    RUVIA_GET("/events", events);
    RUVIA_GET("/boom", boom);
    RUVIA_GET("/limited", plain, ruvia::rate_limit<1, 600000>);
    // Answer without reading the request body.
    RUVIA_POST("/ignore", plain);
    RUVIA_POST("/stream-ignore", events);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> plain(ruvia::context& context_value) {
        co_return context_value.text("plain");
    }

    ruvia::task<ruvia::http_response> upload(ruvia::context& context_value) {
        (void)co_await context_value.req().text();
        co_return context_value.text("uploaded");
    }

    ruvia::task<ruvia::http_response> boom(ruvia::context&) {
        throw std::runtime_error("boom");
        co_return ruvia::http_response{};
    }

    ruvia::task<void> events(ruvia::context& context_value) {
        if (co_await context_value.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await context_value.stream().write("event");
    }
};

// Sends one GET on a fresh connection that the server closes after its
// response; returns everything the server wrote.
std::string exchange(asio::io_context& io, const asio::ip::tcp::endpoint& server_endpoint, std::string_view target,
    std::string_view headers) {
    std::string request{"GET "};
    request.append(target).append(" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n");
    request.append(headers).append("\r\n");
    return ruvia::test::exchange(io, server_endpoint, request);
}

std::string admission_lowercase(std::string_view text) {
    std::string lowered(text);
    for (auto& character : lowered) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return lowered;
}

// Sends `prefix` on a fresh HTTP/1.1 connection, then stalls. Returns the
// bytes the server wrote before closing, or nullopt when it is still open
// after 5 s.
std::optional<std::string> stalled_connection_output(const asio::ip::tcp::endpoint& server_endpoint,
    const asio::ip::address_v4& local_address, std::string_view prefix) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::socket socket(io);
    socket.open(asio::ip::tcp::v4());
    socket.bind({local_address, 0});
    socket.connect(server_endpoint);
    if (!prefix.empty()) {
        asio::write(socket, asio::buffer(prefix));
    }
    std::string output{};
    std::error_code result;
    bool closed = false;
    asio::async_read(socket, asio::dynamic_buffer(output), [&](const std::error_code& error, std::size_t) {
        result = error;
        closed = true;
    });
    io.run_for(std::chrono::seconds(5));
    if (!closed || (result != asio::error::eof && result != asio::error::connection_reset)) {
        return std::nullopt;
    }
    return output;
}

// The status codes of the HTTP/1.1 responses in `output`, concatenated.
std::string status_codes(std::string_view output) {
    constexpr std::string_view status_line = "HTTP/1.1 ";
    std::string codes{};
    for (auto position = output.find(status_line); position != std::string_view::npos;
        position = output.find(status_line, position + status_line.size())) {
        codes.append(output.substr(position + status_line.size(), 3));
    }
    return codes;
}

}  // namespace server_admission_test

TEST(Ruvia, http1_clients_share_406_admission_and_rate_limit_budget) {
    using namespace server_admission_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    configure_admission_app();
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, admission_server_config());
    constexpr std::string_view refuse_every_coding = "Accept-Encoding: identity;q=0, *;q=0\r\n";
    // Each exchange runs on its own connection from the same client address.
    // A response stream commits its representation before a status can be
    // inspected, so a client that accepts no coding gets 406 first. The
    // rejection is a terminal error: the error handler runs once and its
    // answer is not negotiated again.
    for (int connection = 0; connection != 2; ++connection) {
        error_handler_calls = 0;
        const auto response = exchange(io, server.endpoint(), "/events", refuse_every_coding);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 406 "));
        EXPECT_TRUE(response.ends_with("rejected"));
        EXPECT_EQ(error_handler_calls.load(), 1);
    }
    for (int connection = 0; connection != 2; ++connection) {
        const auto response = exchange(io, server.endpoint(), "/plain", {});
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(response.ends_with("plain"));
    }
    // Every connection drew on the same client-address budget; it is spent. A
    // client that accepts no coding still learns why.
    const auto admitted_limit = std::move(std::string("x-ratelimit-limit: ").append(std::to_string(admitted_requests))) + "\r\n";
    for (int connection = 0; connection != 2; ++connection) {
        for (const auto headers : {std::string_view{}, refuse_every_coding}) {
            error_handler_calls = 0;
            const auto response = exchange(io, server.endpoint(), "/plain", headers);
            const auto head = admission_lowercase(response);
            EXPECT_TRUE(response.starts_with("HTTP/1.1 429 "));
            EXPECT_TRUE(head.find("\r\nretry-after: ") != std::string::npos);
            EXPECT_TRUE(head.find(std::string("\r\n") + admitted_limit) != std::string::npos);
            EXPECT_TRUE(response.ends_with("rejected"));
            EXPECT_EQ(error_handler_calls.load(), 1);
        }
    }
    server.finish();
}

TEST(Ruvia, http1_server_closes_connections_stalled_past_request_phase_timeouts) {
    using namespace server_admission_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, admission_server_config());
    const auto loopback = asio::ip::address_v4::loopback();
    // Idle: a connection that never sends a request.
    const auto idle = stalled_connection_output(server.endpoint(), loopback, "");
    EXPECT_TRUE(idle.has_value());
    // Initial read: a request head that never completes.
    const auto head = stalled_connection_output(server.endpoint(), loopback, "GET /plain HTTP/1.1\r\nHost: a\r\n");
    EXPECT_TRUE(head.has_value());
    EXPECT_TRUE((head ? std::string_view(*head) : std::string_view{}).find("plain") == std::string_view::npos);
    // Payload read: an admitted request whose body stalls. A distinct loopback
    // source address keeps it outside the rate-limit budget of the other case.
    const auto upload = stalled_connection_output(server.endpoint(), asio::ip::make_address_v4("127.0.0.2"),
        "POST /upload HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nab");
    EXPECT_TRUE(upload.has_value());
    EXPECT_TRUE((upload ? std::string_view(*upload) : std::string_view{}).find("uploaded") == std::string_view::npos);
    EXPECT_TRUE((upload ? std::string_view(*upload) : std::string_view{}).find(" 429 ") == std::string_view::npos);
    server.finish();
}

TEST(Ruvia, http1_failed_handler_error_response_is_not_renegotiated_into_406) {
    using namespace server_admission_test;
    // The 500 for a throwing handler carries no representation of the target
    // resource, so a refused coding must neither replace it with a 406 nor run
    // the error handler again. No compression is configured, so
    // "gzip, identity;q=0" cannot be satisfied either. Distinct loopback source
    // addresses keep these requests outside the other cases' rate-limit budgets.
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, admission_server_config());
    const std::array<std::pair<std::string_view, std::string_view>, 2> cases{{
        {"127.0.0.3", "identity;q=0, *;q=0"},
        {"127.0.0.4", "gzip, identity;q=0"},
    }};
    for (const auto& [source, refused] : cases) {
        error_handler_calls = 0;
        std::string request("GET /boom HTTP/1.1\r\nHost: a\r\nConnection: close\r\nAccept-Encoding: ");
        request.append(refused);
        request.append("\r\n\r\n");
        const auto output =
            stalled_connection_output(server.endpoint(), asio::ip::make_address_v4(source), request);
        EXPECT_TRUE(output.has_value());
        EXPECT_TRUE((output ? std::string_view(*output) : std::string_view{}).starts_with("HTTP/1.1 500 "));
        EXPECT_TRUE((output ? std::string_view(*output) : std::string_view{}).find("rejected") != std::string_view::npos);
        EXPECT_EQ(error_handler_calls.load(), 1);
    }
    server.finish();
}

TEST(Ruvia, http1_rate_limits_canonicalize_forwarded_client_addresses) {
    using namespace server_admission_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, admission_server_config());
    const auto loopback = asio::ip::address_v4::loopback();
    // Each pair has its own address budget. The second spelling must consume
    // the first spelling's budget through both independently enforced rules.
    const std::array<std::pair<std::string_view, std::string_view>, 6> equivalent{{
        {"fe80::a%12", "fe80:0:0:0:0:0:0:a%12"},
        {"fe80::b%12", "FE80::B%12"},
        {"fe80::c%12", "FE80:0:0:0:0:0:0:C%12"},
        {"2001:db8:10::1", "2001:db8:10::ffff"},
        {"192.0.2.10", "::ffff:192.0.2.10"},
        {"::ffff:192.0.2.11", "192.0.2.11"},
    }};
    auto send = [&](std::string_view target, std::string_view address) {
        std::string request("GET ");
        request.append(target);
        request.append(" HTTP/1.1\r\nHost: a\r\nConnection: close\r\nX-Forwarded-For: ");
        request.append(address);
        request.append("\r\n\r\n");
        auto output = stalled_connection_output(server.endpoint(), loopback, request);
        EXPECT_TRUE(output.has_value());
        return std::move(output).value_or(std::string{});
    };
    for (const auto& [first, alternate] : equivalent) {
        EXPECT_TRUE(send("/limited", first).starts_with("HTTP/1.1 200 "));
        const auto route_rejection = send("/limited", alternate);
        EXPECT_TRUE(route_rejection.starts_with("HTTP/1.1 429 "));
        EXPECT_TRUE(route_rejection.find("X-RateLimit-Limit: 1\r\n") != std::string::npos);
        // The two requests above also consumed two of the four app-wide
        // admissions. Spend the rest without the route-specific middleware.
        for (std::size_t i = 2; i < admitted_requests; ++i) {
            EXPECT_TRUE(send("/plain", first).starts_with("HTTP/1.1 200 "));
        }
        const auto app_rejection = send("/plain", alternate);
        EXPECT_TRUE(app_rejection.starts_with("HTTP/1.1 429 "));
        EXPECT_TRUE(app_rejection.find("X-RateLimit-Limit: 4\r\n") != std::string::npos);
    }
    // Scoped identities preserve all 128 address bits and the numeric scope,
    // and cannot collide with the separate unscoped /64 namespace.
    for (const auto address : {"fe80::a%13", "fe80::d%12", "fe80::a", "2001:db8:11::1", "192.0.2.12"}) {
        EXPECT_TRUE(send("/limited", address).starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(send("/limited", address).starts_with("HTTP/1.1 429 "));
        for (std::size_t i = 2; i < admitted_requests; ++i) {
            EXPECT_TRUE(send("/plain", address).starts_with("HTTP/1.1 200 "));
        }
        EXPECT_TRUE(send("/plain", address).starts_with("HTTP/1.1 429 "));
    }
    server.finish();
}

// A request body that arrived in full with its head costs no keep-alive when
// the response never reads it -- an unresolved target, a handler or stream
// that ignores it, or an app-wide 429 -- so the request pipelined behind it is
// still answered. A body still incomplete closes the connection instead.
// Distinct loopback source addresses give each case its own rate-limit budget.
TEST(Ruvia, http1_unread_buffered_body_keeps_the_connection) {
    using namespace server_admission_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, admission_server_config());
    const auto output_from = [&](std::string_view source, std::string_view requests) {
        auto output =
            stalled_connection_output(server.endpoint(), asio::ip::make_address_v4(source), requests);
        EXPECT_TRUE(output.has_value());
        return std::move(output).value_or(std::string{});
    };
    const std::string next("GET /plain HTTP/1.1\r\nHost: a\r\n\r\n");
    const std::string length_body("Content-Length: 5\r\n\r\nhello");

    EXPECT_EQ(
        status_codes(output_from("127.0.0.10", std::string("POST /missing HTTP/1.1\r\nHost: a\r\n") + length_body + next)),
        std::string("404200"));
    EXPECT_EQ(status_codes(output_from("127.0.0.11",
                  std::string("POST /plain HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
                              "5\r\nhello\r\n0\r\n\r\n") +
                      next)),
        std::string("405200"));
    EXPECT_EQ(
        status_codes(output_from("127.0.0.12", std::string("POST /ignore HTTP/1.1\r\nHost: a\r\n") + length_body + next)),
        std::string("200200"));
    const auto streamed =
        output_from("127.0.0.13", std::string("POST /stream-ignore HTTP/1.1\r\nHost: a\r\n") + length_body + next);
    EXPECT_EQ(status_codes(streamed), std::string("200200"));
    EXPECT_TRUE(streamed.find("event") != std::string::npos);

    // Body bytes still to come: the response closes the connection.
    const auto partial =
        output_from("127.0.0.14", "POST /missing HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\nab");
    EXPECT_EQ(status_codes(partial), std::string("404"));
    EXPECT_TRUE(partial.find("Connection: close\r\n") != std::string::npos);

    // The app-wide limit admits four requests; the 429s that follow keep the
    // connection while the body is in hand, and close it when it is not.
    std::string spent{};
    for (std::size_t i = 0; i < admitted_requests; ++i) {
        spent.append(next);
    }
    EXPECT_EQ(
        status_codes(output_from("127.0.0.15", std::string(spent) + "POST /upload HTTP/1.1\r\nHost: a\r\n" + length_body + next)),
        std::string("200200200200429429"));
    const auto limited_partial =
        output_from("127.0.0.16", std::string(spent) + "POST /upload HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\nab");
    EXPECT_EQ(status_codes(limited_partial), std::string("200200200200429"));
    EXPECT_TRUE(limited_partial.find("Connection: close\r\n") != std::string::npos);
    server.finish();
}
