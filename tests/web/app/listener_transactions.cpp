#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <future>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>

#include <cerrno>
#endif

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/server_connection.h"

#include "app_run.h"
#include "raw_http_peer.h"
#include "tcp_port_reservation.h"
#include "test_tls_identity.h"

namespace listener_transactions_test {

using namespace std::chrono_literals;

struct control_runtime final {
    ruvia::blocking_pool blocking_{{.thread_count_ = 1}};
    ruvia::event_loop_pool loops_{{.loop_count_ = 1, .blocking_pool_ = &blocking_}};

    control_runtime() {
        loops_.start();
    }
    ~control_runtime() {
        loops_.join();
    }
};

control_runtime& controls() {
    static control_runtime value;
    return value;
}

ruvia::event_loop control_loop() {
    return controls().loops_.loop(0);
}

std::binary_semaphore started{0};

class transaction_controller final {
public:
    RUVIA_ROUTES_BEGIN(transaction_controller)
    RUVIA_GET("/ready", ready);
    RUVIA_GET("/limited", ready, ruvia::rate_limit<1, 600000>);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> ready(ruvia::context& context) {
        co_return context.text("ready");
    }
};

ruvia::task<void> serve_connection(ruvia::server_connection& connection) {
    (void)co_await connection.read();
    constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nready";
    co_await connection.write(std::as_bytes(std::span(response.data(), response.size())));
    co_await connection.finish();
}

void wait_until_running(ruvia::application& app) {
    ruvia::test::wait_until([&app] {
        try {
            control_loop().start(app.remove_listener(control_loop().handle(), {.address_ = "0.0.0.0", .port_ = 9})).get();
        } catch (const std::invalid_argument&) {
            return true;
        } catch (const std::logic_error&) {
        }
        return false;
    },
        10s);
}

std::pmr::string request(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view path = "/ready") {
    const auto wire = std::string("GET ") + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    return ruvia::test::exchange(io, endpoint, wire);
}

ruvia::native_socket inherited_socket(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ip::tcp::acceptor socket(io);
    socket.open(asio::ip::tcp::v4());
#if !defined(_WIN32)
    socket.set_option(asio::socket_base::reuse_address(true));
    const int enabled = 1;
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, std::generic_category(), "cannot share inherited test port");
    }
#endif
    socket.bind(endpoint);
    socket.listen();
    return socket.release();
}

bool closed(ruvia::native_socket socket) {
#if defined(_WIN32)
    int value = 0;
    int length = sizeof(value);
    return ::getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&value), &length) == SOCKET_ERROR &&
           ::WSAGetLastError() == WSAENOTSOCK;
#else
    return ::fcntl(socket, F_GETFD) == -1 && errno == EBADF;
#endif
}

}  // namespace listener_transactions_test

using namespace listener_transactions_test;

TEST(Ruvia, adding_first_routed_listener_builds_controllers_and_route_rate_limits_for_the_existing_workers) {
    asio::io_context io;
    ruvia::test::tcp_port_reservation raw(io);
    ruvia::test::tcp_port_reservation routed(io);
    auto& app = ruvia::app();
    app.server({.worker_count_ = 1})
        .listen({.address_ = "127.0.0.1", .http_ = raw.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection})
        .rate_limit(nullptr)
        .on_connection(&serve_connection)
        .on_start(nullptr)
        .on_start([] { started.release(); });
    ruvia::test::app_run run(app);
    ASSERT_TRUE(started.try_acquire_for(10s));
    wait_until_running(app);
    control_loop().start(app.add_listener(control_loop().handle(), {.address_ = "127.0.0.1", .http_ = routed.endpoint().port()})).get();
    EXPECT_TRUE(request(io, routed.endpoint()).ends_with("ready"));
    EXPECT_TRUE(request(io, routed.endpoint(), "/limited").starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(request(io, routed.endpoint(), "/limited").starts_with("HTTP/1.1 429 "));
    EXPECT_TRUE(request(io, raw.endpoint()).ends_with("ready"));
    app.stop();
    run.join();
}

TEST(Ruvia, add_listener_prepare_failure_discards_already_adopted_sockets_without_interrupting_service) {
    const ruvia::test::tls_identity identity("listener-rollback", "DNS:localhost");
    asio::io_context io;
    ruvia::test::tcp_port_reservation original(io);
    ruvia::test::tcp_port_reservation addition(io);
    asio::ip::tcp::acceptor blocker(io, {asio::ip::address_v4::loopback(), 0});
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .http_ = original.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection})
        .on_connection(&serve_connection)
        .on_start(nullptr)
        .on_start([] { started.release(); });
    ruvia::test::app_run run(app);
    ASSERT_TRUE(started.try_acquire_for(10s));
    wait_until_running(app);
    ruvia::listen_config config{
        .address_ = "127.0.0.1",
        .http_ = addition.endpoint().port(),
        .https_ = blocker.local_endpoint().port(),
        .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}},
        .dispatch_ = ruvia::listener_dispatch::connection,
        .inherited_sockets_ = ruvia::listener_sockets{},
    };
#if defined(_WIN32)
    constexpr std::size_t socket_count = 1;
#else
    constexpr std::size_t socket_count = 2;
#endif
    for (std::size_t index = 0; index < socket_count; ++index) {
        config.inherited_sockets_->http_.push_back(inherited_socket(io, addition.endpoint()));
    }
    const auto sockets = config.inherited_sockets_->http_;
    EXPECT_THROW(control_loop().start(app.add_listener(control_loop().handle(), config)).get(), std::system_error);
    for (const auto socket : sockets) {
        EXPECT_TRUE(closed(socket));
    }
    EXPECT_TRUE(request(io, original.endpoint()).ends_with("ready"));
    EXPECT_THROW(control_loop().start(app.remove_listener(control_loop().handle(), {.address_ = "127.0.0.1", .port_ = addition.endpoint().port()})).get(), std::invalid_argument);
    blocker.close();
    config.inherited_sockets_.reset();
    control_loop().start(app.add_listener(control_loop().handle(), std::move(config))).get();
    EXPECT_TRUE(request(io, addition.endpoint()).ends_with("ready"));
    EXPECT_TRUE(request(io, original.endpoint()).ends_with("ready"));
    app.stop();
    run.join();
}

TEST(Ruvia, mixed_listener_adopts_one_physical_socket_group_for_plain_and_tls_raw_dispatch) {
    const ruvia::test::tls_identity identity("mixed-adopted", "DNS:localhost");
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    ruvia::listen_config config{
        .address_ = "127.0.0.1",
        .http_ = endpoint.port(),
        .https_ = endpoint.port(),
        .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}},
        .dispatch_ = ruvia::listener_dispatch::connection,
        .inherited_sockets_ = ruvia::listener_sockets{},
    };
#if defined(_WIN32)
    constexpr std::size_t socket_count = 1;
#else
    constexpr std::size_t socket_count = 2;
#endif
    for (std::size_t index = 0; index < socket_count; ++index) {
        config.inherited_sockets_->https_.push_back(inherited_socket(io, endpoint));
    }
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2}).listen(std::move(config)).on_connection(&serve_connection).on_start(nullptr).on_start([] { started.release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started.try_acquire_for(10s));
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.load_verify_file(identity.ca_file_.string());
    client.set_verify_mode(asio::ssl::verify_peer);
    for (int attempt = 0; attempt < 8; ++attempt) {
        EXPECT_TRUE(request(io, endpoint).ends_with("ready"));
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
        stream.next_layer().connect(endpoint);
        stream.handshake(asio::ssl::stream_base::client);
        asio::write(stream, asio::buffer(std::string_view(
                                "GET /ready HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")));
        EXPECT_TRUE(ruvia::test::read_to_end(stream).ends_with("ready"));
    }
    app.stop();
    runner.join();
}

TEST(Ruvia, runtime_tls_listener_preparation_does_not_block_its_caller_worker) {
    const ruvia::test::tls_identity identity("async-addition", "DNS:localhost");
    asio::io_context io;
    ruvia::test::tcp_port_reservation original(io), secure(io);
    auto& app = ruvia::app();
    app.server({.worker_count_ = 1})
        .listen({.address_ = "127.0.0.1", .http_ = original.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection})
        .on_connection(&serve_connection)
        .on_start(nullptr)
        .on_start([] { started.release(); });
    ruvia::test::app_run run(app);
    ASSERT_TRUE(started.try_acquire_for(10s));
    wait_until_running(app);

    std::binary_semaphore entered{0}, release{0};
    ASSERT_EQ(controls().blocking_.submit([&] {
        entered.release();
        release.acquire();
    }),
        ruvia::blocking_submit_status::accepted);
    entered.acquire();

    const auto loop = control_loop();
    auto addition = loop.start(app.add_listener(loop.handle(),
        {.address_ = "127.0.0.1", .https_ = secure.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::connection}));
    std::promise<void> heartbeat;
    auto observed = heartbeat.get_future();
    const auto posted = loop.handle().post([&heartbeat] { heartbeat.set_value(); });
    EXPECT_EQ(posted, ruvia::post_status::accepted);
    if (posted == ruvia::post_status::accepted) {
        EXPECT_EQ(observed.wait_for(1s), std::future_status::ready);
        EXPECT_EQ(controls().blocking_.stats().queued_, 1U);
    }
    EXPECT_TRUE(request(io, original.endpoint()).ends_with("ready"));
    release.release();
    if (posted == ruvia::post_status::accepted) {
        observed.wait();
    }
    addition.get();

    asio::ssl::context client(asio::ssl::context::tls_client);
    client.load_verify_file(identity.ca_file_.string());
    client.set_verify_mode(asio::ssl::verify_peer);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    stream.next_layer().connect(secure.endpoint());
    stream.handshake(asio::ssl::stream_base::client);
    asio::write(stream, asio::buffer(std::string_view(
                            "GET /ready HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")));
    EXPECT_TRUE(ruvia::test::read_to_end(stream).ends_with("ready"));
    app.stop();
    run.join();
}
