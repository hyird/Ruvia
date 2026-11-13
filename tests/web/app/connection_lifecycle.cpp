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

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_server.h"

#include "http_server_fixture.h"
#if defined(RUVIA_ENABLE_HTTP2)
#include "http_server_fixture.h"
#endif

#include "http_server_fixture.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3_test_client.h"
#endif

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

using namespace connection_lifecycle_test;

TEST(Ruvia, connection_admission_rejects_before_tcp_tls_or_quic_handshake) {
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
