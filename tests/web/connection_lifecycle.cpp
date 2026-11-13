#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <semaphore>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/socket_base.hpp>
#include <asio/ssl.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/objects.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"

#include "http_server_fixture.h"

#if defined(RUVIA_ENABLE_HTTP3)
#include "ruvia/http/http3_connection.h"
#endif
#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/http2_connection.h"
#endif

#include <tuple>

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include "ruvia/http/http_response.h"
#include "ruvia/web/model.h"

#include "content_decoding_fixture.h"
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

namespace http1_websocket_test {
struct websocket_eof_observation {
    std::atomic<bool> first_message_{};
    std::atomic<bool> eof_{};
    std::atomic<bool> read_overlap_rejected_{};
    std::atomic<bool> write_overlap_rejected_{};
    std::atomic<bool> failed_{};
    std::atomic<bool> inform_rejected_{};
    std::atomic<bool> send_read_rejected_{};
    std::atomic<bool> send_close_rejected_{};
    std::atomic<bool> send_during_read_rejected_{};
    std::atomic<bool> send_echo_finished_{};
    std::promise<unsigned> close_rejections_;
    std::atomic<bool> close_completed_{};
};
const auto websocket_eof_observed = std::make_shared<websocket_eof_observation>();
[[maybe_unused]] const bool websocket_eof_observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<websocket_eof_observation>>(
        [] { return websocket_eof_observed; });
    return true;
}();

// Large enough that loopback socket buffers cannot absorb it, so the write
// stays active while the client does not read.
constexpr std::size_t stalled_payload_size = std::size_t{16} << 20;
constexpr std::size_t fair_handoff_messages = 33;

class http1_websocket_routes final {
    RUVIA_ROUTES_BEGIN(http1_websocket_routes)
    // Unsigned input beyond the signed millisecond range saturates before
    // normalization; the Pong timeout defaults to the saturated Ping interval.
    const auto longest_heartbeat = ruvia::websocket_route_config{
        .lifecycle_ = {.heartbeat_ = {.ping_interval_ =
                                          std::chrono::duration<std::uint64_t, std::milli>{
                                              (std::numeric_limits<std::uint64_t>::max)()}}}};
    const auto required_subprotocol =
        ruvia::websocket_route_config{.subprotocols_ = {"chat.v1"}, .require_subprotocol_ = true};
    const auto takeover_deflate =
        ruvia::websocket_route_config{.deflate_ = {.context_takeover_ = true, .deflater_ = {.mem_level_ = 4}}};
    // Short enough for a Ping and a lapsed Pong window inside the handler's
    // idle start, long enough that a tick write delayed by a busy CI host is
    // not mistaken for a stalled peer (that budget is interval + timeout).
    const auto short_heartbeat = ruvia::websocket_route_config{
        .lifecycle_ = {.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(100),
                           .pong_timeout_ = std::chrono::milliseconds(150)}}};
    RUVIA_GET_WS("/http1-websocket-eof", read_until_eof);
    RUVIA_GET_WS("/http1-websocket-close-lanes", close_exclusively);
    RUVIA_GET_WS("/http1-websocket-longest-heartbeat", write_stalled, longest_heartbeat);
    RUVIA_GET_WS("/http1-websocket-write-only-heartbeat", write_without_reading, short_heartbeat);
    RUVIA_GET_WS("/http1-websocket-fair-handoff", write_with_pending_read, longest_heartbeat);
    RUVIA_GET_WS("/http1-websocket-required-subprotocol", accept_only, required_subprotocol);
    RUVIA_GET_WS("/http1-websocket-upstream-reset", fail_on_upstream_reset);
    RUVIA_GET_WS("/http1-websocket-echo-send", echo_send);
    RUVIA_GET_WS("/http1-websocket-deflate-echo", echo_all);
    RUVIA_GET_WS("/http1-websocket-deflate-takeover", echo_all, takeover_deflate);
    const auto small_message = ruvia::websocket_route_config{
        .max_message_bytes_ = ruvia::protocol_byte_limit::limited(256)};
    RUVIA_GET_WS("/http1-websocket-small-message", echo_all, small_message);
    RUVIA_ROUTES_END

    ruvia::task<void> echo_all(ruvia::context& context_value) {
        auto& websocket_value = context_value.websocket();
        for (;;) {
            auto message = co_await websocket_value.read();
            if (!message.has_value()) {
                co_return;
            }
            co_await websocket_value.send(std::move(*message));
        }
    }

    ruvia::task<void> accept_only(ruvia::context&) {
        co_return;
    }

    // Fails the way an awaited upstream request whose connection was reset
    // does; the inbound websocket itself is healthy.
    ruvia::task<void> fail_on_upstream_reset(ruvia::context& context_value) {
        const auto message = co_await context_value.websocket().read();
        if (message.has_value()) {
            throw std::system_error(asio::error::make_error_code(asio::error::connection_reset), "upstream request");
        }
    }

    ruvia::task<void> write_stalled(ruvia::context& context_value) {
        co_await context_value.websocket().binary(std::string(stalled_payload_size, 'w'));
    }

    ruvia::task<void> write_with_pending_read(ruvia::context& context_value) {
        auto& websocket_value = context_value.websocket();
        ruvia::task_scope writes(context_value.worker());
        const auto send = [&]() -> ruvia::task<void> {
            co_await websocket_value.binary(std::string(stalled_payload_size, 'w'));
            for (std::size_t index = 1; index != fair_handoff_messages; ++index) {
                co_await websocket_value.text("tick");
            }
        };
        // spawn() starts the first write up to its transport suspension before
        // read() reserves its turn. The peer has not drained any payload yet.
        writes.spawn(send());
        std::exception_ptr failure;
        try {
            while (co_await websocket_value.read()) {
            }
        } catch (...) {
            failure = std::current_exception();
            websocket_value.abort();
        }
        co_await writes.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    // Never reads, so no Pong is ever polled. Idle long enough for a Ping to
    // go out and its Pong window to lapse unobserved, then keep writing; the
    // peer answers every Ping.
    ruvia::task<void> write_without_reading(ruvia::context& context_value) {
        auto& websocket_value = context_value.websocket();
        if (co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(300)) !=
            ruvia::timer_sleep_result::elapsed) {
            co_return;
        }
        for (int i = 0; i < 20; ++i) {
            co_await websocket_value.text("tick");
            if (co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(10)) !=
                ruvia::timer_sleep_result::elapsed) {
                co_return;
            }
        }
        co_await websocket_value.close();
    }

    // send(websocket_message&&) borrows the read buffer, so it claims the read
    // lane at the call and holds it until the frame is written or dropped.
    ruvia::task<void> echo_send(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<websocket_eof_observation>>();
        auto& websocket_value = context_value.websocket();
        try {
            auto first = co_await websocket_value.read();
            if (!first.has_value()) {
                throw std::runtime_error("missing first message");
            }
            auto echo = websocket_value.send(std::move(*first));
            try {
                auto overlap = websocket_value.read();
                static_cast<void>(co_await std::move(overlap));
            } catch (const std::logic_error&) {
                observed_value.send_read_rejected_ = true;
            }
            try {
                co_await websocket_value.close();
            } catch (const std::logic_error&) {
                observed_value.send_close_rejected_ = true;
            }
            co_await std::move(echo);
            {
                // A dropped, never-started send releases the read lane.
                auto dropped = websocket_value.send(
                    ruvia::websocket_message::borrow(ruvia::websocket_opcode::text, "dropped"));
                static_cast<void>(dropped);
            }
            std::optional<ruvia::websocket_message> second;
            ruvia::task_scope reads(context_value.worker());
            const auto receive = [&]() -> ruvia::task<void> {
                second = co_await websocket_value.read();
            };
            reads.spawn(receive());
            try {
                static_cast<void>(websocket_value.send(
                    ruvia::websocket_message::borrow(ruvia::websocket_opcode::text, "overlap")));
            } catch (const std::logic_error&) {
                observed_value.send_during_read_rejected_ = true;
            }
            co_await reads.join();
            if (!second.has_value()) {
                throw std::runtime_error("missing second message");
            }
            co_await websocket_value.send(std::move(*second));
            const auto end = co_await websocket_value.read();
            observed_value.send_echo_finished_ = !end.has_value();
        } catch (...) {
            observed_value.failed_ = true;
        }
    }

    ruvia::task<void> close_exclusively(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<websocket_eof_observation>>();
        auto& websocket_value = context_value.websocket();
        ruvia::task_scope closing(context_value.worker());
        closing.spawn(websocket_value.close());
        unsigned rejected = 0;
        try {
            static_cast<void>(co_await websocket_value.read());
        } catch (const std::logic_error&) {
            ++rejected;
        }
        try {
            co_await websocket_value.text("must not be sent");
        } catch (const std::logic_error&) {
            ++rejected;
        }
        try {
            co_await websocket_value.close();
        } catch (const std::logic_error&) {
            ++rejected;
        }
        observed_value.close_rejections_.set_value(rejected);
        co_await closing.join();
        observed_value.close_completed_ = true;
    }

    ruvia::task<void> read_until_eof(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<websocket_eof_observation>>();
        auto& websocket_value = context_value.websocket();
        // The 101 head is committed: a 1xx head written now would land inside
        // the frame stream.
        try {
            co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        } catch (const std::logic_error&) {
            observed_value.inform_rejected_ = true;
        }
        try {
            auto first_read = websocket_value.read();
            auto last_read = websocket_value.read();
            std::string payload("ready");
            auto first_write = websocket_value.text(payload);
            auto last_write = websocket_value.text("ack");
            payload.assign("changed");
            {
                auto discarded = websocket_value.text("discarded");
                static_cast<void>(discarded);
            }
            ruvia::task_scope reads(context_value.worker());
            const auto receive = [&]() -> ruvia::task<void> {
                const auto first = co_await std::move(first_read);
                observed_value.first_message_ = first.has_value() && first->payload() == std::string_view("hi");
            };
            reads.spawn(receive());
            try {
                auto overlap = websocket_value.read();
                static_cast<void>(co_await std::move(overlap));
            } catch (const std::logic_error&) {
                observed_value.read_overlap_rejected_ = true;
            }
            ruvia::task_scope writes(context_value.worker());
            const auto send = [&]() -> ruvia::task<void> {
                co_await std::move(first_write);
            };
            writes.spawn(send());
            try {
                auto overlap = websocket_value.ping("overlap");
                co_await std::move(overlap);
            } catch (const std::logic_error&) {
                observed_value.write_overlap_rejected_ = true;
            }
            co_await writes.join();
            co_await reads.join();
            co_await std::move(last_write);
            const auto end = co_await std::move(last_read);
            observed_value.eof_ = !end.has_value();
        } catch (...) {
            observed_value.failed_ = true;
        }
    }
};

bool zero_mask(void*, ruvia::websocket_mask_key_type& key) noexcept {
    key = {};
    return true;
}

// Compressible text that is not a single repeated byte.
std::string sample_message(std::size_t size, unsigned seed) {
    std::string message(size, '\0');
    unsigned state_value = seed * 2654435761U + 7U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        message[i] = static_cast<char>('a' + ((state_value >> 16U) % 5U) + ((i / 32U) % 4U));
    }
    return message;
}

// Bytes DEFLATE cannot shrink.
std::string noise_message(std::size_t size) {
    std::string message(size, '\0');
    std::uint32_t state_value = 1234567;
    for (auto& byte : message) {
        state_value ^= state_value << 13;
        state_value ^= state_value >> 17;
        state_value ^= state_value << 5;
        byte = static_cast<char>(state_value);
    }
    return message;
}

void check_fair_handoff(bool peer_close) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    std::size_t messages = 0;
    std::optional<std::size_t> pong_after;
    std::optional<std::size_t> close_after;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog(io, std::chrono::seconds(10));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                ruvia::close_socket(client);
            }
        });
        try {
            const auto send = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto result = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(client, asio::buffer(bytes), std::move(handler));
                });
                if (result.error_code()) {
                    throw std::system_error(result.error_code());
                }
            };
            const auto receive = [&](char* bytes, std::size_t size) -> ruvia::task<void> {
                const auto result = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_read(client, asio::buffer(bytes, size), std::move(handler));
                });
                if (result.error_code()) {
                    throw std::system_error(result.error_code());
                }
            };
            // Send control frames with the upgrade, before draining the stalled
            // first message. Both are masked with the legal all-zero mask.
            constexpr std::array<char, 10> ping{'\x89', '\x84', '\0', '\0', '\0', '\0', 'f', 'a', 'i', 'r'};
            constexpr std::array<char, 8> close{'\x88', '\x82', '\0', '\0', '\0', '\0', '\x03', '\xe8'};
            std::string upgrade(
                "GET /http1-websocket-fair-handoff HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
            upgrade.append(ping.data(), ping.size());
            if (peer_close) {
                upgrade.append(close.data(), close.size());
            }
            co_await send(upgrade);
            std::string head{};
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                co_await receive(&byte, 1);
                head.push_back(byte);
            }
            EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
            std::array<char, 65536> input{};
            bool close_sent = peer_close;
            while (!close_after.has_value()) {
                std::array<unsigned char, 2> frame{};
                co_await receive(reinterpret_cast<char*>(frame.data()), frame.size());
                EXPECT_TRUE((frame[0] & 0x80U) != 0);
                EXPECT_TRUE((frame[1] & 0x80U) == 0);
                std::uint64_t length = frame[1] & 0x7fU;
                if (length == 126 || length == 127) {
                    const std::size_t size = length == 126 ? 2 : 8;
                    std::array<unsigned char, 8> extended{};
                    co_await receive(reinterpret_cast<char*>(extended.data()), size);
                    length = 0;
                    for (std::size_t index = 0; index != size; ++index) {
                        length = (length << 8U) | extended[index];
                    }
                }
                const auto opcode = frame[0] & 0x0fU;
                if (opcode == 2) {
                    EXPECT_EQ(messages, std::size_t{0});
                    EXPECT_EQ(length, stalled_payload_size);
                    while (length != 0) {
                        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), length));
                        co_await receive(input.data(), size);
                        EXPECT_TRUE(std::string_view(input.data(), size).find_first_not_of('w') == std::string_view::npos);
                        length -= size;
                    }
                    ++messages;
                } else {
                    if (length > input.size()) {
                        throw std::runtime_error("unexpected WebSocket frame size");
                    }
                    co_await receive(input.data(), static_cast<std::size_t>(length));
                    const std::string_view payload(input.data(), static_cast<std::size_t>(length));
                    if (opcode == 1) {
                        EXPECT_EQ(payload, std::string_view("tick"));
                        ++messages;
                    } else if (opcode == 10) {
                        EXPECT_EQ(payload, std::string_view("fair"));
                        pong_after = messages;
                    } else if (opcode == 8) {
                        EXPECT_EQ(payload, std::string_view("\x03\xe8", 2));
                        close_after = messages;
                    } else {
                        throw std::runtime_error("unexpected WebSocket opcode");
                    }
                }
                if (!close_sent && messages == fair_handoff_messages && pong_after.has_value()) {
                    co_await send(std::string_view(close.data(), close.size()));
                    close_sent = true;
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(client);
        (void)watchdog.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(pong_after.has_value());
    if (pong_after.has_value()) {
        EXPECT_TRUE(*pong_after < fair_handoff_messages);
    }
    EXPECT_TRUE(close_after.has_value());
    if (peer_close && close_after.has_value()) {
        EXPECT_TRUE(*close_after < fair_handoff_messages);
    } else if (!peer_close) {
        EXPECT_EQ(messages, fair_handoff_messages);
    }
}
}  // namespace http1_websocket_test

using http1_websocket_test::stalled_payload_size;
using http1_websocket_test::websocket_eof_observed;

// A reader blocked behind one complete application message gets a protocol
// poll and automatic Pong/Close flush before a sequential writer drains its
// entire finite burst. The peer keeps reading; there is no timer-based yield.
TEST(Ruvia, http1_websocket_fair_handoff_polls_ping_between_sequential_writes) {
    using namespace http1_websocket_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    http1_websocket_test::check_fair_handoff(false);
}

TEST(Ruvia, http1_websocket_fair_handoff_flushes_peer_close_between_sequential_writes) {
    using namespace http1_websocket_test;
    http1_websocket_test::check_fair_handoff(true);
}

// A peer that closes its TCP send direction without a Close frame ends the
// websocket read side (RFC 6455 7.1.5, abnormal closure); read() reports the
// end instead of failing as a transport read error.
TEST(Ruvia, http1_websocket_read_reports_orderly_peer_eof_as_end) {
    using namespace http1_websocket_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    websocket_eof_observed->first_message_ = false;
    websocket_eof_observed->eof_ = false;
    websocket_eof_observed->read_overlap_rejected_ = false;
    websocket_eof_observed->write_overlap_rejected_ = false;
    websocket_eof_observed->failed_ = false;
    websocket_eof_observed->inform_rejected_ = false;
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            const auto send = [&](std::string_view bytes_value) -> ruvia::task<void> {
                const auto result_value = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(bytes_value), std::move(handler)); });
                if (result_value.error_code()) {
                    throw std::system_error(result_value.error_code());
                }
            };
            co_await send(
                "GET /http1-websocket-eof HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
            std::string head{};
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
            std::array<char, 7> ready{};
            const auto ready_read = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read(client, asio::buffer(ready), std::move(handler));
            });
            if (ready_read.error_code()) {
                throw std::system_error(ready_read.error_code());
            }
            EXPECT_EQ(std::string_view(ready.data(), ready.size()), std::string_view("\x81\x05ready", 7));
            // One masked text frame (zero masking key), then FIN without Close.
            constexpr std::array<char, 8> frame{'\x81', '\x82', '\0', '\0', '\0', '\0', 'h', 'i'};
            co_await send(std::string_view(frame.data(), frame.size()));
            client.shutdown(asio::ip::tcp::socket::shutdown_send);
            std::array<char, 256> input{};
            for (;;) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    break;
                }
            }
        } catch (...) {
            failure = std::current_exception();
            ruvia::close_socket(client);
        }
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    ruvia::close_socket(client);
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(websocket_eof_observed->first_message_);
    EXPECT_TRUE(websocket_eof_observed->eof_);
    EXPECT_TRUE(websocket_eof_observed->read_overlap_rejected_);
    EXPECT_TRUE(websocket_eof_observed->write_overlap_rejected_);
    EXPECT_TRUE(!websocket_eof_observed->failed_);
    EXPECT_TRUE(websocket_eof_observed->inform_rejected_);
}

TEST(Ruvia, http1_websocket_close_excludes_reads_writes_and_another_close) {
    using namespace http1_websocket_test;
    auto& io = ruvia::test::new_test_io_context();
    websocket_eof_observed->close_rejections_ = std::promise<unsigned>();
    auto rejections = websocket_eof_observed->close_rejections_.get_future();
    websocket_eof_observed->close_completed_ = false;
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    constexpr std::string_view request =
        "GET /http1-websocket-close-lanes HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    asio::write(client, asio::buffer(request));
    std::string response{};
    asio::read_until(client, asio::dynamic_buffer(response), "\r\n\r\n");
    ASSERT_TRUE(response.starts_with("HTTP/1.1 101 "));
    const auto head_end = response.find("\r\n\r\n") + 4;
    // Keep the handshake pending until all overlap attempts have completed.
    ASSERT_EQ(rejections.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(rejections.get(), 3U);
    constexpr std::array<char, 8> close_frame{'\x88', '\x82', '\0', '\0', '\0', '\0', '\x03', '\xe8'};
    asio::write(client, asio::buffer(close_frame));
    client.shutdown(asio::ip::tcp::socket::shutdown_send);
    std::error_code error;
    asio::read(client, asio::dynamic_buffer(response), error);
    EXPECT_EQ(error, asio::error::eof);
    EXPECT_EQ(std::string_view(response).substr(head_end), std::string_view("\x88\x02\x03\xe8", 4));
    ruvia::close_socket(client);
    server.finish();
    EXPECT_TRUE(websocket_eof_observed->close_completed_);
}

// send(websocket_message&&) echoes the borrowed read payload with its own
// opcode, and no read or close can run until that frame is written.
TEST(Ruvia, http1_websocket_send_echoes_borrowed_message_while_holding_read_lane) {
    using namespace http1_websocket_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    websocket_eof_observed->send_read_rejected_ = false;
    websocket_eof_observed->send_close_rejected_ = false;
    websocket_eof_observed->send_during_read_rejected_ = false;
    websocket_eof_observed->send_echo_finished_ = false;
    websocket_eof_observed->failed_ = false;
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            const auto send = [&](std::string_view bytes_value) -> ruvia::task<void> {
                const auto result_value = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(bytes_value), std::move(handler)); });
                if (result_value.error_code()) {
                    throw std::system_error(result_value.error_code());
                }
            };
            const auto expect = [&](std::string_view expected) -> ruvia::task<void> {
                std::string received(expected.size(), '\0');
                const auto result_value = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_read(client, asio::buffer(received), std::move(handler)); });
                if (result_value.error_code()) {
                    throw std::system_error(result_value.error_code());
                }
                EXPECT_EQ(received, expected);
            };
            co_await send(
                "GET /http1-websocket-echo-send HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
            std::string head{};
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
            // Masked frames with a zero masking key: text "hi", then binary "abc".
            constexpr std::array<char, 8> text_frame{'\x81', '\x82', '\0', '\0', '\0', '\0', 'h', 'i'};
            co_await send(std::string_view(text_frame.data(), text_frame.size()));
            co_await expect(std::string_view("\x81\x02hi", 4));
            constexpr std::array<char, 9> binary_frame{'\x82', '\x83', '\0', '\0', '\0', '\0', 'a', 'b', 'c'};
            co_await send(std::string_view(binary_frame.data(), binary_frame.size()));
            co_await expect(std::string_view(
                "\x82\x03"
                "abc",
                5));
            client.shutdown(asio::ip::tcp::socket::shutdown_send);
            std::array<char, 256> input{};
            for (;;) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    break;
                }
            }
        } catch (...) {
            failure = std::current_exception();
            ruvia::close_socket(client);
        }
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    ruvia::close_socket(client);
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(websocket_eof_observed->send_read_rejected_);
    EXPECT_TRUE(websocket_eof_observed->send_close_rejected_);
    EXPECT_TRUE(websocket_eof_observed->send_during_read_rejected_);
    EXPECT_TRUE(websocket_eof_observed->send_echo_finished_);
    EXPECT_TRUE(!websocket_eof_observed->failed_);
}

// The stalled-write budget is the ping interval plus the Pong timeout. At the
// longest configurable heartbeat that budget is effectively unbounded, so a
// write the client takes a while to drain is never aborted.
TEST(Ruvia, http1_websocket_longest_heartbeat_never_aborts_a_slow_write) {
    using namespace http1_websocket_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    std::size_t received = 0;
    bool payload_intact = true;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog_value(io, std::chrono::seconds(10));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            constexpr std::string_view upgrade =
                "GET /http1-websocket-longest-heartbeat HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
            const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(upgrade), std::move(handler)); });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
            std::string head{};
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
            // Leave the server's write blocked across many connection scans.
            asio::steady_timer stall(io, std::chrono::milliseconds(300));
            (void)co_await ruvia::async_asio([&](auto handler) { stall.async_wait(std::move(handler)); });
            std::array<unsigned char, 10> frame_head{};
            const auto head_read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_read(client, asio::buffer(frame_head), std::move(handler)); });
            if (head_read.error_code()) {
                throw std::system_error(head_read.error_code());
            }
            EXPECT_EQ(frame_head[0], static_cast<unsigned char>(0x82));
            EXPECT_EQ(frame_head[1], static_cast<unsigned char>(127));
            std::size_t length = 0;
            for (std::size_t index = 2; index != frame_head.size(); ++index) {
                length = (length << 8) | frame_head[index];
            }
            EXPECT_EQ(length, stalled_payload_size);
            std::array<char, 65536> input{};
            while (received < stalled_payload_size) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    client.async_read_some(asio::buffer(input.data(), std::min(input.size(), stalled_payload_size - received)), std::move(handler));
                });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                payload_intact = payload_intact && std::string_view(input.data(), read.result()).find_first_not_of('w') == std::string_view::npos;
                received += read.result();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(client);
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_EQ(received, stalled_payload_size);
    EXPECT_TRUE(payload_intact);
}

// A handler that only writes never polls the Pong the peer sends. The Pong
// window counts only while a read can observe it, so a responsive peer keeps
// the connection until the handler's own Close instead of being aborted once
// the ping interval plus Pong timeout have elapsed.
TEST(Ruvia, http1_websocket_heartbeat_does_not_abort_an_unread_responsive_peer) {
    using namespace http1_websocket_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    constexpr std::string_view upgrade =
        "GET /http1-websocket-write-only-heartbeat HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    asio::write(client, asio::buffer(upgrade));
    std::string head{};
    char byte{};
    while (!head.ends_with("\r\n\r\n")) {
        asio::read(client, asio::buffer(&byte, 1));
        head.push_back(byte);
    }
    EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
    ruvia::http_io_buffer peer_input;
    ruvia::websocket_connection peer(peer_input, {.role_ = ruvia::websocket_connection_role::client,
                                                     .mask_key_generator_ = &http1_websocket_test::zero_mask});
    const auto flush = [&] {
        const std::string bytes_value(peer.output_plan().bytes());
        if (!bytes_value.empty()) {
            asio::write(client, asio::buffer(bytes_value));
        }
        EXPECT_TRUE(peer.consume_output(bytes_value.size()) == ruvia::websocket_output_consume_status::drained);
    };
    std::size_t messages = 0;
    std::size_t pings = 0;
    std::optional<std::uint16_t> close_code;
    std::array<char, 4096> input{};
    asio::error_code end;
    while (!close_code.has_value() && !end) {
        if (const auto event = peer.next_event()) {
            if (event->message() != nullptr) {
                ++messages;
            } else if (event->ping() != nullptr) {
                // The core queued the Pong; the peer answers promptly.
                ++pings;
                flush();
            } else if (const auto* close = event->close()) {
                close_code = close->close_code();
                flush();
            } else {
                break;
            }
            continue;
        }
        const auto read = client.read_some(asio::buffer(input), end);
        if (!end) {
            peer_input.append(std::string_view(input.data(), read));
        }
    }
    // A Ping went out during the 300 ms idle start and was answered, and
    // the server still ended the session with its own Close, not an abort.
    EXPECT_EQ(messages, std::size_t{20});
    EXPECT_TRUE(pings != 0);
    EXPECT_TRUE(close_code.has_value());
    if (close_code.has_value()) {
        EXPECT_EQ(*close_code, std::uint16_t{1000});
    }
    client.close();
    server.finish();
}

// A route that requires a subprotocol refuses an upgrade offering none of its
// own with 400 instead of switching protocols, and still selects one that is
// offered alongside unknown entries.
TEST(Ruvia, http1_websocket_required_subprotocol_refuses_offers_without_one) {
    using namespace http1_websocket_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    std::array<std::string, 3> heads{std::string{}, std::string{}, std::string{}};
    auto run = [&]() -> ruvia::task<void> {
        constexpr std::array<std::string_view, 3> offers{
            "", "Sec-WebSocket-Protocol: other\r\n", "Sec-WebSocket-Protocol: other, chat.v1\r\n"};
        for (std::size_t index = 0; index != offers.size(); ++index) {
            asio::ip::tcp::socket client(io);
            asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
            watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
            try {
                client.connect(server.endpoint());
                const std::string upgrade(std::string(
                                              "GET /http1-websocket-required-subprotocol HTTP/1.1\r\n"
                                              "Host: 127.0.0.1\r\n"
                                              "Connection: Upgrade\r\n"
                                              "Upgrade: websocket\r\n"
                                              "Sec-WebSocket-Version: 13\r\n"
                                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") +
                                          std::string(offers[index]) + "\r\n");
                const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(upgrade), std::move(handler)); });
                if (sent.error_code()) {
                    throw std::system_error(sent.error_code());
                }
                char byte{};
                while (!heads[index].ends_with("\r\n\r\n")) {
                    const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                    if (read.error_code()) {
                        throw std::system_error(read.error_code());
                    }
                    heads[index].push_back(byte);
                }
            } catch (...) {
                failure = std::current_exception();
            }
            ruvia::close_socket(client);
            (void)watchdog_value.cancel();
            if (failure) {
                break;
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(heads[0].starts_with("HTTP/1.1 400 "));
    EXPECT_TRUE(heads[1].starts_with("HTTP/1.1 400 "));
    EXPECT_TRUE(heads[0].find("Sec-WebSocket-Accept") == std::string::npos);
    EXPECT_TRUE(heads[2].starts_with("HTTP/1.1 101 "));
    EXPECT_TRUE(heads[2].find("\r\nSec-WebSocket-Protocol: chat.v1\r\n") != std::string::npos);
}

// RFC 9110 section 7.8: a server that receives Upgrade together with
// Expect: 100-continue MUST send 100 (Continue) before 101 (Switching Protocols).
TEST(Ruvia, http1_websocket_upgrade_with_continue_expectation_sends_100_before_101) {
    using namespace http1_websocket_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    std::array<std::string, 2> heads{std::string{}, std::string{}};
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket client(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            client.connect(server.endpoint());
            constexpr std::string_view upgrade =
                "GET /http1-websocket-required-subprotocol HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Expect: 100-continue\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                "Sec-WebSocket-Protocol: chat.v1\r\n\r\n";
            const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(upgrade), std::move(handler)); });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
            for (auto& head : heads) {
                char byte{};
                while (!head.ends_with("\r\n\r\n")) {
                    const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                    if (read.error_code()) {
                        throw std::system_error(read.error_code());
                    }
                    head.push_back(byte);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(client);
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_EQ(heads[0], "HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(heads[1].starts_with("HTTP/1.1 101 "));
}

// A websocket handler failure is reported even when it carries a code an
// ending inbound connection also produces, while a peer that resets instead of
// answering the resulting 1011 Close ends the connection normally: exactly one
// failure is counted.
TEST(Ruvia, http1_websocket_handler_failure_is_reported_and_a_close_reset_is_not) {
    using namespace http1_websocket_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto failures_before = ruvia::app().stats().connection_failures_;
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        constexpr std::string_view upgrade =
            "GET /http1-websocket-upstream-reset HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
        asio::write(client, asio::buffer(upgrade));
        std::string head{};
        char byte{};
        while (!head.ends_with("\r\n\r\n")) {
            asio::read(client, asio::buffer(&byte, 1));
            head.push_back(byte);
        }
        EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
        // One masked text frame (zero masking key).
        constexpr std::array<char, 8> frame{'\x81', '\x82', '\0', '\0', '\0', '\0', 'g', 'o'};
        asio::write(client, asio::buffer(frame));
        // The server's Close: code 1011 followed by its reason.
        std::array<char, 4> close_head{};
        asio::read(client, asio::buffer(close_head));
        EXPECT_EQ(close_head[0], '\x88');
        EXPECT_EQ(close_head[2], '\x03');
        EXPECT_EQ(close_head[3], '\xf3');
        // SO_LINGER 0 turns close() into a reset instead of a Close answer.
        client.set_option(asio::socket_base::linger(true, 0));
        client.close();
    }
    server.finish();
    EXPECT_EQ(ruvia::app().stats().connection_failures_, failures_before + 1);
}

// Server permessage-deflate echoes messages of every size. Without context
// takeover the connection borrows the worker's shared codecs, so a repeated
// message compresses to the same frame; with opt-in takeover each direction
// keeps its own dictionary, so the repeat shrinks.
TEST(Ruvia, http1_websocket_deflate_echoes_with_shared_and_takeover_codecs) {
    using namespace http1_websocket_test;
    using http1_websocket_test::noise_message;
    using http1_websocket_test::sample_message;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const bool takeover : {false, true}) {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string upgrade(std::string("GET ") +
                                  (takeover ? "/http1-websocket-deflate-takeover" : "/http1-websocket-deflate-echo") +
                                  " HTTP/1.1\r\n"
                                  "Host: 127.0.0.1\r\n"
                                  "Connection: Upgrade\r\n"
                                  "Upgrade: websocket\r\n"
                                  "Sec-WebSocket-Version: 13\r\n"
                                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                  "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n");
        asio::write(client, asio::buffer(upgrade));
        std::string head{};
        char byte{};
        while (!head.ends_with("\r\n\r\n")) {
            asio::read(client, asio::buffer(&byte, 1));
            head.push_back(byte);
        }
        EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
        EXPECT_TRUE(head.find("permessage-deflate") != std::string::npos);
        EXPECT_TRUE((head.find("server_no_context_takeover") == std::string::npos) == takeover);
        ruvia::http_io_buffer peer_input;
        ruvia::websocket_connection peer(peer_input, {.message_limit_ = ruvia::protocol_byte_limit::unlimited(),
                                                         .compression_ = {.enabled_ = true,
                                                             .server_no_context_takeover_ = !takeover,
                                                             .client_no_context_takeover_ = !takeover},
                                                         .role_ = ruvia::websocket_connection_role::client,
                                                         .mask_key_generator_ = &http1_websocket_test::zero_mask});
        const auto flush = [&] {
            const std::string bytes_value(peer.output_plan().bytes());
            asio::write(client, asio::buffer(bytes_value));
            EXPECT_TRUE(peer.consume_output(bytes_value.size()) == ruvia::websocket_output_consume_status::drained);
        };
        // Returns the wire bytes of the echoed frame.
        const auto echo = [&](const std::string& payload) {
            EXPECT_TRUE(peer.submit_frame(ruvia::websocket_opcode::binary, payload) == ruvia::websocket_frame_submit_status::accepted);
            flush();
            std::size_t wire_bytes = 0;
            std::array<char, 16384> input{};
            for (;;) {
                if (const auto event = peer.next_event()) {
                    if (const auto* message = event->message()) {
                        EXPECT_TRUE(message->payload() == payload);
                        return wire_bytes;
                    }
                    // A heartbeat Ping queues its Pong.
                    flush();
                    continue;
                }
                const auto read = client.read_some(asio::buffer(input));
                wire_bytes += read;
                peer_input.append(std::string_view(input.data(), read));
            }
        };
        // Small, mid-size, past 64 KiB, incompressible, then small again.
        for (const auto& payload : {sample_message(32, 1), sample_message(4096, 2), sample_message(70'000, 3),
                 noise_message(70'000), sample_message(32, 4)}) {
            static_cast<void>(echo(payload));
        }
        const auto repeated = sample_message(2000, 5);
        const auto first = echo(repeated);
        const auto second = echo(repeated);
        EXPECT_TRUE(takeover ? second < first : second == first);
        client.shutdown(asio::ip::tcp::socket::shutdown_send);
        std::array<char, 256> rest{};
        asio::error_code end;
        while (!end) {
            static_cast<void>(client.read_some(asio::buffer(rest), end));
        }
    }
    server.finish();
}

TEST(Ruvia, websocket_route_receive_limit_rejects_before_payload) {
    using namespace http1_websocket_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    constexpr std::string_view upgrade =
        "GET /http1-websocket-small-message HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    asio::write(client, asio::buffer(upgrade));
    std::string head;
    char byte;
    while (!head.ends_with("\r\n\r\n")) {
        asio::read(client, asio::buffer(&byte, 1));
        head.push_back(byte);
    }
    EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
    const std::string declared("\x82\xfe\x01\x01\0\0\0\0", 8);
    asio::write(client, asio::buffer(declared));
    std::array<unsigned char, 4> close{};
    asio::read(client, asio::buffer(close));
    EXPECT_EQ(close[0], 0x88);
    EXPECT_EQ(close[1], 2);
    EXPECT_EQ((static_cast<unsigned>(close[2]) << 8) | close[3], 1009U);
    client.close();
    server.finish();
}

namespace http1_websocket_suite_test {
struct observation {
    std::mutex mutex_;
    std::string bytes_{};
    std::atomic<bool> finish_first_{};
    std::atomic<bool> ended_{};
    std::atomic<bool> retained_stable_{true};
};
const auto observed = std::make_shared<observation>();
[[maybe_unused]] const bool observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<observation>>([] { return observed; });
    return true;
}();

class http1_tunnel_routes final {
    RUVIA_ROUTES_BEGIN(http1_tunnel_routes)
    RUVIA_CONNECT("http1-target.test:443", tunnel);
    RUVIA_ROUTES_END

    ruvia::task<void> tunnel(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<observation>>();
        auto& tunnel_value = context_value.tunnel();
        if (observed_value.finish_first_) {
            co_await tunnel_value.finish();
        }
        std::optional<std::string> retained;
        while (auto bytes = co_await tunnel_value.read()) {
            {
                std::lock_guard lock(observed_value.mutex_);
                observed_value.bytes_.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            }
            if (!retained) {
                retained.emplace(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            }
            if (!observed_value.finish_first_) {
                auto output = tunnel_value.write(*bytes);
                co_await std::move(output);
            }
            observed_value.retained_stable_ = observed_value.retained_stable_ && retained->find_first_not_of('t') == std::string_view::npos;
        }
        observed_value.ended_ = true;
        co_await tunnel_value.finish();
    }
};

void exercise_connect(bool finish_first) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    {
        std::lock_guard lock(observed->mutex_);
        observed->bytes_.clear();
    }
    observed->finish_first_ = finish_first;
    observed->ended_ = false;
    observed->retained_stable_ = true;
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const std::string initial_value(5007, 't');
    const std::string additional(100003, 't');
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            const auto send = [&](std::string_view bytes_value) -> ruvia::task<void> {
                const auto result_value = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(bytes_value), std::move(handler)); });
                if (result_value.error_code()) {
                    throw std::system_error(result_value.error_code());
                }
            };
            const std::string request(std::string("CONNECT HTTP1-TARGET.TEST:0443 HTTP/1.1\r\nHost: HTTP1-TARGET.TEST:0443\r\n\r\n") + initial_value);
            co_await send(request);
            std::string head{};
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
            EXPECT_TRUE(head.find("Content-Length:") == std::string::npos);
            EXPECT_TRUE(head.find("Transfer-Encoding:") == std::string::npos);
            std::array<char, 4096> input{};
            std::string echoed{};
            if (!finish_first) {
                co_await send(additional);
                client.shutdown(asio::ip::tcp::socket::shutdown_send);
            }
            for (;;) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code() == asio::error::eof) {
                    break;
                }
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                echoed.append(input.data(), read.result());
            }
            EXPECT_TRUE(echoed == (finish_first ? std::string{} : std::string(initial_value) + additional));
            if (finish_first) {
                co_await send(additional);
                client.shutdown(asio::ip::tcp::socket::shutdown_send);
            }
        } catch (...) {
            failure = std::current_exception();
            ruvia::close_socket(client);
        }
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    server.finish();
    ruvia::close_socket(client);
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(observed->ended_ && observed->retained_stable_);
    {
        std::lock_guard lock(observed->mutex_);
        EXPECT_TRUE(observed->bytes_ == std::string(initial_value) + additional);
    }
}

class http1_interim_routes final {
    RUVIA_ROUTES_BEGIN(http1_interim_routes)
    RUVIA_GET("/http1-early-hints", hints);
    RUVIA_GET("/http1-cold-hints", cold_hints);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> hints(ruvia::context& context_value) {
        std::string link_value("</asset>; rel=preload");
        const std::array<ruvia::http_header_view, 1> links{{{"link", link_value}}};
        auto first = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, links));
        auto second = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::processing));
        link_value.assign("changed before starting the operation");
        if (context_value.req().protocol_version() == ruvia::http_protocol_version::http11) {
            ruvia::task_scope output(context_value.worker());
            const auto send_first = [&]() -> ruvia::task<void> { co_await std::move(first); };
            output.spawn(send_first());
            unsigned rejected = 0;
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                try {
                    co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::processing));
                } catch (const std::logic_error&) {
                    ++rejected;
                }
            }
            co_await output.join();
            if (rejected != 2) {
                throw std::logic_error("interim response output accepted concurrent operations");
            }
        } else {
            co_await std::move(first);
        }
        co_await std::move(second);
        co_return context_value.text("final");
    }

    ruvia::task<void> cold_hints(ruvia::context& context_value) {
        auto cold = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        auto another = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::processing));
        if (co_await context_value.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await context_value.stream().write("cold-final");
        bool rejected_after_final = false;
        try {
            co_await std::move(cold);
        } catch (const std::logic_error&) {
            rejected_after_final = true;
        }
        if (!rejected_after_final) {
            throw std::logic_error("interim response started after final response commit");
        }
    }
};

// Sends one closing request and returns every byte the server wrote until EOF.
std::string exchange_closing_request(std::string_view request) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::string received{};
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(request), std::move(handler)); });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            std::array<char, 4096> input{};
            for (;;) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code() == asio::error::eof) {
                    break;
                }
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                received.append(input.data(), read.result());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    ruvia::close_socket(client);
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return received;
}
}  // namespace http1_websocket_suite_test

TEST(Ruvia, http1_connect_routes_transfer_buffered_bytes_and_keep_receive_direction_after_send_fin) {
    using namespace http1_websocket_suite_test;
    exercise_connect(false);
    exercise_connect(true);
}

TEST(Ruvia, http1_interim_responses_reach_http11_clients_but_never_http10_clients) {
    using namespace http1_websocket_suite_test;
    const auto http11 = exchange_closing_request("GET /http1-early-hints HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(http11.starts_with("HTTP/1.1 103 "));
    EXPECT_TRUE(http11.find("link: </asset>; rel=preload\r\n") != std::string::npos);
    EXPECT_TRUE(http11.find("\r\n\r\nHTTP/1.1 102 ") != std::string::npos);
    EXPECT_TRUE(http11.find("\r\n\r\nHTTP/1.1 200 ") != std::string::npos);
    EXPECT_TRUE(http11.ends_with("final"));

    // RFC 9110 15.2: an HTTP/1.0 client would take the 1xx head as the final
    // response, so the server omits it and sends only the final response.
    const auto http10 = exchange_closing_request("GET /http1-early-hints HTTP/1.0\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(http10.find(" 103 ") == std::string::npos);
    EXPECT_TRUE(http10.find(" 102 ") == std::string::npos);
    EXPECT_TRUE(std::string_view(http10).substr(0, http10.find("\r\n")).find(" 200 ") != std::string::npos);
    EXPECT_TRUE(http10.ends_with("final"));
}

TEST(Ruvia, http1_unstarted_interim_responses_do_not_block_the_final_response) {
    using namespace http1_websocket_suite_test;
    const auto response = exchange_closing_request("GET /http1-cold-hints HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(response.find(" 103 ") == std::string::npos);
    EXPECT_TRUE(response.find(" 102 ") == std::string::npos);
    EXPECT_TRUE(response.find("cold-final") != std::string::npos);
}

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
            auto first_write = original.send(std::string("ready"));
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
    std::string head_{};
    std::string input_{};
};

[[nodiscard]] upgraded_connection open_upgrade(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target, std::string_view fields) {
    upgraded_connection connection{asio::ip::tcp::socket(io)};
    connection.socket_.connect(endpoint);
    std::string request{"GET "};
    request.append(target).append(" HTTP/1.1\r\nHost: localhost\r\n").append(fields).append("\r\n");
    asio::write(connection.socket_, asio::buffer(request));
    const auto head_size = asio::read_until(connection.socket_, asio::dynamic_buffer(connection.input_), "\r\n\r\n");
    connection.head_.assign(connection.input_.data(), head_size);
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
    connection.head_.append(connection.input_);
    return std::move(connection.head_);
}

// A masked client WebSocket frame (RFC 6455 section 5.3) with a short payload.
[[nodiscard]] std::string client_frame(std::uint8_t opcode, std::string_view payload) {
    constexpr std::array<char, 4> mask{'\x12', '\x34', '\x56', '\x78'};
    std::string frame{};
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
    std::string payload_{};
};

[[nodiscard]] server_frame read_frame(upgraded_connection& connection) {
    while (connection.input_.size() < 2 ||
           connection.input_.size() < 2 + (static_cast<unsigned char>(connection.input_[1]) & 0x7f)) {
        std::array<char, 1024> input{};
        connection.input_.append(input.data(), connection.socket_.read_some(asio::buffer(input)));
    }
    const auto length = static_cast<std::size_t>(static_cast<unsigned char>(connection.input_[1]) & 0x7f);
    server_frame frame{static_cast<std::uint8_t>(connection.input_[0] & 0x0f), std::string(std::string_view(connection.input_).substr(2, length))};
    connection.input_.erase(0, 2 + length);
    return frame;
}

[[nodiscard]] bool eventually(const std::atomic<bool>& flag) {
    ruvia::test::wait_until([&flag] { return flag.load(); }, std::chrono::seconds(5));
    return true;
}
}  // namespace udp_tunnel_test

using udp_tunnel_test::udp_observation;

TEST(Ruvia, http1_udp_tunnel_capsules_echo_preserve_siblings_and_owned_lifetime) {
    using namespace udp_tunnel_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
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
        EXPECT_TRUE(tunnel.head_.starts_with("HTTP/1.1 101 "));
        EXPECT_TRUE(tunnel.head_.find("\r\ncapsule-protocol: ?1\r\n") != std::string::npos ||
                    tunnel.head_.find("\r\nCapsule-Protocol: ?1\r\n") != std::string::npos);
        for (const auto expected : {std::string_view("ready"), std::string_view("second-ready")}) {
            const auto ready = read_datagram(tunnel);
            EXPECT_TRUE(ready.has_value() && *ready == expected);
        }
        std::string query{};
        append_capsule(query, 0, "dns-query");
        asio::write(tunnel.socket_, asio::buffer(query));
        EXPECT_TRUE(get_response(io, server.endpoint(), "/udp-tunnel-sibling").ends_with("\r\n\r\nsibling"));
        tunnel.socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
        const auto echoed = read_datagram(tunnel);
        EXPECT_TRUE(echoed.has_value() && *echoed == "dns-query");
        EXPECT_TRUE(!read_datagram(tunnel));
    }
    EXPECT_TRUE(eventually(observation->eof_));
    EXPECT_TRUE(observation->capsule_bound_rejected_);
    EXPECT_TRUE(observation->read_overlap_rejected_);
    EXPECT_TRUE(observation->write_overlap_rejected_);
    EXPECT_TRUE(!observation->failed_);
    EXPECT_TRUE(get_response(io, server.endpoint(), "/udp-tunnel-sibling").ends_with("\r\n\r\nsibling"));
    {
        auto delayed = open_udp_tunnel(io, server.endpoint(), "/.well-known/masque/udp/cancel.test/53/");
        EXPECT_TRUE(delayed.head_.starts_with("HTTP/1.1 101 "));
        EXPECT_TRUE(!read_datagram(delayed));
    }
    server.finish();
}

// A connect-udp upgrade target is an origin path like any other: dot segments
// are refused before any tunnel route can capture them, and a path with no
// connect-udp route serves its GET route with the Upgrade ignored (RFC 9110 §7.8).
TEST(Ruvia, http1_udp_tunnel_upgrade_routes_only_normalized_tunnel_paths) {
    using namespace udp_tunnel_test;
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
        std::string response{};
        std::array<char, 1024> input{};
        std::error_code ec;
        while (!ec) {
            const auto read = client.read_some(asio::buffer(input), ec);
            response.append(input.data(), read);
        }
        return response;
    };
    EXPECT_TRUE(exchange("/udp-tunnel/../53").starts_with("HTTP/1.1 404 "));
    EXPECT_TRUE(exchange("/udp-tunnel/%2E%2E/53").starts_with("HTTP/1.1 404 "));
    EXPECT_TRUE(!udp_observation->dynamic_invoked_);
    const auto sibling = exchange("/udp-tunnel-sibling");
    EXPECT_TRUE(sibling.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(sibling.ends_with("\r\n\r\nsibling"));
    EXPECT_TRUE(exchange("/udp-tunnel/target.test/53").starts_with("HTTP/1.1 101 "));
    EXPECT_TRUE(udp_observation->dynamic_invoked_);
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        constexpr std::string_view h10_request =
            "GET /udp-tunnel/target.test/53 HTTP/1.0\r\nHost: localhost\r\n"
            "Connection: Upgrade, close\r\nUpgrade: connect-udp\r\nCapsule-Protocol: ?1\r\n\r\n";
        asio::write(client, asio::buffer(h10_request));
        std::string response{};
        std::array<char, 1024> input{};
        std::error_code ec;
        while (!ec) {
            const auto read = client.read_some(asio::buffer(input), ec);
            response.append(input.data(), read);
        }
        EXPECT_TRUE(response.starts_with("HTTP/1.1 404 "));
    }
    server.finish();
}

// RFC 9110 section 7.8: Upgrade together with Expect: 100-continue gets a
// 100 (Continue) before the 101 (Switching Protocols), for a CONNECT-UDP
// upgrade exactly as for a WebSocket one, even though no content follows.
TEST(Ruvia, http1_udp_tunnel_upgrade_with_continue_expectation_sends_100_before_101) {
    using namespace udp_tunnel_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    constexpr std::string_view request =
        "GET /udp-tunnel/target.test/53 HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade, close\r\n"
        "Upgrade: connect-udp\r\nCapsule-Protocol: ?1\r\nExpect: 100-continue\r\n\r\n";
    asio::write(client, asio::buffer(request));
    std::string response{};
    std::array<char, 1024> input{};
    std::error_code ec;
    while (!ec) {
        const auto read = client.read_some(asio::buffer(input), ec);
        response.append(input.data(), read);
    }
    EXPECT_TRUE(response.starts_with("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 101 "));
    client.close();
    server.finish();
}

TEST(Ruvia, server_capsule_and_udp_results_outlive_unread_input_and_connections) {
    using namespace udp_tunnel_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const bool capsule : {true, false}) {
        {
            auto tunnel = open_udp_tunnel(io, server.endpoint(),
                capsule ? "/.well-known/masque/udp/retained-capsule.test/53/"
                        : "/.well-known/masque/udp/retained-datagram.test/53/");
            EXPECT_TRUE(tunnel.head_.starts_with("HTTP/1.1 101 "));
            // One transport write includes a second complete capsule. The
            // handler reads only the first, never advancing to EOF.
            std::string batch{};
            for (const char fill : {'r', 's'}) {
                append_capsule(batch, capsule ? 42 : 0, std::string(1024, fill));
            }
            asio::write(tunnel.socket_, asio::buffer(batch));
            tunnel.socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
            EXPECT_TRUE(!read_datagram(tunnel));
        }
        // Wait for actual connection retirement, not just the peer FIN.
        // Worker state must keep the result alive through this barrier.
        server.finish();
        for (const auto& expected : {std::string(1024, 'r'), std::string("empty")}) {
            EXPECT_TRUE(get_response(io, server.endpoint(), "/udp-tunnel-retained").ends_with(std::string("\r\n\r\n") + expected));
        }
        server.finish();
    }
}

TEST(Ruvia, server_websocket_exposes_negotiated_subprotocol_and_peer_close) {
    using namespace udp_tunnel_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    udp_observation->ws_subprotocol_matched_ = false;
    udp_observation->ws_peer_close_matched_ = false;
    {
        auto ws = open_upgrade(io, server.endpoint(), "/udp-tunnel-ws",
            "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: chat.v1, chat.v2\r\n");
        EXPECT_TRUE(ws.head_.starts_with("HTTP/1.1 101 "));
        EXPECT_TRUE(ws.head_.find(": chat.v2\r\n") != std::string::npos);
        const auto text = client_frame(0x1, "ping-echo");
        asio::write(ws.socket_, asio::buffer(text));
        auto frame = read_frame(ws);
        if (frame.opcode_ == 0xA) {
            frame = read_frame(ws);
        }
        EXPECT_TRUE(frame.opcode_ == 0x1 && frame.payload_ == "ping-echo");
        const auto close_frame = client_frame(0x8, std::string("\x0f\xa1", 2) + "client-done");
        asio::write(ws.socket_, asio::buffer(close_frame));
        const auto close_reply = read_frame(ws);
        EXPECT_TRUE(close_reply.opcode_ == 0x8);
    }
    EXPECT_TRUE(eventually(udp_observation->ws_subprotocol_matched_));
    EXPECT_TRUE(eventually(udp_observation->ws_peer_close_matched_));
    server.finish();
}

#if defined(RUVIA_ENABLE_HTTP2)

namespace http1_websocket_suite_test {
struct connect_observation {
    std::mutex mutex_;
    std::string received_{};
    std::atomic<bool> saw_eof_{};
    std::atomic<bool> stable_{true};
    std::atomic<bool> return_early_{};
    std::atomic<bool> halfclose_capsules_{};
    std::atomic<int> seen_urgency_{-1};
    std::atomic<bool> hold_push_{};
};
const auto observation = std::make_shared<connect_observation>();
[[maybe_unused]] const bool connect_observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<connect_observation>>([] { return observation; });
    return true;
}();

// Sleep before consuming so peer input (DATA filling the receive window, or a
// request half-close) is fully delivered before the server continues.
ruvia::task<void> hold_off(ruvia::context& context_value) {
    if (co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(200)) != ruvia::timer_sleep_result::elapsed) {
        throw std::runtime_error("test hold-off sleep interrupted");
    }
}

class hold_off_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& context_value, ruvia::next& next_value) {
        co_await hold_off(context_value);
        co_await next_value();
    }
};

// A rejected expectation is answered before the request content is read. The
// scoped error handler suspends while the peer keeps sending DATA, then reads
// the request body it was handed.
ruvia::task<ruvia::http_response> answer_rejected_expectation(
    ruvia::context& context_value, ruvia::http_error_info error) {
    co_await hold_off(context_value);
    const auto body = co_await context_value.req().text();
    context_value.draft().status(error.status());
    std::string text;
    text.append("body=");
    text.append(std::to_string(body.size()));
    co_return context_value.text(std::move(text));
}
[[maybe_unused]] const bool expectation_handler_registered = [] {
    ruvia::app().on_error({.prefix_ = "/expect-rejected", .handler_ = &answer_rejected_expectation});
    return true;
}();

class connect_routes final {
public:
    RUVIA_ROUTES_BEGIN(connect_routes)
    const ruvia::http_tunnel_route_config options{
        .peer_transport_fin_timeout_ = std::chrono::milliseconds(40)};
    RUVIA_CONNECT("target.test:443", echo, options);
    RUVIA_CONNECT_PROTOCOL("test-tunnel", "/udp/:host/:port", echo);
    RUVIA_CONNECT_PROTOCOL("slow-tunnel", "/slow-tunnel", slow_tunnel);
    const ruvia::http_tunnel_route_config capsule_options{.datagrams_ = true};
    RUVIA_CONNECT_PROTOCOL("capsule-tunnel", "/capsules", capsules, capsule_options);
    RUVIA_POST("/slow-upload", slow_upload, ruvia::stream_body);
    RUVIA_GET("/after", after);
    RUVIA_POST("/expect-rejected", expect_rejected);
    RUVIA_GET_WS("/late-fin-websocket", late_fin_websocket, hold_off_middleware);
    RUVIA_GET("/prio-idle", prio_idle);
    RUVIA_GET("/push-parent-cancel", push_parent_cancel);
    RUVIA_GET("/pushed-buffered-cancel", pushed_buffered_cancel);
    RUVIA_ROUTES_END
    ruvia::task<void> echo(ruvia::context& context_value);
    ruvia::task<void> slow_tunnel(ruvia::context& context_value);
    ruvia::task<void> capsules(ruvia::context& context_value);
    ruvia::task<ruvia::http_response> slow_upload(ruvia::context& context_value);
    ruvia::task<ruvia::http_response> after(ruvia::context& context_value) {
        co_return context_value.text("sibling");
    }
    ruvia::task<ruvia::http_response> expect_rejected(ruvia::context& context_value) {
        co_return context_value.text("unreachable");
    }
    ruvia::task<void> late_fin_websocket(ruvia::context&) {
        co_return;
    }
    ruvia::task<ruvia::http_response> prio_idle(ruvia::context& context_value) {
        context_value.worker_state<std::shared_ptr<connect_observation>>()->seen_urgency_ =
            static_cast<int>(context_value.req().priority().urgency_.value());
        co_return context_value.text("ok");
    }
    ruvia::task<ruvia::http_response> push_parent_cancel(ruvia::context& context_value) {
        (void)context_value.push({.method_ = "GET", .scheme_ = "http", .authority_ = "localhost", .path_ = "/pushed-buffered-cancel"});
        co_return context_value.text("parent");
    }
    ruvia::task<ruvia::http_response> pushed_buffered_cancel(ruvia::context& context_value) {
        auto& obs = *context_value.worker_state<std::shared_ptr<connect_observation>>();
        while (obs.hold_push_) {
            (void)co_await ruvia::sleep_for(
                context_value.worker(), std::chrono::milliseconds(1), context_value.stop_token());
        }
        co_return context_value.text("pushed-body");
    }
};
ruvia::task<void> connect_routes::echo(ruvia::context& context_value) {
    auto& observation_value = *context_value.worker_state<std::shared_ptr<connect_observation>>();
    if (observation_value.return_early_) {
        co_return;
    }
    std::optional<std::string> retained;
    auto& tunnel = context_value.tunnel();
    {
        auto cold = tunnel.read();
    }
    while (auto chunk = co_await tunnel.read()) {
        {
            std::lock_guard lock(observation_value.mutex_);
            observation_value.received_.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        }
        if (!retained) {
            retained.emplace(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        }
        auto output = tunnel.write(*chunk);
        co_await std::move(output);
        observation_value.stable_ = observation_value.stable_ && retained->find_first_not_of('t') == std::string_view::npos;
    }
    observation_value.saw_eof_ = true;
    co_await tunnel.finish();
}
ruvia::task<void> connect_routes::slow_tunnel(ruvia::context& context_value) {
    co_await hold_off(context_value);
    auto& tunnel = context_value.tunnel();
    std::size_t total = 0;
    while (auto chunk = co_await tunnel.read()) {
        total += chunk->size();
    }
    const auto total_text = std::string(std::to_string(total));
    co_await tunnel.write(std::string_view(total_text));
    co_await tunnel.finish();
}
// Reads capsules until the malformed input aborts the stream.
ruvia::task<void> connect_routes::capsules(ruvia::context& context_value) {
    auto& obs = *context_value.worker_state<std::shared_ptr<connect_observation>>();
    auto stream = context_value.tunnel().capsules();
    if (obs.halfclose_capsules_) {
        co_await stream.finish();
    }
    try {
        while (co_await stream.read()) {
        }
    } catch (const std::runtime_error&) {
    }
}
ruvia::task<ruvia::http_response> connect_routes::slow_upload(ruvia::context& context_value) {
    co_await hold_off(context_value);
    std::size_t total = 0;
    auto& reader = context_value.req().body_reader();
    while (auto chunk = co_await reader.text()) {
        total += chunk->size();
    }
    std::string body;
    body = std::string(std::to_string(total));
    co_return context_value.text(std::move(body));
}
ruvia::task<void> flush_peer(asio::ip::tcp::socket& socket, ruvia::http2_connection& connection) {
    std::string bytes;
    while (connection.wants_write()) {
        bytes.clear();
        (void)connection.take_output_batch(16384, bytes);
        const auto completion = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(socket, asio::buffer(bytes), std::move(handler)); });
        if (completion.error_code()) {
            throw std::system_error(completion.error_code());
        }
    }
}
}  // namespace http1_websocket_suite_test
TEST(Ruvia, http2_connect_routes_echo_large_duplex_streams_and_leave_sibling_requests_available) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        auto& observation_value = *observation;
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            auto peer = ruvia::http2_connection::client();
            std::array<char, 16384> input{};
            std::string received_wire{};
            auto pump = [&]() -> ruvia::task<void> {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                received_wire.append(input.data(), read.result());
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("CONNECT peer protocol failure");
                }
            };
            co_await flush_peer(socket, peer);
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("CONNECT SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            for (unsigned round = 0; round != 3; ++round) {
                observation_value.return_early_ = round == 2;
                const auto request = round != 1 ? peer.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "TARGET.TEST:443"})
                                                : peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "test-tunnel", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/udp/target.test/443"});
                const auto* id_ptr = std::get_if<std::uint32_t>(&request);
                if (id_ptr == nullptr) {
                    throw std::runtime_error("CONNECT request submission rejected");
                }
                const auto id = *id_ptr;
                co_await flush_peer(socket, peer);
                bool established = false;
                while (!established) {
                    co_await pump();
                    while (auto event = peer.next_event()) {
                        if (const auto* head = event->response_head()) {
                            EXPECT_TRUE(head->stream_id() == id);
                            EXPECT_TRUE(head->head().status() == ruvia::http_status::ok);
                            EXPECT_TRUE(std::ranges::none_of(head->head().headers(), [](const auto& header_value) { return header_value.name() == "content-length"; }));
                            established = true;
                        }
                        if (event->stream_closed() || peer.connection_error()) {
                            throw std::runtime_error("CONNECT handshake reset");
                        }
                    }
                    co_await flush_peer(socket, peer);
                }
                const std::string payload_value(100003, 't');
                if (round == 2) {
                    bool reset = false;
                    while (!reset) {
                        co_await pump();
                        while (auto event = peer.next_event()) {
                            if (event->stream_closed()) {
                                reset = true;
                            }
                            EXPECT_TRUE(!peer.connection_error());
                        }
                        co_await flush_peer(socket, peer);
                    }
                    bool sent_fin = false;
                    for (std::size_t offset = 0; offset + ruvia::http2_frame_header_bytes <= received_wire.size();) {
                        const auto header_value = ruvia::parse_http2_frame_header(std::span<const char>(received_wire.data() + offset, received_wire.size() - offset));
                        if (!header_value || header_value->length_ > received_wire.size() - offset - ruvia::http2_frame_header_bytes) {
                            break;
                        }
                        sent_fin = sent_fin || (header_value->stream_id_ == id && header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) && (header_value->flags_ & 1U) != 0);
                        offset += ruvia::http2_frame_header_bytes + header_value->length_;
                    }
                    EXPECT_TRUE(sent_fin);
                    continue;
                }
                const auto submitted = peer.submit_data(id, payload_value, ruvia::http2_end_stream::end_stream, payload_value.size());
                EXPECT_TRUE(submitted.status_ == ruvia::http2_data_submit_status::complete || submitted.status_ == ruvia::http2_data_submit_status::backpressured);
                auto consumed = submitted.consumed_bytes_;
                co_await flush_peer(socket, peer);
                std::string echoed{};
                bool ended = false;
                while (!ended) {
                    co_await pump();
                    while (auto event = peer.next_event()) {
                        if (auto* data = event->tunnel_data()) {
                            EXPECT_TRUE(data->stream_id() == id);
                            echoed.append(data->bytes());
                            (void)data->take_credit();
                        }
                        if (const auto* end = event->tunnel_end()) {
                            EXPECT_TRUE(end->stream_id() == id);
                            ended = true;
                        }
                        if (event->stream_closed() || peer.connection_error()) {
                            throw std::runtime_error("CONNECT stream reset");
                        }
                    }
                    if (consumed != payload_value.size()) {
                        const auto suffix = std::string_view(payload_value).substr(consumed);
                        const auto retried = peer.submit_data(id, suffix, ruvia::http2_end_stream::end_stream, suffix.size());
                        EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete || retried.status_ == ruvia::http2_data_submit_status::backpressured);
                        consumed += retried.consumed_bytes_;
                    }
                    co_await flush_peer(socket, peer);
                }
                EXPECT_EQ(consumed, payload_value.size());
                EXPECT_TRUE(echoed == payload_value);
                EXPECT_TRUE(observation_value.saw_eof_ && observation_value.stable_);
                {
                    std::lock_guard lock(observation_value.mutex_);
                    EXPECT_TRUE(observation_value.received_ == payload_value);
                    observation_value.received_.clear();
                }
                observation_value.saw_eof_ = false;
            }
            auto sibling = peer.submit_request_head(ruvia::http2_regular_request_head_view{.scheme_ = "http", .authority_ = "proxy.test", .target_ = "/after"});
            if (!std::holds_alternative<std::uint32_t>(sibling)) {
                throw std::runtime_error("sibling request failed");
            }
            co_await flush_peer(socket, peer);
            bool ended = false;
            std::string body{};
            while (!ended) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (auto* bytes = event->message_body_chunk()) {
                        body.append(bytes->bytes());
                    }
                    if (event->message_end()) {
                        ended = true;
                    }
                }
                co_await flush_peer(socket, peer);
            }
            EXPECT_TRUE(body == "sibling");
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
// RFC 9113 section 8.5 / RFC 9298 section 5: DATA a client sends before the 2xx
// is tunnel data. The route consumes it once the tunnel is accepted.
TEST(Ruvia, http2_connect_route_receives_data_sent_before_the_response) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        auto& observation_value = *observation;
        observation_value.return_early_ = false;
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            auto peer = ruvia::http2_connection::client();
            std::array<char, 16384> input{};
            auto pump = [&]() -> ruvia::task<void> {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("CONNECT peer protocol failure");
                }
            };
            co_await flush_peer(socket, peer);
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("CONNECT SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            const auto request = peer.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "target.test:443"});
            const auto* id_ptr = std::get_if<std::uint32_t>(&request);
            if (id_ptr == nullptr) {
                throw std::runtime_error("CONNECT request submission rejected");
            }
            const auto id = *id_ptr;
            co_await flush_peer(socket, peer);
            // The client core only sends after the 2xx; write the optimistic
            // DATA frame directly behind its HEADERS.
            const std::string early(64, 't');
            std::string raw{};
            ruvia::test::append_frame(raw, ruvia::http2_frame_type::data, 0, id, early);
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(socket, asio::buffer(raw), std::move(handler)); });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            bool established = false;
            bool finished = false;
            bool ended = false;
            std::string echoed{};
            while (!ended) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        EXPECT_TRUE(head->head().status() == ruvia::http_status::ok);
                        established = true;
                    }
                    if (auto* data = event->tunnel_data()) {
                        echoed.append(data->bytes());
                        (void)data->take_credit();
                    }
                    if (event->tunnel_end()) {
                        ended = true;
                    }
                    if (event->stream_closed() || peer.connection_error()) {
                        throw std::runtime_error("CONNECT stream reset");
                    }
                }
                if (established && !finished && echoed.size() == early.size()) {
                    finished = true;
                    const auto submitted = peer.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 0);
                    EXPECT_TRUE(submitted.status_ == ruvia::http2_data_submit_status::complete);
                    EXPECT_EQ(submitted.consumed_bytes_, std::size_t{0});
                }
                co_await flush_peer(socket, peer);
            }
            EXPECT_TRUE(echoed == early);
            EXPECT_TRUE(observation_value.saw_eof_ && observation_value.stable_);
            {
                std::lock_guard lock(observation_value.mutex_);
                EXPECT_TRUE(observation_value.received_ == early);
                observation_value.received_.clear();
            }
            observation_value.saw_eof_ = false;
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
TEST(Ruvia, http2_slow_body_and_tunnel_consumers_return_receive_window_without_peer_input) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            auto peer = ruvia::http2_connection::client();
            std::array<char, 16384> input{};
            auto pump = [&]() -> ruvia::task<void> {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("slow consumer peer protocol failure");
                }
            };
            co_await flush_peer(socket, peer);
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("slow consumer SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            // Three times the server's 1 MiB stream/connection receive window.
            const std::string payload_value(std::size_t{3} * 1024 * 1024 + 7, 'w');
            const auto expected = std::string(std::to_string(payload_value.size()));

            const auto tunnel = peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "slow-tunnel", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/slow-tunnel"});
            const auto* tunnel_id_ptr = std::get_if<std::uint32_t>(&tunnel);
            if (tunnel_id_ptr == nullptr) {
                throw std::runtime_error("slow tunnel request submission rejected");
            }
            const auto tunnel_id = *tunnel_id_ptr;
            co_await flush_peer(socket, peer);
            bool established = false;
            while (!established) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        EXPECT_TRUE(head->head().status() == ruvia::http_status::ok);
                        established = true;
                    }
                    if (event->stream_closed() || peer.connection_error()) {
                        throw std::runtime_error("slow tunnel handshake reset");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            const auto tunnel_submitted = peer.submit_data(tunnel_id, payload_value, ruvia::http2_end_stream::end_stream, payload_value.size());
            EXPECT_TRUE(tunnel_submitted.status_ == ruvia::http2_data_submit_status::complete || tunnel_submitted.status_ == ruvia::http2_data_submit_status::backpressured);
            auto tunnel_consumed = tunnel_submitted.consumed_bytes_;
            co_await flush_peer(socket, peer);
            std::string tunnel_reply{};
            bool tunnel_ended = false;
            while (!tunnel_ended) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (auto* data = event->tunnel_data()) {
                        tunnel_reply.append(data->bytes());
                        (void)data->take_credit();
                    }
                    if (event->tunnel_end()) {
                        tunnel_ended = true;
                    }
                    if (event->stream_closed() || peer.connection_error()) {
                        throw std::runtime_error("slow tunnel stream reset");
                    }
                }
                if (tunnel_consumed != payload_value.size()) {
                    const auto suffix = std::string_view(payload_value).substr(tunnel_consumed);
                    const auto retried = peer.submit_data(tunnel_id, suffix, ruvia::http2_end_stream::end_stream, suffix.size());
                    EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete || retried.status_ == ruvia::http2_data_submit_status::backpressured);
                    tunnel_consumed += retried.consumed_bytes_;
                }
                co_await flush_peer(socket, peer);
            }
            EXPECT_EQ(tunnel_consumed, payload_value.size());
            EXPECT_TRUE(tunnel_reply == expected);

            const auto upload = peer.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/slow-upload", .content_ = ruvia::http_request_content_plan::streaming()});
            const auto* upload_id_ptr = std::get_if<std::uint32_t>(&upload);
            if (upload_id_ptr == nullptr) {
                throw std::runtime_error("slow upload request submission rejected");
            }
            const auto upload_id = *upload_id_ptr;
            const auto upload_submitted = peer.submit_data(upload_id, payload_value, ruvia::http2_end_stream::end_stream, payload_value.size());
            EXPECT_TRUE(upload_submitted.status_ == ruvia::http2_data_submit_status::complete || upload_submitted.status_ == ruvia::http2_data_submit_status::backpressured);
            auto upload_consumed = upload_submitted.consumed_bytes_;
            co_await flush_peer(socket, peer);
            std::string upload_reply{};
            bool upload_ended = false;
            while (!upload_ended) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        EXPECT_TRUE(head->head().status() == ruvia::http_status::ok);
                    }
                    if (auto* bytes = event->message_body_chunk()) {
                        upload_reply.append(bytes->bytes());
                        (void)bytes->take_credit();
                    }
                    if (event->message_end()) {
                        upload_ended = true;
                    }
                    if (event->stream_closed() || peer.connection_error()) {
                        throw std::runtime_error("slow upload stream reset");
                    }
                }
                if (upload_consumed != payload_value.size()) {
                    const auto suffix = std::string_view(payload_value).substr(upload_consumed);
                    const auto retried = peer.submit_data(upload_id, suffix, ruvia::http2_end_stream::end_stream, suffix.size());
                    EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete || retried.status_ == ruvia::http2_data_submit_status::backpressured);
                    upload_consumed += retried.consumed_bytes_;
                }
                co_await flush_peer(socket, peer);
            }
            EXPECT_EQ(upload_consumed, payload_value.size());
            EXPECT_TRUE(upload_reply == expected);
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
TEST(Ruvia, http2_websocket_request_half_closed_before_handshake_receives_error_response) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            auto peer = ruvia::http2_connection::client();
            std::array<char, 16384> input{};
            auto pump = [&]() -> ruvia::task<void> {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("websocket peer protocol failure");
                }
            };
            co_await flush_peer(socket, peer);
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("websocket SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            const std::array<ruvia::http_header_view, 1> headers{ruvia::http_header_view("sec-websocket-version", "13")};
            const auto request = peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "websocket", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/late-fin-websocket", .headers_ = headers});
            const auto* id_ptr = std::get_if<std::uint32_t>(&request);
            if (id_ptr == nullptr) {
                throw std::runtime_error("websocket request submission rejected");
            }
            const auto id = *id_ptr;
            co_await flush_peer(socket, peer);
            // The client API cannot half-close before the handshake decision, so
            // write the empty DATA(END_STREAM) frame directly. The route
            // middleware delays the handshake until this half-close has arrived.
            const std::array<char, 9> half_close{0, 0, 0, 0, 1, static_cast<char>((id >> 24) & 0x7fU), static_cast<char>((id >> 16) & 0xffU), static_cast<char>((id >> 8) & 0xffU), static_cast<char>(id & 0xffU)};
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(socket, asio::buffer(half_close), std::move(handler)); });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            bool answered = false;
            while (!answered) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        EXPECT_TRUE(head->stream_id() == id);
                        EXPECT_TRUE(head->head().status() != ruvia::http_status::ok);
                        answered = true;
                    }
                    if (peer.connection_error()) {
                        throw std::runtime_error("websocket connection failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
// RFC 9110 §10.1.1: the server answers an unsupported expectation without
// processing the request content. DATA arriving while the suspended error
// handler runs is discarded, so the request it reads stays stable and empty.
TEST(Ruvia, http2_rejected_expectation_discards_request_content_received_during_error_handler) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    std::string body{};
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            // The client API generates Expect only for 100-continue, so the
            // request is framed directly: static-index and literal HPACK fields
            // without indexing, then DATA that keeps arriving after dispatch.
            const auto literal = [](std::string& block, std::string_view value) {
                block.push_back(static_cast<char>(value.size()));
                block.append(value);
            };
            std::string block("\x83\x86", 2);  // :method POST, :scheme http
            block.push_back('\x04');           // :path, literal without indexing
            literal(block, "/expect-rejected");
            block.push_back('\x01');  // :authority, literal without indexing
            literal(block, "localhost");
            block.push_back('\0');  // new-name literal without indexing
            literal(block, "expect");
            literal(block, "x-unknown");
            std::string wire(ruvia::http2_client_preface);
            ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
            ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
            const std::string chunk(16384, 'd');
            for (int index = 0; index < 3; ++index) {
                ruvia::test::append_frame(wire, ruvia::http2_frame_type::data, 0, 1, chunk);
            }
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(socket, asio::buffer(wire), std::move(handler)); });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            std::string received{};
            std::array<char, 16384> input{};
            bool ended = false;
            while (!ended) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                received.append(input.data(), read.result());
                while (!ended && received.size() >= 9) {
                    const auto size = (static_cast<std::size_t>(static_cast<unsigned char>(received[0])) << 16) |
                                      (static_cast<std::size_t>(static_cast<unsigned char>(received[1])) << 8) |
                                      static_cast<std::size_t>(static_cast<unsigned char>(received[2]));
                    if (received.size() < 9 + size) {
                        break;
                    }
                    const auto type = received[3];
                    const auto flags = static_cast<unsigned char>(received[4]);
                    const auto stream_id = (static_cast<std::uint32_t>(static_cast<unsigned char>(received[5]) & 0x7fU) << 24) |
                                           (static_cast<std::uint32_t>(static_cast<unsigned char>(received[6])) << 16) |
                                           (static_cast<std::uint32_t>(static_cast<unsigned char>(received[7])) << 8) |
                                           static_cast<std::uint32_t>(static_cast<unsigned char>(received[8]));
                    if (stream_id == 1 && type == 0x0) {
                        body.append(received, 9, size);
                        ended = (flags & 0x1U) != 0;
                    } else if ((stream_id == 1 && type == 0x3) || type == 0x7) {
                        throw std::runtime_error("rejected expectation stream was reset before its response");
                    }
                    received.erase(0, 9 + size);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_EQ(body, std::string("body=0"));
}
// RFC 9297 section 3.3 / RFC 9113 section 8.1.1: a truncated capsule makes the
// tunnel request malformed, so the server resets its stream with PROTOCOL_ERROR.
TEST(Ruvia, http2_capsule_route_resets_truncated_capsule_with_protocol_error) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    std::optional<ruvia::http2_error_code> reset_error;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            auto peer = ruvia::http2_connection::client();
            std::array<char, 16384> input{};
            auto pump = [&]() -> ruvia::task<void> {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("capsule peer protocol failure");
                }
            };
            co_await flush_peer(socket, peer);
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("capsule SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            const auto request = peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "capsule-tunnel", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/capsules"});
            const auto* id_ptr = std::get_if<std::uint32_t>(&request);
            if (id_ptr == nullptr) {
                throw std::runtime_error("capsule request submission rejected");
            }
            const auto id = *id_ptr;
            co_await flush_peer(socket, peer);
            bool sent = false;
            while (!reset_error) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        EXPECT_TRUE(head->head().status() == ruvia::http_status::ok);
                        // DATAGRAM capsule declaring 10 payload bytes, ended after 2.
                        const auto submitted = peer.submit_data(id, std::string_view("\x00\x0a"
                                                                                     "ab",
                                                                        4),
                            ruvia::http2_end_stream::end_stream, 65535);
                        EXPECT_TRUE(submitted.status_ == ruvia::http2_data_submit_status::complete);
                        EXPECT_EQ(submitted.consumed_bytes_, std::size_t{4});
                        sent = true;
                    }
                    if (const auto* closed = event->stream_closed()) {
                        EXPECT_TRUE(closed->stream_id() == id);
                        reset_error = closed->error();
                    }
                    EXPECT_TRUE(!peer.connection_error());
                }
                co_await flush_peer(socket, peer);
            }
            EXPECT_TRUE(sent);
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(reset_error == ruvia::http2_error_code::protocol_error);
}

// WST-01 (server): When the server half-closes its capsule stream first and the
// client subsequently sends a truncated capsule on DATA(END_STREAM), the server
// rejects the capsule. Both END_STREAM flags closed the stream, so no
// RST_STREAM follows (RFC 9113 section 5.1 forbids frames on a closed stream)
// and the connection keeps serving.
TEST(Ruvia, http2_capsule_route_closed_stream_rejects_truncated_capsule_without_reset) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    observation->halfclose_capsules_ = true;
    ruvia::test::http_server_fixture server(io);
    std::string raw_from_server{};
    // The frames read from the server, as (type, flags, payload).
    const auto server_frames = [&] {
        std::vector<std::tuple<std::uint8_t, std::uint8_t, std::string>> frames{};
        for (std::size_t off = 0; off + ruvia::http2_frame_header_bytes <= raw_from_server.size();) {
            const auto hdr = ruvia::parse_http2_frame_header(
                std::span<const char>(raw_from_server.data() + off, ruvia::http2_frame_header_bytes));
            if (!hdr || off + ruvia::http2_frame_header_bytes + hdr->length_ > raw_from_server.size()) {
                break;
            }
            frames.emplace_back(hdr->type_, hdr->flags_,
                std::string_view(raw_from_server).substr(off + ruvia::http2_frame_header_bytes, hdr->length_));
            off += ruvia::http2_frame_header_bytes + hdr->length_;
        }
        return frames;
    };
    const auto ping_acknowledged = [&] {
        return std::ranges::any_of(server_frames(), [](const auto& frame) {
            return std::get<0>(frame) == static_cast<std::uint8_t>(ruvia::http2_frame_type::ping) &&
                   (std::get<1>(frame) & 0x1) != 0 && std::get<2>(frame) == "12345678";
        });
    };
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        (void)co_await ruvia::async_asio([&](auto h) { socket.async_connect(server.endpoint(), std::move(h)); });
        auto peer = ruvia::http2_connection::client();
        std::array<char, 16384> input{};
        auto pump = [&]() -> ruvia::task<bool> {
            auto r = co_await ruvia::async_asio<std::size_t>([&](auto h) {
                socket.async_read_some(asio::buffer(input), std::move(h));
            });
            if (r.error_code() || r.result() == 0) {
                co_return false;
            }
            raw_from_server.append(input.data(), r.result());
            (void)peer.feed(std::string_view(input.data(), r.result()));
            co_return true;
        };
        co_await flush_peer(socket, peer);
        while (!peer.received_peer_settings()) {
            co_await pump();
            while (peer.next_event()) {
            }
            co_await flush_peer(socket, peer);
        }
        const auto req = peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{
            .protocol_ = "capsule-tunnel", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/capsules"});
        const auto id = std::get<std::uint32_t>(req);
        co_await flush_peer(socket, peer);
        bool saw_server_end = false;
        while (!saw_server_end) {
            co_await pump();
            while (auto ev = peer.next_event()) {
                if (ev->tunnel_end()) {
                    saw_server_end = true;
                }
            }
            co_await flush_peer(socket, peer);
        }
        (void)peer.submit_data(id, std::string_view("\x00\x0a"
                                                    "ab",
                                       4),
            ruvia::http2_end_stream::end_stream, 65535);
        co_await flush_peer(socket, peer);
        const auto& worker_value = attachment.loop().worker();
        (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(30));
        std::string ping_frame{};
        ruvia::test::append_frame(ping_frame, ruvia::http2_frame_type::ping, 0, 0, "12345678");
        (void)co_await ruvia::async_asio<std::size_t>([&](auto h) {
            asio::async_write(socket, asio::buffer(ping_frame), std::move(h));
        });
        while (!ping_acknowledged() && co_await pump()) {
        }
        ruvia::close_socket(socket);
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    observation->halfclose_capsules_ = false;

    EXPECT_TRUE(ping_acknowledged());
    for (const auto& [type, flags, payload] : server_frames()) {
        EXPECT_TRUE(type != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        EXPECT_TRUE(type != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
    }
}

// H2-07 & H2-09: PRIORITY_UPDATE on an idle stream is observed by the route handler
// once HEADERS opens the stream, and a buffered push stream resets with CANCEL when
// the client lowers SETTINGS_MAX_CONCURRENT_STREAMS to 0.
TEST(Ruvia, http2_web_idle_priority_update_and_buffered_push_stream_limit_cancel) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    observation->seen_urgency_ = -1;
    observation->hold_push_ = true;
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket sock(io);
    sock.connect(server.endpoint());

    std::string wire(ruvia::http2_client_preface);
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
        std::string_view("\x00\x02\x00\x00\x00\x01\x00\x03\x00\x00\x00\x01", 12));
    std::array<char, 32> prio_buf{};
    const auto prio_sz =
        std::get<0>(ruvia::encode_http2_priority_update(prio_buf, 1, {.urgency_ = 1, .incremental_ = false}));
    wire.append(prio_buf.data(), prio_sz);
    std::string get1;
    ruvia::hpack_encoder::encode_header(get1, ":method", "GET");
    ruvia::hpack_encoder::encode_header(get1, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(get1, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(get1, ":path", "/prio-idle");
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, get1);

    std::string get3;
    ruvia::hpack_encoder::encode_header(get3, ":method", "GET");
    ruvia::hpack_encoder::encode_header(get3, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(get3, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(get3, ":path", "/push-parent-cancel");
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 3, get3);
    asio::write(sock, asio::buffer(wire));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (observation->seen_urgency_ == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(observation->seen_urgency_.load(), 1);

    std::string wire2{};
    ruvia::test::append_frame(wire2, ruvia::http2_frame_type::settings, 0, 0,
        std::string_view("\x00\x03\x00\x00\x00\x00", 6));
    asio::write(sock, asio::buffer(wire2));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    observation->hold_push_ = false;

    sock.non_blocking(true);
    std::string in_buf{};
    std::uint32_t push_rst_code = 0xffffffffU;
    const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (push_rst_code == 0xffffffffU && std::chrono::steady_clock::now() < read_deadline) {
        std::array<char, 4096> tmp{};
        asio::error_code ec;
        const auto n = sock.read_some(asio::buffer(tmp), ec);
        if (n > 0) {
            in_buf.append(tmp.data(), n);
            std::size_t off = 0;
            while (off + 9 <= in_buf.size()) {
                const auto len = (static_cast<unsigned char>(in_buf[off]) << 16) |
                                 (static_cast<unsigned char>(in_buf[off + 1]) << 8) |
                                 static_cast<unsigned char>(in_buf[off + 2]);
                const auto type = static_cast<unsigned char>(in_buf[off + 3]);
                const auto sid = ((static_cast<unsigned char>(in_buf[off + 5]) & 0x7fU) << 24) |
                                 (static_cast<unsigned char>(in_buf[off + 6]) << 16) |
                                 (static_cast<unsigned char>(in_buf[off + 7]) << 8) |
                                 static_cast<unsigned char>(in_buf[off + 8]);
                if (off + 9 + len > in_buf.size()) {
                    break;
                }
                if (type == static_cast<unsigned char>(ruvia::http2_frame_type::rst_stream) &&
                    sid == 2 && len == 4) {
                    push_rst_code = (static_cast<unsigned char>(in_buf[off + 9]) << 24) |
                                    (static_cast<unsigned char>(in_buf[off + 10]) << 16) |
                                    (static_cast<unsigned char>(in_buf[off + 11]) << 8) |
                                    static_cast<unsigned char>(in_buf[off + 12]);
                }
                off += 9 + len;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    EXPECT_EQ(push_rst_code, static_cast<std::uint32_t>(ruvia::http2_error_code::cancel));
    sock.close();
    server.finish();
}

namespace http1_websocket_suite_test {
struct tunnel_observation {
    std::mutex mutex_;
    std::string bytes_{};
    std::atomic<bool> finish_first_{};
    std::atomic<bool> eof_{};
    [[nodiscard]] std::string received() {
        std::lock_guard lock(mutex_);
        return std::string(bytes_);
    }
};
const auto tunnel_obs = std::make_shared<tunnel_observation>();
[[maybe_unused]] const bool tunnel_obs_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<tunnel_observation>>([] { return tunnel_obs; });
    return true;
}();

class client_tunnel_routes final {
public:
    RUVIA_ROUTES_BEGIN(client_tunnel_routes)
    RUVIA_CONNECT("client-target.test:443", echo);
    RUVIA_CONNECT_PROTOCOL("test-protocol", "/tunnel", echo);
    const ruvia::http_tunnel_route_config capsule_route{.datagrams_ = true};
    RUVIA_CONNECT_PROTOCOL("test-capsules", "/capsules", capsules, capsule_route);
    RUVIA_CONNECT_PROTOCOL("test-no-datagrams", "/no-datagrams", no_datagrams);
    RUVIA_GET("/sibling", sibling);
    RUVIA_ROUTES_END

    ruvia::task<void> echo(ruvia::context& c);
    ruvia::task<void> capsules(ruvia::context& c);
    ruvia::task<void> no_datagrams(ruvia::context& c);
    ruvia::task<ruvia::http_response> sibling(ruvia::context& c) {
        co_return c.text("sibling");
    }
};
ruvia::task<void> client_tunnel_routes::echo(ruvia::context& c) {
    auto& observation_value = *c.worker_state<std::shared_ptr<tunnel_observation>>();
    {
        std::lock_guard lock(observation_value.mutex_);
        observation_value.bytes_.clear();
    }
    observation_value.eof_ = false;
    auto& tunnel = c.tunnel();
    if (observation_value.finish_first_) {
        co_await tunnel.write("greeting");
        co_await tunnel.finish();
    }
    while (auto bytes = co_await tunnel.read()) {
        {
            std::lock_guard lock(observation_value.mutex_);
            observation_value.bytes_.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        }
        if (!observation_value.finish_first_) {
            co_await tunnel.write(*bytes);
        }
    }
    observation_value.eof_ = true;
    co_await tunnel.finish();
}
}  // namespace http1_websocket_suite_test
TEST(Ruvia, http2_client_tunnel_owns_cold_input_half_closes_and_preserves_siblings) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    std::optional<ruvia::http_client_tunnel> retained_tunnel;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().worker();
        auto& observation_value = *tunnel_obs;
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .connection_limits_ = {.max_open_ = 1}, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .versions_ = ruvia::http_client_versions{.http1_ = false}});
        const auto client_origin = client.client();
        try {
            for (unsigned round = 0; round != 4; ++round) {
                observation_value.finish_first_ = round >= 2;
                std::string authority(round == 1 ? "proxy.test" : "client-target.test:443");
                auto cold = client_origin.connect(authority).protocol(round == 1 ? "test-protocol" : "").target(round == 1 ? "/tunnel" : "").open();
                authority.assign("mutated");
                auto result_value = co_await std::move(cold);
                EXPECT_TRUE(result_value.response() == nullptr && result_value.tunnel() != nullptr);
                if (!result_value.tunnel()) {
                    throw std::runtime_error("CONNECT was rejected");
                }
                auto tunnel = std::move(*result_value.tunnel());
                EXPECT_TRUE(tunnel.status() == ruvia::http_status::ok);
                EXPECT_TRUE(!tunnel.header("content-length"));
                std::string echoed{};
                const auto receive = [&]() -> ruvia::task<void> {
                    while (auto bytes = co_await tunnel.read()) {
                        echoed.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                };
                if (observation_value.finish_first_) {
                    co_await receive();
                    EXPECT_TRUE(echoed == "greeting");
                }
                ruvia::task_scope readers(worker_value);
                if (round == 3) {
                    tunnel.abort();
                    bool half_closed_write_rejected = false;
                    try {
                        auto late_write = tunnel.write("late");
                        static_cast<void>(late_write);
                    } catch (const ruvia::http_client_error& error) {
                        half_closed_write_rejected = error.code() == std::make_error_code(std::errc::operation_canceled);
                    }
                    EXPECT_TRUE(half_closed_write_rejected);
                    co_await readers.join();
                    auto sibling = co_await client_origin.get("/sibling").send();
                    std::string sibling_body{};
                    while (auto bytes = co_await sibling.stream().text()) {
                        sibling_body.append(*bytes);
                    }
                    EXPECT_TRUE(sibling_body == "sibling");
                    retained_tunnel.emplace(std::move(tunnel));
                    continue;
                }
                if (!observation_value.finish_first_) {
                    readers.spawn(receive());
                }
                std::string payload_value(100003, 't');
                for (std::size_t offset = 0; offset != payload_value.size();) {
                    const auto count = std::min<std::size_t>(16384, payload_value.size() - offset);
                    auto output = tunnel.write(std::string_view(payload_value).substr(offset, count));
                    {
                        // Discarding a cold finish does not claim the write lane.
                        auto unused_finish = tunnel.finish();
                        static_cast<void>(unused_finish);
                    }
                    co_await std::move(output);
                    offset += count;
                }
                co_await tunnel.finish();
                co_await readers.join();
                if (!observation_value.finish_first_) {
                    EXPECT_TRUE(echoed == payload_value);
                }
                auto sibling = co_await client_origin.get("/sibling").send();
                std::string sibling_body{};
                while (auto bytes = co_await sibling.stream().text()) {
                    sibling_body.append(*bytes);
                }
                EXPECT_TRUE(sibling_body == "sibling");
                EXPECT_TRUE(observation_value.received() == payload_value && observation_value.eof_);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (retained_tunnel) {
            retained_tunnel->abort();
            retained_tunnel->abort();
            bool late_write_rejected = false;
            try {
                auto late_write = retained_tunnel->write("late");
                static_cast<void>(late_write);
            } catch (const ruvia::http_client_error&) {
                late_write_rejected = true;
            }
            EXPECT_TRUE(late_write_rejected);
            retained_tunnel.reset();
        }
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

namespace http1_websocket_suite_test {
ruvia::task<void> client_tunnel_routes::capsules(ruvia::context& c) {
    auto stream = c.tunnel().capsules();
    while (auto capsule = co_await stream.read()) {
        co_await stream.write(capsule->type(), capsule->payload());
    }
    co_await stream.finish();
}

// The route protocol owns Capsule Protocol negotiation (RFC 9297 section 3.4):
// a route that did not declare datagrams cannot open a capsule facade, and the
// raw tunnel stays usable.
ruvia::task<void> client_tunnel_routes::no_datagrams(ruvia::context& c) {
    auto& tunnel = c.tunnel();
    unsigned rejected = 0;
    try {
        static_cast<void>(tunnel.datagrams());
    } catch (const std::logic_error&) {
        ++rejected;
    }
    try {
        static_cast<void>(tunnel.udp());
    } catch (const std::logic_error&) {
        ++rejected;
    }
    try {
        static_cast<void>(tunnel.capsules());
    } catch (const std::logic_error&) {
        ++rejected;
    }
    co_await tunnel.write(rejected == 3 ? std::string_view("rejected") : std::string_view("accepted"));
    co_await tunnel.finish();
}
}  // namespace http1_websocket_suite_test
TEST(Ruvia, http2_client_capsule_stream_retains_results_and_cold_operations_after_shutdown) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_capsule> retained;
        std::unique_ptr<ruvia::task<void>> cold;
        {
            const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
            ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .connection_limits_ = {.max_open_ = 1}, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .versions_ = ruvia::http_client_versions{.http1_ = false}});
            const auto client_origin = client.client();
            try {
                auto result_value = co_await client_origin.connect("proxy.test").protocol("test-capsules").target("/capsules").open({.max_chunk_bytes_ = 1024});
                if (!result_value.tunnel()) {
                    throw std::runtime_error("capsule CONNECT rejected");
                }
                // RFC 9297 section 3.4: the datagram route's 2xx advertises it.
                EXPECT_EQ(result_value.tunnel()->header("Capsule-Protocol"), std::string_view("?1"));
                {
                    auto stream = std::move(*result_value.tunnel()).capsules();
                    std::string bytes_value(4099, 'c');
                    auto write = stream.write(0x123456789ULL, bytes_value);
                    bytes_value.assign("mutated");
                    co_await std::move(write);
                    retained = co_await stream.read();
                    EXPECT_TRUE(retained && retained->type() == 0x123456789ULL && retained->text() == std::string(4099, 'c') && retained->bytes().size() == 4099);
                    co_await stream.write(0, "");
                    auto empty = co_await stream.read();
                    EXPECT_TRUE(empty && empty->type() == 0 && empty->text().empty() && empty->bytes().empty());
                    co_await stream.finish();
                    EXPECT_TRUE(!(co_await stream.read()));
                    // Discarding a cold read expires its scope without running it.
                    auto pending = stream.read();
                }
                {
                    // Without declared datagrams: no Capsule-Protocol, and the
                    // handler's datagrams()/udp()/capsules() throw std::logic_error.
                    auto plain_result = co_await client_origin.connect("proxy.test").protocol("test-no-datagrams").target("/no-datagrams").open();
                    if (!plain_result.tunnel()) {
                        throw std::runtime_error("protocol CONNECT without datagrams rejected");
                    }
                    auto plain = std::move(*plain_result.tunnel());
                    EXPECT_TRUE(!plain.header("Capsule-Protocol"));
                    std::string answer{};
                    while (auto text_chunk = co_await plain.text()) {
                        answer.append(*text_chunk);
                    }
                    EXPECT_EQ(answer, std::string("rejected"));
                    co_await plain.finish();
                }
                auto second = co_await client_origin.connect("proxy.test").protocol("test-capsules").target("/capsules").open();
                if (!second.tunnel()) {
                    throw std::runtime_error("second capsule CONNECT rejected");
                }
                {
                    auto stream = std::move(*second.tunnel()).capsules();
                    cold.reset(new ruvia::task<void>(stream.write(12, std::string(4099, 'x'))));
                }
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        EXPECT_TRUE(retained && retained->payload() == std::string(4099, 'c'));
        cold.reset();
        retained.reset();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// RFC 9297 section 3.2: an unknown capsule type and a DATAGRAM with an
// unknown Context ID (RFC 9298 section 4) are skipped even beyond the reader's
// max_capsule_length_. A malformed DATAGRAM then fails the tunnel as a
// malformed message (RFC 9297 section 3.3): RST_STREAM(PROTOCOL_ERROR) per
// RFC 9113 section 8.1.1, not CANCEL.
TEST(Ruvia, http2_client_udp_tunnel_skips_dropped_capsules_and_resets_malformed_input_with_protocol_error) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    std::optional<ruvia::http2_error_code> reset_error;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        auto connection = ruvia::http2_connection::server();
        std::optional<ruvia::http2_request_head_event> request;
        std::array<char, 16384> input{};
        std::string output;
        while (!reset_error) {
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            std::size_t count = 0;
            try {
                count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            } catch (const std::system_error&) {
                break;
            }
            const auto bytes_value = std::string_view(input.data(), count);
            for (;;) {
                const auto fed = connection.feed(bytes_value);
                EXPECT_TRUE(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    if (auto* head = event->request_head()) {
                        ruvia::http_response accepted;
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        EXPECT_TRUE(connection.submit_connect_response_head(head->stream_id(), accepted) ==
                                    ruvia::http2_submit_status::accepted);
                        std::string capsules{};
                        capsules.append("\x17\x40\xc8", 3).append(200, 'u');
                        capsules.append("\x00\x40\xc9\x03", 4).append(200, 'c');
                        capsules.append(
                            "\x00\x06\x00"
                            "after",
                            8);
                        capsules.append("\x00\x00", 2);
                        EXPECT_TRUE(connection.submit_data(head->stream_id(), capsules, ruvia::http2_end_stream::keep_open, 65535).status_ ==
                                    ruvia::http2_data_submit_status::complete);
                        request.emplace(std::move(*head));
                    } else if (auto* closed = event->stream_closed()) {
                        if (closed->source() == ruvia::http2_stream_close_source::peer) {
                            reset_error = closed->error();
                        }
                    } else if (auto* data = event->tunnel_data()) {
                        (void)data->take_credit();
                    }
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
        request.reset();
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    std::string first{};
    bool malformed_rejected = false;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .connection_limits_ = {.max_open_ = 1}, .request_timeout_ = std::chrono::seconds(5), .versions_ = ruvia::http_client_versions{.http1_ = false}});
        try {
            auto opened = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
            if (!opened.tunnel()) {
                throw std::runtime_error("CONNECT-UDP rejected");
            }
            auto udp = std::move(*opened.tunnel()).udp({.max_capsule_length_ = 100});
            auto datagram = co_await udp.read();
            if (datagram) {
                first.assign(reinterpret_cast<const char*>(datagram->payload().data()), datagram->payload().size());
            }
            try {
                (void)co_await udp.read();
            } catch (const std::runtime_error&) {
                malformed_rejected = true;
            }
            // The reset is queued with the abort; let the session write it
            // before shutdown closes the connection.
            const auto& worker_value = attachment.loop().worker();
            for (unsigned attempt = 0; !reset_error && attempt != 200; ++attempt) {
                (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(10));
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_EQ(first, std::string("after"));
    EXPECT_TRUE(malformed_rejected);
    EXPECT_TRUE(reset_error == ruvia::http2_error_code::protocol_error);
}

// A truncated capsule remains an application error after both HTTP/2 stream
// directions close; a closed stream need not emit another reset frame.
TEST(Ruvia, http2_client_half_closed_tunnel_rejects_truncated_capsule_on_end_stream) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    bool server_done = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        auto connection = ruvia::http2_connection::server();
        std::optional<ruvia::http2_request_head_event> request;
        std::array<char, 16384> input{};
        std::string output;
        while (!server_done) {
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            std::size_t count = 0;
            try {
                count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            } catch (const std::system_error&) {
                break;
            }
            const auto bytes_value = std::string_view(input.data(), count);
            for (;;) {
                const auto fed = connection.feed(bytes_value);
                while (auto event = connection.next_event()) {
                    if (auto* head = event->request_head()) {
                        ruvia::http_response accepted;
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        (void)connection.submit_connect_response_head(head->stream_id(), accepted);
                        request.emplace(std::move(*head));
                    } else if (auto* end = event->tunnel_end()) {
                        std::string truncated(
                            "\x00\x0a"
                            "ab",
                            4);
                        (void)connection.submit_data(
                            end->stream_id(), truncated, ruvia::http2_end_stream::end_stream, 65535);
                    } else if (auto* data = event->tunnel_data()) {
                        (void)data->take_credit();
                    }
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
        request.reset();
    };
    asio::co_spawn(io, serve(), asio::detached);

    bool client_threw = false;

    auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        auto opened = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
        EXPECT_TRUE(opened.tunnel() != nullptr);
        auto udp = std::move(*opened.tunnel()).udp({.max_capsule_length_ = 100});
        co_await udp.finish();
        try {
            (void)co_await udp.read();
        } catch (const std::runtime_error&) {
            client_threw = true;
        }
        server_done = true;
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    EXPECT_TRUE(client_threw);
}

// A5H2-04: A malformed capsule detected on a completed tunnel from an earlier
// HTTP/2 connection generation must not submit RST_STREAM(PROTOCOL_ERROR) on
// the same stream ID of a newly reconnected HTTP/2 session.
TEST(Ruvia, http2_client_stale_tunnel_malformed_abort_does_not_reset_reconnected_session_stream) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::uint32_t second_conn_rst_stream_id = 0;
    std::uint32_t second_conn_rst_code = 0;
    bool server_done = false;

    const auto serve = [&]() -> asio::awaitable<void> {
        {
            auto socket1 = co_await peer.async_accept(asio::use_awaitable);
            auto conn1 = ruvia::http2_connection::server();
            std::optional<ruvia::http2_request_head_event> req1;
            std::array<char, 16384> input{};
            std::string output;
            bool sent_truncated = false;
            while (!sent_truncated) {
                if (conn1.wants_write()) {
                    ruvia::test::drain_output(conn1, output);
                    co_await asio::async_write(socket1, asio::buffer(output), asio::use_awaitable);
                }
                const auto count = co_await socket1.async_read_some(asio::buffer(input), asio::use_awaitable);
                (void)conn1.feed(std::string_view(input.data(), count));
                while (auto ev = conn1.next_event()) {
                    if (auto* head = ev->request_head()) {
                        ruvia::http_response accepted;
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        (void)conn1.submit_connect_response_head(head->stream_id(), accepted);
                        req1.emplace(std::move(*head));
                    } else if (auto* end = ev->tunnel_end()) {
                        (void)conn1.submit_data(
                            end->stream_id(), std::string_view("\x00\x0a"
                                                               "ab",
                                                  4),
                            ruvia::http2_end_stream::end_stream, 65535);
                        sent_truncated = true;
                    }
                }
            }
            if (conn1.wants_write()) {
                ruvia::test::drain_output(conn1, output);
                co_await asio::async_write(socket1, asio::buffer(output), asio::use_awaitable);
            }
            req1.reset();
            std::error_code ec;
            socket1.close(ec);
        }

        {
            auto socket2 = co_await peer.async_accept(asio::use_awaitable);
            auto conn2 = ruvia::http2_connection::server();
            std::optional<ruvia::http2_request_head_event> req2;
            std::array<char, 16384> input{};
            std::string output;
            while (!server_done) {
                if (conn2.wants_write()) {
                    ruvia::test::drain_output(conn2, output);
                    co_await asio::async_write(socket2, asio::buffer(output), asio::use_awaitable);
                }
                std::size_t count = 0;
                try {
                    count = co_await socket2.async_read_some(asio::buffer(input), asio::use_awaitable);
                } catch (const std::system_error&) {
                    break;
                }
                (void)conn2.feed(std::string_view(input.data(), count));
                while (auto ev = conn2.next_event()) {
                    if (auto* head = ev->request_head()) {
                        ruvia::http_response accepted;
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        (void)conn2.submit_connect_response_head(head->stream_id(), accepted);
                        req2.emplace(std::move(*head));
                    } else if (auto* closed = ev->stream_closed()) {
                        second_conn_rst_stream_id = closed->stream_id();
                        second_conn_rst_code = static_cast<std::uint32_t>(closed->error());
                    }
                }
            }
            req2.reset();
        }
    };
    asio::co_spawn(io, serve(), asio::detached);

    bool udp1_threw = false;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().worker();
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        auto opened1 = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
        EXPECT_TRUE(opened1.tunnel() != nullptr);
        auto udp1 = std::move(*opened1.tunnel()).udp({.max_capsule_length_ = 100});
        co_await udp1.finish();
        (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(30));

        auto opened2 = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
        EXPECT_TRUE(opened2.tunnel() != nullptr);
        auto udp2 = std::move(*opened2.tunnel()).udp({.max_capsule_length_ = 100});
        (void)udp2;

        try {
            (void)co_await udp1.read();
        } catch (const std::runtime_error&) {
            udp1_threw = true;
        }
        for (unsigned attempt = 0; second_conn_rst_stream_id == 0 && attempt != 30; ++attempt) {
            (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(10));
        }
        server_done = true;
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    EXPECT_TRUE(udp1_threw);
    EXPECT_EQ(second_conn_rst_stream_id, std::uint32_t{0});
    EXPECT_EQ(second_conn_rst_code, std::uint32_t{0});
}

namespace http2_streaming_test {

struct sleeping_stream_observation final {
    std::atomic<bool> sleep_failed_{};
    std::atomic<bool> sleep_elapsed_{};
    std::atomic<bool> sse_wait_cancelled_{};
    std::atomic<bool> sse_wait_failed_{};
    std::atomic<bool> sse_wait_finished_{};
    std::atomic<int> head_loop_writes_{};
    std::atomic<bool> head_loop_aborted_{};
    std::atomic<bool> head_loop_sleep_rejected_{};
    std::atomic<bool> head_loop_token_stopped_{};
    std::atomic<bool> head_loop_finished_{};
    std::atomic<bool> naive_loop_failed_{};
    std::atomic<bool> naive_loop_finished_{};
    std::atomic<bool> head_trailer_stopped_{};
    std::atomic<bool> head_trailer_sleep_stopped_{};
    std::atomic<bool> head_trailer_aborted_{};
    std::atomic<bool> head_trailer_done_{};
};

const auto sleeping_stream_observed = std::make_shared<sleeping_stream_observation>();
[[maybe_unused]] const bool sleeping_stream_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<sleeping_stream_observation>>([] { return sleeping_stream_observed; });
    return true;
}();

class sleeping_stream_routes final {
    RUVIA_ROUTES_BEGIN(sleeping_stream_routes)
    RUVIA_GET("/http2-sleeping-stream", sleeping);
    RUVIA_POST("/http2-count-upload", count_upload, ruvia::stream_body);
    RUVIA_GET("/http2-quiet-sse", quiet_sse);
    RUVIA_POST("/http2-post-sse", post_sse);
    RUVIA_GET("/http2-paced-sse", paced_sse);
    RUVIA_GET("/http2-head-loop-sse", head_loop_sse);
    RUVIA_GET("/http2-head-naive-sse", head_naive_sse);
    RUVIA_POST("/http2-bounded-buffered", bounded_buffered, ruvia::body_limit<16>);
    RUVIA_POST("/http2-bounded-stream", count_upload, ruvia::stream_body, ruvia::body_limit<16>);
    RUVIA_POST("/http2-bounded-delayed-stream", bounded_delayed_stream, ruvia::stream_body, ruvia::body_limit<16>);
    RUVIA_GET("/http2-head-trailer-write", head_trailer_write);
    RUVIA_GET("/http2-head-trailer-only", head_trailer_only);
    RUVIA_GET("/http2-stream-custom", stream_custom);
    RUVIA_GET("/http2-stream-204", stream_204);
    RUVIA_ROUTES_END

    // HEAD 正常完成，不启动生产循环，也不会取消业务等待 token。
    ruvia::task<void> head_loop_sse(ruvia::context& context_value) {
        if (context_value.req().query("not-modified").has_value()) {
            context_value.draft().status(ruvia::http_status::not_modified);
        }
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto writer = context_value.stream_sse();
        const auto token = context_value.stop_token();
        if (co_await writer.commit() == ruvia::stream_commit_result::body) {
            co_await writer.write({.data_ = "tick"});
            observed.head_loop_writes_ = 1;
        } else {
            observed.head_loop_writes_ = -1;
            try {
                co_await writer.write({.data_ = "late"});
            } catch (const std::logic_error&) {
                observed.head_loop_writes_ = 0;
            }
            try {
                static_cast<void>(co_await writer.sleep(std::chrono::seconds(30)));
            } catch (const std::logic_error&) {
                observed.head_loop_sleep_rejected_ = true;
            }
        }
        observed.head_loop_aborted_ = writer.aborted();
        observed.head_loop_token_stopped_ = token.stop_requested();
        co_await writer.end();
        observed.head_loop_finished_ = true;
    }

    // 提交结果结束无正文响应，不靠轮询取消或等待 peer 断开。
    ruvia::task<void> head_naive_sse(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            observed.naive_loop_finished_ = true;
            co_return;
        }
        try {
            for (int i = 0; i < 1000; ++i) {
                co_await writer.write({.data_ = "tick"});
                static_cast<void>(co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(10)));
            }
        } catch (...) {
            observed.naive_loop_failed_ = true;
            observed.naive_loop_finished_ = true;
            throw;
        }
        observed.naive_loop_finished_ = true;
    }

    ruvia::task<void> head_trailer_write(ruvia::context& context_value) {
        auto& out = context_value.stream_text();
        context_value.draft().header("Trailer", "X-Complete");
        if (co_await out.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await out.write("body");
        const ruvia::http_header_view trailers[] = {{"X-Complete", "yes"}};
        co_await out.end(trailers);
    }

    ruvia::task<void> head_trailer_only(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto& out = context_value.stream_text();
        const auto token = context_value.stop_token();
        context_value.draft().header("Trailer", "X-Complete");
        const ruvia::http_header_view trailers[] = {{"X-Complete", "yes"}};
        if (co_await out.commit() == ruvia::stream_commit_result::body) {
            co_await out.end(trailers);
        }
        try {
            static_cast<void>(co_await out.sleep(std::chrono::seconds(10)));
        } catch (const std::logic_error&) {
            observed.head_trailer_sleep_stopped_ = true;
        }
        observed.head_trailer_aborted_ = out.aborted();
        observed.head_trailer_stopped_ = token.stop_requested();
        observed.head_trailer_done_ = true;
    }

    ruvia::task<void> stream_custom(ruvia::context& context_value) {
        context_value.draft().header("Content-Encoding", "x-custom");
        auto& out = context_value.stream_text();
        if (co_await out.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await out.write("payload");
        co_await out.end();
    }

    ruvia::task<void> stream_204(ruvia::context& context_value) {
        context_value.draft().status(ruvia::http_status::no_content);
        co_await context_value.stream_text().end();
    }

    ruvia::task<ruvia::http_response> bounded_buffered(ruvia::context& context_value) {
        co_return context_value.text(co_await context_value.req().text());
    }

    ruvia::task<ruvia::http_response> bounded_delayed_stream(ruvia::context& context_value) {
        (void)co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(40));
        if (context_value.req().query("read").has_value()) {
            static_cast<void>(co_await context_value.req().body_reader().read());
        }
        co_return context_value.text("accepted");
    }

    // Streams the request body and answers with the byte count.
    // A buffered POST body is readable before the SSE response starts.
    ruvia::task<void> post_sse(ruvia::context& context_value) {
        const auto received = co_await context_value.req().text();
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write({.data_ = received});
    }

    ruvia::task<void> paced_sse(ruvia::context& context_value) {
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        for (int i = 0; i < 4; ++i) {
            co_await writer.write({.data_ = "tick"});
            (void)co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(35));
        }
    }

    ruvia::task<ruvia::http_response> count_upload(ruvia::context& context_value) {
        std::size_t bytes_value = 0;
        auto& reader_value = context_value.req().body_reader();
        while (auto chunk = co_await reader_value.read()) {
            bytes_value += chunk->size();
        }
        std::string text;
        text.append(std::to_string(bytes_value));
        co_return context_value.text(std::move(text));
    }

    ruvia::task<void> quiet_sse(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        // The request's own stop: RST_STREAM of this stream requests it.
        const auto stop = context_value.stop_token();
        co_await writer.write({.data_ = "ready"});
        if (context_value.req().query("writer").has_value()) {
            try {
                observed.sleep_elapsed_ =
                    co_await writer.sleep(std::chrono::seconds(30)) == ruvia::timer_sleep_result::elapsed;
            } catch (const std::system_error&) {
                observed.sleep_failed_ = true;
                throw;
            }
            co_return;
        }
        try {
            observed.sse_wait_cancelled_ =
                co_await ruvia::sleep_for(context_value.worker(), std::chrono::seconds(30), stop) ==
                ruvia::timer_sleep_result::stop_requested;
        } catch (...) {
            observed.sse_wait_failed_ = true;
            throw;
        }
        observed.sse_wait_finished_ = true;
    }

    // Writes one chunk, then pauses far longer than the test waits: only a
    // peer disconnect can end the sleep in time.
    ruvia::task<void> sleeping(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto& writer = context_value.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("first\n");
        try {
            if (co_await writer.sleep(std::chrono::seconds(30)) == ruvia::timer_sleep_result::elapsed) {
                observed.sleep_elapsed_ = true;
            }
        } catch (const std::system_error&) {
            observed.sleep_failed_ = true;
            throw;
        }
        co_await writer.write("second\n");
    }
};

using ruvia::test::append_frame;

// With expect_continue the HEADERS announce the content but the DATA frame is
// left for the caller to send once the server has answered the expectation.
std::string request_wire(std::string_view method,
    std::string_view path, std::string_view body, bool expect_continue = false) {
    std::string wire(ruvia::http2_client_preface);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", method);
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", path);
    if (!body.empty()) {
        ruvia::hpack_encoder::encode_header(block, "content-length", std::to_string(body.size()));
    }
    if (expect_continue) {
        ruvia::hpack_encoder::encode_header(block, "expect", "100-continue");
    }
    append_frame(wire, ruvia::http2_frame_type::headers, static_cast<std::uint8_t>(body.empty() ? 0x5 : 0x4), 1, block);
    if (!body.empty() && !expect_continue) {
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, body);
    }
    return wire;
}

using ruvia::test::read_frame;

// Reads frames until the first HEADERS frame on stream 1 and returns the
// decoded :status; connection-level frames before it are skipped.
std::string read_first_status(asio::ip::tcp::socket& client, ruvia::hpack_decoder& decoder) {
    for (;;) {
        const auto frame = read_frame(client);
        if (frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::headers) ||
            frame.header_.stream_id_ != 1) {
            continue;
        }
        std::string status{};
        const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
            if (name == ":status") {
                status = value;
            }
            return true;
        });
        if (decoded.has_value()) {
            throw std::logic_error("invalid HPACK block");
        }
        return status;
    }
}

// Reads frames until the first DATA frame on stream 1 and returns its payload;
// a GOAWAY or RST_STREAM before that ends the read with an empty result.
std::string read_first_data(asio::ip::tcp::socket& client) {
    for (;;) {
        std::array<char, ruvia::http2_frame_header_bytes> header_bytes{};
        asio::read(client, asio::buffer(header_bytes));
        const auto header_value = ruvia::parse_http2_frame_header(header_bytes);
        if (!header_value.has_value()) {
            return std::string{};
        }
        std::string payload(header_value->length_, '\0');
        if (!payload.empty()) {
            asio::read(client, asio::buffer(payload));
        }
        if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
            header_value->stream_id_ == 1) {
            return payload;
        }
        if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
            header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
            return std::string{};
        }
    }
}

}  // namespace http2_streaming_test

using http2_streaming_test::append_frame;
using http2_streaming_test::read_first_data;
using http2_streaming_test::read_first_status;
using http2_streaming_test::request_wire;
using http2_streaming_test::sleeping_stream_observed;

// A client that disconnects while an HTTP/2 stream producer is inside sleep()
// ends that sleep with the transport error, so the handler, its stream and
// the connection retire instead of staying suspended for the full duration;
// the disconnect is not counted as a connection failure.
TEST(Ruvia, http2_response_stream_sleep_ends_when_the_peer_disconnects) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sleep_failed_ = false;
    sleeping_stream_observed->sleep_elapsed_ = false;
    const auto failures_before = ruvia::app().stats().connection_failures_;
    for (const bool send_goaway : {false, true}) {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const auto wire = request_wire("GET", "/http2-sleeping-stream", {});
        asio::write(client, asio::buffer(wire));
        EXPECT_EQ(read_first_data(client), std::string("first\n"));
        if (send_goaway) {
            std::string goaway{};
            append_frame(goaway, ruvia::http2_frame_type::goaway, 0, 0, std::string_view("\0\0\0\0\0\0\0\0", 8));
            asio::write(client, asio::buffer(goaway));
        }
        client.close();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!sleeping_stream_observed->sleep_failed_ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(sleeping_stream_observed->sleep_failed_);
        EXPECT_TRUE(!sleeping_stream_observed->sleep_elapsed_);
        sleeping_stream_observed->sleep_failed_ = false;
    }
    server.finish();
    // The peer going away is a normal end of the connection, not a fault.
    EXPECT_EQ(ruvia::app().stats().connection_failures_, failures_before);
}

TEST(Ruvia, http2_post_sse_reads_the_request_body_before_streaming) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const auto wire = request_wire("POST", "/http2-post-sse", "{\"id\":\"1\"}");
    asio::write(client, asio::buffer(wire));
    EXPECT_EQ(read_first_data(client), std::string("data: {\"id\":\"1\"}\n\n"));
}

// A quiet SSE producer waits on context::stop_token(), not on a transport
// write or writer.sleep(). RST_STREAM must wake it without closing its sibling.
TEST(Ruvia, http2_quiet_sse_peer_reset_cancels_wait_and_preserves_connection) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sse_wait_cancelled_ = false;
    sleeping_stream_observed->sse_wait_failed_ = false;
    sleeping_stream_observed->sse_wait_finished_ = false;
    const auto stats_before = ruvia::app().stats();
    ruvia::hpack_decoder decoder;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const auto wire = request_wire("GET", "/http2-quiet-sse", {});
    asio::write(client, asio::buffer(wire));
    client.non_blocking(true);
    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string event{};
    while (event.size() < std::string_view("data: ready\n\n").size()) {
        const auto frame = http2_streaming_test::read_frame(client, ready_deadline);
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            const auto decoded = decoder.decode(frame.payload_, [](std::string_view, std::string_view) { return true; });
            EXPECT_TRUE(!decoded.has_value());
        }
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
            frame.header_.stream_id_ == 1) {
            event.append(frame.payload_);
        }
    }
    EXPECT_EQ(event, std::string("data: ready\n\n"));
    EXPECT_TRUE(!sleeping_stream_observed->sse_wait_finished_);

    client.non_blocking(false);
    // Keep a sibling upload open across the reset, rather than merely opening
    // a new stream after cancellation has already finished.
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", "/http2-count-upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", "1");
    std::string sibling_request{};
    append_frame(sibling_request, ruvia::http2_frame_type::headers, 0x4, 3, block);
    asio::write(client, asio::buffer(sibling_request));
    std::string reset{};
    append_frame(reset, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view("\0\0\0\x08", 4));
    asio::write(client, asio::buffer(reset));
    const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!sleeping_stream_observed->sse_wait_finished_ && std::chrono::steady_clock::now() < cancel_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(sleeping_stream_observed->sse_wait_finished_);
    EXPECT_TRUE(sleeping_stream_observed->sse_wait_cancelled_);
    EXPECT_TRUE(!sleeping_stream_observed->sse_wait_failed_);

    std::string sibling_body{};
    append_frame(sibling_body, ruvia::http2_frame_type::data, 0x1, 3, "x");
    asio::write(client, asio::buffer(sibling_body));
    client.non_blocking(true);
    const auto response_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string status{};
    std::string body{};
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, response_deadline);
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                if (frame.header_.stream_id_ == 3 && name == ":status") {
                    status = value;
                }
                return true;
            });
            EXPECT_TRUE(!decoded.has_value());
        }
        if (frame.header_.stream_id_ == 3) {
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                body.append(frame.payload_);
            }
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers) ||
                frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }
    }
    EXPECT_EQ(status, std::string("200"));
    EXPECT_EQ(body, std::string("1"));
    client.close();
    server.finish();
    const auto stats_after = ruvia::app().stats();
    EXPECT_EQ(stats_after.connection_failures_, stats_before.connection_failures_);
    EXPECT_EQ(stats_after.worker_failures_, stats_before.worker_failures_);
}

TEST(Ruvia, http2_opt_in_writer_sleep_preserves_reset_error) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sleep_failed_ = false;
    sleeping_stream_observed->sleep_elapsed_ = false;
    const auto before = ruvia::app().stats().connection_failures_;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("GET", "/http2-quiet-sse?writer=1", {})));
    EXPECT_EQ(read_first_data(client), std::string("data: ready\n\n"));
    std::string reset{};
    append_frame(reset, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view("\0\0\0\x08", 4));
    asio::write(client, asio::buffer(reset));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!sleeping_stream_observed->sleep_failed_ && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(sleeping_stream_observed->sleep_failed_);
    EXPECT_TRUE(!sleeping_stream_observed->sleep_elapsed_);
    client.close();
    server.finish();
    EXPECT_EQ(ruvia::app().stats().connection_failures_, before);
}

// A request whose HEADERS and complete DATA arrive in one read is still a
// streaming upload: the handler is dispatched for the head and consumes the
// already-received content rather than the stream being reset.
TEST(Ruvia, http2_streaming_upload_received_in_one_read_is_dispatched) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const std::size_t body_size : {std::size_t{1}, std::size_t{100}, std::size_t{5000}}) {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const auto wire = request_wire("POST", "/http2-count-upload", std::string(body_size, 'x'));
        asio::write(client, asio::buffer(wire));
        EXPECT_EQ(read_first_data(client), std::string(std::to_string(body_size)));
        client.close();
    }
    server.finish();
}

// RFC 9110 §10.1.1 on HTTP/2 follows the HTTP/1 policy: 100 Continue is sent
// when the server starts consuming the content (a streaming handler's first
// body read), and a request whose final status is known from the head alone
// (404) is answered with that status and never asked for its content.
TEST(Ruvia, http2_expect_continue_is_answered_when_the_content_is_consumed) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string body(40, 'x');
        asio::write(client, asio::buffer(request_wire("POST", "/http2-count-upload", body, true)));
        EXPECT_EQ(read_first_status(client, decoder), std::string("100"));
        std::string data{};
        append_frame(data, ruvia::http2_frame_type::data, 0x1, 1, body);
        asio::write(client, asio::buffer(data));
        EXPECT_EQ(read_first_status(client, decoder), std::string("200"));
        EXPECT_EQ(read_first_data(client), std::string("40"));
        client.close();
    }
    {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire("POST", "/http2-missing-upload", std::string(40, 'x'), true)));
        EXPECT_EQ(read_first_status(client, decoder), std::string("404"));
        client.close();
    }
    server.finish();
}

// RFC 9110 9.3.2: HEAD on an SSE GET route answers 200 with the GET head and
// ends the stream without content.
TEST(Ruvia, http2_head_falls_back_to_sse_get_route_without_content) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    ruvia::hpack_decoder decoder;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("HEAD", "/http2-paced-sse", {})));
    client.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string status{};
    std::string content_type{};
    std::string body{};
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, deadline);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
            (frame.header_.flags_ & 0x1) == 0) {
            std::string ack{};
            append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
            asio::write(client, asio::buffer(ack));
            continue;
        }
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.stream_id_ != 1) {
            continue;
        }
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                if (name == ":status") {
                    status = value;
                } else if (name == "content-type") {
                    content_type = value;
                }
                return true;
            });
            ended = (frame.header_.flags_ & 0x1) != 0;
        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
            body.append(frame.payload_);
            ended = (frame.header_.flags_ & 0x1) != 0;
        }
    }
    EXPECT_EQ(status, std::string("200"));
    EXPECT_TRUE(content_type.starts_with("text/event-stream"));
    EXPECT_TRUE(body.empty());
    client.close();
    server.finish();
}

// 完成态拒绝非法生产/等待，aborted 与取消 token 只表示真实失败。
TEST(Ruvia, http2_head_and_304_streams_complete_without_abort) {
    using namespace http2_streaming_test;
    for (const bool not_modified : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        ruvia::test::http_server_fixture server(io);
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire(not_modified ? "GET" : "HEAD",
                                not_modified ? "/http2-head-loop-sse?not-modified" : "/http2-head-loop-sse", {})));
        client.non_blocking(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool ended = false;
        while (!ended) {
            const auto frame = http2_streaming_test::read_frame(client, deadline);
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                (frame.header_.flags_ & 0x1) == 0) {
                std::string ack{};
                append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                asio::write(client, asio::buffer(ack));
                continue;
            }
            if (frame.header_.stream_id_ == 1) {
                EXPECT_TRUE(frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }

        auto& observed = *http2_streaming_test::sleeping_stream_observed;
        const auto finish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!observed.head_loop_finished_ && std::chrono::steady_clock::now() < finish_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_TRUE(observed.head_loop_finished_.load());
        EXPECT_EQ(observed.head_loop_writes_.load(), 0);
        EXPECT_TRUE(!observed.head_loop_aborted_.load());
        EXPECT_TRUE(!observed.head_loop_token_stopped_.load());
        EXPECT_TRUE(observed.head_loop_sleep_rejected_.load());
        client.close();
        server.finish();
    }
}

// HEAD 在提交时完成，不要求对端断开才结束生产者。
TEST(Ruvia, http2_head_only_producer_finishes_at_commit) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("HEAD", "/http2-head-naive-sse", {})));
    client.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, deadline);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
            (frame.header_.flags_ & 0x1) == 0) {
            std::string ack{};
            append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
            asio::write(client, asio::buffer(ack));
            continue;
        }
        if (frame.header_.stream_id_ == 1) {
            EXPECT_TRUE(frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
            ended = (frame.header_.flags_ & 0x1) != 0;
        }
    }
    client.close();

    auto& observed = *http2_streaming_test::sleeping_stream_observed;
    const auto finish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!observed.naive_loop_finished_ && std::chrono::steady_clock::now() < finish_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(observed.naive_loop_finished_.load());
    EXPECT_TRUE(!observed.naive_loop_failed_.load());
    server.finish();
}

// A GET/SSE stream whose request body is already closed (END_STREAM on HEADERS)
// must not be held in connection_scanner::phase::reading_payload and killed by
// request_body_timeout_ or body_completion_timeout_ while streaming responses.
TEST(Ruvia, http2_sse_response_is_not_killed_by_request_body_completion_timeout) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io,
        ruvia::server_config{
            .worker_count_ = 1,
            .idle_timeout_ = std::chrono::milliseconds(2000),
            .connection_scan_interval_ = std::chrono::milliseconds(10),
            .request_header_timeout_ = std::chrono::milliseconds(500),
            .request_body_timeout_ = std::chrono::milliseconds(60),
            .body_completion_timeout_ = std::chrono::milliseconds(80),
        });
    ruvia::hpack_decoder decoder;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("GET", "/http2-paced-sse", {})));
    client.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string status{};
    std::string body{};
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, deadline);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
            (frame.header_.flags_ & 0x1) == 0) {
            std::string ack{};
            append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
            asio::write(client, asio::buffer(ack));
            continue;
        }
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.stream_id_ != 1) {
            continue;
        }
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                if (name == ":status") {
                    status = value;
                }
                return true;
            });
            ended = (frame.header_.flags_ & 0x1) != 0;
        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
            body.append(frame.payload_);
            ended = (frame.header_.flags_ & 0x1) != 0;
        }
    }
    EXPECT_EQ(status, std::string("200"));
    EXPECT_EQ(body, std::string("data: tick\n\ndata: tick\n\ndata: tick\n\ndata: tick\n\n"));
    client.close();
    server.finish();
}

TEST(Ruvia, http2_oversized_request_body_answers_413_without_100_continue) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const std::string oversized(64, 'x');
    // Read through END_STREAM: a valid head alone cannot prove that the
    // rejection's borrowed diagnostic survived until its response was sent.
    const auto check_body_limit_response = [&](asio::ip::tcp::socket& client, ruvia::hpack_decoder& decoder) {
        client.non_blocking(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        std::string body{};
        unsigned response_heads = 0;
        bool ended = false;
        while (!ended) {
            const auto frame = http2_streaming_test::read_frame(client, deadline);
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                (frame.header_.flags_ & 0x1) == 0) {
                std::string ack{};
                append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                asio::write(client, asio::buffer(ack));
                continue;
            }
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
                (frame.header_.stream_id_ == 1 &&
                    frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream))) {
                throw std::runtime_error("oversized HTTP/2 request reset before its complete 413 response");
            }
            if (frame.header_.stream_id_ != 1) {
                continue;
            }
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                ++response_heads;
                std::string status{};
                const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                    if (name == ":status") {
                        status = value;
                    }
                    return true;
                });
                EXPECT_TRUE(!decoded.has_value());
                EXPECT_EQ(status, std::string("413"));
                ended = (frame.header_.flags_ & 0x1) != 0;
            } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                body.append(frame.payload_);
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }
        EXPECT_EQ(response_heads, 1U);
        const auto error = ruvia::json_object::parse(body);
        const auto status = error.get<ruvia::uint32>("status").value();
        const auto code = error.get<ruvia::string>("code").value();
        EXPECT_TRUE(status && static_cast<std::uint32_t>(*status) == 413);
        EXPECT_TRUE(code && code->view() == "request_body_too_large");
    };

    // 1. Declared Content-Length exceeding route body_limit<16> with Expect: 100-continue
    //    must answer 413 immediately without sending 100 Continue first.
    for (const std::string_view path : {"/http2-bounded-buffered", "/http2-bounded-stream"}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire("POST", path, oversized, true)));
        check_body_limit_response(client, decoder);
        client.close();
    }

    // 2. Unannounced (no Content-Length) DATA exceeding route body_limit<16> on both
    //    buffered and stream_body routes must answer 413 rather than RST_STREAM(CANCEL).
    for (const std::string_view path : {"/http2-bounded-buffered", "/http2-bounded-stream"}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        std::string wire(ruvia::http2_client_preface);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(block, ":path", path);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, oversized);
        asio::write(client, asio::buffer(wire));
        check_body_limit_response(client, decoder);
        client.close();
    }

    // 3. Post-dispatch oversized DATA while stream_body handler is awaiting another
    //    async operation:
    //    - when the handler returns 200 without reading the body, the server still
    //      answers 413 instead of committing 200;
    //    - when the request carries Expect: 100-continue (without Content-Length) and
    //      the handler performs its first read after oversized DATA arrived, 100 Continue
    //      is skipped and 413 is returned directly.
    for (const bool read_after_wait : {false, true}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        std::string wire(ruvia::http2_client_preface);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(
            block, ":path", read_after_wait ? "/http2-bounded-delayed-stream?read=1" : "/http2-bounded-delayed-stream");
        if (read_after_wait) {
            ruvia::hpack_encoder::encode_header(block, "expect", "100-continue");
        }
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, oversized);
        asio::write(client, asio::buffer(wire));
        check_body_limit_response(client, decoder);
        client.close();
    }
    server.finish();
}

TEST(Ruvia, http2_head_stream_with_trailers_strips_trailer_header_and_stops_eof_token) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    auto& observed = *sleeping_stream_observed;
    observed.head_trailer_stopped_ = false;
    observed.head_trailer_sleep_stopped_ = false;
    observed.head_trailer_aborted_ = false;
    observed.head_trailer_done_ = false;

    for (const std::string_view path : {"/http2-head-trailer-write", "/http2-head-trailer-only"}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire("HEAD", path, {})));
        client.non_blocking(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool ended = false;
        bool saw_trailer_header = false;
        unsigned header_frames = 0;
        while (!ended) {
            const auto frame = http2_streaming_test::read_frame(client, deadline);
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                (frame.header_.flags_ & 0x1) == 0) {
                std::string ack{};
                append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                asio::write(client, asio::buffer(ack));
                continue;
            }
            if (frame.header_.stream_id_ == 1) {
                EXPECT_TRUE(frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
                ++header_frames;
                (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view) {
                    if (name == "trailer" || name == "x-complete") {
                        saw_trailer_header = true;
                    }
                    return true;
                });
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }
        EXPECT_EQ(header_frames, 1U);
        EXPECT_TRUE(!saw_trailer_header);
        client.close();
    }

    const auto finish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!observed.head_trailer_done_ && std::chrono::steady_clock::now() < finish_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(observed.head_trailer_done_.load());
    EXPECT_TRUE(!observed.head_trailer_aborted_.load());
    EXPECT_TRUE(!observed.head_trailer_stopped_.load());
    EXPECT_TRUE(observed.head_trailer_sleep_stopped_.load());
    server.finish();
}

TEST(Ruvia, http2_stream_custom_content_encoding_and_bodyless_status_negotiation) {
    using namespace http2_streaming_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto check_status = [&](std::string_view path, std::string_view accept_encoding,
                                  std::string_view expected_status) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        std::string wire(ruvia::http2_client_preface);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(block, ":path", path);
        ruvia::hpack_encoder::encode_header(block, "accept-encoding", accept_encoding);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        asio::write(client, asio::buffer(wire));
        EXPECT_EQ(read_first_status(client, decoder), std::string(expected_status));
        client.close();
    };
    check_status("/http2-stream-custom", "x-custom, identity;q=0", "200");
    check_status("/http2-stream-custom", "gzip", "406");
    check_status("/http2-stream-204", "*;q=0", "204");
    server.finish();
}

namespace http1_websocket_suite_test {
ruvia::task<std::string> collect_text(ruvia::http_client_response& response) {
    const auto bytes_value = co_await response.stream().read_all();
    const auto view = bytes_value.bytes();
    co_return std::string(reinterpret_cast<const char*>(view.data()), view.size());
}
struct push_observation final {
    std::atomic<unsigned> promised_{};
    std::atomic<unsigned> refused_{};
    std::atomic<unsigned> streamed_{};
};

const auto push_obs = std::make_shared<push_observation>();
[[maybe_unused]] const bool push_obs_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<push_observation>>([] { return push_obs; });
    return true;
}();

std::shared_ptr<push_observation> prepare_observation() {
    push_obs->promised_ = 0;
    push_obs->refused_ = 0;
    push_obs->streamed_ = 0;
    return push_obs;
}

class push_routes final {
public:
    RUVIA_ROUTES_BEGIN(push_routes)
    RUVIA_GET("/parent", parent);
    RUVIA_GET("/two", parent);
    RUVIA_GET("/large", parent);
    RUVIA_GET("/asset", asset);
    RUVIA_GET("/large-asset", large);
    RUVIA_GET("/delayed-parent", parent);
    RUVIA_GET("/delayed-asset", delayed);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> parent(ruvia::context& context_value) {
        auto& owner_value = *context_value.worker_state<std::shared_ptr<push_observation>>();
        const auto count = context_value.req().path() == "/two" ? 2U : 1U;
        const auto path = context_value.req().path() == "/large"            ? "/large-asset"
                          : context_value.req().path() == "/delayed-parent" ? "/delayed-asset"
                                                                            : "/asset";
        const std::array<ruvia::http_header_view, 1> headers{{{"x-promise", "owned-request"}}};
        for (unsigned i = 0; i != count; ++i) {
            if (context_value.push({.scheme_ = "http", .authority_ = context_value.req().authority(), .path_ = path, .headers_ = headers})) {
                ++owner_value.promised_;
            } else {
                ++owner_value.refused_;
            }
        }
        co_return context_value.text("parent");
    }
    ruvia::task<ruvia::http_response> asset(ruvia::context& context_value) {
        if (context_value.req().header("x-promise") != "owned-request") {
            throw std::runtime_error("lost push request metadata");
        }
        context_value.draft().header("x-asset", "metadata");
        co_return context_value.text("pushed-body");
    }
    // Answers well after the client's first, interrupted wait for it.
    ruvia::task<ruvia::http_response> delayed(ruvia::context& context_value) {
        (void)co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(300), context_value.stop_token());
        co_return context_value.text("late-asset");
    }
    ruvia::task<void> large(ruvia::context& context_value) {
        auto& owner_value = *context_value.worker_state<std::shared_ptr<push_observation>>();
        std::string chunk(16384, 'p');
        if (co_await context_value.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        for (unsigned i = 0; i != 80; ++i) {
            co_await context_value.stream().write(chunk);
        }
        ++owner_value.streamed_;
        co_await context_value.stream().end();
    }
};

}  // namespace http1_websocket_suite_test

TEST(Ruvia, http2_push_routes_and_client_preserve_owners_flow_control_cold_reads_and_shutdown) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto observed = prepare_observation();
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_push> retained;
        std::optional<ruvia::http_client_response> retained_response;
        {
            const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
            ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                             .versions_ = ruvia::http_client_versions{.http1_ = false},
                                                             .push_ = {.enabled_ = true}});
            const auto client_origin = client.client();
            std::exception_ptr failure;
            try {
                for (unsigned repeat = 0; repeat != 128; ++repeat) {
                    auto parent_value = co_await client_origin.get("/parent").send();
                    EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                    auto push = client_origin.next_push();
                    EXPECT_TRUE(push.has_value());
                    if (!push) {
                        throw std::runtime_error("missing push");
                    }
                    EXPECT_TRUE(push->request().path_ == "/asset");
                    EXPECT_TRUE(push->request().headers_.front().value() == "owned-request");
                    {
                        auto cold = push->response();
                    }
                    auto pending = push->response();
                    bool busy = false;
                    try {
                        auto overlap = push->response();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    EXPECT_TRUE(busy);
                    auto moved = std::move(*push);
                    auto response = co_await std::move(pending);
                    EXPECT_TRUE(response.status() == ruvia::http_status::ok);
                    EXPECT_TRUE(response.header("x-asset") == "metadata");
                    EXPECT_TRUE((co_await collect_text(response)) == "pushed-body");
                    EXPECT_TRUE(!client_origin.next_push());
                    if (repeat == 0) {
                        retained.emplace(std::move(moved));
                        retained_response.emplace(std::move(response));
                    }
                    EXPECT_TRUE(retained->request().path_ == "/asset");
                    EXPECT_TRUE(retained_response->header("x-asset") == "metadata");
                }
                auto parent_value = co_await client_origin.get("/large").send();
                EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                auto pushed = client_origin.next_push();
                EXPECT_TRUE(pushed.has_value());
                if (!pushed) {
                    throw std::runtime_error("missing large push");
                }
                auto response = co_await pushed->response();
                std::size_t bytes_value = 0;
                while (auto chunk = co_await response.stream().text()) {
                    EXPECT_TRUE(chunk->find_first_not_of('p') == std::string_view::npos);
                    bytes_value += chunk->size();
                }
                EXPECT_EQ(bytes_value, std::size_t{80 * 16384});
                EXPECT_EQ(observed->streamed_.load(), 1U);
                auto cancelled_parent = co_await client_origin.get("/large").send();
                EXPECT_TRUE((co_await collect_text(cancelled_parent)) == "parent");
                auto cancelled_push = client_origin.next_push();
                EXPECT_TRUE(cancelled_push.has_value());
                cancelled_push.reset();
                auto after_cancel = co_await client_origin.get("/parent").send();
                EXPECT_TRUE((co_await collect_text(after_cancel)) == "parent");
                auto after_push = client_origin.next_push();
                EXPECT_TRUE(after_push.has_value());
                if (after_push) {
                    auto after_response = co_await after_push->response();
                    EXPECT_TRUE((co_await collect_text(after_response)) == "pushed-body");
                }
                EXPECT_EQ(client_origin.stats().received_pushes_, std::size_t{131});
                EXPECT_EQ(client_origin.stats().rejected_pushes_, std::size_t{0});
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            server.finish();
            if (failure) {
                std::rethrow_exception(failure);
            }
        }
        EXPECT_TRUE(retained->request().path_ == "/asset");
        EXPECT_TRUE(retained_response->header("x-asset") == "metadata");
        retained_response.reset();
        retained.reset();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, http2_push_queue_overflow_and_disabled_permission_keep_parent_response_usable) {
    using namespace http1_websocket_suite_test;
    for (const bool enabled : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        const auto observed = prepare_observation();
        ruvia::test::http_server_fixture server(io);
        auto run = [&]() -> ruvia::task<void> {
            const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
            ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                             .versions_ = ruvia::http_client_versions{.http1_ = false},
                                                             .push_ = {.enabled_ = enabled, .max_queued_pushes_ = 1}});
            const auto client_origin = client.client();
            std::exception_ptr failure;
            try {
                auto parent_value = co_await client_origin.get("/two").send();
                EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                auto push = client_origin.next_push();
                EXPECT_TRUE(push.has_value() == enabled);
                if (push) {
                    auto response = co_await push->response();
                    EXPECT_TRUE((co_await collect_text(response)) == "pushed-body");
                }
                EXPECT_EQ(client_origin.stats().received_pushes_, enabled ? std::size_t{1} : std::size_t{0});
                EXPECT_EQ(client_origin.stats().rejected_pushes_, enabled ? std::size_t{1} : std::size_t{0});
                EXPECT_EQ(observed->refused_.load(), enabled ? 0U : 2U);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            server.finish();
            if (failure) {
                std::rethrow_exception(failure);
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        std::move(root).get();
    }
}

// Waiting for a pushed response is a read: its timeout or an already-stopped
// token ends only that wait, and a later response() still receives it.
TEST(Ruvia, http2_push_response_wait_honours_its_operation) {
    using namespace http1_websocket_suite_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto observed = prepare_observation();
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false},
                                                         .push_ = {.enabled_ = true}});
        const auto client_origin = client.client();
        std::exception_ptr failure;
        try {
            auto parent_value = co_await client_origin.get("/delayed-parent").send();
            EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
            auto push = client_origin.next_push();
            EXPECT_TRUE(push.has_value());
            if (push) {
                EXPECT_THROW((void)push->response({.timeout_ = std::chrono::milliseconds(0)}), std::invalid_argument);
                const auto failure_of = [](ruvia::task<ruvia::http_client_response> wait) -> ruvia::task<std::optional<std::error_code>> {
                    try {
                        (void)co_await std::move(wait);
                    } catch (const ruvia::http_client_error& error) {
                        co_return error.code();
                    }
                    co_return std::nullopt;
                };
                EXPECT_TRUE((co_await failure_of(push->response({.timeout_ = std::chrono::milliseconds(50)}))) ==
                            std::make_error_code(std::errc::timed_out));
                ruvia::stop_source stop;
                stop.request_stop();
                EXPECT_TRUE((co_await failure_of(push->response({.stop_token_ = stop.token()}))) ==
                            std::make_error_code(std::errc::operation_canceled));
                auto response = co_await push->response();
                EXPECT_TRUE((co_await collect_text(response)) == "late-asset");
            }
            EXPECT_EQ(observed->promised_.load(), 1U);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        server.finish();
        if (failure) {
            std::rethrow_exception(failure);
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

namespace http2_file_flow_control_test {

constexpr std::size_t file_size = 65535;

std::filesystem::path& file_path() {
    static std::filesystem::path path;
    return path;
}

[[nodiscard]] const std::filesystem::path& empty_directory() {
    static const ruvia::test::scoped_temp_directory directory("ruvia-http2-empty-file");
    return directory.path();
}

std::unique_ptr<ruvia::static_root> empty_root;

class file_routes final {
    RUVIA_ROUTES_BEGIN(file_routes)
    RUVIA_GET("/http2-file-flow-control", file);
    RUVIA_GET("/http2-file-mutated", mutated_file);
    RUVIA_GET("/http2-file-ok", ok_text);
    RUVIA_GET("/http2-indexed-empty", indexed_empty);
    RUVIA_GET("/http2-empty-mutated", mutated_empty);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> file(ruvia::context& context_value) {
        auto response = co_await context_value.file({.path_ = file_path()});
        // Make the exercised transport path observable, rather than assuming
        // that the file is above the context::file in-memory threshold.
        response.header("x-file-backed", response.file_body().has_value() ? "yes" : "no");
        co_return response;
    }

    ruvia::task<ruvia::http_response> mutated_file(ruvia::context& context_value) {
        {
            std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
            const std::string bytes(file_size, 'f');
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        // The client truncates the file once HEADERS arrive: the file was
        // validated before them, and the multipart boundary DATA is submitted
        // before the file range read fails and resets the stream.
        co_return co_await context_value.file({.path_ = file_path()});
    }

    ruvia::task<ruvia::http_response> mutated_empty(ruvia::context& context_value) {
        const auto path = empty_directory() / "body.bin";
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
        }
        auto response = co_await context_value.file({.path_ = path});
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "changed";
        }
        co_return response;
    }

    ruvia::task<ruvia::http_response> indexed_empty(ruvia::context& context_value) {
        co_return co_await context_value.static_file(*empty_root, {.relative_path_ = "body.bin"});
    }

    ruvia::task<ruvia::http_response> ok_text(ruvia::context& context_value) {
        co_return context_value.text("alive");
    }
};

struct test_file final {
    test_file() {
        file_path() = directory_.path() / "body.bin";
        std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
        const std::string bytes(file_size, 'f');
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("cannot write HTTP/2 flow-control test file");
        }
    }

private:
    ruvia::test::scoped_temp_directory directory_{"ruvia-http2-file"};
};

using ruvia::test::append_frame;

std::string request_wire(bool exhaust_connection) {
    std::string wire(ruvia::http2_client_preface);
    // In each case exactly one window reaches zero after the file. Keep the
    // stream window positive when testing the connection window, and vice versa.
    const std::string_view settings = exhaust_connection
                                          ? std::string_view("\0\4\0\1\xff\xfe", 6)   // INITIAL_WINDOW_SIZE = 131070
                                          : std::string_view("\0\4\0\0\xff\xff", 6);  // INITIAL_WINDOW_SIZE = 65535
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
    if (!exhaust_connection) {
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0,
            std::string_view("\0\0\xff\xff", 4));
    }
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-flow-control");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    return wire;
}

using ruvia::test::received_frame;

ruvia::task<received_frame> read_frame(asio::ip::tcp::socket& socket) {
    std::array<char, ruvia::http2_frame_header_bytes> header_bytes{};
    const auto head = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
        asio::async_read(socket, asio::buffer(header_bytes), std::move(handler));
    });
    if (head.error_code()) {
        throw std::system_error(head.error_code());
    }
    const auto header = ruvia::parse_http2_frame_header(header_bytes);
    if (!header) {
        throw std::logic_error("invalid HTTP/2 flow-control response frame");
    }
    received_frame frame{*header, std::string(header->length_, '\0')};
    if (!frame.payload_.empty()) {
        const auto body = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_read(socket, asio::buffer(frame.payload_), std::move(handler));
        });
        if (body.error_code()) {
            throw std::system_error(body.error_code());
        }
    }
    co_return frame;
}

void check_file_end(bool exhaust_connection) {
    const test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool ended = false;
    bool file_backed = false;
    std::string status{};
    std::string body{};
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        // Bound missing END_STREAM without a sleeping thread or a race between
        // reading the final file bytes and deciding whether to return credit.
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const auto wire = request_wire(exhaust_connection);
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(wire), std::move(handler));
            });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            ruvia::hpack_decoder decoder;
            bool credited = false;
            while (!ended) {
                const auto frame = co_await read_frame(socket);
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    std::string ack{};
                    append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                    const auto acknowledged = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(ack), std::move(handler));
                    });
                    if (acknowledged.error_code()) {
                        throw std::system_error(acknowledged.error_code());
                    }
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
                    frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    throw std::runtime_error("HTTP/2 file response aborted");
                }
                if (frame.header_.stream_id_ != 1) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            status = value;
                        } else if (name == "x-file-backed") {
                            file_backed = value == "yes";
                        }
                        return true;
                    });
                    if (decoded.has_value()) {
                        throw std::runtime_error("invalid HTTP/2 file response headers");
                    }
                    EXPECT_TRUE((frame.header_.flags_ & 0x1) == 0);
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    EXPECT_TRUE((frame.header_.flags_ & 0x8) == 0);
                    body.append(frame.payload_);
                    ended = (frame.header_.flags_ & 0x1) != 0;
                    if (ended) {
                        EXPECT_TRUE(frame.payload_.empty());
                        EXPECT_TRUE(credited);
                    }
                }
                // The file exhausted one send window. The empty END_STREAM DATA
                // frame waits for that window to open (RFC 9113 section 6.9.1
                // permits sending it at zero window; nghttp2 does not), so one
                // byte of credit releases it.
                if (!ended && !credited && body.size() == file_size) {
                    credited = true;
                    std::string increment{};
                    append_frame(increment, ruvia::http2_frame_type::window_update, 0, exhaust_connection ? 0 : 1,
                        std::string_view("\0\0\0\1", 4));
                    const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(increment), std::move(handler));
                    });
                    if (sent.error_code()) {
                        throw std::system_error(sent.error_code());
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_EQ(status, std::string("200"));
    EXPECT_TRUE(file_backed);
    EXPECT_EQ(body, std::string(file_size, 'f'));
    EXPECT_TRUE(ended);
    EXPECT_TRUE(!expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http2_file_flow_control_test

TEST(Ruvia, http2_file_response_end_waits_for_exhausted_connection_window) {
    using namespace http2_file_flow_control_test;
    http2_file_flow_control_test::check_file_end(true);
}

TEST(Ruvia, http2_file_response_end_waits_for_exhausted_stream_window) {
    using namespace http2_file_flow_control_test;
    http2_file_flow_control_test::check_file_end(false);
}

TEST(Ruvia, http2_buffered_response_resumes_after_stream_window_update) {
    using namespace http2_file_flow_control_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool ended = false;
    bool credited = false;
    std::string status{};
    std::string body{};
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            std::string wire(ruvia::http2_client_preface);
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view("\0\4\0\0\0\1", 6));
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-ok");
            append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(wire), std::move(handler));
            });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            ruvia::hpack_decoder decoder;
            while (!ended) {
                const auto frame = co_await read_frame(socket);
                std::string reply;
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    append_frame(reply, ruvia::http2_frame_type::settings, 0x1, 0, {});
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
                    frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    throw std::runtime_error("HTTP/2 buffered response aborted");
                }
                if (frame.header_.stream_id_ == 1) {
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                        const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                            if (name == ":status") {
                                status = value;
                            }
                            return true;
                        });
                        if (decoded.has_value()) {
                            throw std::runtime_error("invalid HTTP/2 buffered response headers");
                        }
                        ended = (frame.header_.flags_ & 0x1) != 0;
                    } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                        body.append(frame.payload_);
                        ended = (frame.header_.flags_ & 0x1) != 0;
                        if (!credited && !body.empty()) {
                            EXPECT_EQ(body, std::string_view("a"));
                            EXPECT_FALSE(ended);
                            credited = true;
                            append_frame(reply, ruvia::http2_frame_type::window_update, 0, 1,
                                std::string_view("\0\0\0\4", 4));
                        }
                    }
                }
                if (!reply.empty()) {
                    const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(reply), std::move(handler));
                    });
                    if (sent.error_code()) {
                        throw std::system_error(sent.error_code());
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_EQ(status, std::string_view("200"));
    EXPECT_EQ(body, std::string_view("alive"));
    EXPECT_TRUE(credited);
    EXPECT_TRUE(ended);
    EXPECT_FALSE(expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// A file body is validated before HEADERS but read only once the peer grants
// send window: bytes the file held before the window opened are never sent.
// Truncating the file between HEADERS and the first WINDOW_UPDATE therefore
// fails the read and resets the stream without a single DATA byte.
TEST(Ruvia, http2_file_body_is_read_only_after_send_window_is_granted) {
    using namespace http2_file_flow_control_test;
    const test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool reset = false;
    std::string status{};
    std::size_t data_bytes = 0;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            std::string wire(ruvia::http2_client_preface);
            // INITIAL_WINDOW_SIZE = 0: the stream has no send window at all.
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\0\4\0\0\0\0", 6));
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-flow-control");
            append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
            co_await send_bytes(wire);
            ruvia::hpack_decoder decoder;
            while (!reset) {
                const auto frame = co_await read_frame(socket);
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    std::string ack{};
                    append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                    co_await send_bytes(ack);
                }
                if (frame.header_.stream_id_ != 1) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            status = value;
                        }
                        return true;
                    });
                    {
                        std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
                        output << "x";
                    }
                    std::string update{};
                    append_frame(update, ruvia::http2_frame_type::window_update, 0, 1, std::string_view("\0\0\xff\xff", 4));
                    co_await send_bytes(update);
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    data_bytes += frame.payload_.size();
                    if ((frame.header_.flags_ & 0x1) != 0) {
                        break;
                    }
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    reset = true;
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_TRUE(!expired);
    EXPECT_EQ(status, std::string("200"));
    EXPECT_TRUE(reset);
    EXPECT_EQ(data_bytes, std::size_t{0});
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http2_failed_file_writes_release_output_budget_slots) {
    using namespace http2_file_flow_control_test;
    const http2_file_flow_control_test::test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    std::string final_status{};
    std::string final_body{};
    std::size_t resets_seen = 0;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(15));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            std::string wire(ruvia::http2_client_preface);
            // A 10-byte initial stream window splits the first multipart
            // boundary chunk (~80 bytes) across window updates, so DATA output
            // is in flight when the file range read fails and resets the stream.
            http2_file_flow_control_test::append_frame(
                wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\0\4\0\0\0\x0a", 6));
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            co_await send_bytes(wire);
            ruvia::hpack_decoder decoder;
            // Five failed file responses exceed the 4 connection-wide DATA
            // output budget slots if any reset leaks its slot.
            for (std::uint32_t stream_id = 1; stream_id <= 9; stream_id += 2) {
                std::string block;
                ruvia::hpack_encoder::encode_header(block, ":method", "GET");
                ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
                ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
                ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-mutated");
                ruvia::hpack_encoder::encode_header(block, "range", "bytes=0-3, 10-13");
                std::string req{};
                http2_file_flow_control_test::append_frame(
                    req, ruvia::http2_frame_type::headers, 0x5, stream_id, block);
                co_await send_bytes(req);
                bool stream_done = false;
                while (!stream_done) {
                    const auto frame = co_await http2_file_flow_control_test::read_frame(socket);
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                        (frame.header_.flags_ & 0x1) == 0) {
                        std::string ack{};
                        http2_file_flow_control_test::append_frame(
                            ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                        co_await send_bytes(ack);
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                        (void)decoder.decode(frame.payload_, [](std::string_view, std::string_view) { return true; });
                        std::ofstream output(http2_file_flow_control_test::file_path(), std::ios::binary | std::ios::trunc);
                        output << "x";
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                        std::string update{};
                        http2_file_flow_control_test::append_frame(
                            update, ruvia::http2_frame_type::window_update, 0, stream_id,
                            std::string_view("\0\0\x10\0", 4));
                        co_await send_bytes(update);
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                        ++resets_seen;
                        stream_done = true;
                    }
                }
            }
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-ok");
            std::string req{};
            http2_file_flow_control_test::append_frame(req, ruvia::http2_frame_type::headers, 0x5, 11, block);
            co_await send_bytes(req);
            bool ended = false;
            while (!ended) {
                const auto frame = co_await http2_file_flow_control_test::read_frame(socket);
                if (frame.header_.stream_id_ != 11) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            final_status = value;
                        }
                        return true;
                    });
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    final_body.append(frame.payload_);
                    ended = (frame.header_.flags_ & 0x1) != 0;
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_TRUE(!expired);
    EXPECT_EQ(resets_seen, std::size_t{5});
    EXPECT_EQ(final_status, std::string("200"));
    EXPECT_EQ(final_body, std::string("alive"));
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http2_empty_file_validation_precedes_end_stream_headers) {
    using namespace http2_file_flow_control_test;
    const auto& directory = empty_directory();
    const auto path = directory / "body.bin";
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            ruvia::hpack_decoder decoder;
            std::string preface(ruvia::http2_client_preface);
            append_frame(preface, ruvia::http2_frame_type::settings, 0, 0, {});
            co_await send_bytes(preface);
            // Valid empty, changed, removed, changed after framing, then a
            // healthy stream proves failed validation remains stream-local.
            for (std::uint32_t stream_id = 1; stream_id <= 9; stream_id += 2) {
                // Changed, removed, and changed after framing: each fails
                // validation before HEADERS and gets an error response.
                const bool invalid = stream_id == 3 || stream_id == 5 || stream_id == 7;
                if (stream_id <= 5) {
                    {
                        std::ofstream output(path, std::ios::binary | std::ios::trunc);
                    }
                    empty_root = std::make_unique<ruvia::static_root>(directory,
                        ruvia::static_root_options{
                            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
                        });
                    if (stream_id == 3) {
                        std::ofstream output(path, std::ios::binary | std::ios::trunc);
                        output << "changed";
                    } else if (stream_id == 5) {
                        EXPECT_TRUE(std::filesystem::remove(path));
                    }
                }
                std::string block;
                ruvia::hpack_encoder::encode_header(block, ":method", "GET");
                ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
                ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
                ruvia::hpack_encoder::encode_header(block, ":path",
                    stream_id == 9 ? "/http2-file-ok" : stream_id == 7 ? "/http2-empty-mutated"
                                                                       : "/http2-indexed-empty");
                std::string request{};
                append_frame(request, ruvia::http2_frame_type::headers, 0x5, stream_id, block);
                co_await send_bytes(request);
                bool ended = false;
                bool reset = false;
                bool saw_head = false;
                std::string status{};
                std::string body{};
                while (!ended && !reset) {
                    const auto frame = co_await read_frame(socket);
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                        (frame.header_.flags_ & 0x1) == 0) {
                        std::string ack{};
                        append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                        co_await send_bytes(ack);
                    } else if (frame.header_.stream_id_ == stream_id) {
                        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                            saw_head = true;
                            (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                                if (name == ":status") {
                                    status = value;
                                }
                                return true;
                            });
                            ended = (frame.header_.flags_ & 0x1) != 0;
                        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                            body.append(frame.payload_);
                            ended = (frame.header_.flags_ & 0x1) != 0;
                        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                            reset = true;
                        }
                    }
                }
                EXPECT_TRUE(saw_head);
                if (invalid) {
                    EXPECT_TRUE(ended);
                    EXPECT_TRUE(!reset);
                    EXPECT_EQ(status, std::string("500"));
                } else {
                    EXPECT_TRUE(ended);
                    EXPECT_TRUE(!reset);
                    EXPECT_EQ(status, std::string("200"));
                    EXPECT_EQ(body, stream_id == 9 ? std::string("alive") : std::string{});
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto operation = attachment.loop().start(run());
    io.run();
    std::move(operation).get();
    empty_root.reset();
    EXPECT_TRUE(!expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

namespace http2_client_upload_test {

enum class response_mode { ordinary,
    continued,
    rejected,
    reset,
    abandoned,
    abandoned_pending,
    buffered,
    streaming,
    disconnected,
    refused,
    refused_always,
    connection_lost,
    connection_lost_upload,
    goaway_upload };

constexpr std::size_t streaming_payload_bytes = 256 * 1024;

using ruvia::test::append_frame;

// A raw peer deliberately keeps the request half open after the final response.
// It advertises one stream and never supplies the optional server-side reset, so
// client retirement cannot be masked by a server request-lease destructor.
class early_final_peer final {
public:
    early_final_peer(asio::io_context& io, const ruvia::worker_ref& worker, response_mode mode)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          done_(worker),
          mode_(mode) {}
    early_final_peer(asio::io_context&, ruvia::worker_ref&&, response_mode) = delete;

    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        asio::co_spawn(acceptor_.get_executor(), run(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = failure;
            }
            finished_ = true;
            done_.notify();
        });
    }

    void stop() {
        stopping_ = true;
        std::error_code ignored;
        acceptor_.close(ignored);
        socket_.close(ignored);
    }

    ruvia::task<void> join() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    std::string received_{};
    bool request_ended_{false};
    bool request_reset_{false};
    unsigned requests_{0};

private:
    asio::awaitable<void> run() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
        if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
            throw std::runtime_error("missing HTTP/2 client preface");
        }
        if (mode_ == response_mode::connection_lost || mode_ == response_mode::connection_lost_upload ||
            mode_ == response_mode::goaway_upload) {
            std::string wire{};
            const std::array<char, 6> no_streams{0, 3, 0, 0, 0, 0};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {no_streams.data(), no_streams.size()});
            const std::array<char, 8> ping{};
            append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {ping.data(), ping.size()});
            co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
            for (;;) {
                std::array<char, ruvia::http2_frame_header_bytes> bytes;
                co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
                const auto header = ruvia::parse_http2_frame_header(bytes);
                if (!header) {
                    throw std::runtime_error("invalid client HTTP/2 frame");
                }
                std::string payload(header->length_, '\0');
                if (!payload.empty()) {
                    co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
                }
                const auto type = static_cast<ruvia::http2_frame_type>(header->type_);
                if (type == ruvia::http2_frame_type::headers) {
                    throw std::runtime_error("request submitted despite zero stream limit");
                }
                if (type == ruvia::http2_frame_type::ping && (header->flags_ & 1U) != 0) {
                    break;
                }
            }
            if (mode_ == response_mode::goaway_upload) {
                wire.clear();
                const std::array<char, 8> goaway{};
                append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, {goaway.data(), goaway.size()});
                co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
                std::array<char, 1024> remaining;
                std::error_code error;
                do {
                    co_await socket_.async_read_some(asio::buffer(remaining), asio::redirect_error(asio::use_awaitable, error));
                } while (!error);
            }
            socket_.close();
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
            if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
                throw std::runtime_error("missing retried HTTP/2 client preface");
            }
        }
        std::string wire{};
        const std::array<char, 6> one_stream{0, 3, 0, 0, 0, 1};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {one_stream.data(), one_stream.size()});
        if (mode_ == response_mode::disconnected) {
            const std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {zero_window.data(), zero_window.size()});
        }
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        ruvia::hpack_decoder decoder;
        std::uint32_t first_stream = 0;
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid client HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            wire.clear();
            const auto type = static_cast<ruvia::http2_frame_type>(header->type_);
            if (type == ruvia::http2_frame_type::settings && (header->flags_ & 1U) == 0) {
                append_frame(wire, type, 1, 0, {});
            } else if (type == ruvia::http2_frame_type::headers) {
                std::string method{};
                std::string path{};
                const auto decoded = decoder.decode(payload, [&](std::string_view name, std::string_view value) {
                    if (name == ":method") {
                        method = value;
                    }
                    if (name == ":path") {
                        path = value;
                    }
                    return true;
                });
                if (decoded.has_value() || (header->flags_ & 4U) == 0) {
                    throw std::runtime_error("invalid client request fields");
                }
                ++requests_;
                if ((requests_ == 1 && mode_ == response_mode::refused) || mode_ == response_mode::refused_always) {
                    // RFC 9113 section 8.7: the request was not processed.
                    if (method != "GET" || path != "/next") {
                        throw std::runtime_error("unexpected refused request");
                    }
                    first_stream = header->stream_id_;
                    request_ended_ = (header->flags_ & 1U) != 0;
                    const std::array<char, 4> refused_stream{0, 0, 0, 7};
                    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, first_stream,
                        {refused_stream.data(), refused_stream.size()});
                    co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
                    continue;
                }
                if (requests_ == 1 && mode_ == response_mode::connection_lost) {
                    if (method != "GET" || path != "/next") {
                        throw std::runtime_error("unexpected retried request");
                    }
                    first_stream = header->stream_id_;
                } else if (requests_ == 1) {
                    if (method != "POST" || path != "/upload") {
                        throw std::runtime_error("unexpected initial request");
                    }
                    first_stream = header->stream_id_;
                    if (mode_ == response_mode::continued) {
                        std::string interim;
                        ruvia::hpack_encoder::encode_header(interim, ":status", "100");
                        append_frame(wire, type, 4, first_stream, interim);
                    }
                } else if (requests_ == 2) {
                    if (method != "GET" || path != "/next" || header->stream_id_ != first_stream + 2 ||
                        (!request_ended_ && !request_reset_ && mode_ != response_mode::reset)) {
                        throw std::runtime_error("previous request half was not retired on the reused connection");
                    }
                } else {
                    throw std::runtime_error("unexpected extra request");
                }
                const bool rejected = requests_ == 1 && mode_ == response_mode::rejected;
                const std::string_view body = requests_ == 2 || mode_ == response_mode::connection_lost ? "next" : rejected ? "no"
                                                                                                                            : "ok";
                std::string fields;
                const bool empty_final = requests_ == 1 &&
                                         (mode_ == response_mode::buffered || mode_ == response_mode::streaming);
                ruvia::hpack_encoder::encode_header(fields, ":status", empty_final ? "204" : rejected ? "413"
                                                                                                      : "200");
                if (!empty_final) {
                    ruvia::hpack_encoder::encode_header(fields, "content-length", std::to_string(body.size()));
                }
                append_frame(wire, type, empty_final ? 5 : 4, header->stream_id_, fields);
                if (!empty_final && (requests_ != 1 || mode_ != response_mode::abandoned_pending)) {
                    append_frame(wire, ruvia::http2_frame_type::data, 1, header->stream_id_, body);
                }
                if (requests_ == 1 && mode_ == response_mode::reset) {
                    const std::array<char, 4> no_error{};
                    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, first_stream, {no_error.data(), no_error.size()});
                }
            } else if (header->stream_id_ == first_stream && type == ruvia::http2_frame_type::data) {
                received_.append(payload);
                request_ended_ = (header->flags_ & 1U) != 0;
                if (mode_ == response_mode::buffered && received_.size() == 65535 && !window_returned_) {
                    window_returned_ = true;
                    const std::string_view increment("\0\0\0\1", 4);
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, increment);
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, first_stream, increment);
                } else if (mode_ == response_mode::streaming && !payload.empty()) {
                    const auto increment_value = static_cast<std::uint32_t>(payload.size());
                    const std::array<char, 4> increment{
                        static_cast<char>((increment_value >> 24U) & 0x7fU),
                        static_cast<char>((increment_value >> 16U) & 0xffU),
                        static_cast<char>((increment_value >> 8U) & 0xffU),
                        static_cast<char>(increment_value & 0xffU)};
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0,
                        {increment.data(), increment.size()});
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, first_stream,
                        {increment.data(), increment.size()});
                }
            } else if (header->stream_id_ == first_stream && type == ruvia::http2_frame_type::rst_stream) {
                request_reset_ = true;
            }
            if (!wire.empty()) {
                co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
            }
            if (requests_ == 2 || (requests_ == 1 && (mode_ == response_mode::disconnected ||
                                                         mode_ == response_mode::connection_lost ||
                                                         ((mode_ == response_mode::connection_lost_upload ||
                                                              mode_ == response_mode::goaway_upload) &&
                                                             request_ended_)))) {
                co_return;
            }
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    ruvia::worker_signal done_;
    response_mode mode_;
    std::exception_ptr failure_;
    bool stopping_{false};
    bool finished_{false};
    bool window_returned_{false};
};

class draining_peer final {
public:
    draining_peer(asio::io_context& io, const ruvia::worker_ref& worker)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          start_drain_(io),
          ready_(worker),
          done_(worker) {
        start_drain_.expires_at(asio::steady_timer::time_point::max());
    }
    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void start() {
        asio::co_spawn(acceptor_.get_executor(), run(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = failure;
            }
            finished_ = true;
            ready_.notify();
            done_.notify();
        });
    }
    void drain() {
        start_drain_.expires_at(asio::steady_timer::time_point::min());
    }
    void stop() {
        stopping_ = true;
        std::error_code ignored;
        start_drain_.cancel();
        socket_.close(ignored);
        acceptor_.close(ignored);
    }
    [[nodiscard]] ruvia::task<void> wait_ready() {
        while (!draining_ && !finished_) {
            co_await ready_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    [[nodiscard]] ruvia::task<void> join() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    [[nodiscard]] asio::awaitable<void> run() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
        if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
            throw std::runtime_error("missing HTTP/2 preface");
        }
        std::string wire{};
        const std::array<char, 6> one_stream{0, 3, 0, 0, 0, 1};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {one_stream.data(), one_stream.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        std::uint32_t first_stream = 0;
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::headers) {
                first_stream = header->stream_id_;
                break;
            }
        }
        wire.clear();
        const std::array<char, 1> ok{static_cast<char>(0x88)};
        append_frame(wire, ruvia::http2_frame_type::headers, 4, first_stream, {ok.data(), ok.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        std::error_code error;
        co_await start_drain_.async_wait(asio::redirect_error(asio::use_awaitable, error));
        if (stopping_) {
            co_return;
        }
        wire.clear();
        const std::array<char, 8> goaway{0, 0, 0, static_cast<char>(first_stream), 0, 0, 0, 0};
        const std::array<char, 8> ping{};
        append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, {goaway.data(), goaway.size()});
        append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {ping.data(), ping.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::headers) {
                throw std::runtime_error("second request was submitted despite stream limit");
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::ping &&
                (header->flags_ & 1U) != 0) {
                draining_ = true;
                ready_.notify();
                break;
            }
        }
        std::array<char, 1024> remaining;
        do {
            co_await socket_.async_read_some(asio::buffer(remaining), asio::redirect_error(asio::use_awaitable, error));
        } while (!error);
    }
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer start_drain_;
    ruvia::worker_signal ready_;
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    bool draining_{false};
    bool stopping_{false};
    bool finished_{false};
};

void exercise_abandon_during_drain() {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    draining_peer peer(io, worker);
    peer.start();
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        try {
            auto held = co_await client.client().get("/hold").send();
            EXPECT_TRUE(held.status() == ruvia::http_status::ok);
            {
                auto exchange = co_await client.client().post("/abandon").upload({.content_length_ = 7});
                // The peer's single stream and max_open_ leave the upload
                // buffered for admission, its head never submitted.
                for (unsigned i = 0; i != 100 && client.client().stats().buffered_requests_ != 1; ++i) {
                    (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
                }
                EXPECT_EQ(client.client().stats().buffered_requests_, 1U);
                EXPECT_EQ(client.client().stats().in_flight_requests_, 1U);
                peer.drain();
                co_await peer.wait_ready();
                (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(50));
            }
            for (unsigned i = 0; i != 100 && client.client().stats().buffered_requests_ != 0; ++i) {
                (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
            }
            EXPECT_EQ(client.client().stats().in_flight_requests_, 1U);
            EXPECT_EQ(client.client().stats().buffered_requests_, 0U);
            EXPECT_TRUE(!held.stream().complete());
        } catch (...) {
            failure = std::current_exception();
        }
        peer.stop();
        try {
            co_await client.shutdown();
            co_await peer.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void exercise(response_mode mode) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    // The peer's signal borrows this handle through completion and join.
    const auto worker = attachment.loop().worker();
    early_final_peer peer(io, worker, mode);
    peer.start();
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(2),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        try {
            if (mode == response_mode::refused_always) {
                bool rejected = false;
                try {
                    (void)co_await client.client().get("/next").send();
                } catch (const ruvia::http_client_error& error) {
                    rejected = true;
                    EXPECT_EQ(error.code(), ruvia::http_client_errc::protocol_error);
                    EXPECT_TRUE(error.request_written());
                    EXPECT_NE(error.connection_info(), nullptr);
                }
                EXPECT_TRUE(rejected);
                EXPECT_EQ(peer.requests_, 2U);
            } else if (mode == response_mode::refused || mode == response_mode::connection_lost) {
                // These cases start with the request below.
            } else if (mode == response_mode::connection_lost_upload || mode == response_mode::goaway_upload) {
                auto exchange = co_await client.client().post("/upload").upload({.content_length_ = 7});
                co_await exchange.body().write("payload");
                co_await exchange.body().end();
                auto response = co_await exchange.response();
                const auto bytes = co_await response.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), "ok");
                co_await peer.join();
                EXPECT_EQ(peer.requests_, 1U);
                EXPECT_EQ(peer.received_, "payload");
                EXPECT_TRUE(peer.request_ended_);
            } else if (mode == response_mode::abandoned_pending) {
                std::optional<ruvia::http_client_response> retained;
                {
                    auto exchange = co_await client.client().post("/upload").upload({.content_length_ = 7});
                    retained.emplace(co_await exchange.response());
                    EXPECT_TRUE(retained->status() == ruvia::http_status::ok);
                    EXPECT_TRUE(!retained->stream().complete());
                }
                bool cancelled = false;
                try {
                    (void)co_await retained->stream().read_all();
                } catch (const ruvia::http_client_error& error) {
                    cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                }
                EXPECT_TRUE(cancelled);
                bool completion_cancelled = false;
                try {
                    (void)retained->stream().complete();
                } catch (const ruvia::http_client_error& error) {
                    completion_cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                }
                EXPECT_TRUE(completion_cancelled);
            } else if (mode == response_mode::buffered) {
                const std::string payload(65536, 'b');
                auto response = co_await client.client().post("/upload").body(payload).send();
                EXPECT_TRUE(response.status() == ruvia::http_status::no_content);
                const auto bytes = co_await response.stream().read_all();
                EXPECT_TRUE(bytes.bytes().empty());
                EXPECT_TRUE(response.stream().complete());
            } else if (mode == response_mode::streaming) {
                const std::string payload(streaming_payload_bytes, 's');
                auto exchange = co_await client.client().request("POST", "/upload").upload({.content_length_ = payload.size(), .max_chunk_bytes_ = payload.size()});
                auto response = co_await exchange.response();
                EXPECT_TRUE(response.status() == ruvia::http_status::no_content);
                const auto bytes = co_await response.stream().read_all();
                EXPECT_TRUE(bytes.bytes().empty());
                EXPECT_TRUE(response.stream().complete());
                co_await exchange.body().write(payload);
                co_await exchange.body().end();
                EXPECT_TRUE(exchange.body().complete());
            } else {
                const bool expect = mode == response_mode::continued || mode == response_mode::rejected;
                auto exchange = co_await client.client().request("POST", "/upload").upload({.content_length_ = 7, .expectation_ = expect ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none});
                auto response = co_await exchange.response();
                EXPECT_EQ(response.status().value(), mode == response_mode::rejected ? 413 : 200);
                const auto bytes = co_await response.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()),
                    mode == response_mode::rejected ? "no" : "ok");
                EXPECT_TRUE(response.stream().complete());
                if (mode == response_mode::disconnected) {
                    // Start a write with no stream credit, then close the peer.
                    // The write must report its I/O error without replacing a
                    // response EOF that the application has already observed.
                    ruvia::worker_signal started(worker);
                    auto waiting = started.wait();
                    std::optional<std::error_code> write_error;
                    ruvia::task_scope writers(worker);
                    const auto write = [&]() -> ruvia::task<void> {
                        started.notify();
                        try {
                            co_await exchange.body().write("payload");
                        } catch (const ruvia::http_client_error& error) {
                            write_error = error.code();
                        }
                    };
                    writers.spawn(write());
                    co_await std::move(waiting);
                    peer.stop();
                    co_await writers.join();
                    EXPECT_TRUE(write_error == ruvia::make_error_code(ruvia::http_client_errc::io_error));
                    co_await client.shutdown();
                    EXPECT_TRUE(!(co_await response.stream().text()).has_value());
                    EXPECT_TRUE(response.stream().complete());
                } else if (mode == response_mode::ordinary || mode == response_mode::continued) {
                    co_await exchange.body().write("payload");
                    co_await exchange.body().end();
                    EXPECT_TRUE(exchange.body().complete());
                } else if (mode != response_mode::abandoned) {
                    bool cancelled = false;
                    try {
                        co_await exchange.body().write("payload");
                    } catch (const ruvia::http_client_error& error) {
                        cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                    }
                    EXPECT_TRUE(cancelled);
                }
            }
            if (mode != response_mode::disconnected && mode != response_mode::connection_lost_upload &&
                mode != response_mode::goaway_upload && mode != response_mode::refused_always) {
                auto next = co_await client.client().get("/next").send();
                const auto bytes = co_await next.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), "next");
                co_await peer.join();
                EXPECT_EQ(peer.requests_, mode == response_mode::connection_lost ? 1U : 2U);
                if (mode == response_mode::ordinary || mode == response_mode::continued ||
                    mode == response_mode::buffered || mode == response_mode::streaming) {
                    const auto expected = mode == response_mode::buffered
                                              ? std::string(65536, 'b')
                                          : mode == response_mode::streaming
                                              ? std::string(streaming_payload_bytes, 's')
                                              : std::string("payload");
                    EXPECT_EQ(peer.received_, expected);
                    EXPECT_TRUE(peer.request_ended_);
                } else if (mode == response_mode::refused || mode == response_mode::connection_lost) {
                    EXPECT_TRUE(peer.received_.empty());
                    EXPECT_TRUE(!peer.request_reset_);
                } else {
                    EXPECT_TRUE(peer.received_.empty());
                    EXPECT_TRUE(mode == response_mode::reset || peer.request_reset_);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.stop();
        try {
            co_await peer.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http2_client_upload_test

TEST(Ruvia, http2_client_early_final_keeps_ordinary_upload_open) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::ordinary);
}
TEST(Ruvia, http2_client_early_final_after_continue_keeps_upload_open) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::continued);
}
TEST(Ruvia, http2_client_early_final_before_continue_retires_stream) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::rejected);
}
TEST(Ruvia, http2_client_early_final_no_error_reset_preserves_response) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::reset);
}
TEST(Ruvia, http2_client_early_final_abandoned_upload_retires_stream) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::abandoned);
}
TEST(Ruvia, http2_client_abandoned_upload_cancels_its_retained_incomplete_response) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::abandoned_pending);
}

TEST(Ruvia, http2_client_early_final_drains_window_blocked_buffered_upload_before_reuse) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::buffered);
}

TEST(Ruvia, http2_client_streaming_upload_drains_one_large_chunk_across_flow_control_windows) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::streaming);
}

TEST(Ruvia, http2_client_upload_transport_failure_preserves_completed_response) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::disconnected);
}

TEST(Ruvia, http2_client_retries_request_refused_with_refused_stream) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::refused);
}

TEST(Ruvia, http2_client_retries_unsubmitted_request_after_connection_loss) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::connection_lost);
}

TEST(Ruvia, http2_client_retries_unsubmitted_streaming_upload_after_connection_loss) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::connection_lost_upload);
}

TEST(Ruvia, http2_client_retries_unsubmitted_streaming_upload_after_goaway) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::goaway_upload);
}

TEST(Ruvia, http2_abandoned_unsubmitted_upload_retires_while_older_stream_drains) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise_abandon_during_drain();
}

TEST(Ruvia, http2_client_refused_request_preserves_possible_delivery) {
    using namespace http2_client_upload_test;
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::refused_always);
}

#endif

namespace client_operation_interruption_test {

void exercise(const std::string& url, ruvia::http_client_versions versions) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client owner(attachment.loop(), {.base_url_ = url, .versions_ = versions});
        try {
            co_await ruvia::test::client_operations::exercise(owner.client(), worker);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await owner.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace client_operation_interruption_test

TEST(Ruvia, http_client_operations_end_one_wait_or_abort_their_stream_over_http1_and_http2) {
    using namespace client_operation_interruption_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const std::string url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
    std::vector<ruvia::http_client_versions> protocols({ruvia::http_client_versions{.http2_ = false}});
#ifdef RUVIA_ENABLE_HTTP2
    protocols.push_back(ruvia::http_client_versions{.http1_ = false});
#endif
    for (const auto versions : protocols) {
        SCOPED_TRACE(versions.http1_ ? "HTTP/1.1" : "HTTP/2");
        client_operation_interruption_test::exercise(url, versions);
        server.finish();
    }
}

namespace client_handle_and_response_test {

template <typename callback_type>
[[nodiscard]] bool throws_logic_error(callback_type&& callback) {
    try {
        callback();
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

// An independent legacy server, with an absolute deadline covering accept,
// TLS and HTTP. It never uses the production TLS policy under test.
class legacy_http_peer final {
public:
    explicit legacy_http_peer(asio::ssl::context& context)
        : acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          socket_(io_),
          deadline_(io_),
          context_(context),
          port_(acceptor_.local_endpoint().port()),
          done_(asio::co_spawn(io_, serve(), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    ~legacy_http_peer() {
        asio::post(io_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
            deadline_.cancel();
        });
        finish();
    }

    void finish() {
        if (done_.valid()) {
            try {
                done_.get();
            } catch (const std::system_error&) {
                // Handshake rejection or connection close is expected in
                // negative cases; assertions below inspect all received data.
            }
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::string url() const {
        return "https://127.0.0.1:" + std::to_string(port_);
    }

    std::string request_;
    int tls_version_{};
    bool timed_out_{};

private:
    asio::awaitable<void> serve() {
        deadline_.expires_after(std::chrono::seconds(5));
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                timed_out_ = true;
                std::error_code ignored;
                acceptor_.close(ignored);
                socket_.close(ignored);
            }
        });
        struct cancel_deadline final {
            asio::steady_timer& timer_;
            ~cancel_deadline() {
                timer_.cancel();
            }
        } guard{deadline_};
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        asio::ssl::stream<asio::ip::tcp::socket&> stream(socket_, context_);
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        tls_version_ = SSL_version(stream.native_handle());
        std::error_code error;
        co_await asio::async_read_until(stream, asio::dynamic_buffer(request_),
            "\r\n\r\n", asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nlegacy";
        co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, error));
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer deadline_;
    asio::ssl::context& context_;
    std::uint16_t port_;
    std::future<void> done_;
    std::thread thread_;
};

void exercise_legacy_http_peer(int version, ruvia::http_client_versions protocol,
    bool select_h2, bool matching_san = true, bool elliptic_curve = false) {
    ruvia::test::tls_identity identity("server.example",
        matching_san ? "DNS:server.example" : "DNS:other.example", elliptic_curve);
    auto* context = identity.context_.native_handle();
    ASSERT_EQ(SSL_CTX_set_min_proto_version(context, version), 1);
    ASSERT_EQ(SSL_CTX_set_max_proto_version(context, version), 1);
    ASSERT_EQ(SSL_CTX_set_cipher_list(context,
                  "ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA"),
        1);
    // Only the independent fixture allows legacy handshake signatures this
    // way. Trust and SAN verification remain enabled in the real client.
    SSL_CTX_set_security_level(context, 0);
    if (select_h2) {
        SSL_CTX_set_alpn_select_cb(context, [](SSL*, const unsigned char** selected, unsigned char* size, const unsigned char*, unsigned int, void*) -> int {
                static constexpr unsigned char h2[] = {'h', '2'};
                *selected = h2;
                *size = sizeof(h2);
                return SSL_TLSEXT_ERR_OK; }, nullptr);
    }
    legacy_http_peer peer(identity.context_);
    const auto url = peer.url();
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::http_client client(pool.loop(0),
        {.base_url_ = url,
            .connect_timeout_ = std::chrono::seconds(2),
            .request_timeout_ = std::chrono::seconds(2),
            .versions_ = protocol,
            .tls_ = {.ca_file_ = identity.ca_file_, .server_name_ = "server.example"}});
    const bool expect_success = version == TLS1_1_VERSION && !select_h2 && matching_san &&
                                protocol != ruvia::http_client_versions{.http1_ = false};
    std::optional<std::error_code> failure_code;
    std::exception_ptr unexpected;
    bool exchanged = false;
    const auto run = [&]() -> ruvia::task<void> {
        try {
            auto response = co_await client.client().get("/legacy").send();
            EXPECT_TRUE(response.protocol_version() == ruvia::http_protocol_version::http11);
            EXPECT_TRUE(response.status() == ruvia::http_status::ok);
            const auto body = co_await response.stream().read_all();
            EXPECT_EQ(body.text(), std::string_view("legacy"));
            exchanged = true;
        } catch (const ruvia::http_client_error& error) {
            failure_code = error.code();
        } catch (...) {
            unexpected = std::current_exception();
        }
        co_await client.shutdown();
    };
    pool.start();
    pool.loop(0).start(run()).get();
    pool.join();
    peer.finish();
    if (unexpected) {
        std::rethrow_exception(unexpected);
    }
    EXPECT_FALSE(peer.timed_out_);
    EXPECT_EQ(exchanged, expect_success);
    if (expect_success) {
        EXPECT_FALSE(failure_code.has_value());
        EXPECT_EQ(peer.tls_version_, TLS1_1_VERSION);
        EXPECT_TRUE(peer.request_.starts_with("GET /legacy HTTP/1.1\r\n"));
    } else {
        EXPECT_EQ(failure_code, ruvia::make_error_code(ruvia::http_client_errc::tls_failed));
        EXPECT_TRUE(peer.request_.empty());
        if (version == TLS1_1_VERSION && select_h2 &&
            protocol == ruvia::http_client_versions{} && matching_san) {
            // The intentionally noncompliant peer really selected h2 in a
            // completed TLS 1.1 handshake, rather than failing earlier.
            EXPECT_EQ(peer.tls_version_, TLS1_1_VERSION);
        }
    }
}

}  // namespace client_handle_and_response_test

TEST(Ruvia, http_client_tls11_http1_and_negotiation_exchange_with_verified_san) {
    using namespace client_handle_and_response_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    for (const auto protocol : {ruvia::http_client_versions{.http2_ = false},
             ruvia::http_client_versions{}}) {
        for (const bool elliptic_curve : {false, true}) {
            exercise_legacy_http_peer(TLS1_1_VERSION, protocol, false, true, elliptic_curve);
        }
        exercise_legacy_http_peer(TLS1_1_VERSION, protocol, false, false);
    }
}

TEST(Ruvia, http_client_rejects_tls10_and_h2_below_tls12_without_http2_bytes) {
    using namespace client_handle_and_response_test;
    for (const auto protocol : {ruvia::http_client_versions{.http2_ = false},
             ruvia::http_client_versions{}}) {
        exercise_legacy_http_peer(TLS1_VERSION, protocol, false);
    }
#ifdef RUVIA_ENABLE_HTTP2
    exercise_legacy_http_peer(TLS1_1_VERSION, ruvia::http_client_versions{}, true);
    exercise_legacy_http_peer(TLS1_1_VERSION, ruvia::http_client_versions{.http1_ = false}, true);
#endif
}

TEST(Ruvia, http_client_ref_origin_and_expired_send_reject_after_shutdown) {
    using namespace client_handle_and_response_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(),
            {.base_url_ = "http://127.0.0.1:8089"});
        auto handle = client.client();
        try {
            EXPECT_TRUE(handle.scheme() == ruvia::http_scheme::http);
            EXPECT_EQ(handle.host(), std::string_view("127.0.0.1"));
            EXPECT_EQ(handle.port(), std::uint16_t{8089});
            const auto origin = handle.origin();
            EXPECT_TRUE(origin.scheme() == ruvia::http_scheme::http);
            EXPECT_EQ(origin.host(), std::string_view("127.0.0.1"));
            EXPECT_EQ(origin.port(), std::uint16_t{8089});
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (!failure) {
            try {
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.get("/").send(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.get("/").upload({}); }));
                EXPECT_TRUE(
                    throws_logic_error([&] { (void)handle.connect("127.0.0.1:8089").open(); }));
                EXPECT_TRUE(
                    throws_logic_error([&] { (void)handle.udp("/udp").open(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.with_options({}); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.stats(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.next_push(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.next_advertisement(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.host(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.port(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.scheme(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.origin(); }));
            } catch (...) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http_client_response_and_body_moved_from_throw_logic_error) {
    using namespace client_handle_and_response_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(
            socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Test: ok\r\nConnection: close\r\n\r\nhi";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr error) { peer_failure = error; });

    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(),
            {.base_url_ = client_url,
                .request_timeout_ = std::chrono::seconds(5)});
        try {
            auto original = co_await client.client().get("/moved").send();
            auto active = std::move(original);

            EXPECT_TRUE(original.stream().complete());
            EXPECT_TRUE(throws_logic_error([&] { (void)original.status(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.received_status(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.protocol_version(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.headers(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.trailers(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.header("x-test"); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.trailer("x-test"); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.informational_responses(); }));
            EXPECT_TRUE(throws_logic_error([&] { original.reprioritize({}); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.stream().read(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.stream().text(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.stream().read_all(); }));

            EXPECT_TRUE(active.status() == ruvia::http_status::ok);
            EXPECT_EQ(active.header("x-test"), std::optional<std::string_view>("ok"));
            auto body = co_await active.stream().read_all();
            EXPECT_EQ(body.text(), std::string_view("hi"));
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

RUVIA_MODEL(fluent_client_observation,
    RUVIA_REQUIRED_FIELD(method, ruvia::string),
    RUVIA_REQUIRED_FIELD(body, ruvia::string),
    RUVIA_REQUIRED_FIELD(query, ruvia::string),
    RUVIA_REQUIRED_FIELD(single_header, ruvia::string),
    RUVIA_REQUIRED_FIELD(span_header, ruvia::string),
    RUVIA_REQUIRED_FIELD(authorization, ruvia::string),
    RUVIA_REQUIRED_FIELD(proxy_authorization, ruvia::string),
    RUVIA_REQUIRED_FIELD(cookie, ruvia::string),
    RUVIA_REQUIRED_FIELD(content_type, ruvia::string),
    RUVIA_REQUIRED_FIELD(authority, ruvia::string));

namespace fluent_client_tests {

std::atomic<std::size_t> host_cancel_waiters{};
std::atomic<std::size_t> host_cancelled_requests{};

struct redirect_destinations final {
    std::string source_{};
    std::string destination_{};
    std::string secure_{};
    std::string unregistered_{};
};

class fluent_routes final {
public:
    RUVIA_ROUTES_BEGIN(fluent_routes)
    RUVIA_GET("/fluent/inspect", inspect);
    RUVIA_POST("/fluent/inspect", inspect);
    RUVIA_PUT("/fluent/inspect", inspect);
    RUVIA_PATCH("/fluent/inspect", inspect);
    RUVIA_DELETE("/fluent/inspect", inspect);
    RUVIA_HEAD("/fluent/inspect", inspect);
    RUVIA_OPTIONS("/fluent/inspect", inspect);
    RUVIA_GET("/fluent/redirect", redirect);
    RUVIA_POST("/fluent/redirect", redirect);
    RUVIA_PUT("/fluent/redirect", redirect);
    RUVIA_HEAD("/fluent/redirect", redirect);
    RUVIA_GET("/fluent/registered", registered_redirect);
    RUVIA_GET("/fluent/stream-source", stream_source);
    RUVIA_GET("/fluent/host-cancel", host_cancel);
    RUVIA_GET("/fluent/hold", hold);
    RUVIA_GET("/fluent/pipe", pipe);
    RUVIA_GET("/fluent/pipe-bounded", pipe_bounded);
    RUVIA_GET_WS("/fluent/websocket", websocket_echo,
        (ruvia::websocket_route_config{.subprotocols_ = {"echo.v1"}, .require_subprotocol_ = true, .deflate_ = {.enabled_ = true}}));
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> inspect(ruvia::context& c) {
        const auto body = co_await c.req().bytes();
        fluent_client_observation observed;
        observed.set<"method">(c.req().method())
            .set<"body">(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()))
            .set<"query">(c.req().query("escaped name").value_or(""))
            .set<"single_header">(c.req().header("x-owned").value_or(""))
            .set<"span_header">(c.req().header("x-span").value_or(""))
            .set<"authorization">(c.req().header("authorization").value_or(""))
            .set<"proxy_authorization">(c.req().header("proxy-authorization").value_or(""))
            .set<"cookie">(c.req().header("cookie").value_or(""))
            .set<"content_type">(c.req().header("content-type").value_or(""))
            .set<"authority">(c.req().authority().empty() ? c.req().header("host").value_or("") : c.req().authority());
        c.draft().header("x-observed-method", c.req().method());
        c.draft().header("x-observed-user-agent", c.req().header("user-agent").value_or(""));
        if (const auto cookie = c.req().query("set-cookie")) {
            c.draft().header("set-cookie", *cookie);
        }
        co_return c.json(observed);
    }

    ruvia::task<ruvia::http_response> redirect(ruvia::context& c) {
        (void)co_await c.req().bytes();
        c.draft().status(ruvia::http_status_code::from_value(c.req().query<std::uint16_t>("status").value_or(302)));
        c.draft().header("location", c.req().query("location").value_or("/fluent/inspect"));
        co_return c.text("redirect");
    }

    ruvia::task<ruvia::http_response> hold(ruvia::context& c) {
        ++host_cancel_waiters;
        (void)co_await ruvia::sleep_for(c.worker(), std::chrono::seconds(5), c.stop_token());
        co_return c.text("released");
    }

    // Streaming dispatch observes downstream disconnects while the upstream waits.
    ruvia::task<void> host_cancel(ruvia::context& c) {
        ruvia::stop_source independent;
        try {
            auto response = co_await c.client("source")
                                .with_options({.timeout_ = std::chrono::milliseconds(500), .stop_token_ = independent.token()})
                                .get("/fluent/hold")
                                .send();
            (void)co_await response.bytes();
        } catch (const ruvia::http_client_error& error) {
            if (error.code() == std::make_error_code(std::errc::operation_canceled) && !independent.stop_requested()) {
                ++host_cancelled_requests;
            }
        }
        if (c.stop_token().stop_requested()) {
            co_return;
        }
        auto& output = c.stream_text();
        if (co_await output.commit() == ruvia::stream_commit_result::body) {
            co_await output.write("finished");
            co_await output.end();
        }
    }

    ruvia::task<ruvia::http_response> registered_redirect(ruvia::context& c) {
        const auto& targets = *c.worker_state<std::shared_ptr<redirect_destinations>>();
        const auto mode = c.req().query("mode").value_or("cross");
        const auto& target = mode == "unregistered" ? targets.unregistered_ : targets.destination_;
        auto origin = c.client(mode == "downgrade" ? "secure" : "source");
        auto response = co_await origin.post("/fluent/redirect")
                            .query("location", std::string(target) + "/fluent/inspect")
                            .header("authorization", "Bearer source-secret")
                            .header("proxy-authorization", "Basic proxy-secret")
                            .header("cookie", "explicit=source-secret")
                            .header("content-type", "text/plain")
                            .body("source-body")
                            .send();
        c.draft().status(response.status());
        auto body = co_await response.bytes();
        co_return c.body(body.bytes());
    }

    ruvia::task<void> stream_source(ruvia::context& c) {
        c.draft().status(ruvia::http_status::created);
        c.draft().header("x-stream-source", "retained");
        c.draft().header("Content-Type", "application/octet-stream");
        c.draft().header("Date", "Thu, 01 Jan 1970 00:00:00 GMT");
        c.draft().header("Link", "</first>; rel=preload");
        c.draft().header("Link", "</second>; rel=preload", {.mode_ = ruvia::http_response_header_mode::append});
        auto& output = c.stream();
        if (co_await output.commit() == ruvia::stream_commit_result::body) {
            co_await output.write("first");
            co_await output.write("second");
            const std::array trailers{ruvia::http_header_view{"x-finished", "yes"}};
            co_await output.end(trailers);
        }
    }

    ruvia::task<void> pipe(ruvia::context& c) {
        auto response = co_await c.client("source").get("/fluent/stream-source").send();
        auto forwarding = response.pipe_to(c);
        auto moved = std::move(response);
        co_await std::move(forwarding);
        if (!moved.stream().complete()) {
            throw std::runtime_error("response pipe did not consume the moved source");
        }
    }

    // An already-stopped token forwards nothing, so the bounded forward that
    // follows still commits the whole response.
    ruvia::task<void> pipe_bounded(ruvia::context& c) {
        auto response = co_await c.client("source").get("/fluent/stream-source").send();
        ruvia::stop_source stop;
        stop.request_stop();
        bool cancelled = false;
        try {
            co_await response.pipe_to(c, {.stop_token_ = stop.token()});
        } catch (const ruvia::http_client_error& error) {
            cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        if (!cancelled || response.stream().complete()) {
            throw std::runtime_error("stopped response pipe did not fail before forwarding");
        }
        bool rejected = false;
        try {
            (void)response.pipe_to(c, {.timeout_ = std::chrono::milliseconds(0)});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        if (!rejected) {
            throw std::runtime_error("response pipe accepted a non-positive timeout");
        }
        co_await response.pipe_to(c, {.timeout_ = std::chrono::seconds(5)});
    }

    ruvia::task<void> websocket_echo(ruvia::context& c) {
        if (c.req().header("x-owned") != "pooled") {
            throw std::runtime_error("WebSocket builder did not retain its fields");
        }
        if (c.req().query("oversize")) {
            const std::string payload(64, 'm');
            co_await c.websocket().text(payload);
        }
        while (auto message = co_await c.websocket().read()) {
            co_await c.websocket().send(std::move(*message));
        }
    }
};

class server final {
public:
    server()
        : source_port_(io_),
          destination_port_(io_),
          secure_port_(io_),
          unknown_port_(io_),
          identity_("localhost"),
          targets_(std::make_shared<redirect_destinations>()) {
        targets_->source_ = "http://127.0.0.1:" + std::string(std::to_string(source_port_.endpoint().port()));
        targets_->destination_ = "http://127.0.0.1:" + std::string(std::to_string(destination_port_.endpoint().port()));
        targets_->secure_ = "https://localhost:" + std::string(std::to_string(secure_port_.endpoint().port()));
        targets_->unregistered_ = "http://127.0.0.1:" + std::string(std::to_string(unknown_port_.endpoint().port()));
        const auto& key = identity_.private_key_file();
        const std::array listeners{
            ruvia::listen_config{.address_ = "127.0.0.1", .http_ = source_port_.endpoint().port(), .https_ = secure_port_.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity_.ca_file_, .private_key_file_ = key}}},
            ruvia::listen_config{.address_ = "127.0.0.1", .http_ = destination_port_.endpoint().port()}};
        auto& app = ruvia::app();
        app.server({.worker_count_ = 1})
            .listen(std::span<const ruvia::listen_config>(listeners))
            .client({.alias_ = "source", .base_url_ = targets_->source_, .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}, .cookies_ = {{"configured", "source-secret"}}})
            .client({.alias_ = "destination", .base_url_ = targets_->destination_, .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}})
            .client({.alias_ = "secure", .base_url_ = targets_->secure_, .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}, .tls_ = {.ca_file_ = identity_.ca_file_, .server_name_ = "localhost"}});
        app.use_worker_state<std::shared_ptr<redirect_destinations>>([targets = targets_] { return targets; });
        app.on_start([this] {
            ready_.set_value();
        });
        auto ready = ready_.get_future();
        run_.emplace(app);
        ruvia::test::wait_until([&] {
            if (const auto failure = run_->failure()) {
                std::rethrow_exception(failure);
            }
            return ready.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        },
            std::chrono::seconds(10));
        ready.get();
    }

    ~server() = default;

    const redirect_destinations& targets() const noexcept {
        return *targets_;
    }

    ruvia::http_client_config secure_config() const {
        return {.alias_ = "secure", .base_url_ = targets_->secure_, .versions_ = ruvia::http_client_versions{.http2_ = false}, .tls_ = {.ca_file_ = identity_.ca_file_, .server_name_ = "localhost"}};
    }

    void finish() {
        ruvia::app().stop();
        run_->join();
    }

private:
    asio::io_context io_;
    ruvia::test::tcp_port_reservation source_port_;
    ruvia::test::tcp_port_reservation destination_port_;
    ruvia::test::tcp_port_reservation secure_port_;
    ruvia::test::tcp_port_reservation unknown_port_;
    ruvia::test::tls_identity identity_;
    std::shared_ptr<redirect_destinations> targets_;
    std::promise<void> ready_;
    std::optional<ruvia::test::app_run> run_;
};

// The process-wide application keeps its registrations after shutdown. Run
// these scenarios against the fluent test's one configured, live host.
void verify_standalone_origins(server& server);
void verify_request_host_cancellation(server& server);

}  // namespace fluent_client_tests

TEST(Ruvia, http_client_fluent_ownership_helpers_json_and_registered_redirects) {
    using namespace client_handle_and_response_test;
    fluent_client_tests::server server;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = server.targets().source_,
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            const auto origin = client.client();
            auto cold = [&] {
                std::string path("/fluent/inspect");
                std::string name("x-owned");
                std::string value("owned-value");
                std::string query_name("escaped name");
                std::string query_value("a &b=+/%?#\xc3\xa9");
                std::string span_value("span-value");
                std::string payload("owned-payload");
                const std::array fields{ruvia::http_header_view{"x-span", span_value}};
                auto request = origin.post(path);
                request.header(name, value).headers(fields).query(query_name, query_value).body(std::as_bytes(std::span(payload)));
                path.assign("/changed");
                name.assign("x-changed");
                value.assign("changed");
                query_name.assign("changed");
                query_value.assign("changed");
                span_value.assign("changed");
                payload.assign("changed");
                return request.send();
            }();
            auto response = co_await std::move(cold);
            EXPECT_TRUE(response.ok());
            auto observed = co_await response.json<fluent_client_observation>();
            EXPECT_EQ(observed.get<"method">().view(), "POST");
            EXPECT_EQ(observed.get<"body">().view(), "owned-payload");
            EXPECT_EQ(observed.get<"query">().view(), "a &b=+/%?#\xc3\xa9");
            EXPECT_EQ(observed.get<"single_header">().view(), "owned-value");
            EXPECT_EQ(observed.get<"span_header">().view(), "span-value");
            EXPECT_TRUE((co_await response.bytes()).empty());
            EXPECT_TRUE(!(co_await response.read()));
            bool replay_rejected = false;
            try {
                (void)co_await response.json<fluent_client_observation>();
            } catch (const std::exception&) {
                replay_rejected = true;
            }
            EXPECT_TRUE(replay_rejected);
            EXPECT_TRUE(ruvia::test::throws_on([&] {
                auto consumed = origin.get("/fluent/inspect");
                auto first = consumed.send();
                (void)consumed.send();
            }));

            auto temporary = origin.post(std::string("/fluent/inspect"))
                                 .body(std::string("temporary-body"))
                                 .send();
            auto temporary_response = co_await std::move(temporary);
            auto temporary_observed = co_await temporary_response.json<fluent_client_observation>();
            EXPECT_EQ(temporary_observed.get<"body">().view(), "temporary-body");
            {
                auto piped = co_await origin.get("/fluent/pipe").send();
                EXPECT_TRUE(piped.status() == ruvia::http_status::created);
                EXPECT_TRUE(piped.header("x-stream-source") == "retained");
                EXPECT_TRUE(piped.header("content-type") == "application/octet-stream");
                EXPECT_TRUE(piped.header("date") == "Thu, 01 Jan 1970 00:00:00 GMT");
                std::size_t links = 0;
                for (const auto& field : piped.headers()) {
                    if (ruvia::http_ascii_equals_ignore_case(field.name(), "link")) {
                        EXPECT_EQ(field.value(), links == 0 ? "</first>; rel=preload" : "</second>; rel=preload");
                        ++links;
                    }
                }
                EXPECT_EQ(links, std::size_t{2});
                const auto contents = co_await piped.text();
                EXPECT_EQ(contents.text(), "firstsecond");
                EXPECT_TRUE(piped.trailer("x-finished") == "yes");
                auto bounded = co_await origin.get("/fluent/pipe-bounded").send();
                EXPECT_TRUE(bounded.status() == ruvia::http_status::created);
                EXPECT_TRUE(bounded.header("x-stream-source") == "retained");
                const auto bounded_contents = co_await bounded.text();
                EXPECT_EQ(bounded_contents.text(), "firstsecond");
                EXPECT_TRUE(bounded.trailer("x-finished") == "yes");
            }

            auto json_request = [&] {
                const auto encoded = ruvia::to_json(observed);
                auto model = ruvia::from_json<fluent_client_observation>(encoded);
                return origin.post("/fluent/inspect").json(model).send();
            }();
            auto json_response = co_await std::move(json_request);
            auto json_observed = co_await json_response.json<fluent_client_observation>();
            EXPECT_EQ(json_observed.get<"content_type">().view(), "application/json");
            auto echoed_model = ruvia::from_json<fluent_client_observation>(json_observed.get<"body">().view());
            EXPECT_EQ(echoed_model.get<"body">().view(), "owned-payload");

            using method_factory = ruvia::http_client_request (ruvia::http_client_ref::*)(std::string_view) const;
            const std::array<std::pair<std::string_view, method_factory>, 7> methods{{
                {"GET", &ruvia::http_client_ref::get},
                {"POST", &ruvia::http_client_ref::post},
                {"PUT", &ruvia::http_client_ref::put},
                {"PATCH", &ruvia::http_client_ref::patch},
                {"DELETE", &ruvia::http_client_ref::del},
                {"HEAD", &ruvia::http_client_ref::head},
                {"OPTIONS", &ruvia::http_client_ref::options},
            }};
            for (const auto& [method, make] : methods) {
                auto result = co_await (origin.*make)("/fluent/inspect").send();
                EXPECT_TRUE(result.ok());
                EXPECT_TRUE(result.header("x-observed-method") == method);
                auto body = co_await result.text();
                if (method == "HEAD") {
                    EXPECT_TRUE(body.empty());
                } else {
                    auto report = ruvia::from_json<fluent_client_observation>(body.text());
                    EXPECT_EQ(report.get<"method">().view(), method);
                }
            }

            struct redirect_case final {
                std::string_view status_;
                std::string_view method_;
                std::string_view expected_method_;
                bool keep_body_;
            };
            for (const auto test : std::array{
                     redirect_case{"301", "POST", "GET", false},
                     redirect_case{"302", "POST", "GET", false},
                     redirect_case{"303", "PUT", "GET", false},
                     redirect_case{"307", "POST", "POST", true},
                     redirect_case{"308", "PUT", "PUT", true}}) {
                auto result = co_await origin.request(test.method_, "/fluent/redirect")
                                  .query("status", test.status_)
                                  .header("authorization", "Bearer same-origin")
                                  .header("content-type", "text/plain")
                                  .body("redirect-body")
                                  .send();
                EXPECT_TRUE(result.ok());
                auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"method">().view(), test.expected_method_);
                EXPECT_EQ(report.get<"body">().view(), test.keep_body_ ? "redirect-body" : "");
                EXPECT_EQ(report.get<"content_type">().view(), test.keep_body_ ? "text/plain" : "");
                EXPECT_EQ(report.get<"authorization">().view(), "Bearer same-origin");
            }
            {
                auto result = co_await origin.head("/fluent/redirect").query("status", "303").send();
                EXPECT_TRUE(result.ok());
                EXPECT_TRUE(result.header("x-observed-method") == "HEAD");
                EXPECT_TRUE((co_await result.bytes()).empty());
            }
            {
                auto result = co_await origin.get("/fluent/redirect").redirects(0).send();
                EXPECT_TRUE(!result.ok());
                EXPECT_TRUE(result.status() == ruvia::http_status::found);
                auto text = co_await result.text();
                EXPECT_EQ(text.text(), "redirect");
            }
            {
                auto result = co_await origin.get("/fluent/registered").send();
                EXPECT_TRUE(result.ok());
                auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"method">().view(), "GET");
                EXPECT_TRUE(report.get<"body">().view().empty());
                EXPECT_TRUE(report.get<"authorization">().view().empty());
                EXPECT_TRUE(report.get<"proxy_authorization">().view().empty());
                EXPECT_TRUE(report.get<"cookie">().view().empty());
                EXPECT_TRUE(report.get<"content_type">().view().empty());
                EXPECT_EQ(report.get<"authority">().view(),
                    std::string_view(server.targets().destination_).substr(std::string_view("http://").size()));
            }
            for (const auto mode : {"unregistered", "downgrade"}) {
                auto result = co_await origin.get("/fluent/registered").query("mode", mode).redirects(0).send();
                EXPECT_TRUE(!result.ok());
                EXPECT_TRUE(result.status() == ruvia::http_status::found);
                auto text = co_await result.text();
                EXPECT_EQ(text.text(), "redirect");
            }
            for (const auto protocol : {
                     ruvia::http_client_versions{.http2_ = false},
#if defined(RUVIA_ENABLE_HTTP2)
                     ruvia::http_client_versions{.http1_ = false},
#endif
                 }) {
                ruvia::http_client websocket_client(attachment.loop(),
                    {.base_url_ = server.targets().source_,
                        .request_timeout_ = std::chrono::seconds(1),
                        .versions_ = protocol});
                std::exception_ptr websocket_failure;
                try {
                    auto connecting = [&] {
                        const std::string path("/fluent/websocket");
                        const std::string value("pooled");
                        const std::string subprotocol("echo.v1");
                        const std::array fields{ruvia::http_header_view{"x-owned", value}};
                        return websocket_client.client().websocket(path).headers(fields).subprotocol(subprotocol).deflate({.compression_ = {.enabled_ = true}}).connect();
                    }();
                    auto connected = co_await std::move(connecting);
                    auto session = std::move(connected);
                    EXPECT_EQ(session.subprotocol(), "echo.v1");
                    // Accepted WebSockets are not ordinary finite HTTP responses.
                    // The HTTP exchange deadline must not become their lifetime.
                    EXPECT_TRUE(co_await ruvia::sleep_for(session.worker(), std::chrono::milliseconds(1250)) ==
                                ruvia::timer_sleep_result::elapsed);
                    EXPECT_TRUE(session.connected());
                    const std::string payload(32768, 'w');
                    for (const bool compress : {false, true}) {
                        auto writing = [&] {
                            const std::string owned(payload);
                            return compress ? session.binary(std::as_bytes(std::span(owned)), {.compress_ = true})
                                            : session.text(owned, {.compress_ = false});
                        }();
                        co_await std::move(writing);
                        const auto message = co_await session.read();
                        EXPECT_TRUE(message.has_value());
                        if (message) {
                            EXPECT_EQ(message->payload(), payload);
                            EXPECT_TRUE(message->binary() == compress);
                            EXPECT_TRUE(message->text() != compress);
                        }
                    }
                    co_await session.close({.code_ = ruvia::websocket_close_status::normal_closure, .reason_ = "pooled"});
                    EXPECT_TRUE(!(co_await session.read()));
                    {
                        auto timed = co_await websocket_client.client().websocket("/fluent/websocket").header("x-owned", "pooled").subprotocol("echo.v1").connect();
                        bool timed_out = false;
                        try {
                            (void)co_await timed.read({.timeout_ = std::chrono::milliseconds(20)});
                        } catch (const std::system_error& error) {
                            timed_out = error.code() == std::errc::timed_out;
                        }
                        EXPECT_TRUE(timed_out);
                        // A read deadline ends only that read: the session keeps
                        // its buffered input and serves the next read.
                        EXPECT_TRUE(timed.connected());
                        co_await timed.text("after-timeout");
                        const auto echoed = co_await timed.read({.timeout_ = std::chrono::seconds(5)});
                        EXPECT_TRUE(echoed.has_value() && echoed->payload() == "after-timeout");
                        EXPECT_TRUE(timed.termination() == ruvia::websocket_termination::none);
                        co_await timed.close();
                        EXPECT_TRUE(timed.termination() == ruvia::websocket_termination::local_close);
                    }
                    {
                        auto limited = co_await websocket_client.client().websocket("/fluent/websocket").header("x-owned", "pooled").subprotocol("echo.v1").max_message_bytes(32).connect();
                        bool rejected = false;
                        try {
                            co_await limited.text(std::string(33, 'm'));
                        } catch (const std::invalid_argument&) {
                            rejected = true;
                        }
                        EXPECT_TRUE(rejected);
                        EXPECT_TRUE(limited.connected());
                        limited.abort();
                    }
                    {
                        auto limited = co_await websocket_client.client().websocket("/fluent/websocket?oversize=1").header("x-owned", "pooled").subprotocol("echo.v1").max_message_bytes(32).connect();
                        EXPECT_FALSE((co_await limited.read()).has_value());
                        EXPECT_TRUE(limited.termination() == ruvia::websocket_termination::message_too_large);
                        limited.abort();
                    }
                    auto active = co_await websocket_client.client().websocket("/fluent/websocket").header("x-owned", "pooled").subprotocol("echo.v1").connect();
                    const auto worker = attachment.loop().worker();
                    ruvia::task_scope readers(worker);
                    bool cancelled = false;
                    const auto receive = [&]() -> ruvia::task<void> {
                        try {
                            cancelled = !(co_await active.read()).has_value();
                        } catch (const std::system_error&) {
                            cancelled = true;
                        }
                    };
                    readers.spawn(receive());
                    co_await websocket_client.shutdown();
                    co_await readers.join();
                    EXPECT_TRUE(cancelled);
                    EXPECT_TRUE(!active.connected());
                    EXPECT_TRUE(!session.connected());
                    EXPECT_TRUE(session.peer_close_code() == std::uint16_t{1000});
                    EXPECT_EQ(session.peer_close_reason(), "pooled");
                } catch (...) {
                    websocket_failure = std::current_exception();
                }
                co_await websocket_client.shutdown();
                if (websocket_failure) {
                    std::rethrow_exception(websocket_failure);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (!failure) {
        fluent_client_tests::verify_standalone_origins(server);
        fluent_client_tests::verify_request_host_cancellation(server);
    }
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void fluent_client_tests::verify_standalone_origins(server& server) {
    SCOPED_TRACE("standalone origins, isolated policies and shared budget");
    auto& io = ruvia::test::new_test_io_context();
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &lookups});
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        auto secure = server.secure_config();
        auto untrusted = secure;
        untrusted.alias_ = "untrusted";
        untrusted.tls_ = {};
        const std::array configs{
            ruvia::http_client_config{.base_url_ = server.targets().source_,
                .versions_ = ruvia::http_client_versions{.http2_ = false},
                .received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send,
                .user_agent_ = "source-agent",
                .cookies_ = {{"configured", "source"}}},
            ruvia::http_client_config{.alias_ = "destination", .base_url_ = server.targets().destination_, .versions_ = ruvia::http_client_versions{.http2_ = false}, .user_agent_ = "destination-agent", .cookies_ = {{"configured", "destination"}}},
            secure, untrusted};
        ruvia::http_client client(attachment.loop(), std::span<const ruvia::http_client_config>(configs));
        try {
            const auto source = client.client();
            const auto destination = client.client("destination");
            for (const auto status : {"302", "307"}) {
                auto result = co_await source.post("/fluent/redirect")
                                  .query("status", status)
                                  .query("location", std::string(server.targets().destination_) + "/fluent/inspect")
                                  .header("authorization", "Bearer source-secret")
                                  .header("proxy-authorization", "Basic proxy-secret")
                                  .header("cookie", "explicit=source-secret")
                                  .header("content-type", "text/plain")
                                  .body("source-body")
                                  .send();
                EXPECT_TRUE(result.ok());
                EXPECT_TRUE(result.header("x-observed-user-agent") == "destination-agent");
                const auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"method">().view(), std::string_view(status) == "302" ? "GET" : "POST");
                EXPECT_EQ(report.get<"body">().view(), std::string_view(status) == "302" ? "" : "source-body");
                EXPECT_EQ(report.get<"content_type">().view(), std::string_view(status) == "302" ? "" : "text/plain");
                EXPECT_EQ(report.get<"authorization">().view(), "");
                EXPECT_EQ(report.get<"proxy_authorization">().view(), "");
                EXPECT_EQ(report.get<"cookie">().view(), "configured=destination");
                EXPECT_EQ(report.get<"authority">().view(),
                    std::string_view(server.targets().destination_).substr(7));
            }
            for (const auto& origin : {source, destination}) {
                auto set = co_await origin.get("/fluent/inspect").query("set-cookie", "received=retained; Path=/").send();
                (void)co_await set.bytes();
            }
            {
                auto result = co_await source.get("/fluent/inspect").send();
                EXPECT_TRUE(result.header("x-observed-user-agent") == "source-agent");
                const auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"cookie">().view(), "configured=source; received=retained");
            }
            {
                auto result = co_await destination.get("/fluent/inspect").send();
                const auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"cookie">().view(), "configured=destination");
            }
            {
                auto result = co_await source.get("/fluent/redirect")
                                  .query("location", std::string(server.targets().unregistered_) + "/fluent/inspect")
                                  .send();
                EXPECT_TRUE(result.status() == ruvia::http_status::found);
                const auto text = co_await result.text();
                EXPECT_EQ(text.text(), "redirect");
            }
            {
                auto result = co_await client.client("secure").get("/fluent/redirect").query("location", std::string(server.targets().destination_) + "/fluent/inspect").send();
                EXPECT_TRUE(result.status() == ruvia::http_status::found);
                const auto text = co_await result.text();
                EXPECT_EQ(text.text(), "redirect");
            }
            bool tls_rejected = false;
            try {
                (void)co_await client.client("untrusted").get("/fluent/inspect").send();
            } catch (const ruvia::http_client_error& error) {
                tls_rejected = error.code() == ruvia::make_error_code(ruvia::http_client_errc::tls_failed);
            }
            EXPECT_TRUE(tls_rejected);
            bool unknown_rejected = false;
            try {
                (void)client.client("unknown");
            } catch (const ruvia::http_client_error& error) {
                unknown_rejected = error.code() == ruvia::make_error_code(ruvia::http_client_errc::not_configured);
            }
            EXPECT_TRUE(unknown_rejected);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (!failure) {
            const std::span<const ruvia::http_client_config> origins(configs.data(), 2);
            ruvia::http_client bounded(attachment.loop(), origins, {.result_budget_ = {.max_retained_bytes_ = 16}});
            try {
                auto first = co_await bounded.client().get("/fluent/stream-source").send();
                const auto retained = co_await first.bytes(11);
                EXPECT_EQ(retained.text(), "firstsecond");
                auto second = co_await bounded.client("destination").get("/fluent/stream-source").send();
                bool exhausted = false;
                try {
                    (void)co_await second.bytes(11);
                } catch (const ruvia::http_client_error& error) {
                    exhausted = error.code() == ruvia::make_error_code(ruvia::http_client_errc::result_budget_exceeded);
                }
                EXPECT_TRUE(exhausted);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await bounded.shutdown();
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void fluent_client_tests::verify_request_host_cancellation(server& server) {
    SCOPED_TRACE("user options preserve request-host cancellation");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client owner(attachment.loop(), {.base_url_ = server.targets().source_,
                                                        .request_timeout_ = std::chrono::seconds(2),
                                                        .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            const auto before_waiters = fluent_client_tests::host_cancel_waiters.load();
            const auto before_cancelled = fluent_client_tests::host_cancelled_requests.load();
            ruvia::stop_source stop;
            bool cancelled = false;
            const auto request = [&]() -> ruvia::task<void> {
                try {
                    (void)co_await owner.client().with_options({.stop_token_ = stop.token()}).get("/fluent/host-cancel").send();
                } catch (const ruvia::http_client_error& error) {
                    cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                }
            };
            ruvia::task_scope tasks(owner.worker());
            tasks.spawn(request());
            const auto started = std::chrono::steady_clock::now();
            while (fluent_client_tests::host_cancel_waiters.load() == before_waiters &&
                   std::chrono::steady_clock::now() - started < std::chrono::seconds(1)) {
                (void)co_await ruvia::sleep_for(owner.worker(), std::chrono::milliseconds(1));
            }
            EXPECT_GT(fluent_client_tests::host_cancel_waiters.load(), before_waiters);
            stop.request_stop();
            co_await tasks.join();
            EXPECT_TRUE(cancelled);
            const auto stopped = std::chrono::steady_clock::now();
            while (fluent_client_tests::host_cancelled_requests.load() == before_cancelled &&
                   std::chrono::steady_clock::now() - stopped < std::chrono::seconds(1)) {
                (void)co_await ruvia::sleep_for(owner.worker(), std::chrono::milliseconds(1));
            }
            EXPECT_GT(fluent_client_tests::host_cancelled_requests.load(), before_cancelled);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await owner.shutdown();
    };
    ruvia::test::run_worker_tasks(attachment, run());
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http_client_unconsumed_builders_outlive_shutdown_and_client_storage) {
    using namespace client_handle_and_response_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_request> request;
        std::optional<ruvia::http_client_connect_request> connect;
        std::optional<ruvia::http_client_websocket_request> websocket;
        {
            ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:1"});
            {
                const std::string path("/" + std::string(2048, 'p'));
                const std::string value(16384, 'v');
                const std::string body(32768, 'b');
                const std::string authority(std::string(63, 'a') + "." + std::string(63, 'b') + ".test:443");
                const auto origin = client.client();
                request.emplace(origin.post(path));
                request->header("x-owned", value).query("owned", path).body(body);
                connect.emplace(origin.connect(authority));
                connect->protocol("owned-protocol").target(path).header("x-owned", value);
                websocket.emplace(origin.websocket(path));
                websocket->header("x-owned", value).subprotocol(std::string(256, 's'));
            }
            co_await client.shutdown();
        }
        try {
            EXPECT_TRUE(throws_logic_error([&] { (void)request->send(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)connect->open(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)websocket->connect(); }));
            request.reset();
            connect.reset();
            websocket.reset();
        } catch (...) {
            failure = std::current_exception();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
