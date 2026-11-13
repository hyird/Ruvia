#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/objects.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"

#include "http_server_fixture.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include <asio/ip/udp.hpp>

#include "http_server_fixture.h"
#endif

#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <span>

#include <asio/read.hpp>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>

#include <cerrno>
#endif
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <semaphore>
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
#include <asio/socket_base.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/middleware.h"
#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/http2_connection.h"
#endif
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/middleware.h"

#include "http_server_fixture.h"
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#include <atomic>
#include <cstddef>
#include <functional>
#include <future>
#include <map>
#include <semaphore>
#include <span>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http_client.h"
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis.h"

#include "http_server_fixture.h"
#endif
#if !defined(_WIN32)
#include <fcntl.h>

#include <cerrno>
#endif
#if defined(RUVIA_ENABLE_HTTP2)
#include "http_server_fixture.h"
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include "http_server_fixture.h"
#endif
#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/http2_connection.h"

#include "http_server_fixture.h"
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include "http_server_fixture.h"
#endif
namespace listener_sockets_test {

using namespace std::chrono_literals;
using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;

ruvia::event_loop control_loop() {
    struct runtime final {
        ruvia::blocking_pool blocking_{{.thread_count_ = 2}};
        ruvia::event_loop_pool loops_{{.loop_count_ = 1, .blocking_pool_ = &blocking_}};

        runtime() {
            loops_.start();
        }
        ~runtime() {
            loops_.join();
        }
    };
    static runtime value;
    return value.loops_.loop(0);
}

std::atomic<std::uint16_t>& control_addition_port() {
    static std::atomic<std::uint16_t> value{0};
    return value;
}

std::binary_semaphore& started() {
    static std::binary_semaphore value{0};
    return value;
}

// The endpoint a route duplicates from a business worker.
std::atomic<std::uint16_t>& controlled_port() {
    static std::atomic<std::uint16_t> value{0};
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

// on_start hooks run before the app serves; the runtime listener controls
// need it running, which an unknown endpoint then reports as such.
void wait_until_running(ruvia::application& app) {
    while (failure_of([&app] { control_loop().start(app.remove_listener(control_loop().worker(), {.address_ = "0.0.0.0", .port_ = 9})).get(); }) != "invalid_argument") {
        std::this_thread::sleep_for(1ms);
    }
}

class listener_controller final {
public:
    RUVIA_ROUTES_BEGIN(listener_controller)
    RUVIA_GET("/hello", hello);
    RUVIA_GET("/control", control);
    RUVIA_GET("/listener-worker", worker);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> hello(ruvia::context& context) {
        co_return context.text(std::string_view("hello"));
    }

    ruvia::task<ruvia::http_response> worker(ruvia::context& context) {
        co_return context.text(std::to_string(context.worker().id()));
    }

    // Runtime controls suspend instead of waiting on the current business worker.
    ruvia::task<ruvia::http_response> control(ruvia::context& context) {
        const ruvia::listener_endpoint endpoint{.address_ = "127.0.0.1", .port_ = controlled_port().load()};
#if !defined(_WIN32)
        auto sockets = co_await ruvia::app().duplicate_listener_sockets(context.worker(), endpoint);
        for (const auto* service : {&sockets.http_, &sockets.https_, &sockets.http3_}) {
            for (const auto fd : *service) {
                (void)::close(fd);
            }
        }
#endif
        const auto port = control_addition_port().load();
        co_await ruvia::app().add_listener(context.worker(), {.address_ = "127.0.0.1", .http_ = port});
        co_await ruvia::app().remove_listener(context.worker(), {.address_ = "127.0.0.1", .port_ = port});
#if defined(_WIN32)
        co_return context.text("added removed");
#else
        co_return context.text("added removed duplicated");
#endif
    }
};

asio::ip::tcp::endpoint loopback(std::uint16_t port) {
    return {asio::ip::address_v4::loopback(), port};
}

ruvia::tls_config tls_of(const ruvia::test::tls_identity& identity) {
    return {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}};
}

// The body of GET `target` over a fresh connection; empty when none came.
std::string plain_body(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target) {
    asio::ip::tcp::socket socket(io);
    asio::error_code error;
    socket.connect(endpoint, error);
    const auto request = "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (!error) {
        asio::write(socket, asio::buffer(request), error);
    }
    std::string response;
    std::array<char, 1024> buffer;
    while (!error) {
        const auto size = socket.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    const auto body = response.find("\r\n\r\n");
    return body == std::string::npos ? std::string() : response.substr(body + 4);
}

// Nothing listens on the endpoint any more.
bool refuses(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ip::tcp::socket socket(io);
    asio::error_code error;
    socket.connect(endpoint, error);
    return error == asio::error::connection_refused;
}

// One keep-alive GET /hello on an established connection.
template <typename stream_type>
bool serves_hello(stream_type& stream) {
    asio::error_code error;
    asio::write(stream, asio::buffer(std::string_view("GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n")), error);
    std::string response;
    if (!error) {
        asio::read_until(stream, asio::dynamic_buffer(response), "\r\n\r\nhello", error);
    }
    return !error && response.starts_with("HTTP/1.1 200 ");
}

// Authenticates nothing: tests read the certificate they were served.
asio::ssl::context tls_client() {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    return client;
}

std::unique_ptr<tls_stream> connect_tls(
    asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint) {
    auto stream = std::make_unique<tls_stream>(io, client);
    if (SSL_set_tlsext_host_name(stream->native_handle(), "localhost") != 1) {
        throw std::runtime_error("cannot configure the TLS test connection");
    }
    stream->next_layer().connect(endpoint);
    stream->handshake(asio::ssl::stream_base::client);
    return stream;
}

// The subject CN a fresh handshake is served, after which GET /hello
// succeeds; empty when either fails.
std::string https_served_name(asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint) {
    try {
        const auto stream = connect_tls(io, client, endpoint);
        const std::unique_ptr<X509, decltype(&X509_free)> peer(SSL_get1_peer_certificate(stream->native_handle()), X509_free);
        std::array<char, 256> name{};
        const auto size = peer ? X509_NAME_get_text_by_NID(X509_get_subject_name(peer.get()), NID_commonName, name.data(),
                                     static_cast<int>(name.size()))
                               : -1;
        if (size < 0 || !serves_hello(*stream)) {
            return {};
        }
        return std::string(name.data(), static_cast<std::size_t>(size));
    } catch (const std::exception&) {
        return {};
    }
}

// Split the TLS record prefix without introducing a second TLS implementation.
// asio's ordinary TLS driver still writes and reads every handshake message.
class fragmented_tcp final {
public:
    using lowest_layer_type = asio::ip::tcp::socket;
    using executor_type = lowest_layer_type::executor_type;

    explicit fragmented_tcp(asio::ip::tcp::socket socket)
        : socket_(std::move(socket)) {}

    executor_type get_executor() noexcept {
        return socket_.get_executor();
    }
    lowest_layer_type& lowest_layer() noexcept {
        return socket_;
    }
    const lowest_layer_type& lowest_layer() const noexcept {
        return socket_;
    }

    template <typename buffers_type>
    std::size_t read_some(const buffers_type& buffers, asio::error_code& error) {
        return socket_.read_some(buffers, error);
    }

    template <typename buffers_type>
    std::size_t write_some(const buffers_type& buffers, asio::error_code& error) {
        if (prefix_bytes_ != 0) {
            --prefix_bytes_;
            std::this_thread::sleep_for(2ms);
            const auto first = *asio::buffer_sequence_begin(buffers);
            return socket_.write_some(asio::buffer(first, 1), error);
        }
        return socket_.write_some(buffers, error);
    }

private:
    lowest_layer_type socket_;
    std::size_t prefix_bytes_{5};
};

bool eventually_closed(asio::ip::tcp::socket& socket) {
    socket.non_blocking(true);
    bool ended = false;
    ruvia::test::wait_until([&] {
        std::array<char, 256> bytes{};
        asio::error_code error;
        (void)socket.read_some(asio::buffer(bytes), error);
        ended = error == asio::error::eof || error == asio::error::connection_reset;
        return ended;
    },
        3s);
    return ended;
}

#if defined(RUVIA_ENABLE_HTTP3)
bool serves_hello(ruvia::test::http3_test_client& client) {
    const auto response = client.request("GET", "/hello");
    return response.status_ == 200 && response.body_ == "hello";
}

// Whether a fresh HTTP/3 connection serves GET /hello.
bool http3_serves(std::uint16_t port, const ruvia::test::tls_identity& identity, std::chrono::milliseconds timeout = 3s) {
    try {
        ruvia::test::http3_test_client client({asio::ip::address_v4::loopback(), port}, identity.ca_file_, "localhost", timeout);
        return serves_hello(client);
    } catch (const std::exception&) {
        return false;
    }
}
#endif

#if defined(RUVIA_ENABLE_HTTP3)
// Retired SO_REUSEPORT sockets must route fresh Initials to the current
// listener, and current sockets must still route older connection IDs.
TEST(Ruvia, readded_http3_listener_serves_new_and_retired_connections_on_every_worker) {
    const ruvia::test::tls_identity first("retired-quic", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity second("readded-quic", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation https_reservation(io);
    ruvia::test::udp_port_reservation udp_reservation(io);
    const auto https = https_reservation.endpoint();
    const auto udp = udp_reservation.endpoint();
    ruvia::listen_config config{
        .address_ = "127.0.0.1", .https_ = https.port(), .http3_ = udp.port(), .tls_ = tls_of(first)};
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2}).listen(config).on_start(nullptr).on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    wait_until_running(app);

    std::array<std::unique_ptr<ruvia::test::http3_test_client>, 2> established;
    std::string first_worker;
    for (int attempt = 0; attempt < 32 && established[1] == nullptr; ++attempt) {
        auto client = std::make_unique<ruvia::test::http3_test_client>(udp, first.ca_file_, "localhost", 5s);
        const auto response = client->request("GET", "/listener-worker");
        ASSERT_EQ(response.status_, 200);
        const std::string worker(response.body_);
        if (established[0] == nullptr) {
            first_worker = worker;
            established[0] = std::move(client);
        } else if (worker != first_worker) {
            established[1] = std::move(client);
        }
    }
    ASSERT_NE(established[1], nullptr);
    const ruvia::listener_endpoint endpoint{.address_ = "127.0.0.1", .port_ = https.port()};
    control_loop().start(app.remove_listener(control_loop().worker(), endpoint)).get();
    config.tls_ = tls_of(second);
    ASSERT_NO_THROW(control_loop().start(app.add_listener(control_loop().worker(), config)).get());
    std::vector<std::unique_ptr<ruvia::test::http3_test_client>> current;
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto client = std::make_unique<ruvia::test::http3_test_client>(udp, second.ca_file_, "localhost", 5s);
        EXPECT_TRUE(serves_hello(*client));
        current.push_back(std::move(client));
        for (auto& old : established) {
            EXPECT_TRUE(serves_hello(*old));
        }
    }

#if defined(__linux__) && defined(SO_DETACH_REUSEPORT_BPF)
    // Exercise the same dispatch without the optional kernel CID selector.
    auto duplicated = control_loop().start(app.duplicate_listener_sockets(control_loop().worker(), endpoint)).get();
    const int ignored = 0;
    (void)::setsockopt(duplicated.http3_.front(), SOL_SOCKET, SO_DETACH_REUSEPORT_BPF, &ignored, sizeof(ignored));
    for (const auto fd : duplicated.https_) {
        (void)::close(fd);
    }
    for (const auto fd : duplicated.http3_) {
        (void)::close(fd);
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        EXPECT_TRUE(http3_serves(udp.port(), second));
        for (auto& old : established) {
            EXPECT_TRUE(serves_hello(*old));
        }
        for (auto& client : current) {
            EXPECT_TRUE(serves_hello(*client));
        }
    }
#endif
    for (auto& old : established) {
        old.reset();
    }
    // Once the old workers release their endpoints, the current listener
    // must still receive (Windows shares the old exclusive transport).
    for (int attempt = 0; attempt < 4; ++attempt) {
        EXPECT_TRUE(http3_serves(udp.port(), second));
        for (auto& client : current) {
            EXPECT_TRUE(serves_hello(*client));
        }
    }
    current.clear();
    app.stop();
    runner.join();
}

TEST(Ruvia, readded_http3_listener_without_connections_releases_its_udp_sockets) {
    const ruvia::test::tls_identity identity("unused-quic", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation base_reservation(io);
    ruvia::test::tcp_port_reservation https_reservation(io);
    auto udp_reservation = std::make_unique<ruvia::test::udp_port_reservation>(io);
    const auto udp = udp_reservation->endpoint();
    const auto https = https_reservation.endpoint();
    const ruvia::listen_config config{
        .address_ = "127.0.0.1", .https_ = https.port(), .http3_ = udp.port(), .tls_ = tls_of(identity)};
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .http_ = base_reservation.endpoint().port()})
        .on_start(nullptr)
        .on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    wait_until_running(app);
    for (int attempt = 0; attempt < 4; ++attempt) {
        ASSERT_NO_THROW(control_loop().start(app.add_listener(control_loop().worker(), config)).get());
#if !defined(_WIN32)
        // The wildcard reservation would itself prevent an exclusive bind.
        udp_reservation.reset();
#endif
        control_loop().start(app.remove_listener(control_loop().worker(), {.address_ = "127.0.0.1", .port_ = https.port()})).get();
        // Windows releases the shared socket on its acceptor control queue.
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        asio::ip::udp::socket exclusive(io);
        exclusive.open(udp.protocol());
        asio::error_code error;
        do {
            exclusive.bind(udp, error);
            if (!error || (error != asio::error::address_in_use && error != asio::error::access_denied)) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        EXPECT_FALSE(error) << error.message();
    }
    app.stop();
    runner.join();
}
#endif

#if !defined(_WIN32)
// A socket bound to 127.0.0.1:`port`, deliberately inheritable, listening
// for SOCK_STREAM when `listening`. `group` joins the port's SO_REUSEPORT group.
int bound_socket(int type, std::uint16_t port, bool listening, bool group = true) {
    const int fd = ::socket(AF_INET, type, 0);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "cannot open a test socket");
    }
    const int enabled = 1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if ((group && (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
                      ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0)) ||
        ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        (listening && ::listen(fd, 64) != 0)) {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "cannot bind a test socket");
    }
    return fd;
}

int listening_socket(std::uint16_t port, bool group = true) {
    return bound_socket(SOCK_STREAM, port, true, group);
}

[[nodiscard]] bool closed(int fd) {
    return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

[[nodiscard]] bool close_on_exec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    return flags != -1 && (flags & FD_CLOEXEC) != 0;
}

[[nodiscard]] bool listening(int fd) {
    int value = 0;
    socklen_t length = sizeof(value);
    return ::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &value, &length) == 0 && value != 0;
}

[[nodiscard]] std::uint16_t local_port(int fd) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        throw std::system_error(errno, std::generic_category(), "cannot name a test socket");
    }
    return ntohs(address.sin_port);
}
#endif

}  // namespace listener_sockets_test

#if !defined(_WIN32)
// listen() owns the sockets it is handed: the next run() serves on them
// instead of binding -- here a port bound without SO_REUSEPORT, which the
// server could not have bound itself -- and later runs bind. A socket that
// does not serve its service, a socket given twice or one for a disabled
// service closes every socket of that call; a socket count unlike the worker
// count fails run(), which consumed and closed them.
TEST(Ruvia, listen_adopts_inherited_tcp_sockets_and_closes_rejected_ones) {
    using namespace listener_sockets_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    asio::io_context io;
    auto& app = ruvia::app();
    app.on_start(nullptr).on_start([] { started().release(); });

    const int exclusive = listening_socket(0, false);
    const auto exclusive_endpoint = loopback(local_port(exclusive));
    app.server({.worker_count_ = 1})
        .listen({.address_ = "127.0.0.1",
            .http_ = exclusive_endpoint.port(),
            .inherited_sockets_ = ruvia::listener_sockets{.http_ = {exclusive}}});
    EXPECT_TRUE(close_on_exec(exclusive));
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started().try_acquire_for(10s));
        EXPECT_EQ(plain_body(io, exclusive_endpoint, "/hello"), "hello");
        app.stop();
        runner.join();
    }

    ruvia::test::tcp_port_reservation reservation(io);
    ruvia::test::tcp_port_reservation other(io);
    const auto endpoint = reservation.endpoint();
    const auto port = endpoint.port();
    app.server({.worker_count_ = 2});
    const auto listen_with = [&](ruvia::listener_sockets sockets) {
        return failure_of([&] {
            app.listen({.address_ = "127.0.0.1", .http_ = port, .inherited_sockets_ = std::move(sockets)});
        });
    };
    {
        const int good = listening_socket(port);
        const int wrong_port = listening_socket(other.endpoint().port());
        EXPECT_EQ(listen_with({.http_ = {good, wrong_port}}), "invalid_argument");
        EXPECT_TRUE(closed(good));
        EXPECT_TRUE(closed(wrong_port));
    }
    {
        const int datagram = bound_socket(SOCK_DGRAM, port, false);
        EXPECT_EQ(listen_with({.http_ = {datagram}}), "invalid_argument");
        EXPECT_TRUE(closed(datagram));
    }
    {
        const int idle = bound_socket(SOCK_STREAM, port, false);
        EXPECT_EQ(listen_with({.http_ = {idle}}), "invalid_argument");
        EXPECT_TRUE(closed(idle));
    }
    {
        const int twice = listening_socket(port);
        EXPECT_EQ(listen_with({.http_ = {twice, twice}}), "invalid_argument");
        EXPECT_TRUE(closed(twice));
    }
    {
        const int https = listening_socket(port);
        EXPECT_EQ(listen_with({.https_ = {https}}), "invalid_argument");
        EXPECT_TRUE(closed(https));
    }
    {
        const int lonely = listening_socket(port);
        EXPECT_EQ(listen_with({.http_ = {lonely}}), "none");
        EXPECT_FALSE(closed(lonely));
        EXPECT_THROW(app.run(), std::invalid_argument);
        EXPECT_TRUE(closed(lonely));
    }

    const int first = listening_socket(port);
    const int second = listening_socket(port);
    EXPECT_EQ(listen_with({.http_ = {first, second}}), "none");
    EXPECT_TRUE(close_on_exec(first));
    EXPECT_TRUE(close_on_exec(second));
    for (int run = 0; run < 2; ++run) {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started().try_acquire_for(10s));
        for (int request = 0; request < 8; ++request) {
            EXPECT_EQ(plain_body(io, endpoint, "/hello"), "hello");
        }
        app.stop();
        runner.join();
    }
}

#if defined(RUVIA_ENABLE_HTTP3)
// An HTTPS listener adopts its TCP sockets and its HTTP/3 service its
// SO_REUSEPORT group of UDP sockets, one per worker in worker order.
TEST(Ruvia, listen_adopts_inherited_https_and_http3_sockets) {
    using namespace listener_sockets_test;
    const ruvia::test::tls_identity identity("adopted", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation tcp_reservation(io);
    ruvia::test::udp_port_reservation udp_reservation(io);
    const auto https = tcp_reservation.endpoint();
    const auto http3 = udp_reservation.endpoint().port();
    ruvia::listener_sockets sockets;
    for (int worker = 0; worker < 2; ++worker) {
        sockets.https_.push_back(listening_socket(https.port()));
        sockets.http3_.push_back(bound_socket(SOCK_DGRAM, http3, false));
    }
    const auto adopted = sockets;
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1",
            .https_ = https.port(),
            .http3_ = http3,
            .tls_ = tls_of(identity),
            .inherited_sockets_ = std::move(sockets)})
        .on_start(nullptr)
        .on_start([] { started().release(); });
    for (const auto fd : adopted.http3_) {
        EXPECT_TRUE(close_on_exec(fd));
    }
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    auto client = tls_client();
    for (int attempt = 0; attempt < 4; ++attempt) {
        EXPECT_EQ(https_served_name(io, client, https), "adopted");
        EXPECT_TRUE(http3_serves(http3, identity));
    }
    app.stop();
    runner.join();
}
#endif

// duplicate_listener_sockets() hands out every worker's sockets of a running
// TCP service and its HTTP/3 service, inheritable and listening, while the
// app keeps serving; a later run -- as an exec'd successor would -- serves on
// the duplicates after this one stopped.
TEST(Ruvia, duplicated_listener_sockets_serve_a_successor) {
    using namespace listener_sockets_test;
    const ruvia::test::tls_identity identity("duplicated", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation http_reservation(io);
    ruvia::test::tcp_port_reservation https_reservation(io);
    const auto http = http_reservation.endpoint();
    const auto https = https_reservation.endpoint();
    controlled_port() = https.port();
    ruvia::test::tcp_port_reservation control_reservation(io);
    control_addition_port() = control_reservation.endpoint().port();
    ruvia::listen_config config{
        .address_ = "127.0.0.1",
        .http_ = http.port(),
        .https_ = https.port(),
        .tls_ = tls_of(identity),
    };
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation udp_reservation(io);
    const auto http3 = udp_reservation.endpoint().port();
    config.http3_ = http3;
    constexpr std::size_t http3_sockets = 2;
#else
    constexpr std::size_t http3_sockets = 0;
#endif
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2}).listen(config).on_start(nullptr).on_start([] { started().release(); });
    const ruvia::listener_endpoint https_endpoint{.address_ = "127.0.0.1", .port_ = https.port()};
    EXPECT_EQ(failure_of([&] { (void)control_loop().start(app.duplicate_listener_sockets(control_loop().worker(), https_endpoint)).get(); }), "logic_error");

    ruvia::listener_sockets https_sockets;
    ruvia::listener_sockets http_sockets;
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started().try_acquire_for(10s));
        wait_until_running(app);
        EXPECT_EQ(failure_of([&] { (void)control_loop().start(app.duplicate_listener_sockets(control_loop().worker(), {.address_ = "127.0.0.2", .port_ = https.port()})).get(); }),
            "invalid_argument");
        EXPECT_EQ(plain_body(io, http, "/control"), "added removed duplicated");
        EXPECT_TRUE(refuses(io, control_reservation.endpoint()));
        https_sockets = control_loop().start(app.duplicate_listener_sockets(control_loop().worker(), https_endpoint)).get();
        http_sockets = control_loop().start(app.duplicate_listener_sockets(control_loop().worker(), {.address_ = "127.0.0.1", .port_ = http.port()})).get();
        EXPECT_TRUE(https_sockets.http_.empty());
        EXPECT_EQ(https_sockets.https_.size(), 2U);
        EXPECT_EQ(https_sockets.http3_.size(), http3_sockets);
        EXPECT_EQ(http_sockets.http_.size(), 2U);
        EXPECT_TRUE(http_sockets.https_.empty());
        EXPECT_TRUE(http_sockets.http3_.empty());
        for (const auto* service : {&https_sockets.https_, &https_sockets.http3_, &http_sockets.http_}) {
            for (const auto fd : *service) {
                EXPECT_FALSE(close_on_exec(fd));
            }
        }
        for (const auto fd : https_sockets.https_) {
            EXPECT_TRUE(listening(fd));
        }
        for (const auto fd : http_sockets.http_) {
            EXPECT_TRUE(listening(fd));
        }
        EXPECT_EQ(plain_body(io, http, "/hello"), "hello");
        app.stop();
        runner.join();
    }

    for (const auto fd : https_sockets.https_) {
        EXPECT_TRUE(listening(fd));
    }
    config.inherited_sockets_ = ruvia::listener_sockets{
        .http_ = http_sockets.http_,
        .https_ = https_sockets.https_,
        .http3_ = https_sockets.http3_,
    };
    app.listen(config);
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    auto client = tls_client();
    for (int attempt = 0; attempt < 4; ++attempt) {
        EXPECT_EQ(plain_body(io, http, "/hello"), "hello");
        EXPECT_EQ(https_served_name(io, client, https), "duplicated");
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_TRUE(http3_serves(http3, identity));
#endif
    }
    app.stop();
    runner.join();
}

// add_listener() adopts inherited sockets as listen() does, and a count
// unlike the worker count closes them.
TEST(Ruvia, added_listeners_adopt_inherited_sockets) {
    using namespace listener_sockets_test;
    asio::io_context io;
    ruvia::test::tcp_port_reservation base_reservation(io);
    ruvia::test::tcp_port_reservation adopted_reservation(io);
    ruvia::test::tcp_port_reservation rejected_reservation(io);
    const auto base = base_reservation.endpoint();
    const auto adopted = adopted_reservation.endpoint();
    const auto rejected = rejected_reservation.endpoint();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .http_ = base.port()})
        .on_start(nullptr)
        .on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    wait_until_running(app);

    const int lonely = listening_socket(rejected.port());
    EXPECT_EQ(failure_of([&] {
        control_loop().start(app.add_listener(control_loop().worker(), {.address_ = "127.0.0.1",
                                                                           .http_ = rejected.port(),
                                                                           .inherited_sockets_ = ruvia::listener_sockets{.http_ = {lonely}}}))
            .get();
    }),
        "invalid_argument");
    EXPECT_TRUE(closed(lonely));
    EXPECT_TRUE(refuses(io, rejected));

    // Dropping a cold addition releases the sockets transferred at task creation.
    const int abandoned = listening_socket(rejected.port());
    {
        auto cold = app.add_listener(control_loop().worker(),
            {.address_ = "127.0.0.1", .http_ = rejected.port(), .inherited_sockets_ = ruvia::listener_sockets{.http_ = {abandoned}}});
    }
    EXPECT_TRUE(closed(abandoned));
    EXPECT_TRUE(refuses(io, rejected));

    const int first = listening_socket(adopted.port());
    const int second = listening_socket(adopted.port());
    // Neither the config nor its borrowed address survives until execution.
    auto addition = [&] {
        std::string address = "127.0.0.1";
        return app.add_listener(control_loop().worker(),
            {.address_ = address, .http_ = adopted.port(), .inherited_sockets_ = ruvia::listener_sockets{.http_ = {first, second}}});
    }();
    EXPECT_EQ(failure_of([&] { control_loop().start(std::move(addition)).get(); }), "none");
    EXPECT_TRUE(close_on_exec(first));
    for (int request = 0; request < 8; ++request) {
        EXPECT_EQ(plain_body(io, adopted, "/hello"), "hello");
    }
    EXPECT_EQ(plain_body(io, base, "/hello"), "hello");
    app.stop();
    runner.join();
}
#endif

// add_listener() serves a new HTTP, HTTPS and HTTP/3 service while the app
// runs -- the first HTTP/3 one included -- and keeps it for later runs. A
// failure on any worker (here an HTTPS port bound elsewhere) discards the
// addition everywhere, its HTTP port included, while the app serves on. A
// stopped app and a duplicate endpoint are refused, and business workers can
// await runtime controls. replace_tls_identities() reaches an added listener
// after others were removed.
TEST(Ruvia, listeners_are_added_while_running_and_kept_for_later_runs) {
    using namespace listener_sockets_test;
    const ruvia::test::tls_identity first("first", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity second("second", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation base_reservation(io);
    ruvia::test::tcp_port_reservation added_reservation(io);
    ruvia::test::tcp_port_reservation secure_reservation(io);
    ruvia::test::tcp_port_reservation discarded_reservation(io);
    ruvia::test::tcp_port_reservation racing_reservation(io);
    const auto racing = racing_reservation.endpoint();
    const auto base = base_reservation.endpoint();
    const auto added = added_reservation.endpoint();
    const auto secure = secure_reservation.endpoint();
    const auto discarded = discarded_reservation.endpoint();
    controlled_port() = added.port();
    ruvia::test::tcp_port_reservation control_reservation(io);
    control_addition_port() = control_reservation.endpoint().port();
    // Bound without sharing, so a worker cannot bind the port.
    asio::ip::tcp::acceptor busy(io);
    busy.open(asio::ip::tcp::v4());
    busy.bind(loopback(0));
    const auto busy_port = busy.local_endpoint().port();
    ruvia::listen_config secure_config{.address_ = "127.0.0.1", .https_ = secure.port(), .tls_ = tls_of(first)};
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation udp_reservation(io);
    const auto http3 = udp_reservation.endpoint().port();
    secure_config.http3_ = http3;
#endif

    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .http_ = base.port()})
        .on_start(nullptr)
        .on_start([] { started().release(); });
    EXPECT_EQ(failure_of([&] { control_loop().start(app.add_listener(control_loop().worker(), {.address_ = "127.0.0.1", .http_ = added.port()})).get(); }), "logic_error");
    auto client = tls_client();
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started().try_acquire_for(10s));
        wait_until_running(app);
        EXPECT_TRUE(refuses(io, added));
        EXPECT_EQ(failure_of([&] { control_loop().start(app.add_listener(control_loop().worker(), {.address_ = "127.0.0.1", .http_ = added.port()})).get(); }), "none");
        EXPECT_EQ(plain_body(io, added, "/hello"), "hello");
        EXPECT_EQ(failure_of([&] { control_loop().start(app.add_listener(control_loop().worker(), secure_config)).get(); }), "none");
        EXPECT_EQ(https_served_name(io, client, secure), "first");
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_TRUE(http3_serves(http3, first));
        // The UDP endpoint is a separate namespace, taken all the same.
        EXPECT_EQ(failure_of([&] {
            control_loop().start(app.add_listener(control_loop().worker(), {.address_ = "127.0.0.1",
                                                                               .https_ = discarded.port(),
                                                                               .http3_ = http3,
                                                                               .tls_ = tls_of(second)}))
                .get();
        }),
            "invalid_argument");
#endif
        EXPECT_EQ(failure_of([&] { control_loop().start(app.add_listener(control_loop().worker(), {.address_ = "127.0.0.1", .http_ = added.port()})).get(); }),
            "invalid_argument");
        EXPECT_EQ(failure_of([&] {
            control_loop().start(app.add_listener(control_loop().worker(), {.address_ = "127.0.0.1",
                                                                               .http_ = discarded.port(),
                                                                               .https_ = busy_port,
                                                                               .tls_ = tls_of(second)}))
                .get();
        }),
            "system_error");
        EXPECT_TRUE(refuses(io, discarded));
        EXPECT_EQ(plain_body(io, base, "/hello"), "hello");
        EXPECT_EQ(plain_body(io, added, "/hello"), "hello");
#if defined(_WIN32)
        EXPECT_EQ(plain_body(io, base, "/control"), "added removed");
#else
        EXPECT_EQ(plain_body(io, base, "/control"), "added removed duplicated");
#endif

        EXPECT_TRUE(refuses(io, control_reservation.endpoint()));
        control_loop().start(app.remove_listener(control_loop().worker(), {.address_ = "127.0.0.1", .port_ = base.port()})).get();
        control_loop().start(app.replace_tls_identities(control_loop().worker(), {.address_ = "127.0.0.1", .port_ = secure.port()}, tls_of(second))).get();
        EXPECT_EQ(https_served_name(io, client, secure), "second");
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_TRUE(http3_serves(http3, second));
#endif
        EXPECT_EQ(plain_body(io, added, "/hello"), "hello");

        // Concurrent additions take effect one at a time: of two for one
        // endpoint, exactly one serves it.
        std::array<std::string, 2> outcomes;
        {
            const auto add_racing = [&](std::string& outcome) {
                outcome = failure_of([&] { control_loop().start(app.add_listener(control_loop().worker(), {.address_ = "127.0.0.1", .http_ = racing.port()})).get(); });
            };
            std::thread first_adder(add_racing, std::ref(outcomes[0]));
            std::thread second_adder(add_racing, std::ref(outcomes[1]));
            first_adder.join();
            second_adder.join();
        }
        EXPECT_EQ(std::ranges::count(outcomes, std::string("none")), 1);
        EXPECT_EQ(std::ranges::count(outcomes, std::string("invalid_argument")), 1);
        EXPECT_EQ(plain_body(io, racing, "/hello"), "hello");
        app.stop();
        runner.join();
    }

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    EXPECT_TRUE(refuses(io, base));
    EXPECT_TRUE(refuses(io, discarded));
    EXPECT_EQ(plain_body(io, added, "/hello"), "hello");
    EXPECT_EQ(plain_body(io, racing, "/hello"), "hello");
    EXPECT_EQ(https_served_name(io, client, secure), "second");
#if defined(RUVIA_ENABLE_HTTP3)
    EXPECT_TRUE(http3_serves(http3, second));
#endif
    app.stop();
    runner.join();
}

// remove_listener() closes a TCP service, and its HTTP/3 service, to new
// connections before it returns, while established ones -- keep-alive HTTP,
// TLS and QUIC -- still complete requests. Later runs no longer serve it. An
// unknown endpoint and a stopped app are refused; business workers can await controls.
TEST(Ruvia, removed_listeners_refuse_new_connections_and_serve_established_ones) {
    using namespace listener_sockets_test;
    const ruvia::test::tls_identity identity("removed", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation kept_reservation(io);
    ruvia::test::tcp_port_reservation removed_reservation(io);
    ruvia::test::tcp_port_reservation secure_reservation(io);
    const auto kept = kept_reservation.endpoint();
    const auto removed = removed_reservation.endpoint();
    const auto secure = secure_reservation.endpoint();
    controlled_port() = removed.port();
    ruvia::test::tcp_port_reservation control_reservation(io);
    control_addition_port() = control_reservation.endpoint().port();
    ruvia::listen_config secure_config{.address_ = "127.0.0.1", .https_ = secure.port(), .tls_ = tls_of(identity)};
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation udp_reservation(io);
    const auto http3 = udp_reservation.endpoint().port();
    secure_config.http3_ = http3;
#endif
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({
            {.address_ = "127.0.0.1", .http_ = kept.port()},
            {.address_ = "127.0.0.1", .http_ = removed.port()},
            secure_config,
        })
        .on_start(nullptr)
        .on_start([] { started().release(); });
    const ruvia::listener_endpoint removed_endpoint{.address_ = "127.0.0.1", .port_ = removed.port()};
    const ruvia::listener_endpoint secure_endpoint{.address_ = "127.0.0.1", .port_ = secure.port()};
    EXPECT_EQ(failure_of([&] { control_loop().start(app.remove_listener(control_loop().worker(), removed_endpoint)).get(); }), "logic_error");
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started().try_acquire_for(10s));
        wait_until_running(app);
        asio::ip::tcp::socket established(io);
        established.connect(removed);
        ASSERT_TRUE(serves_hello(established));
        auto client = tls_client();
        const auto established_tls = connect_tls(io, client, secure);
        ASSERT_TRUE(serves_hello(*established_tls));
#if defined(RUVIA_ENABLE_HTTP3)
        ruvia::test::http3_test_client established_http3(
            {asio::ip::address_v4::loopback(), http3}, identity.ca_file_, "localhost", 5s);
        ASSERT_TRUE(serves_hello(established_http3));
#endif
#if defined(_WIN32)
        EXPECT_EQ(plain_body(io, kept, "/control"), "added removed");
#else
        EXPECT_EQ(plain_body(io, kept, "/control"), "added removed duplicated");
#endif

        EXPECT_TRUE(refuses(io, control_reservation.endpoint()));
        EXPECT_EQ(failure_of([&] { control_loop().start(app.remove_listener(control_loop().worker(), removed_endpoint)).get(); }), "none");
        EXPECT_EQ(failure_of([&] { control_loop().start(app.remove_listener(control_loop().worker(), secure_endpoint)).get(); }), "none");
        EXPECT_EQ(failure_of([&] { control_loop().start(app.remove_listener(control_loop().worker(), removed_endpoint)).get(); }), "invalid_argument");
        EXPECT_EQ(failure_of([&] { control_loop().start(app.remove_listener(control_loop().worker(), {.address_ = "127.0.0.1", .port_ = 1})).get(); }), "invalid_argument");
        EXPECT_TRUE(refuses(io, removed));
        EXPECT_TRUE(refuses(io, secure));
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_FALSE(http3_serves(http3, identity, 1s));
#endif
        EXPECT_TRUE(serves_hello(established));
        EXPECT_TRUE(serves_hello(*established_tls));
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_TRUE(serves_hello(established_http3));
#endif
        EXPECT_EQ(plain_body(io, kept, "/hello"), "hello");
        app.stop();
        runner.join();
    }

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    EXPECT_EQ(plain_body(io, kept, "/hello"), "hello");
    EXPECT_TRUE(refuses(io, removed));
    EXPECT_TRUE(refuses(io, secure));
    app.stop();
    runner.join();
}

TEST(Ruvia, mixed_listener_shares_one_socket_group_and_preserves_transport_through_runtime_controls) {
    using namespace listener_sockets_test;
    const ruvia::test::tls_identity first("mixed-first", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity second("mixed-second", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation base(io), shared(io);
    const auto endpoint = shared.endpoint();
    const ruvia::listener_endpoint target{.address_ = "127.0.0.1", .port_ = endpoint.port()};
    ruvia::listen_config config{.address_ = "127.0.0.1", .http_ = endpoint.port(), .https_ = endpoint.port(), .tls_ = tls_of(first)};
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation udp(io);
    config.http3_ = udp.endpoint().port();
#endif
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .http_ = base.endpoint().port()})
        .on_start(nullptr)
        .on_start([] { started().release(); });
    auto client = tls_client();
#if !defined(_WIN32)
    ruvia::listener_sockets inherited;
#endif
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started().try_acquire_for(10s));
        wait_until_running(app);
        ASSERT_NO_THROW(control_loop().start(app.add_listener(control_loop().worker(), config)).get());
        std::thread plaintext([&] {
            asio::io_context peer_io;
            for (int attempt = 0; attempt < 12; ++attempt) {
                EXPECT_EQ(plain_body(peer_io, endpoint, "/hello"), "hello");
            }
        });
        for (int attempt = 0; attempt < 12; ++attempt) {
            EXPECT_EQ(https_served_name(io, client, endpoint), "mixed-first");
        }
        plaintext.join();
        asio::ip::tcp::socket established(io);
        established.connect(endpoint);
        ASSERT_TRUE(serves_hello(established));
        const auto established_tls = connect_tls(io, client, endpoint);
        ASSERT_TRUE(serves_hello(*established_tls));
        control_loop().start(app.replace_tls_identities(control_loop().worker(), target, tls_of(second))).get();
        EXPECT_EQ(https_served_name(io, client, endpoint), "mixed-second");
        EXPECT_EQ(plain_body(io, endpoint, "/hello"), "hello");
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_TRUE(http3_serves(udp.endpoint().port(), second));
#endif
#if !defined(_WIN32)
        inherited = control_loop().start(app.duplicate_listener_sockets(control_loop().worker(), target)).get();
        EXPECT_TRUE(inherited.http_.empty());
        EXPECT_EQ(inherited.https_.size(), 2U);
        for (const auto fd : inherited.https_) {
            EXPECT_TRUE(listening(fd));
        }
#endif
        control_loop().start(app.remove_listener(control_loop().worker(), target)).get();
#if defined(_WIN32)
        EXPECT_TRUE(refuses(io, endpoint));
#endif
        EXPECT_TRUE(serves_hello(established));
        EXPECT_TRUE(serves_hello(*established_tls));
        app.stop();
        runner.join();
    }
    config.tls_ = tls_of(second);
#if !defined(_WIN32)
    config.inherited_sockets_ = std::move(inherited);
#endif
    app.listen(config);
    ruvia::test::app_run successor(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    wait_until_running(app);
    for (int attempt = 0; attempt < 8; ++attempt) {
        EXPECT_EQ(plain_body(io, endpoint, "/hello"), "hello");
        EXPECT_EQ(https_served_name(io, client, endpoint), "mixed-second");
    }
    control_loop().start(app.remove_listener(control_loop().worker(), target)).get();
    EXPECT_TRUE(refuses(io, endpoint));
    app.stop();
    successor.join();
}

TEST(Ruvia, mixed_listener_redirects_only_plaintext_and_accepts_fragmented_client_hello) {
    using namespace listener_sockets_test;
    const ruvia::test::tls_identity identity("mixed-redirect", "DNS:localhost");
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .http_ = endpoint.port(), .https_ = endpoint.port(), .tls_ = tls_of(identity), .auto_https_redirect_ = true})
        .on_start(nullptr)
        .on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    const auto response = ruvia::test::exchange(io, endpoint,
        "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 308 "));
    EXPECT_NE(response.find("https://localhost:" + std::to_string(endpoint.port()) + "/hello"), std::string::npos);
    auto client = tls_client();
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::ssl::stream<fragmented_tcp> stream(fragmented_tcp(std::move(socket)), client);
    stream.handshake(asio::ssl::stream_base::client);
    EXPECT_TRUE(serves_hello(stream));
    EXPECT_EQ(https_served_name(io, client, endpoint), "mixed-redirect");
    app.stop();
    runner.join();
}

TEST(Ruvia, mixed_listener_detection_and_partial_handshakes_share_deadlines_and_immediate_stop) {
    using namespace listener_sockets_test;
    const ruvia::test::tls_identity identity("mixed-deadlines", "DNS:localhost");
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2, .connection_scan_interval_ = 5ms, .request_header_timeout_ = 80ms, .header_completion_timeout_ = 180ms})
        .listen({.address_ = "127.0.0.1", .http_ = endpoint.port(), .https_ = endpoint.port(), .tls_ = tls_of(identity)})
        .on_start(nullptr)
        .on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    asio::ip::tcp::socket silent(io), partial_tls(io), slow_plain(io);
    silent.connect(endpoint);
    partial_tls.connect(endpoint);
    slow_plain.connect(endpoint);
    asio::write(partial_tls, asio::buffer(std::string_view("\x16\x03", 2)));
    const auto started_at = std::chrono::steady_clock::now();
    for (const char byte : std::string_view("GET ")) {
        asio::error_code error;
        asio::write(slow_plain, asio::buffer(&byte, 1), error);
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_TRUE(eventually_closed(silent));
    EXPECT_TRUE(eventually_closed(partial_tls));
    EXPECT_TRUE(eventually_closed(slow_plain));
    EXPECT_LT(std::chrono::steady_clock::now() - started_at, 1s);
    auto client = tls_client();
    EXPECT_EQ(plain_body(io, endpoint, "/hello"), "hello");
    EXPECT_EQ(https_served_name(io, client, endpoint), "mixed-deadlines");
    asio::ip::tcp::socket pending_detection(io), pending_tls(io);
    pending_detection.connect(endpoint);
    pending_tls.connect(endpoint);
    asio::write(pending_tls, asio::buffer(std::string_view("\x16", 1)));
    const auto stopped_at = std::chrono::steady_clock::now();
    app.stop();
    runner.join();
    EXPECT_LT(std::chrono::steady_clock::now() - stopped_at, 1s);
    EXPECT_TRUE(eventually_closed(pending_detection));
    EXPECT_TRUE(eventually_closed(pending_tls));
}

#if !defined(_WIN32)
TEST(Ruvia, mixed_listener_rejects_conflicting_inheritance_vectors_and_closes_every_handle) {
    using namespace listener_sockets_test;
    const ruvia::test::tls_identity identity("mixed-inherit", "DNS:localhost");
    const int http = listening_socket(0);
    const auto port = local_port(http);
    const int https = listening_socket(port);
    auto& app = ruvia::app();
    EXPECT_THROW(app.listen({.address_ = "127.0.0.1", .http_ = port, .https_ = port, .tls_ = tls_of(identity), .inherited_sockets_ = ruvia::listener_sockets{.http_ = {http}, .https_ = {https}}}), std::invalid_argument);
    EXPECT_TRUE(closed(http));
    EXPECT_TRUE(closed(https));
}
#endif

namespace server_connection_test {

using namespace std::chrono_literals;

class ping_controller final {
public:
    RUVIA_ROUTES_BEGIN(ping_controller)
    RUVIA_GET("/ping", ping);
    RUVIA_GET("/tls", tls);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> ping(ruvia::context& context) {
        co_return context.text("pong");
    }

    // What the request's TLS handshake negotiated.
    ruvia::task<ruvia::http_response> tls(ruvia::context& context) {
        const auto* transport = context.conn().tls();
        co_return context.text(transport == nullptr
                                   ? std::string("plain")
                                   : std::string(transport->alpn()) + "|" + std::string(transport->server_name()));
    }
};

// What one handler saw of its connection, copied on its worker.
struct connection_view final {
    std::uint64_t id_{};
    std::string local_address_;
    std::uint16_t local_port_{};
    std::string remote_address_;
    std::uint16_t remote_port_{};
    bool tls_{};
    std::string alpn_;
    std::string server_name_;
    bool client_certificate_{};
    // The second read after the peer's end returned std::nullopt again.
    bool end_repeated_{};
    // The handler's pending read failed, and the stop token it saw then.
    bool read_failed_{};
    bool stop_requested_{};
};

class observed_connections final {
public:
    void add(const connection_view& view) {
        std::lock_guard lock(mutex_);
        views_.push_back(view);
    }

    [[nodiscard]] std::vector<connection_view> views() const {
        std::lock_guard lock(mutex_);
        return views_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return views_.size();
    }

    void clear() {
        std::lock_guard lock(mutex_);
        views_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<connection_view> views_;
};

observed_connections& observed() {
    static observed_connections value;
    return value;
}

std::binary_semaphore& server_connection_started() {
    static std::binary_semaphore value{0};
    return value;
}

std::counting_semaphore<>& handler_entered() {
    static std::counting_semaphore<> value{0};
    return value;
}

struct failure_copy final {
    std::string remote_address_;
    std::string message_;
};

std::mutex& failures_mutex() {
    static std::mutex value;
    return value;
}

std::vector<failure_copy>& failures() {
    static std::vector<failure_copy> value;
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
    failures().push_back({std::string(record.remote_address().to_string()), std::move(message)});
}

[[nodiscard]] connection_view describe(const ruvia::server_connection& connection) {
    connection_view view;
    view.id_ = connection.connection_id();
    view.local_address_ = connection.local().address().to_string();
    view.local_port_ = connection.local().port();
    view.remote_address_ = connection.remote().address().to_string();
    view.remote_port_ = connection.remote().port();
    if (const auto* tls = connection.tls()) {
        view.tls_ = true;
        view.alpn_ = tls->alpn();
        view.server_name_ = tls->server_name();
        view.client_certificate_ = tls->client_certificate_subject().has_value();
    }
    return view;
}

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

struct unstarted_connection_operations final {
    std::optional<ruvia::task<std::optional<std::span<const std::byte>>>> read_;
    std::optional<ruvia::task<void>> write_;
    std::optional<ruvia::task<void>> finish_;
};

// Echoes every chunk until the peer ends its direction. A cleartext peer then
// gets "|eof" before the finish; a TLS peer's close_notify is answered by the
// finish alone. The end repeats, and returning closes the connection.
ruvia::task<void> echo(ruvia::server_connection& connection) {
    auto view = describe(connection);
    for (;;) {
        auto chunk = co_await connection.read();
        if (!chunk) {
            break;
        }
        co_await connection.write(*chunk);
    }
    if (connection.tls() == nullptr) {
        co_await connection.write(bytes_of("|eof"));
    }
    co_await connection.finish();
    view.end_repeated_ = !(co_await connection.read()).has_value();
    observed().add(view);
}

// Waits on input that never comes; the server ends the connection.
ruvia::task<void> wait_for_input(ruvia::server_connection& connection) {
    auto view = describe(connection);
    handler_entered().release();
    try {
        (void)co_await connection.read();
    } catch (const std::system_error&) {
        view.read_failed_ = true;
        view.stop_requested_ = connection.stop_token().stop_requested();
        observed().add(view);
        throw;
    }
    observed().add(view);
}

ruvia::task<void> fail_after_first_read(ruvia::server_connection& connection) {
    (void)co_await connection.read();
    throw std::runtime_error("raw handler failed");
}

[[nodiscard]] std::string read_until_closed(asio::ip::tcp::socket& socket) {
    std::string result;
    std::array<char, 1024> bytes;
    asio::error_code error;
    while (!error) {
        const auto count = socket.read_some(asio::buffer(bytes), error);
        result.append(bytes.data(), count);
    }
    return result;
}

[[nodiscard]] ruvia::listen_config connection_listener(const asio::ip::tcp::endpoint& http) {
    return ruvia::listen_config{
        .address_ = "127.0.0.1",
        .http_ = http.port(),
        .dispatch_ = ruvia::listener_dispatch::connection,
    };
}

}  // namespace server_connection_test

// One application serves byte streams on a cleartext and a TLS connection
// listener beside an ordinary HTTP listener. The server parses nothing on the
// connection listeners: an HTTP request is echoed back verbatim. The handler
// sees the connection's id and endpoints, the TLS listener's own ALPN choice
// (in server preference) and the client's server_name; read() ends with
// std::nullopt at the peer's FIN or close_notify and keeps returning it, and
// finish() ends the send direction with a FIN or close_notify. A client
// offering ALPN with none of the listener's protocols fails the handshake.
TEST(Ruvia, connection_listeners_serve_byte_streams_beside_routes) {
    using namespace server_connection_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation raw_reservation(io);
    ruvia::test::tcp_port_reservation raw_tls_reservation(io);
    ruvia::test::tcp_port_reservation routes_reservation(io);
    ruvia::test::tcp_port_reservation routes_tls_reservation(io);
    const auto raw_endpoint = raw_reservation.endpoint();
    const auto raw_tls_endpoint = raw_tls_reservation.endpoint();
    const auto routes_endpoint = routes_reservation.endpoint();
    const auto routes_tls_endpoint = routes_tls_reservation.endpoint();
    const ruvia::tls_identity_config server_identity{
        .certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()};
    auto raw_listener = connection_listener(raw_endpoint);
    raw_listener.https_ = raw_tls_endpoint.port();
    raw_listener.tls_ = {.identity_ = server_identity, .alpn_ = {"echo/1", "h2"}};
    const ruvia::listen_config routes_listener{.address_ = "127.0.0.1",
        .http_ = routes_endpoint.port(),
        .https_ = routes_tls_endpoint.port(),
        .tls_ = {.identity_ = server_identity}};

    observed().clear();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({raw_listener, routes_listener})
        .on_start(nullptr)
        .on_start([] { server_connection_started().release(); })
        .on_connection(&echo);

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(server_connection_started().try_acquire_for(10s));
    EXPECT_THROW(app.on_connection(nullptr), std::logic_error);

    constexpr std::string_view payload = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::uint16_t plain_client_port = 0;
    {
        asio::ip::tcp::socket socket(io);
        socket.connect(raw_endpoint);
        plain_client_port = socket.local_endpoint().port();
        asio::write(socket, asio::buffer(payload));
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        EXPECT_EQ(read_until_closed(socket), std::string(payload) + "|eof");
    }

    std::uint16_t tls_client_port = 0;
    {
        asio::ssl::context client(asio::ssl::context::tls_client);
        client.set_verify_mode(asio::ssl::verify_none);
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
        static constexpr unsigned char alpn[] = {2, 'h', '2', 6, 'e', 'c', 'h', 'o', '/', '1'};
        ASSERT_EQ(SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)), 0);
        ASSERT_EQ(SSL_set_tlsext_host_name(stream.native_handle(), "localhost"), 1);
        stream.next_layer().connect(raw_tls_endpoint);
        tls_client_port = stream.next_layer().local_endpoint().port();
        stream.handshake(asio::ssl::stream_base::client);
        asio::write(stream, asio::buffer(payload));
        std::string echoed(payload.size(), '\0');
        asio::read(stream, asio::buffer(echoed));
        EXPECT_EQ(echoed, payload);
        // Sends close_notify and waits for the server's: the handler's read
        // ends, and its finish() answers.
        asio::error_code shutdown_error;
        stream.shutdown(shutdown_error);
        EXPECT_FALSE(shutdown_error) << shutdown_error.message();
    }

    {
        asio::ssl::context client(asio::ssl::context::tls_client);
        client.set_verify_mode(asio::ssl::verify_none);
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
        static constexpr unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        ASSERT_EQ(SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)), 0);
        stream.next_layer().connect(raw_tls_endpoint);
        asio::error_code handshake_error;
        stream.handshake(asio::ssl::stream_base::client, handshake_error);
        EXPECT_TRUE(handshake_error);
    }

    const auto pong = ruvia::test::exchange(
        io, routes_endpoint, "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(pong.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(pong.ends_with("pong"));

    // Routed HTTPS requests see the same negotiated values through conn_info.
    {
        asio::ssl::context client(asio::ssl::context::tls_client);
        client.set_verify_mode(asio::ssl::verify_none);
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
        static constexpr unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        ASSERT_EQ(SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)), 0);
        ASSERT_EQ(SSL_set_tlsext_host_name(stream.native_handle(), "localhost"), 1);
        stream.next_layer().connect(routes_tls_endpoint);
        stream.handshake(asio::ssl::stream_base::client);
        asio::write(stream, asio::buffer(std::string_view("GET /tls HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")));
        const auto response = ruvia::test::read_to_end(stream);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(response.ends_with("http/1.1|localhost"));
    }

    ruvia::test::wait_until([] { return observed().size() == 2; }, 10s);
    app.stop();
    runner.join();

    const auto views = observed().views();
    ASSERT_EQ(views.size(), 2U);
    const auto& plain = views[0].tls_ ? views[1] : views[0];
    const auto& tls = views[0].tls_ ? views[0] : views[1];
    EXPECT_NE(plain.id_, 0U);
    EXPECT_NE(tls.id_, 0U);
    EXPECT_NE(plain.id_, tls.id_);
    EXPECT_FALSE(plain.tls_);
    EXPECT_EQ(plain.local_address_, "127.0.0.1");
    EXPECT_EQ(plain.local_port_, raw_endpoint.port());
    EXPECT_EQ(plain.remote_address_, "127.0.0.1");
    EXPECT_EQ(plain.remote_port_, plain_client_port);
    EXPECT_TRUE(plain.end_repeated_);
    EXPECT_TRUE(tls.tls_);
    EXPECT_EQ(tls.local_port_, raw_tls_endpoint.port());
    EXPECT_EQ(tls.remote_port_, tls_client_port);
    EXPECT_EQ(tls.alpn_, "echo/1");
    EXPECT_EQ(tls.server_name_, "localhost");
    EXPECT_FALSE(tls.client_certificate_);
    EXPECT_TRUE(tls.end_repeated_);
}

// Cancellation belongs to the connection's transport, not to the numeric
// error code alone: application exceptions after abort() must still report.
TEST(Ruvia, raw_connection_abort_cancels_outputs_without_reporting_faults) {
    using namespace server_connection_test;
    struct observations final {
        std::atomic<unsigned> closed_reads_{0};
        std::atomic<unsigned> closed_writes_{0};
        std::atomic<unsigned> canceled_finishes_{0};
        std::atomic<unsigned> canceled_outputs_{0};
        std::atomic<unsigned> application_cancellations_{0};
        std::atomic<unsigned> application_failures_{0};
        std::atomic<unsigned> unexpected_failures_{0};
    };
    const auto results = std::make_shared<observations>();
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation plain_reservation(io);
    ruvia::test::tcp_port_reservation tls_reservation(io);
    const auto plain_endpoint = plain_reservation.endpoint();
    const auto tls_endpoint = tls_reservation.endpoint();
    auto listener = connection_listener(plain_endpoint);
    listener.https_ = tls_endpoint.port();
    listener.tls_.identity_ = {.certificate_chain_file_ = identity.ca_file_,
        .private_key_file_ = identity.private_key_file()};
    auto& app = ruvia::app();
    app.server({.worker_count_ = 1})
        .listen(listener)
        .on_start(nullptr)
        .on_start([] { server_connection_started().release(); })
        .on_connection([results](ruvia::server_connection& connection) -> ruvia::task<void> {
            const auto input = co_await connection.read();
            if (!input || input->empty()) {
                throw std::runtime_error("missing abort command");
            }
            const auto command = std::to_integer<char>((*input)[0]);
            if (command == 'f') {
                connection.abort();
                try {
                    (void)co_await connection.read();
                } catch (const std::system_error&) {
                    ++results->closed_reads_;
                }
                try {
                    co_await connection.write(bytes_of("closed"));
                } catch (const std::system_error&) {
                    ++results->closed_writes_;
                }
                try {
                    co_await connection.finish();
                } catch (const std::system_error&) {
                    ++results->canceled_finishes_;
                    throw;
                }
                co_return;
            }
            if (command == 'w' || command == 'n') {
                ruvia::task_scope outputs(connection.worker());
                bool completed = false;
                const auto send = [&]() -> ruvia::task<void> {
                    try {
                        if (command == 'w') {
                            const std::string payload(256 * 1024, 'x');
                            co_await connection.write(bytes_of(payload));
                        } else {
                            co_await connection.finish();
                        }
                    } catch (const std::system_error&) {
                        completed = true;
                        ++results->canceled_outputs_;
                        throw;
                    }
                    completed = true;
                };
                outputs.spawn(send());
                EXPECT_FALSE(completed);
                connection.abort();
                co_await outputs.join();
                co_return;
            }
            connection.abort();
            if (command == 'c') {
                throw std::system_error(std::make_error_code(std::errc::operation_canceled), "raw application cancelled");
            }
            throw std::runtime_error("raw application failed after abort");
        })
        .on_connection_failure([results](const ruvia::connection_failure_record& record) noexcept {
            try {
                std::rethrow_exception(record.exception());
            } catch (const std::system_error& error) {
                if (error.code() == std::errc::operation_canceled &&
                    std::string_view(error.what()).starts_with("raw application cancelled")) {
                    ++results->application_cancellations_;
                } else {
                    ++results->unexpected_failures_;
                }
            } catch (const std::runtime_error& error) {
                if (std::string_view(error.what()) == "raw application failed after abort") {
                    ++results->application_failures_;
                } else {
                    ++results->unexpected_failures_;
                }
            } catch (...) {
                ++results->unexpected_failures_;
            }
        });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(server_connection_started().try_acquire_for(10s));
    unsigned expected_faults = 0;
    for (const bool tls : {false, true}) {
        for (const char command : std::string_view("fwnac")) {
            // Cleartext finish() is a synchronous shutdown; only TLS has an
            // in-flight close_notify to cancel.
            if (!tls && command == 'n') {
                continue;
            }
            const auto exchange = [command](auto& stream) {
                asio::write(stream, asio::buffer(&command, 1));
                std::array<char, 1024> bytes;
                asio::error_code error;
                while (!error) {
                    (void)stream.read_some(asio::buffer(bytes), error);
                }
            };
            if (tls) {
                asio::ssl::context client(asio::ssl::context::tls_client);
                client.set_verify_mode(asio::ssl::verify_none);
                asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
                stream.next_layer().connect(tls_endpoint);
                stream.handshake(asio::ssl::stream_base::client);
                exchange(stream);
            } else {
                asio::ip::tcp::socket socket(io);
                socket.connect(plain_endpoint);
                exchange(socket);
            }
            if (command == 'a' || command == 'c') {
                ++expected_faults;
            }
            ruvia::test::wait_until([&app] { return app.stats().active_connections_ == 0; }, 10s);
            EXPECT_EQ(app.stats().connection_failures_, expected_faults) << "TLS: " << tls << ", command: " << command;
        }
    }
    app.stop();
    runner.join();
    app.on_connection_failure(nullptr);
    EXPECT_EQ(results->closed_reads_.load(), 2U);
    EXPECT_EQ(results->closed_writes_.load(), 2U);
    EXPECT_EQ(results->canceled_finishes_.load(), 2U);
    EXPECT_EQ(results->canceled_outputs_.load(), 3U);
    EXPECT_EQ(results->application_cancellations_.load(), 2U);
    EXPECT_EQ(results->application_failures_.load(), 2U);
    EXPECT_EQ(results->unexpected_failures_.load(), 0U);
}

// idle_timeout_ closes a connection without I/O; its handler's pending read
// fails, which is a normal end rather than a connection failure.
TEST(Ruvia, connection_idle_timeout_closes_raw_connection) {
    using namespace server_connection_test;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    observed().clear();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 1, .idle_timeout_ = 300ms, .connection_scan_interval_ = 50ms})
        .listen(connection_listener(endpoint))
        .on_start(nullptr)
        .on_start([] { server_connection_started().release(); })
        .on_connection(&wait_for_input);

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(server_connection_started().try_acquire_for(10s));
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    ASSERT_TRUE(handler_entered().try_acquire_for(10s));
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(read_until_closed(socket), "");
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    EXPECT_GE(elapsed, 200ms);
    EXPECT_LT(elapsed, 5s);
    ruvia::test::wait_until([] { return observed().size() == 1; }, 10s);
    EXPECT_EQ(app.stats().connection_failures_, 0U);
    app.stop();
    runner.join();
    const auto views = observed().views();
    ASSERT_EQ(views.size(), 1U);
    EXPECT_TRUE(views[0].read_failed_);
    EXPECT_FALSE(views[0].stop_requested_);
}

// stop() closes every raw connection at once and requests the handler's stop
// token; run() returns once the handlers ended.
TEST(Ruvia, stop_disconnects_raw_connections) {
    using namespace server_connection_test;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    observed().clear();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen(connection_listener(endpoint))
        .on_start(nullptr)
        .on_start([] { server_connection_started().release(); })
        .on_connection(&wait_for_input);

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(server_connection_started().try_acquire_for(10s));
    std::array<asio::ip::tcp::socket, 3> sockets{
        asio::ip::tcp::socket(io), asio::ip::tcp::socket(io), asio::ip::tcp::socket(io)};
    for (auto& socket : sockets) {
        socket.connect(endpoint);
        ASSERT_TRUE(handler_entered().try_acquire_for(10s));
    }
    EXPECT_EQ(app.stats().active_connections_, sockets.size());
    app.stop();
    runner.join();
    for (auto& socket : sockets) {
        EXPECT_EQ(read_until_closed(socket), "");
    }
    const auto views = observed().views();
    ASSERT_EQ(views.size(), sockets.size());
    for (const auto& view : views) {
        EXPECT_TRUE(view.read_failed_);
        EXPECT_TRUE(view.stop_requested_);
    }
}

// An exception escaping the handler aborts the connection and reaches
// on_connection_failure, counted in connection_failures_.
TEST(Ruvia, raw_handler_failure_reaches_connection_failure_hook) {
    using namespace server_connection_test;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 1})
        .listen(connection_listener(endpoint))
        .on_start(nullptr)
        .on_start([] { server_connection_started().release(); })
        .on_connection(&fail_after_first_read)
        .on_connection_failure(&record_failure);

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(server_connection_started().try_acquire_for(10s));
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::write(socket, asio::buffer(std::string_view("x")));
    EXPECT_EQ(read_until_closed(socket), "");
    ruvia::test::wait_until([&app] { return app.stats().connection_failures_ == 1; }, 10s);
    app.stop();
    runner.join();
    app.on_connection_failure(nullptr);
    std::lock_guard lock(failures_mutex());
    ASSERT_EQ(failures().size(), 1U);
    EXPECT_EQ(failures()[0].remote_address_, "127.0.0.1");
    EXPECT_EQ(failures()[0].message_, "raw handler failed");
}

TEST(Ruvia, raw_connection_return_expires_unstarted_read_write_and_finish) {
    using namespace server_connection_test;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    const auto pending = std::make_shared<unstarted_connection_operations>();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 1})
        .listen(connection_listener(endpoint))
        .on_start(nullptr)
        .on_start([] { server_connection_started().release(); })
        .on_connection([pending](ruvia::server_connection& connection) -> ruvia::task<void> {
            if (!pending->read_) {
                pending->read_.emplace(connection.read());
                pending->write_.emplace(connection.write(bytes_of("must-not-be-sent")));
                pending->finish_.emplace(connection.finish());
                co_return;
            }
            unsigned expired = 0;
            try {
                (void)co_await std::move(*pending->read_);
            } catch (const std::logic_error&) {
                ++expired;
            }
            try {
                co_await std::move(*pending->write_);
            } catch (const std::logic_error&) {
                ++expired;
            }
            try {
                co_await std::move(*pending->finish_);
            } catch (const std::logic_error&) {
                ++expired;
            }
            pending->read_.reset();
            pending->write_.reset();
            pending->finish_.reset();
            co_await connection.write(bytes_of(expired == 3 ? "expired" : "invalid"));
        });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(server_connection_started().try_acquire_for(10s));
    {
        asio::ip::tcp::socket first(io);
        first.connect(endpoint);
        EXPECT_EQ(read_until_closed(first), "");
    }
    {
        asio::ip::tcp::socket second(io);
        second.connect(endpoint);
        EXPECT_EQ(read_until_closed(second), "expired");
    }
    ruvia::test::wait_until([&app] { return app.stats().active_connections_ == 0; }, 10s);
    EXPECT_EQ(app.stats().connection_failures_, 0U);
    app.stop();
    runner.join();
}

// A connection listener needs on_connection at run(); listen() rejects
// what such a listener cannot serve, and an ALPN list on a routes listener.
TEST(Ruvia, connection_dispatch_configuration_is_validated) {
    using namespace server_connection_test;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    ruvia::test::tcp_port_reservation tls_reservation(io);
    const auto endpoint = reservation.endpoint();
    const auto tls_port = tls_reservation.endpoint().port();
    ruvia::test::tls_identity identity("localhost");
    const ruvia::tls_config tls{.identity_ = {.certificate_chain_file_ = identity.ca_file_,
                                    .private_key_file_ = identity.private_key_file()}};
    auto& app = ruvia::app();
    app.server({.worker_count_ = 1}).listen(connection_listener(endpoint)).on_start(nullptr).on_connection(nullptr);
    EXPECT_THROW(app.run(), std::invalid_argument);

    auto with_http3 = connection_listener(endpoint);
    with_http3.https_ = tls_port;
    with_http3.http3_ = with_http3.https_;
    with_http3.tls_ = tls;
    EXPECT_THROW(app.listen(with_http3), std::invalid_argument);

    auto redirecting = connection_listener(endpoint);
    redirecting.https_ = tls_port;
    redirecting.tls_ = tls;
    redirecting.auto_https_redirect_ = true;
    EXPECT_THROW(app.listen(redirecting), std::invalid_argument);

    auto empty_protocol = connection_listener(endpoint);
    empty_protocol.https_ = tls_port;
    empty_protocol.tls_ = tls;
    empty_protocol.tls_.alpn_ = {"h2", ""};
    EXPECT_THROW(app.listen(empty_protocol), std::invalid_argument);

    auto cleartext_protocols = connection_listener(endpoint);
    cleartext_protocols.tls_.alpn_ = {"h2"};
    EXPECT_THROW(app.listen(cleartext_protocols), std::invalid_argument);

    auto routes_protocols = ruvia::listen_config{.address_ = "127.0.0.1",
        .https_ = endpoint.port(),
        .tls_ = tls};
    routes_protocols.tls_.alpn_ = {"h2"};
    EXPECT_THROW(app.listen(routes_protocols), std::invalid_argument);
}

// Each operation's operation_options bound only that wait. A read that times
// out or is stopped (from another thread) loses no input and reading
// continues; a stop requested before a write or finish starts sends nothing;
// a write that times out or is stopped aborts the connection. Non-positive
// timeouts are rejected before an operation exists.
TEST(Ruvia, raw_connection_operation_options_bound_each_wait) {
    using namespace server_connection_test;
    struct observations final {
        std::atomic<unsigned> rejected_{0};
        std::atomic<unsigned> read_timeouts_{0};
        std::atomic<unsigned> read_cancellations_{0};
        std::atomic<unsigned> prestopped_outputs_{0};
        std::atomic<unsigned> write_timeouts_{0};
        std::atomic<unsigned> write_cancellations_{0};
        std::atomic<unsigned> aborted_after_write_{0};
        std::atomic<unsigned> handled_{0};
    };
    const auto results = std::make_shared<observations>();
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation plain_reservation(io);
    ruvia::test::tcp_port_reservation tls_reservation(io);
    const auto plain_endpoint = plain_reservation.endpoint();
    const auto tls_endpoint = tls_reservation.endpoint();
    auto listener = connection_listener(plain_endpoint);
    listener.https_ = tls_endpoint.port();
    listener.tls_.identity_ = {.certificate_chain_file_ = identity.ca_file_,
        .private_key_file_ = identity.private_key_file()};
    const auto read_text = [](ruvia::server_connection& connection, std::size_t size) -> ruvia::task<std::string> {
        std::string received;
        while (received.size() < size) {
            const auto chunk = co_await connection.read();
            if (!chunk) {
                break;
            }
            received.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        }
        co_return received;
    };
    auto& app = ruvia::app();
    app.server({.worker_count_ = 1})
        .listen(listener)
        .on_start(nullptr)
        .on_start([] { server_connection_started().release(); })
        .on_connection([results, read_text](ruvia::server_connection& connection) -> ruvia::task<void> {
            const auto command = co_await read_text(connection, 1);
            if (command == "r") {
                try {
                    (void)connection.read({.timeout_ = 0ms});
                } catch (const std::invalid_argument&) {
                    ++results->rejected_;
                }
                try {
                    (void)connection.write(bytes_of("x"), {.timeout_ = -1ms});
                } catch (const std::invalid_argument&) {
                    ++results->rejected_;
                }
                try {
                    (void)connection.finish({.timeout_ = 0ms});
                } catch (const std::invalid_argument&) {
                    ++results->rejected_;
                }
                try {
                    (void)co_await connection.read({.timeout_ = 50ms});
                } catch (const std::system_error& error) {
                    if (error.code() == std::errc::timed_out) {
                        ++results->read_timeouts_;
                    }
                }
                co_await connection.write(bytes_of("1"));
                EXPECT_EQ(co_await read_text(connection, 3), "abc");
                ruvia::stop_source stop;
                std::thread canceller([&stop] {
                    std::this_thread::sleep_for(50ms);
                    stop.request_stop();
                });
                try {
                    (void)co_await connection.read({.stop_token_ = stop.token()});
                } catch (const std::system_error& error) {
                    if (error.code() == std::errc::operation_canceled) {
                        ++results->read_cancellations_;
                    }
                }
                canceller.join();
                co_await connection.write(bytes_of("2"));
                EXPECT_EQ(co_await read_text(connection, 3), "def");
                try {
                    co_await connection.write(bytes_of("must-not-be-sent"), {.stop_token_ = stop.token()});
                } catch (const std::system_error& error) {
                    if (error.code() == std::errc::operation_canceled) {
                        ++results->prestopped_outputs_;
                    }
                }
                try {
                    co_await connection.finish({.stop_token_ = stop.token()});
                } catch (const std::system_error& error) {
                    if (error.code() == std::errc::operation_canceled) {
                        ++results->prestopped_outputs_;
                    }
                }
                co_await connection.write(bytes_of("3"));
                co_await connection.finish();
                ++results->handled_;
                co_return;
            }
            // The peer reads nothing, so the write stalls once the socket
            // buffers fill.
            const std::string payload(64 * 1024 * 1024, 'x');
            ruvia::stop_source stop;
            std::optional<std::thread> canceller;
            ruvia::operation_options operation{.timeout_ = 200ms};
            if (command == "c") {
                operation = {.stop_token_ = stop.token()};
                canceller.emplace([&stop] {
                    std::this_thread::sleep_for(200ms);
                    stop.request_stop();
                });
            }
            try {
                co_await connection.write(bytes_of(payload), operation);
            } catch (const std::system_error& error) {
                if (error.code() == std::errc::timed_out) {
                    ++results->write_timeouts_;
                } else if (error.code() == std::errc::operation_canceled) {
                    ++results->write_cancellations_;
                }
            }
            if (canceller) {
                canceller->join();
            }
            try {
                co_await connection.write(bytes_of("after"));
            } catch (const std::system_error&) {
                ++results->aborted_after_write_;
            }
            ++results->handled_;
        });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(server_connection_started().try_acquire_for(10s));
    const auto read_exactly = [](auto& stream, std::size_t size) {
        std::string received(size, '\0');
        asio::read(stream, asio::buffer(received));
        return received;
    };
    const auto drain = [](auto& stream) {
        std::string received;
        std::array<char, 64 * 1024> bytes;
        asio::error_code error;
        while (!error) {
            const auto count = stream.read_some(asio::buffer(bytes), error);
            received.append(bytes.data(), count);
        }
        return received;
    };
    const auto read_session = [&](auto& stream) {
        asio::write(stream, asio::buffer(std::string_view("r")));
        EXPECT_EQ(read_exactly(stream, 1), "1");
        asio::write(stream, asio::buffer(std::string_view("abc")));
        EXPECT_EQ(read_exactly(stream, 1), "2");
        asio::write(stream, asio::buffer(std::string_view("def")));
        EXPECT_EQ(drain(stream), "3");
    };
    const auto stalled_session = [&](auto& stream, char command, unsigned handled) {
        asio::write(stream, asio::buffer(&command, 1));
        ruvia::test::wait_until([&] { return results->handled_.load() == handled; }, 10s);
        (void)drain(stream);
    };
    unsigned handled = 0;
    for (const bool tls : {false, true}) {
        const auto open = [&](auto&& session) {
            if (tls) {
                asio::ssl::context client(asio::ssl::context::tls_client);
                client.set_verify_mode(asio::ssl::verify_none);
                asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
                stream.next_layer().connect(tls_endpoint);
                stream.handshake(asio::ssl::stream_base::client);
                session(stream);
            } else {
                asio::ip::tcp::socket socket(io);
                socket.connect(plain_endpoint);
                session(socket);
            }
        };
        open([&](auto& stream) { read_session(stream); });
        ++handled;
        for (const char command : {'w', 'c'}) {
            ++handled;
            open([&](auto& stream) { stalled_session(stream, command, handled); });
        }
        ruvia::test::wait_until([&] { return results->handled_.load() == handled; }, 10s);
    }
    ruvia::test::wait_until([&app] { return app.stats().active_connections_ == 0; }, 10s);
    EXPECT_EQ(app.stats().connection_failures_, 0U);
    app.stop();
    runner.join();
    EXPECT_EQ(results->rejected_.load(), 6U);
    EXPECT_EQ(results->read_timeouts_.load(), 2U);
    EXPECT_EQ(results->read_cancellations_.load(), 2U);
    EXPECT_EQ(results->prestopped_outputs_.load(), 4U);
    EXPECT_EQ(results->write_timeouts_.load(), 2U);
    EXPECT_EQ(results->write_cancellations_.load(), 2U);
    EXPECT_EQ(results->aborted_after_write_.load(), 4U);
}

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
            control_loop().start(app.remove_listener(control_loop().worker(), {.address_ = "0.0.0.0", .port_ = 9})).get();
        } catch (const std::invalid_argument&) {
            return true;
        } catch (const std::logic_error&) {
        }
        return false;
    },
        10s);
}

std::string request(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view path = "/ready") {
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

TEST(Ruvia, adding_first_routed_listener_builds_controllers_and_route_rate_limits_for_the_existing_workers) {
    using namespace listener_transactions_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
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
    control_loop().start(app.add_listener(control_loop().worker(), {.address_ = "127.0.0.1", .http_ = routed.endpoint().port()})).get();
    EXPECT_TRUE(request(io, routed.endpoint()).ends_with("ready"));
    EXPECT_TRUE(request(io, routed.endpoint(), "/limited").starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(request(io, routed.endpoint(), "/limited").starts_with("HTTP/1.1 429 "));
    EXPECT_TRUE(request(io, raw.endpoint()).ends_with("ready"));
    app.stop();
    run.join();
}

TEST(Ruvia, add_listener_prepare_failure_discards_already_adopted_sockets_without_interrupting_service) {
    using namespace listener_transactions_test;
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
    EXPECT_THROW(control_loop().start(app.add_listener(control_loop().worker(), config)).get(), std::system_error);
    for (const auto socket : sockets) {
        EXPECT_TRUE(closed(socket));
    }
    EXPECT_TRUE(request(io, original.endpoint()).ends_with("ready"));
    EXPECT_THROW(control_loop().start(app.remove_listener(control_loop().worker(), {.address_ = "127.0.0.1", .port_ = addition.endpoint().port()})).get(), std::invalid_argument);
    blocker.close();
    config.inherited_sockets_.reset();
    control_loop().start(app.add_listener(control_loop().worker(), std::move(config))).get();
    EXPECT_TRUE(request(io, addition.endpoint()).ends_with("ready"));
    EXPECT_TRUE(request(io, original.endpoint()).ends_with("ready"));
    app.stop();
    run.join();
}

TEST(Ruvia, mixed_listener_adopts_one_physical_socket_group_for_plain_and_tls_raw_dispatch) {
    using namespace listener_transactions_test;
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
    using namespace listener_transactions_test;
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
    auto addition = loop.start(app.add_listener(loop.worker(),
        {.address_ = "127.0.0.1", .https_ = secure.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::connection}));
    std::promise<void> heartbeat;
    auto observed = heartbeat.get_future();
    const auto posted = loop.worker().post([&heartbeat] { heartbeat.set_value(); });
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
    std::move(addition).get();

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

TEST(Ruvia, application_prepared_batch_waits_for_publication_and_rolls_back_failure_abandonment_and_stop) {
    using namespace listener_transactions_test;
    asio::io_context io;
    ruvia::test::tcp_port_reservation original(io), addition(io), second(io), abandoned(io);
    asio::ip::tcp::acceptor blocker(io, {asio::ip::address_v4::loopback(), 0});
    std::atomic<unsigned> calls{};
    std::atomic<bool> published{true};
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .http_ = original.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection})
        .on_connection([&](ruvia::server_connection& connection) -> ruvia::task<void> {
            EXPECT_TRUE(published.load());
            ++calls;
            co_await serve_connection(connection);
        })
        .on_start(nullptr)
        .on_start([] { started.release(); });
    ruvia::test::app_run run(app);
    ASSERT_TRUE(started.try_acquire_for(10s));
    wait_until_running(app);
    const auto loop = control_loop();
    const auto config = [](std::uint16_t port) {
        return ruvia::listen_config{.address_ = "127.0.0.1", .http_ = port, .dispatch_ = ruvia::listener_dispatch::connection};
    };
    const std::array failed_configs{config(addition.endpoint().port()), config(blocker.local_endpoint().port())};
    EXPECT_THROW(loop.start(app.prepare_listeners(loop.worker(), failed_configs)).get(), std::system_error);
    EXPECT_TRUE(request(io, original.endpoint()).ends_with("ready"));
    asio::ip::tcp::socket refused(io);
    asio::error_code error;
    refused.connect(addition.endpoint(), error);
    EXPECT_EQ(error, asio::error::connection_refused);
    blocker.close();
    calls.store(0);
    published.store(false);
    const std::array configs{config(addition.endpoint().port()), config(second.endpoint().port())};
    auto prepared = loop.start(app.prepare_listeners(loop.worker(), configs)).get();
    std::array<asio::ip::tcp::socket, 2> queued{asio::ip::tcp::socket(io), asio::ip::tcp::socket(io)};
    const std::array endpoints{addition.endpoint(), second.endpoint()};
    constexpr std::string_view head = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    for (std::size_t index = 0; index != queued.size(); ++index) {
        queued[index].connect(endpoints[index]);
        asio::write(queued[index], asio::buffer(head));
    }
    EXPECT_EQ(calls.load(), 0U);
    published.store(true);
    loop.start(std::move(prepared).activate()).get();
    for (auto& peer : queued) {
        EXPECT_TRUE(ruvia::test::read_to_end(peer).ends_with("ready"));
    }
    EXPECT_EQ(calls.load(), 2U);
    loop.start(app.remove_listener(loop.worker(), {.address_ = "127.0.0.1", .port_ = original.endpoint().port()})).get();
    loop.start(app.remove_listener(loop.worker(), {.address_ = "127.0.0.1", .port_ = addition.endpoint().port()})).get();
    loop.start(app.remove_listener(loop.worker(), {.address_ = "127.0.0.1", .port_ = second.endpoint().port()})).get();
    const std::array abandoned_configs{config(abandoned.endpoint().port()), config(second.endpoint().port())};
    {
        auto pending = loop.start(app.prepare_listeners(loop.worker(), abandoned_configs)).get();
    }
    auto pending = loop.start(app.prepare_listeners(loop.worker(), abandoned_configs)).get();
    app.stop();
    run.join();
    EXPECT_THROW(loop.start(std::move(pending).activate()).get(), std::logic_error);
    // Only the cancelled candidate remained; restart has zero bound listeners.
    ruvia::test::app_run empty_run(app);
    ASSERT_TRUE(started.try_acquire_for(10s));
    wait_until_running(app);
    auto first = loop.start(app.prepare_listener(loop.worker(), config(addition.endpoint().port()))).get();
    loop.start(std::move(first).activate()).get();
    EXPECT_TRUE(request(io, addition.endpoint()).ends_with("ready"));
    app.stop();
    empty_run.join();
    app.on_connection(&serve_connection);
}

TEST(Ruvia, application_cold_prepared_batch_owns_all_inherited_sockets_and_rejects_duplicate_handles) {
    using namespace listener_transactions_test;
    asio::io_context io;
    ruvia::test::tcp_port_reservation first(io), second(io);
    const auto config = [&](const asio::ip::tcp::endpoint& endpoint) {
        return ruvia::listen_config{.address_ = "127.0.0.1", .http_ = endpoint.port(), .dispatch_ = ruvia::listener_dispatch::connection, .inherited_sockets_ = ruvia::listener_sockets{.http_ = {inherited_socket(io, endpoint)}}};
    };
    const std::array configs{config(first.endpoint()), config(second.endpoint())};
    {
        auto cold = ruvia::app().prepare_listeners(control_loop().worker(), configs);
    }
    for (const auto& entry : configs) {
        EXPECT_TRUE(closed(entry.inherited_sockets_->http_.front()));
    }
    const auto repeated = config(first.endpoint());
    const std::array duplicates{repeated, repeated};
    EXPECT_THROW((void)ruvia::app().prepare_listeners(control_loop().worker(), duplicates), std::invalid_argument);
    EXPECT_TRUE(closed(repeated.inherited_sockets_->http_.front()));
}

#if defined(RUVIA_ENABLE_HTTP2)

namespace http2_goaway_test {
using namespace std::chrono_literals;

struct observation final {
    std::binary_semaphore started_{0};
    std::atomic<bool> entered_{};
    std::atomic<bool> cancelled_{};
    std::atomic<int> uploads_{};
    std::atomic<std::size_t> uploaded_bytes_{};
};
observation& observed() {
    static observation value;
    return value;
}

class goaway_routes final {
public:
    RUVIA_ROUTES_BEGIN(goaway_routes)
    RUVIA_GET("/held", held);
    RUVIA_GET("/stalled", stalled);
    RUVIA_GET("/brief", brief);
    RUVIA_POST("/upload", upload);
    RUVIA_ROUTES_END

    // Ends only on its stop token.
    ruvia::task<ruvia::http_response> held(ruvia::context& context) {
        auto& state = observed();
        state.entered_ = true;
        const auto result = co_await ruvia::sleep_for(context.worker(), 1h, context.stop_token());
        state.cancelled_ = result == ruvia::timer_sleep_result::stop_requested;
        co_return context.text("held");
    }
    ruvia::task<ruvia::http_response> brief(ruvia::context& context) {
        co_return context.text("brief");
    }
    ruvia::task<ruvia::http_response> upload(ruvia::context& context) {
        const auto body = co_await context.req().text();
        observed().uploaded_bytes_ = body.size();
        ++observed().uploads_;
        co_return context.text("uploaded");
    }
    // A response stream that goes quiet after its first chunk; only the end
    // of its stream ends the sleep in time.
    ruvia::task<void> stalled(ruvia::context& context) {
        auto& writer = context.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write(std::string_view("ready"));
        try {
            (void)co_await writer.sleep(std::chrono::seconds(30));
        } catch (...) {
            observed().cancelled_ = true;
            throw;
        }
    }
};

using ruvia::test::app_run;

using ruvia::test::append_frame;
std::string request(std::uint32_t stream, std::string_view path, std::string_view method = "GET") {
    std::string block{};
    ruvia::hpack_encoder::encode_header(block, ":method", method);
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", path);
    std::string wire{};
    // END_HEADERS, and END_STREAM unless a request body follows.
    append_frame(wire, ruvia::http2_frame_type::headers, method == "GET" ? 0x5 : 0x4, stream, block);
    return wire;
}
std::string data_frame(std::uint32_t stream, std::string_view payload, bool end_stream) {
    std::string wire{};
    append_frame(wire, ruvia::http2_frame_type::data, end_stream ? 0x1 : 0x0, stream, payload);
    return wire;
}

using frame = ruvia::test::received_frame;
// Nonblocking reads bound every wire assertion, including a missing GOAWAY.
class peer final {
public:
    explicit peer(asio::ip::tcp::socket& socket)
        : socket_(socket) {
        socket_.non_blocking(true);
    }
    frame next() {
        return ruvia::test::read_frame(socket_, std::chrono::steady_clock::now() + 3s);
    }
    bool ended() {
        asio::error_code error;
        char byte{};
        const auto count = socket_.read_some(asio::buffer(&byte, 1), error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            return false;
        }
        if (error == asio::error::eof || error == asio::error::connection_reset) {
            return true;
        }
        if (error) {
            throw std::system_error(error);
        }
        (void)count;
        return false;
    }

private:
    asio::ip::tcp::socket& socket_;
};
std::uint32_t uint32(std::string_view bytes) {
    if (bytes.size() < 4) {
        throw std::runtime_error("truncated HTTP/2 GOAWAY frame");
    }
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    }
    return value;
}

void register_start_hook(ruvia::application& app) {
    static std::once_flag hooks;
    std::call_once(hooks, [&app] { app.on_start([] { observed().started_.release(); }); });
}

frame next_of_type(peer& client, ruvia::http2_frame_type type) {
    frame value;
    do {
        value = client.next();
    } while (value.header_.type_ != static_cast<std::uint8_t>(type) ||
             (type == ruvia::http2_frame_type::ping && (value.header_.flags_ & 0x1) != 0));
    return value;
}

// application::stop() disconnects directly (no drain): an HTTP/2 connection
// with a request in flight is closed at once, without GOAWAY or a response,
// and the request's handler observes its stop token.
void check_stop_closes_connection() {
    auto& state = observed();
    state.entered_ = false;
    state.cancelled_ = false;
    auto& app = ruvia::app();
    register_start_hook(app);
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    app_run run(app);
    if (!state.started_.try_acquire_for(3s)) {
        throw std::runtime_error("HTTP/2 app did not start");
    }
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    std::string opening(ruvia::http2_client_preface);
    append_frame(opening, ruvia::http2_frame_type::settings, 0, 0, {});
    opening.append(request(1, "/held"));
    asio::write(socket, asio::buffer(opening));
    peer client(socket);
    // The server's SETTINGS shows the session runs before stop begins.
    (void)next_of_type(client, ruvia::http2_frame_type::settings);
    ruvia::test::wait_until([&state] { return state.entered_.load(); }, 3s);

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    // Every frame the server still sends precedes the close; none is a
    // GOAWAY or a response to the admitted stream.
    for (;;) {
        frame received;
        try {
            received = client.next();
        } catch (const std::system_error&) {
            break;
        }
        EXPECT_TRUE(received.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(received.header_.stream_id_ != 1);
    }
    run.join();
    EXPECT_TRUE(std::chrono::steady_clock::now() - stop_started < 3s);
    EXPECT_TRUE(state.cancelled_.load());
}

// Serves `config` and opens an HTTP/2 connection (preface, SETTINGS carrying
// `settings_payload` unless it is absent, then `requests`) that never
// acknowledges the server's PING.
class silent_peer_run final {
public:
    silent_peer_run(ruvia::server_config config, std::string_view requests,
        std::optional<std::string_view> settings_payload = std::string_view{})
        : app_(ruvia::app()),
          reservation_(io_),
          endpoint_(reservation_.endpoint()),
          run_(configure(app_, std::move(config), endpoint_)),
          socket_(io_) {
        if (!observed().started_.try_acquire_for(3s)) {
            throw std::runtime_error("HTTP/2 drain app did not start");
        }
        socket_.connect(endpoint_);
        std::string opening(ruvia::http2_client_preface);
        if (settings_payload.has_value()) {
            append_frame(opening, ruvia::http2_frame_type::settings, 0, 0, *settings_payload);
        }
        opening.append(requests);
        asio::write(socket_, asio::buffer(opening));
    }
    [[nodiscard]] ruvia::application& app() noexcept {
        return app_;
    }
    [[nodiscard]] asio::ip::tcp::socket& socket() noexcept {
        return socket_;
    }
    void join() {
        run_.join();
    }

private:
    static ruvia::application& configure(
        ruvia::application& app, ruvia::server_config config, const asio::ip::tcp::endpoint& endpoint) {
        register_start_hook(app);
        app.server(std::move(config));
        app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
        return app;
    }

    asio::io_context io_;
    ruvia::application& app_;
    ruvia::test::tcp_port_reservation reservation_;
    asio::ip::tcp::endpoint endpoint_;
    app_run run_;
    asio::ip::tcp::socket socket_;
};

// The GOAWAY(2^31-1) and PING a connection's drain starts with.
void expect_round_trip_start(peer& client) {
    const auto first = next_of_type(client, ruvia::http2_frame_type::goaway);
    EXPECT_EQ(uint32(first.payload_) & 0x7fffffffU, 0x7fffffffU);
    (void)next_of_type(client, ruvia::http2_frame_type::ping);
}

// The final GOAWAY(NO_ERROR) with `last_stream_id`, then the server's end of
// the connection.
void expect_final_goaway_and_end(
    peer& client, std::uint32_t last_stream_id) {
    const auto last = next_of_type(client, ruvia::http2_frame_type::goaway);
    EXPECT_EQ(uint32(last.payload_) & 0x7fffffffU, last_stream_id);
    EXPECT_EQ(uint32(std::string_view(last.payload_).substr(4)), 0U);
    ruvia::test::wait_until([&client] { return client.ended(); }, 3s);
}

void check_idle_timeout_drain(bool stalled_stream) {
    auto& state = observed();
    state.entered_ = false;
    state.cancelled_ = false;
    silent_peer_run run(
        {.worker_count_ = 1, .idle_timeout_ = 300ms, .connection_scan_interval_ = 5ms},
        stalled_stream ? request(1, "/stalled") : std::string());
    peer client(run.socket());
    expect_round_trip_start(client);
    // Without the ACK the drain still ends within its round-trip bound
    // (1 s): a stalled stream is cancelled, and the final GOAWAY precedes the
    // close.
    expect_final_goaway_and_end(client, stalled_stream ? 1U : 0U);
    // Ends the server's lingering close at once.
    run.socket().close();
    if (stalled_stream) {
        ruvia::test::wait_until([&state] { return state.cancelled_.load(); }, 3s);
    }
    run.app().stop();
    run.join();
}

[[nodiscard]] bool is_frame(const frame& value, ruvia::http2_frame_type type, std::uint32_t stream_id) {
    return value.header_.type_ == static_cast<std::uint8_t>(type) && value.header_.stream_id_ == stream_id;
}

// Pings the server and returns the frames it sent up to the ACK, or up to a
// GOAWAY (a drain may never answer).
std::vector<frame> ping_round_trip(asio::ip::tcp::socket& socket, peer& client, std::uint8_t tag) {
    const std::string opaque(8, static_cast<char>(tag));
    std::string wire{};
    append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, opaque);
    asio::write(socket, asio::buffer(wire));
    std::vector<frame> frames{};
    for (;;) {
        auto value = client.next();
        const bool ack = is_frame(value, ruvia::http2_frame_type::ping, 0) && (value.header_.flags_ & 0x1) != 0 &&
                         value.payload_ == opaque;
        const bool goaway = is_frame(value, ruvia::http2_frame_type::goaway, 0);
        frames.push_back(std::move(value));
        if (ack || goaway) {
            return frames;
        }
    }
}

// Every 50 ms sends `traffic` and a PING -- input that would renew any
// connection-wide inactivity clock -- until the server sends a frame `done`
// accepts. Returns every frame received; fails after 3 s.
template <typename predicate_type>
std::vector<frame> keep_busy_until(peer& client, asio::ip::tcp::socket& socket, std::string_view traffic,
    predicate_type done) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    std::vector<frame> seen{};
    for (std::uint8_t tag = 1;; ++tag) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("HTTP/2 deadline did not fire under peer traffic");
        }
        asio::write(socket, asio::buffer(traffic));
        bool finished = false;
        for (auto& value : ping_round_trip(socket, client, tag)) {
            finished = finished || done(value);
            seen.push_back(std::move(value));
        }
        if (finished) {
            return seen;
        }
        std::this_thread::sleep_for(50ms);
    }
}

// Reads until stream `stream_id` ends; a GOAWAY on the way fails the check.
void expect_stream_end(peer& client, std::uint32_t stream_id) {
    for (;;) {
        const auto value = client.next();
        EXPECT_TRUE(!is_frame(value, ruvia::http2_frame_type::goaway, 0));
        EXPECT_TRUE(!is_frame(value, ruvia::http2_frame_type::rst_stream, stream_id));
        const bool ending = is_frame(value, ruvia::http2_frame_type::headers, stream_id) ||
                            is_frame(value, ruvia::http2_frame_type::data, stream_id);
        if (ending && (value.header_.flags_ & 0x1) != 0) {
            return;
        }
    }
}

// The RST_STREAM error code the server sent for `stream_id`; also checks
// that no GOAWAY ended the connection meanwhile.
std::uint32_t reset_code(const std::vector<frame>& frames,
    std::uint32_t stream_id) {
    std::uint32_t code = 0xffffffffU;
    for (const auto& value : frames) {
        EXPECT_TRUE(!is_frame(value, ruvia::http2_frame_type::goaway, 0));
        if (is_frame(value, ruvia::http2_frame_type::rst_stream, stream_id)) {
            code = uint32(value.payload_);
        }
    }
    return code;
}
}  // namespace http2_goaway_test

TEST(Ruvia, http2_app_stop_closes_connection_without_goaway) {
    ruvia::test::reset_app_callbacks(ruvia::app());
    http2_goaway_test::check_stop_closes_connection();
}

// A connection timeout drains the connection (RFC 9113 §6.8) instead of
// cutting it: the final GOAWAY and an orderly end follow within the
// round-trip bound, not a second idle timeout that closes the socket without
// them.
TEST(Ruvia, http2_idle_timeout_drain_ends_with_final_goaway) {
    http2_goaway_test::check_idle_timeout_drain(false);
}
TEST(Ruvia, http2_idle_timeout_drain_cancels_stalled_stream_after_final_goaway) {
    http2_goaway_test::check_idle_timeout_drain(true);
}

// RFC 9113 section 6.8: "Endpoints SHOULD always send a GOAWAY frame before
// closing a connection" -- also when the peer's end of input ends the session.
TEST(Ruvia, http2_peer_end_of_input_gets_final_goaway_before_close) {
    using namespace http2_goaway_test;
    silent_peer_run run({.worker_count_ = 1, .connection_scan_interval_ = 5ms}, {});
    peer client(run.socket());
    (void)next_of_type(client, ruvia::http2_frame_type::settings);
    run.socket().shutdown(asio::ip::tcp::socket::shutdown_send);
    expect_final_goaway_and_end(client, 0);
    run.socket().close();
    run.app().stop();
    run.join();
}

// Pingora's h2_idle_timeout: idleness runs from the end of the last stream,
// and PINGs (with the server's ACK writes) open no stream, so they do not
// renew it.
TEST(Ruvia, http2_idle_timeout_runs_from_last_stream_end_despite_pings) {
    using namespace http2_goaway_test;
    silent_peer_run run(
        {.worker_count_ = 1, .idle_timeout_ = 300ms, .connection_scan_interval_ = 5ms}, request(1, "/brief"));
    peer client(run.socket());
    expect_stream_end(client, 1);
    const auto frames = keep_busy_until(client, run.socket(), {}, [](const frame& value) {
        return is_frame(value, ruvia::http2_frame_type::goaway, 0);
    });
    EXPECT_EQ(uint32(frames.back().payload_) & 0x7fffffffU, 0x7fffffffU);
    expect_final_goaway_and_end(client, 1);
    run.socket().close();
    run.app().stop();
    run.join();
}

// request_body_timeout_ times each stream's body: a stalled body is reset
// even while another stream's DATA keeps the connection busy, and only that
// stream ends.
TEST(Ruvia, http2_request_body_timeout_resets_only_the_stalled_stream) {
    using namespace http2_goaway_test;
    observed().uploads_ = 0;
    observed().uploaded_bytes_ = 0;
    silent_peer_run run(
        {.worker_count_ = 1, .connection_scan_interval_ = 5ms, .request_body_timeout_ = 300ms},
        request(1, "/upload", "POST") + data_frame(1, "a", false) + request(3, "/upload", "POST"));
    peer client(run.socket());
    const auto frames = keep_busy_until(client, run.socket(), data_frame(3, "b", false), [](const frame& value) {
        return is_frame(value, ruvia::http2_frame_type::rst_stream, 1);
    });
    // RFC 9113 section 7: CANCEL.
    EXPECT_EQ(reset_code(frames, 1), 8U);
    EXPECT_EQ(reset_code(frames, 3), 0xffffffffU);
    const auto finish = data_frame(3, {}, true);
    asio::write(run.socket(), asio::buffer(finish));
    expect_stream_end(client, 3);
    EXPECT_EQ(observed().uploads_.load(), 1);
    EXPECT_TRUE(observed().uploaded_bytes_.load() != 0);
    run.socket().close();
    run.app().stop();
    run.join();
}

// write_timeout_ bounds a stream's wait for peer flow-control window, as
// Pingora's h2 write timeout does: a peer that never opens the window gets
// that stream reset, while the connection keeps serving.
TEST(Ruvia, http2_send_window_wait_is_bounded_by_write_timeout) {
    using namespace http2_goaway_test;
    // SETTINGS_INITIAL_WINDOW_SIZE = 0.
    silent_peer_run run({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .write_timeout_ = 300ms},
        request(1, "/brief"), std::string_view("\x00\x04\x00\x00\x00\x00", 6));
    peer client(run.socket());
    const auto frames = keep_busy_until(client, run.socket(), {}, [](const frame& value) {
        return is_frame(value, ruvia::http2_frame_type::rst_stream, 1);
    });
    EXPECT_EQ(reset_code(frames, 1), 8U);
    std::string reopened{};
    append_frame(reopened, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x04\x00\x00\xff\xff", 6));
    reopened.append(request(3, "/brief"));
    asio::write(run.socket(), asio::buffer(reopened));
    expect_stream_end(client, 3);
    run.socket().close();
    run.app().stop();
    run.join();
}

// Until the client preface and its SETTINGS arrive the connection is not
// ready: the request-header deadlines bound it, not idle_timeout_.
TEST(Ruvia, http2_readiness_is_bounded_by_header_completion_timeout) {
    using namespace http2_goaway_test;
    silent_peer_run run({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .header_completion_timeout_ = 300ms},
        {}, std::nullopt);
    peer client(run.socket());
    expect_round_trip_start(client);
    expect_final_goaway_and_end(client, 0);
    run.socket().close();
    run.app().stop();
    run.join();
}

// A connection whose max_requests_per_connection_ is reached starts its
// graceful GOAWAY drain on the request head that reached the limit. The first
// GOAWAY advertises 2^31-1 (RFC 9113 section 6.8), so that head is served like
// any accepted stream even when it is a CONNECT tunnel or an RFC 8441 WebSocket
// handshake.

namespace http2_request_limit_test {
using namespace std::chrono_literals;

std::binary_semaphore& started() {
    static std::binary_semaphore value{0};
    return value;
}

class limit_routes final {
public:
    RUVIA_ROUTES_BEGIN(limit_routes)
    RUVIA_CONNECT("target.test:443", tunnel);
    RUVIA_GET_WS("/socket", socket);
    RUVIA_ROUTES_END

    // Both long-lived handlers run until the client closes the connection.
    ruvia::task<void> tunnel(ruvia::context& context_value) {
        auto& tunnel_value = context_value.tunnel();
        while (co_await tunnel_value.read()) {
        }
    }
    ruvia::task<void> socket(ruvia::context& context_value) {
        auto& socket_value = context_value.websocket();
        while (co_await socket_value.read()) {
        }
    }
};

using ruvia::test::append_frame;
using ruvia::test::read_frame;
std::uint32_t limit_uint32(std::string_view bytes) {
    if (bytes.size() < 4) {
        throw std::runtime_error("truncated HTTP/2 frame");
    }
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    }
    return value;
}

// The server's answer to stream 1: the decoded :status of its first HEADERS
// frame, or "RST_STREAM:<code>" when the stream was reset first. GOAWAY frames
// seen on the way record their last stream id.
struct stream_outcome final {
    std::string answer_{};
    std::optional<std::uint32_t> goaway_last_stream_id_;
};
stream_outcome first_outcome(asio::ip::tcp::socket& socket, ruvia::hpack_decoder& decoder) {
    stream_outcome outcome;
    for (;;) {
        const auto value = read_frame(socket);
        if (value.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway)) {
            outcome.goaway_last_stream_id_ = limit_uint32(value.payload_) & 0x7fffffffU;
            continue;
        }
        if (value.header_.stream_id_ != 1) {
            continue;
        }
        if (value.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
            outcome.answer_ = "RST_STREAM:" + std::string(std::to_string(limit_uint32(value.payload_)));
            return outcome;
        }
        if (value.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            const auto decoded = decoder.decode(value.payload_, [&](std::string_view name, std::string_view field) {
                if (name == ":status") {
                    outcome.answer_ = field;
                }
                return true;
            });
            if (decoded.has_value()) {
                throw std::runtime_error("invalid HPACK response block");
            }
            return outcome;
        }
    }
}

enum class upgrade_kind : std::uint8_t {
    connect_tunnel,
    websocket,
};
stream_outcome open_upgrade(const asio::ip::tcp::endpoint& endpoint, upgrade_kind kind) {
    asio::io_context io;
    ruvia::hpack_decoder decoder;
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    std::string wire(ruvia::http2_client_preface);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
    if (kind == upgrade_kind::websocket) {
        ruvia::hpack_encoder::encode_header(block, ":protocol", "websocket");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(block, ":path", "/socket");
        ruvia::hpack_encoder::encode_header(block, "sec-websocket-version", "13");
    } else {
        ruvia::hpack_encoder::encode_header(block, ":authority", "target.test:443");
    }
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    asio::write(socket, asio::buffer(wire));
    auto outcome = first_outcome(socket, decoder);
    // Closing the transport ends the established tunnel or socket.
    socket.close();
    return outcome;
}
}  // namespace http2_request_limit_test

TEST(Ruvia, http2_upgrade_on_the_request_that_reaches_the_connection_limit_is_served) {
    using namespace http2_request_limit_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    using namespace std::chrono_literals;
    auto& app = ruvia::app();
    app.on_start([] { started().release(); });
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .max_requests_per_connection_ = 1});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    ruvia::test::app_run thread(app);
    if (!started().try_acquire_for(3s)) {
        app.stop();
        thread.join();
        throw std::runtime_error("HTTP/2 request limit app did not start");
    }
    std::optional<stream_outcome> tunnel;
    std::optional<stream_outcome> websocket;
    std::exception_ptr client_failure;
    try {
        tunnel = open_upgrade(endpoint, upgrade_kind::connect_tunnel);
        websocket = open_upgrade(endpoint, upgrade_kind::websocket);
    } catch (...) {
        client_failure = std::current_exception();
    }
    app.stop();
    thread.join();
    if (client_failure) {
        std::rethrow_exception(client_failure);
    }
    // The limit was reached by this very request: the drain's first GOAWAY
    // (2^31-1) precedes its response, and the upgrade still completes on it.
    EXPECT_EQ(tunnel->answer_, std::string("200"));
    EXPECT_TRUE(tunnel->goaway_last_stream_id_.has_value());
    EXPECT_EQ(tunnel->goaway_last_stream_id_.value_or(0), 0x7fffffffU);
    EXPECT_EQ(websocket->answer_, std::string("200"));
    EXPECT_TRUE(websocket->goaway_last_stream_id_.has_value());
    EXPECT_EQ(websocket->goaway_last_stream_id_.value_or(0), 0x7fffffffU);
}

namespace http2_frame_observation_test {
using namespace std::chrono_literals;

class observer_controller final {
public:
    RUVIA_ROUTES_BEGIN(observer_controller)
    RUVIA_GET("/fingerprint", fingerprint);
    RUVIA_ROUTES_END
    ruvia::task<ruvia::http_response> fingerprint(ruvia::context& context) {
        co_return context.text(std::to_string(context.conn().connection_id()));
    }
};

struct frame_observation final {
    std::vector<ruvia::http2_frame_metadata> frames_;
    std::vector<std::string> pseudo_names_;
    std::vector<std::uint64_t> first_frames_;
    std::size_t begins_{0};
    std::size_t ends_{0};
};

class observation_log final {
public:
    void frame(const ruvia::http2_frame_record& record) {
        std::lock_guard lock(mutex_);
        connections_[record.connection_id()].frames_.push_back(record.metadata());
    }
    void header(const ruvia::http2_headers_record& record) {
        std::lock_guard lock(mutex_);
        auto& result = connections_[record.connection_id()];
        const auto& metadata = record.metadata();
        if (metadata.phase_ == ruvia::http2_header_observation_phase::begin) {
            ++result.begins_;
        } else if (metadata.phase_ == ruvia::http2_header_observation_phase::end) {
            ++result.ends_;
        } else if (metadata.name_.starts_with(':')) {
            result.pseudo_names_.emplace_back(metadata.name_);
            result.first_frames_.push_back(metadata.first_frame_observation_sequence_);
        }
    }
    frame_observation get(std::uint64_t id) const {
        std::lock_guard lock(mutex_);
        return connections_.at(id);
    }

private:
    mutable std::mutex mutex_;
    std::map<std::uint64_t, frame_observation> connections_;
};

void setting(std::string& payload, std::uint16_t id, std::uint32_t value) {
    const std::array<char, 6> bytes{static_cast<char>(id >> 8), static_cast<char>(id),
        static_cast<char>(value >> 24), static_cast<char>(value >> 16),
        static_cast<char>(value >> 8), static_cast<char>(value)};
    payload.append(bytes.data(), bytes.size());
}

std::string opening(bool reverse, bool empty_initial) {
    std::string wire(ruvia::http2_client_preface);
    if (empty_initial) {
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    }
    std::string payload;
    setting(payload, 3, 17);
    setting(payload, 0xfafa, 101);
    setting(payload, 3, 29);
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, payload);
    // Exclusive dependency zero, weight 256: a standalone PRIORITY on idle 3.
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::priority, 0, 3,
        std::string_view("\x80\0\0\0\xff", 5));
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0,
        std::string_view("\0\x70\0\x01", 4));
    std::string block;
    const std::array names = reverse
                                 ? std::array{":path", ":authority", ":scheme", ":method"}
                                 : std::array{":method", ":scheme", ":authority", ":path"};
    for (const std::string_view name : names) {
        const auto value = name == ":method" ? "GET" : name == ":scheme"  ? "https"
                                                   : name == ":authority" ? "localhost"
                                                                          : "/fingerprint";
        ruvia::hpack_encoder::encode_header(block, name, value);
    }
    // PADDED | PRIORITY | END_STREAM, followed by two CONTINUATION frames.
    payload.assign(1, '\x02');
    payload.append("\0\0\0\x03\x29", 5);
    payload.append(block.data(), 1);
    payload.append(2, '\0');
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x29, 1, payload);
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::continuation, 0, 1,
        std::string_view(block).substr(1, 2));
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::continuation, 0x4, 1,
        std::string_view(block).substr(3));
    return wire;
}

std::uint64_t exchange(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    bool reverse, bool empty_initial, std::size_t fragment) {
    asio::ssl::context tls(asio::ssl::context::tls_client);
    tls.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, tls);
    static constexpr unsigned char alpn[]{2, 'h', '2'};
    if (SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)) != 0 ||
        SSL_set_tlsext_host_name(stream.native_handle(), "localhost") != 1) {
        throw std::runtime_error("cannot configure TLS h2 test client");
    }
    stream.next_layer().connect(endpoint);
    stream.handshake(asio::ssl::stream_base::client);
    const unsigned char* selected = nullptr;
    unsigned selected_size = 0;
    SSL_get0_alpn_selected(stream.native_handle(), &selected, &selected_size);
    if (std::string_view(reinterpret_cast<const char*>(selected), selected_size) != "h2") {
        throw std::runtime_error("TLS client did not negotiate h2");
    }
    const auto wire = opening(reverse, empty_initial);
    for (std::size_t cursor = 0; cursor < wire.size(); cursor += fragment) {
        asio::write(stream, asio::buffer(std::string_view(wire).substr(cursor, fragment)));
    }
    std::string body;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        const auto frame = ruvia::test::read_frame(stream, deadline);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
            frame.header_.stream_id_ == 1) {
            body.append(frame.payload_);
            if ((frame.header_.flags_ & 1) != 0) {
                return std::stoull(body);
            }
        }
    }
}
}  // namespace http2_frame_observation_test

TEST(Ruvia, http2_typed_observers_fingerprint_tls_frames_independently_of_record_boundaries) {
    using namespace http2_frame_observation_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    std::binary_semaphore started{0};
    observation_log observations;
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .https_ = endpoint.port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}})
        .on_start([&] { started.release(); })
        .on_http2_frame([&](const ruvia::http2_frame_record& record) noexcept { observations.frame(record); })
        .on_http2_headers([&](const ruvia::http2_headers_record& record) noexcept { observations.header(record); });
    {
        ruvia::test::app_run running(app);
        ASSERT_TRUE(started.try_acquire_for(10s));
        EXPECT_THROW(app.on_http2_frame(nullptr), std::logic_error);
        EXPECT_THROW(app.on_http2_headers(nullptr), std::logic_error);
        for (const bool reverse : {false, true}) {
            for (const bool empty_initial : {false, true}) {
                for (const auto fragment : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
                    const auto id = exchange(io, endpoint, reverse, empty_initial, fragment);
                    const auto observed = observations.get(id);
                    ASSERT_EQ(observed.frames_.size(), empty_initial ? 7u : 6u);
                    for (std::size_t i = 0; i < observed.frames_.size(); ++i) {
                        EXPECT_EQ(observed.frames_[i].observation_sequence_, i);
                    }
                    const auto offset = empty_initial ? 1u : 0u;
                    if (empty_initial) {
                        EXPECT_EQ(observed.frames_[0].settings().size(), 0u);
                    }
                    const auto& settings = observed.frames_[offset];
                    ASSERT_EQ(settings.settings().size(), 3u);
                    EXPECT_TRUE(settings.settings_complete_);
                    EXPECT_EQ(settings.settings()[0].value_, 17u);
                    EXPECT_EQ(static_cast<std::uint16_t>(settings.settings()[1].id_), 0xfafa);
                    EXPECT_EQ(settings.settings()[1].value_, 101u);
                    EXPECT_EQ(settings.settings()[2].value_, 29u);
                    ASSERT_TRUE(observed.frames_[offset + 1].priority_);
                    EXPECT_EQ(observed.frames_[offset + 1].priority_->weight_, 256u);
                    EXPECT_TRUE(observed.frames_[offset + 1].priority_->exclusive_);
                    ASSERT_TRUE(observed.frames_[offset + 2].window_increment_);
                    EXPECT_EQ(*observed.frames_[offset + 2].window_increment_, 7340033u);
                    ASSERT_TRUE(observed.frames_[offset + 3].priority_);
                    EXPECT_EQ(observed.frames_[offset + 3].priority_->stream_dependency_, 3u);
                    EXPECT_EQ(observed.frames_[offset + 3].priority_->weight_, 42u);
                    EXPECT_TRUE(observed.frames_[offset + 4].type_ == ruvia::http2_frame_type::continuation);
                    EXPECT_TRUE(observed.frames_[offset + 5].type_ == ruvia::http2_frame_type::continuation);
                    const std::vector<std::string> expected = reverse
                                                                  ? std::vector<std::string>{":path", ":authority", ":scheme", ":method"}
                                                                  : std::vector<std::string>{":method", ":scheme", ":authority", ":path"};
                    EXPECT_EQ(observed.pseudo_names_, expected);
                    ASSERT_EQ(observed.first_frames_.size(), 4u);
                    for (const auto sequence : observed.first_frames_) {
                        EXPECT_EQ(sequence, offset + 3u);
                    }
                    EXPECT_EQ(observed.begins_, 1u);
                    EXPECT_EQ(observed.ends_, 1u);
                }
            }
        }
        app.stop();
        running.join();
    }
    app.on_http2_frame(nullptr).on_http2_headers(nullptr);
}
#endif

namespace route_dispatch_isolation_test {

using namespace std::chrono_literals;

enum class invalid_route_kind {
    none,
    dynamic_conflict,
    dot_segment,
    encoded_duplicate,
    invalid_protocol,
};

ruvia::event_loop control_loop() {
    struct runtime final {
        ruvia::blocking_pool blocking_{{.thread_count_ = 1}};
        ruvia::event_loop_pool loops_{{.loop_count_ = 1, .blocking_pool_ = &blocking_}};

        runtime() {
            loops_.start();
        }
        ~runtime() {
            loops_.join();
        }
    };
    static runtime value;
    return value.loops_.loop(0);
}

std::atomic<int> controller_constructions{0};
std::atomic<int> middleware_constructions{0};
std::atomic<bool> reject_controller{false};
std::atomic<bool> reject_middleware{false};
std::atomic<invalid_route_kind> active_invalid_route{invalid_route_kind::none};
std::binary_semaphore isolation_started{0};

struct scoped_isolation_mode final {
    scoped_isolation_mode(invalid_route_kind kind, bool reject_ctrl = false, bool reject_mw = false) noexcept {
        active_invalid_route.store(kind);
        reject_controller.store(reject_ctrl);
        reject_middleware.store(reject_mw);
    }
    ~scoped_isolation_mode() {
        active_invalid_route.store(invalid_route_kind::none);
        reject_controller.store(false);
        reject_middleware.store(false);
    }
};

class conflicting_controller final {
public:
    conflicting_controller() {
        ++controller_constructions;
        if (reject_controller.load()) {
            throw std::runtime_error("unused controller constructed");
        }
    }

    RUVIA_ROUTES_BEGIN(conflicting_controller)
    switch (active_invalid_route.load()) {
        case invalid_route_kind::none:
            break;
        case invalid_route_kind::dynamic_conflict:
            RUVIA_GET("/conflict/:id/files", first);
            RUVIA_GET("/conflict/:name/files", second);
            break;
        case invalid_route_kind::dot_segment:
            RUVIA_GET("/a/../b", first);
            break;
        case invalid_route_kind::encoded_duplicate:
            RUVIA_GET("/dup/admin", first);
            RUVIA_GET("/dup/%61dmin", second);
            break;
        case invalid_route_kind::invalid_protocol:
            RUVIA_CONNECT_PROTOCOL("bad protocol", "/foo/:host/:port", tunnel);
            break;
    }
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> first(ruvia::context& context) {
        co_return context.text("first");
    }

    ruvia::task<ruvia::http_response> second(ruvia::context& context) {
        co_return context.text("second");
    }

    ruvia::task<void> tunnel(ruvia::context&) {
        co_return;
    }
};

class unused_middleware final {
public:
    unused_middleware() {
        ++middleware_constructions;
        if (reject_middleware.load()) {
            throw std::runtime_error("unused middleware constructed");
        }
    }

    ruvia::task<void> invoke(ruvia::context&, ruvia::next& next) {
        co_await next();
    }
};

ruvia::task<void> serve_connection(ruvia::server_connection& connection) {
    (void)co_await connection.read();
    constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\nconnection";
    co_await connection.write(std::as_bytes(std::span(response.data(), response.size())));
    co_await connection.finish();
}

ruvia::task<void> serve_request(ruvia::request_stream& request) {
    (void)co_await request.response().commit();
    co_await request.response().write(std::string_view("request-stream"));
    co_await request.response().end();
}

}  // namespace route_dispatch_isolation_test

TEST(Ruvia, route_conflict_rejects_equal_precedence_dynamic_routes_at_startup) {
    using namespace route_dispatch_isolation_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    const scoped_isolation_mode mode(invalid_route_kind::dynamic_conflict);
    ruvia::test_app app;
    std::string message{};
    try {
        (void)app.request(ruvia::test_request::get("/conflict/1/files"));
    } catch (const std::invalid_argument& error) {
        message = error.what();
    }
    EXPECT_TRUE(message.find("GET /conflict/:id/files") != std::string::npos);
    EXPECT_TRUE(message.find("GET /conflict/:name/files") != std::string::npos);
}

TEST(Ruvia, route_dot_segment_pattern_is_rejected_at_startup) {
    using namespace route_dispatch_isolation_test;
    const scoped_isolation_mode mode(invalid_route_kind::dot_segment);
    ruvia::test_app app;
    std::string message{};
    try {
        (void)app.request(ruvia::test_request::get("/b"));
    } catch (const std::invalid_argument& error) {
        message = error.what();
    }
    EXPECT_TRUE(message.find("dot segment") != std::string::npos);
}

TEST(Ruvia, route_encoded_duplicate_rejects_equivalent_spellings_at_startup) {
    using namespace route_dispatch_isolation_test;
    const scoped_isolation_mode mode(invalid_route_kind::encoded_duplicate);
    ruvia::test_app app;
    bool rejected = false;
    try {
        (void)app.request(ruvia::test_request::get("/dup/admin"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, route_protocol_rejects_invalid_connect_protocol_at_startup) {
    using namespace route_dispatch_isolation_test;
    const scoped_isolation_mode mode(invalid_route_kind::invalid_protocol);
    ruvia::test_app app;
    bool rejected = false;
    try {
        (void)app.request(ruvia::test_request::get("/foo/example.test/53"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, nonrouted_listeners_ignore_conflicts_and_unused_controller_and_middleware_constructors) {
    using namespace route_dispatch_isolation_test;
    const scoped_isolation_mode mode(invalid_route_kind::dynamic_conflict, true, true);
    asio::io_context io;
    auto& app = ruvia::app();
    controller_constructions = 0;
    middleware_constructions = 0;
    app.server({.worker_count_ = 2})
        .use<unused_middleware>()
        .on_connection(&serve_connection)
        .on_request_stream(&serve_request)
        .on_start(nullptr)
        .on_start([] { isolation_started.release(); });
    for (const auto dispatch : {ruvia::listener_dispatch::connection, ruvia::listener_dispatch::request_stream}) {
        ruvia::test::tcp_port_reservation reservation(io);
        const auto endpoint = reservation.endpoint();
        app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port(), .dispatch_ = dispatch});
        ruvia::test::app_run run(app);
        ASSERT_TRUE(isolation_started.try_acquire_for(10s));
        const auto response = ruvia::test::exchange(io, endpoint,
            "GET /conflict/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
        EXPECT_NE(response.find(dispatch == ruvia::listener_dispatch::connection ? "connection" : "request-stream"), response.npos);
        app.stop();
        run.join();
    }
    EXPECT_EQ(controller_constructions.load(), 0);
    EXPECT_EQ(middleware_constructions.load(), 0);
}

TEST(Ruvia, mixed_routed_and_nonrouted_listeners_still_reject_conflicting_routes) {
    using namespace route_dispatch_isolation_test;
    const scoped_isolation_mode mode(invalid_route_kind::dynamic_conflict, false, false);
    asio::io_context io;
    ruvia::test::tcp_port_reservation raw(io);
    ruvia::test::tcp_port_reservation routed(io);
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({
            {.address_ = "127.0.0.1", .http_ = raw.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection},
            {.address_ = "127.0.0.1", .http_ = routed.endpoint().port()},
        })
        .on_connection(&serve_connection)
        .on_start(nullptr);
    EXPECT_THROW(app.run(), std::invalid_argument);
}

TEST(Ruvia, adding_first_routed_listener_rejects_conflicts_without_interrupting_nonrouted_service) {
    using namespace route_dispatch_isolation_test;
    const scoped_isolation_mode mode(invalid_route_kind::dynamic_conflict, false, false);
    asio::io_context io;
    ruvia::test::tcp_port_reservation raw(io);
    ruvia::test::tcp_port_reservation routed(io);
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .http_ = raw.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection})
        .on_connection(&serve_connection)
        .on_start(nullptr)
        .on_start([] { isolation_started.release(); });
    ruvia::test::app_run run(app);
    ASSERT_TRUE(isolation_started.try_acquire_for(10s));
    ruvia::test::wait_until([&app] {
        try {
            control_loop().start(app.remove_listener(control_loop().worker(), {.address_ = "0.0.0.0", .port_ = 9})).get();
        } catch (const std::invalid_argument&) {
            return true;
        } catch (const std::logic_error&) {
        }
        return false;
    },
        10s);
    EXPECT_THROW(control_loop().start(app.add_listener(control_loop().worker(), {.address_ = "127.0.0.1", .http_ = routed.endpoint().port()})).get(), std::invalid_argument);
    EXPECT_TRUE(ruvia::test::exchange(io, raw.endpoint(),
        "GET /still-serving HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
            .ends_with("connection"));
    app.stop();
    run.join();
}

namespace standalone_server_test {
using namespace std::chrono_literals;

std::atomic<bool> enable_standalone_conflict{false};
struct scoped_standalone_conflict final {
    scoped_standalone_conflict() noexcept {
        enable_standalone_conflict.store(true);
    }
    ~scoped_standalone_conflict() {
        enable_standalone_conflict.store(false);
    }
};
class conflicting_routes final {
public:
    RUVIA_ROUTES_BEGIN(conflicting_routes)
    if (enable_standalone_conflict.load()) {
        RUVIA_GET("/unused/:first", first);
        RUVIA_GET("/unused/:second", second);
    }
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

TEST(Ruvia, independent_servers_share_an_external_loop_without_routes_or_shared_shutdown) {
    using namespace standalone_server_test;
    const standalone_server_test::scoped_standalone_conflict conflict_guard;
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
    using namespace standalone_server_test;
    const standalone_server_test::scoped_standalone_conflict conflict_guard;
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
    using namespace standalone_server_test;
    const standalone_server_test::scoped_standalone_conflict conflict_guard;
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
    using namespace standalone_server_test;
    const standalone_server_test::scoped_standalone_conflict conflict_guard;
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
    using namespace standalone_server_test;
    const standalone_server_test::scoped_standalone_conflict conflict_guard;
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
    using namespace standalone_server_test;
    const standalone_server_test::scoped_standalone_conflict conflict_guard;
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
    using namespace standalone_server_test;
    const standalone_server_test::scoped_standalone_conflict conflict_guard;
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
    using namespace standalone_server_test;
    const standalone_server_test::scoped_standalone_conflict conflict_guard;
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
    using namespace standalone_server_test;
    const standalone_server_test::scoped_standalone_conflict conflict_guard;
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
    using namespace standalone_server_test;
    const standalone_server_test::scoped_standalone_conflict conflict_guard;
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

namespace request_stream_files_test {
using namespace std::chrono_literals;
namespace fs = std::filesystem;

template <typename root_type>
concept accepts_static_root = requires(ruvia::request_stream& stream, root_type&& root) {
    stream.static_file(std::forward<root_type>(root), ruvia::static_file_response_options{});
};

template <typename root_type>
concept accepts_default_static_root = requires(ruvia::request_stream& stream, root_type&& root) {
    stream.static_file(std::forward<root_type>(root));
};

static_assert(accepts_static_root<ruvia::static_root&>);
static_assert(accepts_static_root<const ruvia::static_root&>);
static_assert(!accepts_static_root<ruvia::static_root>);
static_assert(!accepts_static_root<const ruvia::static_root>);
static_assert(accepts_default_static_root<ruvia::static_root&>);
static_assert(accepts_default_static_root<const ruvia::static_root&>);
static_assert(!accepts_default_static_root<ruvia::static_root>);
static_assert(!accepts_default_static_root<const ruvia::static_root>);

void write_file(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string representation() {
    std::string bytes(100 * 1024 + 17, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>('a' + index % 23);
    }
    return bytes;
}

template <typename check_type>
void with_server(ruvia::request_stream_callback callback, check_type check) {
    asio::io_context io;
    ruvia::blocking_pool pool({.thread_count_ = 2, .queue_capacity_ = 16});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &pool});
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server server(attachment.loop(), {.request_stream_ = std::move(callback)});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        std::exception_ptr failure;
        try {
            co_await server.start();
            co_await check(client, server);
        } catch (...) {
            failure = std::current_exception();
        }
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
    pool.stop();
    pool.join();
    EXPECT_EQ(pool.stats().queued_, 0);
    EXPECT_EQ(pool.stats().running_, 0);
}

ruvia::task<void> expect_file_responses(ruvia::http_client& client, std::string_view bytes, std::string_view target) {
    auto full = co_await client.client().get(target).send();
    EXPECT_EQ(full.status(), ruvia::http_status::ok);
    EXPECT_EQ(full.header("content-type"), std::optional<std::string_view>("application/octet-stream"));
    EXPECT_EQ(full.header("content-length"), std::optional<std::string_view>(std::to_string(bytes.size())));
    EXPECT_FALSE(full.header("content-range").has_value());
    EXPECT_EQ(full.header("x-file"), std::optional<std::string_view>("retained"));
    EXPECT_EQ(full.header("set-cookie"), std::optional<std::string_view>("file=retained; Path=/"));
    const std::string etag(full.header("etag").value_or(""));
    EXPECT_FALSE(etag.empty());
    const auto body = co_await full.text();
    EXPECT_EQ(body.text(), bytes);

    auto head = co_await client.client().head(target).send();
    EXPECT_EQ(head.status(), ruvia::http_status::ok);
    EXPECT_EQ(head.header("content-type"), full.header("content-type"));
    EXPECT_EQ(head.header("etag"), std::optional<std::string_view>(etag));
    EXPECT_EQ(head.header("content-length"), std::optional<std::string_view>(std::to_string(bytes.size())));
    EXPECT_TRUE((co_await head.text()).empty());

    auto unchanged = co_await client.client().get(target).header("if-none-match", etag).send();
    EXPECT_EQ(unchanged.status(), ruvia::http_status::not_modified);
    EXPECT_EQ(unchanged.header("etag"), std::optional<std::string_view>(etag));
    EXPECT_FALSE(unchanged.header("content-type").has_value());
    EXPECT_FALSE(unchanged.header("content-range").has_value());
    EXPECT_TRUE((co_await unchanged.text()).empty());

    auto partial = co_await client.client().get(target).header("range", "bytes=7-19").send();
    EXPECT_EQ(partial.status(), ruvia::http_status::partial_content);
    EXPECT_EQ(partial.header("content-type"), std::optional<std::string_view>("application/octet-stream"));
    EXPECT_EQ(partial.header("content-length"), std::optional<std::string_view>("13"));
    EXPECT_EQ(partial.header("content-range"),
        std::optional<std::string_view>("bytes 7-19/" + std::to_string(bytes.size())));
    const auto partial_body = co_await partial.text();
    EXPECT_EQ(partial_body.text(), bytes.substr(7, 13));

    auto multipart = co_await client.client().get(target).header("range", "bytes=3-8,90000-90009").send();
    EXPECT_EQ(multipart.status(), ruvia::http_status::partial_content);
    EXPECT_TRUE(multipart.header("content-type").value_or("").starts_with("multipart/byteranges; boundary="));
    EXPECT_FALSE(multipart.header("content-range").has_value());
    const auto parts = co_await multipart.text();
    EXPECT_EQ(multipart.header("content-length"), std::optional<std::string_view>(std::to_string(parts.text().size())));
    EXPECT_NE(parts.text().find("Content-Type: application/octet-stream\r\n"), std::string_view::npos);
    EXPECT_NE(parts.text().find("Content-Range: bytes 3-8/102417"), std::string_view::npos);
    EXPECT_NE(parts.text().find("Content-Range: bytes 90000-90009/102417"), std::string_view::npos);
    EXPECT_NE(parts.text().find(bytes.substr(3, 6)), std::string_view::npos);
    EXPECT_NE(parts.text().find(bytes.substr(90000, 10)), std::string_view::npos);
}

void metadata(ruvia::request_stream& stream) {
    // A default-status draft must not replace the file factory's 206 or 304.
    stream.draft().header("x-file", "retained");
    stream.draft().header("set-cookie", "file=retained; Path=/");
}

}  // namespace request_stream_files_test

TEST(Ruvia, independent_request_stream_submits_complete_file_and_multipart_responses) {
    using namespace request_stream_files_test;
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-file");
    const auto path = directory.path() / "file.bin";
    const auto bytes = representation();
    write_file(path, bytes);
    with_server([&](ruvia::request_stream& stream) -> ruvia::task<void> {
        metadata(stream);
        auto caller_path = path;
        std::string type = "application/octet-stream";
        auto pending = stream.file({.path_ = caller_path, .content_type_ = type});
        caller_path = directory.path() / "missing";
        type = "invalid/later";
        auto response = co_await std::move(pending);
        if (response.status() == ruvia::http_status::not_modified) {
            EXPECT_FALSE(response.header("Content-Type").has_value());
        } else if (response.has_multipart_file_body()) {
            EXPECT_TRUE(response.header("Content-Type").value_or("").starts_with("multipart/byteranges; boundary="));
        } else {
            EXPECT_EQ(response.header("Content-Type"), std::optional<std::string_view>("application/octet-stream"));
        }
        stream.respond(std::move(response));
        EXPECT_FALSE(stream.response().committed()); }, [&](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> { co_await expect_file_responses(client, bytes, "/file"); });
}

TEST(Ruvia, independent_request_stream_static_root_rejects_paths_and_changed_identity) {
    using namespace request_stream_files_test;
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-static");
    const auto path = directory.path() / "file.bin";
    const auto replacement = directory.path() / "replacement.bin";
    const auto bytes = representation();
    write_file(path, bytes);
    write_file(replacement, std::string(bytes.size(), 'z'));
    ruvia::static_root root(directory.path(), {.default_content_type_ = "application/octet-stream",
                                                  .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}});
    with_server([&](ruvia::request_stream& stream) -> ruvia::task<void> {
        metadata(stream);
        const auto target = stream.request().path();
        if (target == "/deny") {
            stream.respond(co_await stream.static_file(root, {.relative_path_ = "../file.bin"}));
        } else if (target == "/absolute") {
            stream.respond(co_await stream.static_file(root, {.relative_path_ = "/file.bin"}));
        } else if (target == "/unindexed") {
            stream.respond(co_await stream.static_file(root, {.relative_path_ = "later.bin"}));
        } else {
            std::string relative = "file.bin";
            auto pending = stream.static_file(root, {.relative_path_ = relative});
            relative = "missing.bin";
            stream.respond(co_await std::move(pending));
        } }, [&](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> {
        co_await expect_file_responses(client, bytes, "/static");
        for (const auto target : {"/deny", "/absolute"}) {
            auto denied = co_await client.client().get(target).send();
            EXPECT_EQ(denied.status(), ruvia::http_status::forbidden);
            (void)co_await denied.text();
        }
        write_file(directory.path() / "later.bin", "not indexed");
        auto missing = co_await client.client().get("/unindexed").send();
        EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
        (void)co_await missing.text();
        fs::rename(path, directory.path() / "original.bin");
        fs::rename(replacement, path);
        auto changed = co_await client.client().head("/static").send();
        EXPECT_EQ(changed.status(), ruvia::http_status::internal_server_error);
        EXPECT_TRUE((co_await changed.text()).empty());
        auto stale = co_await client.client().get("/static").send();
        EXPECT_EQ(stale.status(), ruvia::http_status::internal_server_error);
        (void)co_await stale.text(); });
}

#ifndef _WIN32
// As in the shared file-transfer regression, moving a parent directory leaves
// the opened file's change time intact; moving the file itself does not.
// Windows does not permit this directory move while the file is open.
TEST(Ruvia, independent_request_stream_reuses_open_file_after_path_replacement) {
    using namespace request_stream_files_test;
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-open");
    const auto bytes = representation();
    const auto single = directory.path() / "single" / "file.bin";
    const auto partial = directory.path() / "partial" / "file.bin";
    const auto multiple = directory.path() / "multiple" / "file.bin";
    fs::create_directory(single.parent_path());
    fs::create_directory(partial.parent_path());
    fs::create_directory(multiple.parent_path());
    write_file(single, bytes);
    write_file(partial, bytes);
    write_file(multiple, bytes);
    with_server([&](ruvia::request_stream& stream) -> ruvia::task<void> {
        const auto target = stream.request().path();
        const auto path = target == "/single" ? single : target == "/partial" ? partial : multiple;
        auto response = co_await stream.file({.path_ = path});
        EXPECT_TRUE(response.has_file_representation());
        const auto replaced = co_await stream.run_blocking([path, bytes] {
            fs::rename(path.parent_path(), fs::path(path.parent_path()).concat(".opened"));
            fs::create_directory(path.parent_path());
            write_file(path, std::string(bytes.size(), 'Z'));
        });
        EXPECT_EQ(replaced.status(), ruvia::blocking_status::completed);
        replaced.value();
        stream.respond(std::move(response)); }, [&](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> {
        auto single_response = co_await client.client().get("/single").send();
        EXPECT_EQ(single_response.status(), ruvia::http_status::ok);
        const auto single_body = co_await single_response.text();
        EXPECT_EQ(single_body.text(), bytes);
        auto partial_response = co_await client.client().get("/partial").header("range", "bytes=7-19").send();
        EXPECT_EQ(partial_response.status(), ruvia::http_status::partial_content);
        EXPECT_EQ(partial_response.header("content-range"), std::optional<std::string_view>("bytes 7-19/102417"));
        const auto partial_body = co_await partial_response.text();
        EXPECT_EQ(partial_body.text(), std::string_view(bytes).substr(7, 13));
        auto multiple_response = co_await client.client().get("/multiple").header("range", "bytes=0-9,90000-90009").send();
        EXPECT_EQ(multiple_response.status(), ruvia::http_status::partial_content);
        const auto parts = co_await multiple_response.text();
        EXPECT_NE(parts.text().find(bytes.substr(0, 10)), std::string_view::npos);
        EXPECT_NE(parts.text().find(bytes.substr(90000, 10)), std::string_view::npos);
        EXPECT_EQ(parts.text().find(std::string(10, 'Z')), std::string_view::npos); });
}
#endif

TEST(Ruvia, independent_request_stream_revalidates_open_files_before_sending_their_head) {
    using namespace request_stream_files_test;
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-changed");
    const auto bytes = representation();
    for (const auto name : {"file-full.bin", "file-multiple.bin", "static-full.bin", "static-multiple.bin"}) {
        write_file(directory.path() / name, bytes);
    }
    ruvia::static_root root(directory.path(), {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}});
    with_server([&](ruvia::request_stream& stream) -> ruvia::task<void> {
        const std::string relative(stream.request().path().substr(1));
        const auto path = directory.path() / relative;
        auto response = relative.starts_with("static-")
                            ? co_await stream.static_file(root, {.relative_path_ = relative})
                            : co_await stream.file({.path_ = path});
        EXPECT_TRUE(response.has_file_representation());
        // Keep the opened inode, but invalidate its framed size. Even a range
        // that still fits must fail validation before any multipart boundary
        // or success head leaves the server.
        const auto changed = co_await stream.run_blocking([path, size = bytes.size()] {
            write_file(path, std::string(size + 1, 'Z'));
        });
        EXPECT_EQ(changed.status(), ruvia::blocking_status::completed);
        changed.value();
        stream.respond(std::move(response)); }, [](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> {
        for (const auto target : {"/file-full.bin", "/file-multiple.bin", "/static-full.bin", "/static-multiple.bin"}) {
            auto request = client.client().get(target);
            if (std::string_view(target).find("multiple") != std::string_view::npos) {
                request.header("range", "bytes=0-9,90000-90009");
            }
            auto response = co_await request.send();
            EXPECT_EQ(response.status(), ruvia::http_status::internal_server_error);
            EXPECT_FALSE(response.header("content-range").has_value());
            const auto body = co_await response.text();
            EXPECT_NE(body.text().find("\"code\":\"file_read_failed\""), std::string_view::npos);
            EXPECT_EQ(body.text().find(std::string(10, 'Z')), std::string_view::npos);
        } });
}

TEST(Ruvia, independent_request_stream_response_forms_are_mutually_exclusive_and_failures_replace_buffered_selection) {
    using namespace request_stream_files_test;
    with_server([](ruvia::request_stream& stream) -> ruvia::task<void> {
        const auto target = stream.request().path();
        if (target == "/stream-first") {
            (void)co_await stream.response().commit();
            ruvia::http_response rejected;
            EXPECT_THROW(stream.respond(std::move(rejected)), std::logic_error);
            co_await stream.response().write(std::string_view("streamed"));
            co_return;
        }
        ruvia::http_response response;
        response.status(ruvia::http_status::conflict);
        response.header("x-selected", "retained");
        response.body("buffered");
        stream.respond(std::move(response));
        EXPECT_FALSE(stream.response().committed());
        if (target == "/failure") {
            throw ruvia::http_error({.status_ = ruvia::http_status::forbidden, .message_ = "replace selected response"});
        }
        bool refused = false;
        try {
            if (target == "/end") {
                co_await stream.response().end();
            } else if (target == "/commit") {
                (void)co_await stream.response().commit();
            } else if (target == "/tunnel") {
                (void)co_await stream.accept_tunnel();
            } else if (target == "/websocket") {
                (void)co_await stream.accept_websocket();
            } else {
                ruvia::http_response second;
                stream.respond(std::move(second));
            }
        } catch (const std::logic_error&) {
            refused = true;
        }
        EXPECT_TRUE(refused);
        EXPECT_FALSE(stream.response().committed()); }, [](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> {
        for (const auto target : {"/commit", "/end", "/twice", "/tunnel", "/websocket"}) {
            auto request = client.client().get(target);
            if (std::string_view(target) == "/tunnel" || std::string_view(target) == "/websocket") {
                request.header("connection", "Upgrade");
                request.header("upgrade", std::string_view(target) == "/websocket" ? "websocket" : "test");
                request.header("sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==");
                request.header("sec-websocket-version", "13");
            }
            auto response = co_await request.send();
            EXPECT_EQ(response.status(), ruvia::http_status::conflict);
            EXPECT_EQ(response.header("x-selected"), std::optional<std::string_view>("retained"));
            const auto body = co_await response.text();
            EXPECT_EQ(body.text(), "buffered");
        }
        auto failure = co_await client.client().get("/failure").send();
        EXPECT_EQ(failure.status(), ruvia::http_status::forbidden);
        EXPECT_FALSE(failure.header("x-selected").has_value());
        (void)co_await failure.text();
        auto streamed = co_await client.client().get("/stream-first").send();
        EXPECT_EQ(streamed.status(), ruvia::http_status::ok);
        const auto streamed_body = co_await streamed.text();
        EXPECT_EQ(streamed_body.text(), "streamed"); });
}

TEST(Ruvia, independent_request_stream_retires_unstarted_file_and_accept_tasks_with_callback) {
    using namespace request_stream_files_test;
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-cold");
    const auto path = directory.path() / "cold.bin";
    write_file(path, "cold");
    ruvia::static_root root(directory.path(), {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}});
    std::array<std::optional<ruvia::task<ruvia::http_response>>, 2> files;
    std::optional<ruvia::task<ruvia::http_tunnel*>> tunnel;
    std::optional<ruvia::task<ruvia::websocket_accept_result>> websocket;
    with_server([&](ruvia::request_stream& stream) -> ruvia::task<void> {
        files[0].emplace(stream.file({.path_ = path}));
        files[1].emplace(stream.static_file(root, {.relative_path_ = "cold.bin"}));
        tunnel.emplace(stream.accept_tunnel());
        websocket.emplace(stream.accept_websocket());
        ruvia::http_response response;
        response.body("no cold operation started");
        stream.respond(std::move(response));
        co_return; }, [&](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> {
        auto response = co_await client.client().get("/cold").send();
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        const auto body = co_await response.text();
        EXPECT_EQ(body.text(), "no cold operation started");
        for (auto& file : files) {
            bool expired = false;
            try {
                (void)co_await std::move(*file);
            } catch (const std::logic_error&) {
                expired = true;
            }
            EXPECT_TRUE(expired);
            file.reset();
        }
        bool tunnel_expired = false;
        try {
            (void)co_await std::move(*tunnel);
        } catch (const std::logic_error&) {
            tunnel_expired = true;
        }
        EXPECT_TRUE(tunnel_expired);
        tunnel.reset();
        bool websocket_expired = false;
        try {
            (void)co_await std::move(*websocket);
        } catch (const std::logic_error&) {
            websocket_expired = true;
        }
        EXPECT_TRUE(websocket_expired);
        websocket.reset(); });
}

#ifndef _WIN32
namespace request_stream_files_test {
int releases{};
void release_counted_file(ruvia::http_response_file_descriptor::native_type native) noexcept {
    ++releases;
    ::close(static_cast<int>(native));
}

ruvia::http_response counted_file(ruvia::request_stream& stream, const fs::path& path) {
    const auto native = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (native < 0) {
        throw std::runtime_error("cannot open counted response file");
    }
    ruvia::http_response_file_descriptor opened(static_cast<std::uintptr_t>(native), &release_counted_file);
    ruvia::http_response response;
    response.file_body(path.native(), ruvia::http_content_length(4), ruvia::http_content_length(0),
        ruvia::http_content_length(4), ruvia::http_response_file_identity::unchecked(), std::move(opened));
    return response;
}
}  // namespace request_stream_files_test

TEST(Ruvia, independent_request_stream_releases_selected_file_exactly_once_on_error_and_shutdown) {
    using namespace request_stream_files_test;
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-release");
    const auto path = directory.path() / "owned.bin";
    write_file(path, "file");
    releases = 0;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        const auto worker = attachment.loop().worker();
        ruvia::worker_signal selected(worker);
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
            stream.respond(counted_file(stream, path));
            if (stream.request().path() == "/error") {
                throw ruvia::http_error({.status_ = ruvia::http_status::conflict, .message_ = "release file"});
            }
            selected.notify();
            EXPECT_EQ(co_await ruvia::sleep_for(stream.worker(), 10s, stream.stop_token()), ruvia::timer_sleep_result::stop_requested);
        }});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        std::exception_ptr failure;
        try {
            co_await server.start();
            auto error = co_await client.client().get("/error").send();
            EXPECT_EQ(error.status(), ruvia::http_status::conflict);
            (void)co_await error.text();
            EXPECT_EQ(releases, 1);
            asio::ip::tcp::socket peer(io);
            auto connected = co_await ruvia::async_asio([&](auto completion) {
                peer.async_connect({asio::ip::address_v4::loopback(), port.endpoint().port()}, std::move(completion));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const std::string request = "GET /cancel HTTP/1.1\r\nHost: localhost\r\n\r\n";
            auto sent = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
                asio::async_write(peer, asio::buffer(request), std::move(completion));
            });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
            co_await selected.wait();
            EXPECT_EQ(releases, 1);
            co_await server.shutdown();
            EXPECT_EQ(releases, 2);
        } catch (...) {
            failure = std::current_exception();
        }
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
    EXPECT_EQ(releases, 2);
}
#endif

namespace public_router_test {
using namespace std::chrono_literals;

struct scoped_header final {
    ruvia::task<void> invoke(ruvia::context& context, ruvia::next& next) {
        context.policy().header("X-Router", "scoped");
        co_await next();
    }
};

struct unmatched_header final {
    static constexpr bool ruvia_runs_on_unmatched_requests = true;
    ruvia::task<void> invoke(ruvia::context& context, ruvia::next& next) {
        context.policy().header("X-All-Routes", "present");
        co_await next();
    }
};

struct double_next final {
    ruvia::task<void> invoke(ruvia::context&, ruvia::next& next) {
        co_await next();
        co_await next();
    }
};

struct lifetime final {
    unsigned live_{};
    unsigned destroyed_{};
    bool fail_{};
};

struct counted_middleware final {
    explicit counted_middleware(std::shared_ptr<lifetime> state)
        : state_(std::move(state)) {
        if (state_->fail_) {
            throw std::runtime_error("constructor failure");
        }
        ++state_->live_;
    }
    ~counted_middleware() {
        --state_->live_;
        ++state_->destroyed_;
    }
    ruvia::task<void> invoke(ruvia::context&, ruvia::next& next) {
        co_await next();
    }
    std::shared_ptr<lifetime> state_;
};

ruvia::task<ruvia::http_response> parameter(ruvia::context& context) {
    co_return context.text(context.req().param("id").value_or("missing"));
}

struct upgrade_marker final {
    static inline unsigned calls_{};
    ruvia::task<void> invoke(ruvia::context& context, ruvia::next& next) {
        ++calls_;
        context.draft().header("X-Upgrade-Middleware", "ran");
        co_await next();
    }
};

struct visit_counter final {
    unsigned visits_{};
};

ruvia::task<void> echo_websocket(ruvia::context& context) {
    const auto room = context.req().param("room").value_or("missing");
    while (auto message = co_await context.websocket().read()) {
        std::string echoed(room);
        echoed.append(":");
        echoed.append(message->payload());
        co_await context.websocket().text(std::move(echoed));
    }
}

ruvia::task<void> echo_tunnel(ruvia::context& context) {
    auto& tunnel = context.tunnel();
    while (auto chunk = co_await tunnel.read()) {
        co_await tunnel.write(*chunk);
    }
    co_await tunnel.finish();
}

ruvia::task<void> echo_udp(ruvia::context& context) {
    EXPECT_EQ(context.req().param("host").value_or(""), "echo.test");
    auto channel = context.tunnel().udp();
    while (auto datagram = co_await channel.read()) {
        co_await channel.send(datagram->payload());
    }
    co_await channel.finish();
}

ruvia::task<void> check(ruvia::http_client& client, std::string_view method, std::string_view path,
    ruvia::http_status_code status, std::string_view body) {
    auto response = co_await client.client().request(method, path).send();
    EXPECT_EQ(response.status(), status);
    const auto content = co_await response.text();
    EXPECT_EQ(content.text(), body);
}

}  // namespace public_router_test

TEST(Ruvia, public_router_dispatches_parameters_bodies_streams_methods_and_middleware) {
    using namespace public_router_test;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::router routes(attachment.loop().worker());
        unsigned errors = 0;
        unsigned single_shot_calls = 0;
        unsigned connection_failures = 0;
        routes.use_at<scoped_header>({.prefix_ = "/api"});
        routes.use<unmatched_header>();
        routes.on_error([&](ruvia::context& context, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
            ++errors;
            context.draft().status(error.status());
            co_return context.text("router error");
        });
        routes.add("GET", "/api/:id", &parameter);
        routes.add("GET", "/api/literal", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text("literal");
        });
        routes.add("GET", "/api/files/*", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text(context.req().param("*").value());
        });
        routes.add("GET", "/api/url/:id", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text(context.url_for("/api/:id", {context.req().param("id").value()}));
        });
        routes.add<ruvia::body_limit<4>>("POST", "/api/body", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text(co_await context.req().text());
        });
        routes.stream("GET", "/api/stream/:id", [](ruvia::context& context) -> ruvia::task<void> {
            if (co_await context.stream().commit() == ruvia::stream_commit_result::body) {
                co_await context.stream().write(context.req().param("id").value());
            }
        });
        routes.stream<ruvia::stream_body, ruvia::body_limit<4>>("POST", "/api/upload", [](ruvia::context& context) -> ruvia::task<void> {
            std::string content;
            while (auto chunk = co_await context.req().body_reader().text()) {
                content.append(*chunk);
            }
            if (co_await context.stream().commit() == ruvia::stream_commit_result::body) {
                co_await context.stream().write(std::move(content));
            }
        });
        routes.add("GET", "/api/fail", [](ruvia::context&) -> ruvia::task<ruvia::http_response> {
            throw ruvia::http_error({.status_ = ruvia::http_status::conflict});
            co_return ruvia::http_response{};
        });
        routes.stream("GET", "/api/stream-fail", [](ruvia::context&) -> ruvia::task<void> {
            throw ruvia::http_error({.status_ = ruvia::http_status::conflict});
            co_return;
        });
        routes.stream("GET", "/api/committed-fail", [](ruvia::context& context) -> ruvia::task<void> {
            if (co_await context.stream().commit() == ruvia::stream_commit_result::body) {
                co_await context.stream().write("partial");
                throw std::runtime_error("failure after commitment");
            }
        });
        routes.add<double_next>("GET", "/api/twice", [&](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            ++single_shot_calls;
            co_return context.text("once");
        });
        routes.add<ruvia::rate_limit<1, 60'000>>("GET", "/api/limited", &parameter);
        routes.add("PROPFIND", "/extension", &parameter);
        routes.freeze();
        EXPECT_THROW(routes.add("GET", "/late", &parameter), std::logic_error);

        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) { return routes.dispatch(stream); },
                                                         .connection_failure_ = [&](const ruvia::connection_failure_record&) noexcept { ++connection_failures; }});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        std::exception_ptr failure;
        try {
            co_await server.start();
            co_await check(client, "GET", "/api/hello%20world", ruvia::http_status::ok, "hello world");
            co_await check(client, "GET", "/api/%6citeral", ruvia::http_status::ok, "literal");
            co_await check(client, "GET", "/api/files/a/b", ruvia::http_status::ok, "a/b");
            co_await check(client, "GET", "/api/url/a%2Fb", ruvia::http_status::ok, "/api/a%2Fb");
            co_await check(client, "GET", "/api/stream/value", ruvia::http_status::ok, "value");
            co_await check(client, "HEAD", "/api/value", ruvia::http_status::ok, "");
            co_await check(client, "HEAD", "/api/stream/value", ruvia::http_status::ok, "");
            for (const auto method : {"OPTIONS", "PUT"}) {
                auto response = co_await client.client().request(method, "/api/stream/value").send();
                EXPECT_EQ(response.status(), std::string_view(method) == "OPTIONS" ? ruvia::http_status::no_content : ruvia::http_status::method_not_allowed);
                const auto allow = response.header("Allow").value_or("");
                EXPECT_NE(allow.find("GET"), std::string_view::npos);
                EXPECT_NE(allow.find("HEAD"), std::string_view::npos);
                EXPECT_EQ(response.header("X-All-Routes").value_or(""), "present");
                EXPECT_FALSE(response.header("X-Router").has_value());
                (void)co_await response.text();
            }
            for (const auto path : {"/api/body", "/api/upload"}) {
                auto response = co_await client.client().post(path).body("four").send();
                EXPECT_EQ(response.status(), ruvia::http_status::ok);
                EXPECT_EQ(response.header("X-Router").value_or(""), "scoped");
                const auto content = co_await response.text();
                EXPECT_EQ(content.text(), "four");
                auto oversized = co_await client.client().post(path).body("large").send();
                EXPECT_EQ(oversized.status(), ruvia::http_status::content_too_large);
                const auto rejected = co_await oversized.text();
                EXPECT_EQ(rejected.text(), "router error");
            }
            co_await check(client, "GET", "/api/fail", ruvia::http_status::conflict, "router error");
            co_await check(client, "GET", "/api/stream-fail", ruvia::http_status::conflict, "router error");
            auto misuse = co_await client.client().get("/api/twice").send();
            EXPECT_EQ(misuse.status(), ruvia::http_status::internal_server_error);
            const auto misuse_body = co_await misuse.text();
            EXPECT_NE(misuse_body.text().find("next_called_multiple_times"), std::string_view::npos);
            EXPECT_EQ(single_shot_calls, 1);
            co_await check(client, "GET", "/api/limited", ruvia::http_status::ok, "missing");
            co_await check(client, "GET", "/api/limited", ruvia::http_status::too_many_requests, "router error");
            co_await check(client, "PROPFIND", "/extension", ruvia::http_status::ok, "missing");
            co_await check(client, "UNREGISTERED", "/extension", ruvia::http_status::not_implemented, "router error");
            auto absent = co_await client.client().get("/apix/missing").send();
            EXPECT_EQ(absent.status(), ruvia::http_status::not_found);
            EXPECT_FALSE(absent.header("X-Router").has_value());
            (void)co_await absent.text();
            EXPECT_GE(errors, 7);
            bool truncated = false;
            try {
                auto response = co_await client.client().get("/api/committed-fail").send();
                (void)co_await response.text();
            } catch (const ruvia::http_client_error&) {
                truncated = true;
            }
            EXPECT_TRUE(truncated);
            EXPECT_EQ(connection_failures, 1);
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

TEST(Ruvia, public_router_freeze_rejects_ambiguity_and_rolls_back_middleware_lifetimes) {
    using namespace public_router_test;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    const auto run = [&]() -> ruvia::task<void> {
        const auto& worker = attachment.loop().worker();
        for (const auto other : {"/items/:second", "/%69tems/:first"}) {
            ruvia::router conflict(worker);
            conflict.add("GET", "/items/:first", &parameter);
            conflict.add("GET", other, &parameter);
            EXPECT_THROW(conflict.freeze(), std::invalid_argument);
        }
        ruvia::router extension(worker);
        extension.add("PROPFIND", "/items/:id", &parameter);
        EXPECT_THROW(extension.freeze(), std::invalid_argument);
        auto first = std::make_shared<lifetime>();
        auto second = std::make_shared<lifetime>();
        second->fail_ = true;
        {
            ruvia::router routes(worker);
            routes.use<counted_middleware>(first);
            routes.use<counted_middleware>(second);
            routes.add("GET", "/items/:id", &parameter);
            EXPECT_THROW(routes.freeze(), std::runtime_error);
            EXPECT_EQ(first->live_, 0);
            EXPECT_EQ(first->destroyed_, 1);
            EXPECT_EQ(second->live_, 0);
            second->fail_ = false;
            routes.freeze();
            EXPECT_EQ(first->live_, 1);
            EXPECT_EQ(second->live_, 1);
            EXPECT_THROW(routes.freeze(), std::logic_error);
        }
        EXPECT_EQ(first->live_, 0);
        EXPECT_EQ(first->destroyed_, 2);
        EXPECT_EQ(second->live_, 0);
        EXPECT_EQ(second->destroyed_, 1);
        attachment.stop();
        co_return;
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, public_router_retains_cold_dispatch_after_facade_destruction_and_joins_cancellation) {
    using namespace public_router_test;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket peer(io);
    peer.connect(acceptor.local_endpoint());
    auto accepted = acceptor.accept();
    const std::string request = "GET /wait HTTP/1.1\r\nHost: localhost\r\n\r\n";
    asio::write(peer, asio::buffer(request));
    auto state = std::make_shared<lifetime>();
    bool cancelled = false;
    const auto run = [&]() -> ruvia::task<void> {
        const auto& worker = attachment.loop().worker();
        ruvia::worker_signal entered(worker);
        auto routes = std::make_unique<ruvia::router>(worker);
        routes->use<counted_middleware>(state);
        routes->add("GET", "/wait", [&](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            entered.notify();
            cancelled = co_await ruvia::sleep_for(context.worker(), 1h, context.stop_token()) == ruvia::timer_sleep_result::stop_requested;
            co_return context.text("cancelled");
        });
        routes->freeze();
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
            auto pending = routes->dispatch(stream);
            routes.reset();
            EXPECT_EQ(state->live_, 1);
            co_await std::move(pending);
        }});
        co_await server.add_connection_service({.address_ = "127.0.0.1", .http_ = acceptor.local_endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        co_await server.start();
        server.adopt({.address_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port()}, accepted.release());
        co_await entered.wait();
        EXPECT_EQ(routes, nullptr);
        EXPECT_EQ(state->live_, 1);
        co_await server.shutdown();
        EXPECT_TRUE(cancelled);
        EXPECT_EQ(state->live_, 0);
        EXPECT_EQ(state->destroyed_, 1);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, public_router_applies_host_representation_policies_and_its_rate_limit) {
    using namespace public_router_test;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        const auto& worker = attachment.loop().worker();
        {
            ruvia::router invalid(worker);
            EXPECT_THROW(invalid.rate_limit({.rule_ = {.max_requests_ = 0, .window_ = 1s}}), std::invalid_argument);
            EXPECT_THROW(invalid.rate_limit({.rule_ = {.max_requests_ = 1, .window_ = 1s}, .capacity_per_worker_ = 3}), std::invalid_argument);
        }
        static const std::string text_body(4096, 'r');
        ruvia::router routes(worker);
        routes.rate_limit({.rule_ = {.max_requests_ = 4, .window_ = 60s}, .capacity_per_worker_ = 64});
        routes.on_error([](ruvia::context& context, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
            context.draft().status(error.status());
            co_return context.text("router error");
        });
        routes.add("GET", "/text", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text(text_body);
        });
        routes.stream("GET", "/stream", [](ruvia::context& context) -> ruvia::task<void> {
            context.draft().header("Content-Type", "text/plain");
            if (co_await context.stream().commit() == ruvia::stream_commit_result::body) {
                co_await context.stream().write(std::string_view(text_body));
            }
        });
        routes.freeze();
        EXPECT_THROW(routes.rate_limit({.rule_ = {.max_requests_ = 1, .window_ = 1s}}), std::logic_error);

        ruvia::http_server server(attachment.loop(),
            {.request_stream_ = [&](ruvia::request_stream& stream) { return routes.dispatch(stream); }},
            {.compression_ = ruvia::compression_config{.min_bytes_ = 16}, .cors_ = ruvia::http_cors_policy_config{}});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        std::exception_ptr failure;
        try {
            co_await server.start();
            for (const auto path : {"/text", "/stream"}) {
                auto response = co_await client.client()
                                    .get(path)
                                    .header("Origin", "https://site.test")
                                    .header("Accept-Encoding", "gzip")
                                    .configure({.decode_content_ = false})
                                    .send();
                EXPECT_EQ(response.status(), ruvia::http_status::ok) << path;
                EXPECT_EQ(response.header("Content-Encoding").value_or(""), "gzip") << path;
                EXPECT_EQ(response.header("Access-Control-Allow-Origin").value_or(""), "*") << path;
                const auto encoded = co_await response.text();
                EXPECT_LT(encoded.text().size(), text_body.size()) << path;
            }
            auto preflight = co_await client.client()
                                 .request("OPTIONS", "/text")
                                 .header("Origin", "https://site.test")
                                 .header("Access-Control-Request-Method", "GET")
                                 .send();
            EXPECT_EQ(preflight.status(), ruvia::http_status::no_content);
            EXPECT_EQ(preflight.header("Access-Control-Allow-Origin").value_or(""), "*");
            (void)co_await preflight.text();
            co_await check(client, "GET", "/text", ruvia::http_status::ok, text_body);
            auto limited = co_await client.client().get("/missing").send();
            EXPECT_EQ(limited.status(), ruvia::http_status::too_many_requests);
            EXPECT_TRUE(limited.header("Retry-After").has_value());
            const auto limited_body = co_await limited.text();
            EXPECT_EQ(limited_body.text(), "router error");
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

TEST(Ruvia, public_router_routes_websocket_and_connect_upgrades_over_http1_and_http2) {
    using namespace public_router_test;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::router routes(attachment.loop().worker());
        unsigned errors = 0;
        routes.on_error([&](ruvia::context& context, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
            ++errors;
            context.draft().status(error.status());
            co_return context.text("router error");
        });
        routes.websocket<upgrade_marker>("/ws/:room", &echo_websocket);
        routes.connect<upgrade_marker>("echo.test:443", &echo_tunnel);
        routes.connect_protocol<upgrade_marker>("connect-udp", "/.well-known/masque/udp/:host/:port/", &echo_udp);
        routes.freeze();

        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) { return routes.dispatch(stream); }});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        std::exception_ptr failure;
        try {
            co_await server.start();
            for (const auto protocol : {ruvia::http_client_versions{.http2_ = false}, ruvia::http_client_versions{.http1_ = false}}) {
                const std::string label = protocol == ruvia::http_client_versions{.http2_ = false} ? "HTTP/1.1" : "h2c";
                ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = protocol});
                std::exception_ptr exchange_failure;
                try {
                    upgrade_marker::calls_ = 0;
                    {
                        auto session = co_await client.client().websocket("/ws/lobby").connect();
                        co_await session.text("hello");
                        const auto message = co_await session.read();
                        EXPECT_TRUE(message.has_value()) << label;
                        if (message) {
                            EXPECT_EQ(message->payload(), "lobby:hello") << label;
                        }
                        co_await session.close();
                        EXPECT_FALSE(co_await session.read()) << label;
                    }
                    {
                        auto opened = co_await client.client().connect("echo.test:443").open();
                        auto* tunnel = opened.tunnel();
                        EXPECT_TRUE(tunnel != nullptr) << label;
                        if (tunnel != nullptr) {
                            EXPECT_EQ(tunnel->status().value(), 200) << label;
                            co_await tunnel->write(std::string_view("tunnel bytes"));
                            co_await tunnel->finish();
                            std::string echoed;
                            while (auto chunk = co_await tunnel->text()) {
                                echoed.append(*chunk);
                            }
                            EXPECT_EQ(echoed, "tunnel bytes") << label;
                        }
                    }
                    {
                        auto opened = co_await client.client().udp("/.well-known/masque/udp/echo.test/53/").open();
                        auto* tunnel = opened.tunnel();
                        EXPECT_TRUE(tunnel != nullptr) << label;
                        if (tunnel != nullptr) {
                            auto channel = std::move(*tunnel).udp();
                            co_await channel.send(std::string_view("datagram"));
                            const auto packet = co_await channel.read();
                            EXPECT_TRUE(packet.has_value()) << label;
                            if (packet) {
                                EXPECT_EQ(packet->text(), "datagram") << label;
                            }
                            co_await channel.finish();
                            EXPECT_FALSE((co_await channel.read()).has_value()) << label;
                        }
                    }
                    EXPECT_EQ(upgrade_marker::calls_, 3U) << label;
                    {
                        auto refused = co_await client.client().connect("other.test:443").open();
                        auto* response = refused.response();
                        EXPECT_TRUE(response != nullptr) << label;
                        if (response != nullptr) {
                            EXPECT_FALSE(response->status().is_successful()) << label;
                            (void)co_await response->text();
                        }
                    }
                    const auto before = errors;
                    auto plain = co_await client.client().get("/ws/lobby").send();
                    EXPECT_EQ(plain.status(), ruvia::http_status::bad_request) << label;
                    const auto plain_body = co_await plain.text();
                    EXPECT_EQ(plain_body.text(), "router error") << label;
                    EXPECT_EQ(errors, before + 1) << label;
                } catch (...) {
                    exchange_failure = std::current_exception();
                }
                co_await client.shutdown();
                if (exchange_failure) {
                    std::rethrow_exception(exchange_failure);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
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

#ifdef RUVIA_ENABLE_REDIS
TEST(Ruvia, public_router_lends_borrowed_clients_and_state_to_session_middleware) {
    using namespace public_router_test;
    ruvia::test::redis_session_store_peer peer;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        const auto& worker = attachment.loop().worker();
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::redis_client redis(attachment.loop(), {.host_ = "127.0.0.1", .port_ = peer.port(), .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1, .connect_timeout_ = 2s, .command_timeout_ = 2s});
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        visit_counter visits;
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            ruvia::router routes(worker);
            routes.bind_redis("default", redis).bind_http_client("default", client).bind_worker_state(visits);
            EXPECT_THROW(routes.bind_redis("default", redis), std::invalid_argument);
            EXPECT_THROW(routes.bind_http_client("default", client), std::invalid_argument);
            EXPECT_THROW(routes.bind_worker_state(visits), std::invalid_argument);
            routes.use<ruvia::session_middleware>(ruvia::session_config{.cookie_name_ = "sid"});
            routes.add("GET", "/set", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
                ++context.worker_state<visit_counter>().visits_;
                context.session().set("private");
                co_return context.text("set");
            });
            routes.add("GET", "/read", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
                ++context.worker_state<visit_counter>().visits_;
                auto value = context.session();
                co_return context.text(value.data().empty() ? "empty" : value.data());
            });
            routes.add("GET", "/rotate", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
                context.session().regenerate();
                co_return context.text("rotate");
            });
            routes.add("GET", "/plain", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
                co_return context.text("plain");
            });
            routes.add("GET", "/relay", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
                auto upstream = co_await context.client().get("/plain").send();
                const auto content = co_await upstream.text();
                const auto stored = co_await context.redis().get("absent");
                co_return context.text(std::string(content.text()) + (stored ? ":stored" : ":absent"));
            });
            routes.freeze();
            EXPECT_THROW(routes.bind_redis("other", redis), std::logic_error);

            ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) { return routes.dispatch(stream); }});
            server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
            std::exception_ptr serving_failure;
            try {
                co_await server.start();
                const auto request = [&](std::string_view path, std::string_view cookie) -> ruvia::task<std::pair<std::string, std::string>> {
                    auto builder = client.client().get(path);
                    if (!cookie.empty()) {
                        builder.header("Cookie", cookie);
                    }
                    auto response = co_await builder.send();
                    EXPECT_EQ(response.status(), ruvia::http_status::ok) << path;
                    std::string set_cookie(response.header("Set-Cookie").value_or(""));
                    set_cookie = set_cookie.substr(0, set_cookie.find(';'));
                    const auto content = co_await response.text();
                    co_return std::pair<std::string, std::string>(std::string(content.text()), std::move(set_cookie));
                };
                const auto [created_body, created] = co_await request("/set", {});
                EXPECT_EQ(created_body, "set");
                EXPECT_TRUE(created.starts_with("sid="));
                EXPECT_EQ(created.size(), std::size_t{4 + 48});
                EXPECT_EQ((co_await request("/read", created)).first, "private");
                const auto [rotated_body, rotated] = co_await request("/rotate", created);
                EXPECT_EQ(rotated_body, "rotate");
                EXPECT_TRUE(rotated.starts_with("sid="));
                EXPECT_NE(rotated, created);
                EXPECT_EQ((co_await request("/read", created)).first, "empty");
                EXPECT_EQ((co_await request("/read", rotated)).first, "private");
                EXPECT_EQ(visits.visits_, 4U);
                EXPECT_EQ((co_await request("/relay", {})).first, "plain:absent");
            } catch (...) {
                serving_failure = std::current_exception();
            }
            co_await server.shutdown();
            if (serving_failure) {
                std::rethrow_exception(serving_failure);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        co_await redis.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}
#endif

// A buffered body read through the router is bounded per call on every
// protocol: a timeout or stop ends only that call, the bytes that arrived stay
// buffered, and the next call returns the whole body.
TEST(Ruvia, public_router_buffered_body_reads_honour_their_operation) {
    using namespace std::chrono_literals;
    asio::io_context io;
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &preparation});
    ruvia::test::tcp_port_reservation plain(io), secure(io);
    ruvia::test::udp_port_reservation udp(io);
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    const auto run = [&]() -> ruvia::task<void> {
        unsigned phase = 0;
        unsigned rejected = 0;
        unsigned prestopped = 0;
        unsigned interruptions = 0;
        const auto code_of = [](auto operation) -> ruvia::task<std::error_code> {
            try {
                (void)co_await std::move(operation);
            } catch (const std::system_error& error) {
                co_return error.code();
            }
            co_return std::error_code{};
        };
        ruvia::router routes(attachment.loop().worker());
        routes.add("POST", "/buffered", [&](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            const auto request = context.req();
            for (const auto attempt : {0, 1, 2}) {
                try {
                    if (attempt == 0) {
                        (void)request.text({.timeout_ = 0ms});
                    } else if (attempt == 1) {
                        (void)request.bytes({.timeout_ = -1ms});
                    } else {
                        (void)request.discard_body({.timeout_ = 0ms});
                    }
                } catch (const std::invalid_argument&) {
                    ++rejected;
                }
            }
            ruvia::stop_source stopped;
            stopped.request_stop();
            prestopped += (co_await code_of(request.text({.stop_token_ = stopped.token()}))) == std::errc::operation_canceled;
            interruptions += (co_await code_of(request.text({.timeout_ = 50ms}))) == std::errc::timed_out;
            phase = 1;
            ruvia::stop_source stop;
            ruvia::task_scope stopper(context.worker());
            const auto stop_later = [&]() -> ruvia::task<void> {
                (void)co_await ruvia::sleep_for(context.worker(), 50ms);
                std::thread([&stop] { stop.request_stop(); }).join();
            };
            stopper.spawn(stop_later());
            interruptions += (co_await code_of(request.bytes({.stop_token_ = stop.token()}))) == std::errc::operation_canceled;
            co_await stopper.join();
            phase = 2;
            co_return context.text(co_await request.text());
        });
        routes.freeze();
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) { return routes.dispatch(stream); }});
        server.listen({.address_ = "127.0.0.1", .http_ = plain.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        ruvia::listen_config tls{.address_ = "127.0.0.1", .https_ = secure.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
        tls.http3_ = udp.endpoint().port();
#endif
        server.listen(std::move(tls));
        struct target final {
            std::string base_url_;
            ruvia::http_client_versions versions_;
            std::string label_;
        };
        std::vector<target> targets{{"http://127.0.0.1:" + std::to_string(plain.endpoint().port()), ruvia::http_client_versions{.http2_ = false}, "HTTP/1"}};
#if defined(RUVIA_ENABLE_HTTP2)
        targets.push_back({"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http1_ = false}, "HTTP/2"});
#endif
#if defined(RUVIA_ENABLE_HTTP3)
        targets.push_back({"https://127.0.0.1:" + std::to_string(udp.endpoint().port()), ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true}, "HTTP/3"});
#endif
        std::exception_ptr failure;
        try {
            co_await server.start();
            for (const auto& entry : targets) {
                ruvia::http_client_config config{.base_url_ = entry.base_url_, .versions_ = entry.versions_};
                config.tls_.ca_file_ = identity.ca_file_;
                config.request_timeout_ = 10s;
                ruvia::http_client client(attachment.loop(), config);
                std::exception_ptr exchange_failure;
                try {
                    const auto await_phase = [&](unsigned expected) -> ruvia::task<void> {
                        for (unsigned count = 0; phase != expected && count != 5000; ++count) {
                            (void)co_await ruvia::sleep_for(client.worker(), 1ms);
                        }
                        EXPECT_EQ(phase, expected) << entry.label_;
                    };
                    phase = 0;
                    auto exchange = co_await client.client().post("/buffered").upload({});
                    co_await exchange.body().write("abc");
                    co_await await_phase(1);
                    co_await exchange.body().write("def");
                    co_await await_phase(2);
                    co_await exchange.body().write("ghi");
                    co_await exchange.body().end();
                    auto response = co_await exchange.response();
                    EXPECT_EQ(response.status(), ruvia::http_status::ok) << entry.label_;
                    const auto content = co_await response.text();
                    EXPECT_EQ(content.text(), "abcdefghi") << entry.label_;
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
        if (failure) {
            std::rethrow_exception(failure);
        }
        EXPECT_EQ(rejected, 3 * targets.size());
        EXPECT_EQ(prestopped, targets.size());
        EXPECT_EQ(interruptions, 2 * targets.size());
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, public_router_binds_http_clients_by_its_own_unique_aliases) {
    using namespace public_router_test;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::http_client first(attachment.loop(), {.base_url_ = origin, .versions_ = {.http2_ = false}});
        ruvia::http_client second(attachment.loop(), {.base_url_ = origin, .versions_ = {.http2_ = false}});
        ruvia::router routes(attachment.loop().worker());
        routes.bind_http_client("one", first).bind_http_client("two", second).bind_http_client("default", first);
        EXPECT_THROW(routes.bind_http_client("one", second), std::invalid_argument);
        EXPECT_THROW(routes.bind_http_client("", first), std::invalid_argument);
        routes.add("GET", "/one", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text("one");
        });
        routes.add("GET", "/two", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text("two");
        });
        routes.add("GET", "/relay", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            auto one = co_await context.client("one").get("/one").send();
            const auto first = co_await one.text();
            auto two = co_await context.client("two").get("/two").send();
            const auto second = co_await two.text();
            co_return context.text(std::string(first.text()) + ":" + std::string(second.text()));
        });
        routes.freeze();
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) { return routes.dispatch(stream); }});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port()});
        std::exception_ptr failure;
        try {
            co_await server.start();
            auto response = co_await first.client().get("/relay").send();
            EXPECT_EQ(response.status(), ruvia::http_status::ok);
            const auto content = co_await response.text();
            EXPECT_EQ(content.text(), "one:two");
        } catch (...) {
            failure = std::current_exception();
        }
        co_await first.shutdown();
        co_await second.shutdown();
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

ruvia::task<void> group_answer(ruvia::request_stream& stream) {
    std::string body = std::to_string(stream.worker().id()) + ":" + std::to_string(stream.conn().connection_id());
    if (stream.request().path() == "/proxy") {
        body = std::string(stream.conn().client().address().to_string());
        body += stream.conn().via_trusted_proxy() ? ":proxy:" : ":direct:";
        body += stream.conn().scheme() == ruvia::http_scheme::https ? "https" : "http";
    }
    if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
        co_await stream.response().write(body);
        co_await stream.response().end();
    }
}

ruvia::http_server_callbacks answer_callbacks(const ruvia::worker_ref&) {
    return {.request_stream_ = &group_answer};
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

TEST(Ruvia, independent_group_trusted_proxy_replacement_is_atomic_and_defaults_to_direct) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation trusted_port(ports), direct_port(ports);
    ruvia::http_server_group trusted(runtime.loops_, &answer_callbacks);
    ruvia::http_server_group direct(runtime.loops_, &answer_callbacks);
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
    runtime.loops_[0].start(trusted.start(runtime.loops_[0].worker())).get();
    runtime.loops_[0].start(direct.start(runtime.loops_[0].worker())).get();
    const std::string forwarded = "X-Forwarded-For: 203.0.113.7, 198.51.100.9, 10.1.2.3\r\nX-Forwarded-Proto: http, https, http\r\n";
    EXPECT_NE(request(trusted_port.endpoint().port(), "/proxy", forwarded).find("198.51.100.9:proxy:https"), std::string::npos);
    EXPECT_NE(request(direct_port.endpoint().port(), "/proxy", forwarded).find("127.0.0.1:direct:http"), std::string::npos);
    EXPECT_THROW(trusted.trusted_proxies({}), std::logic_error);
    runtime.loops_[0].start(trusted.shutdown(runtime.loops_[0].worker())).get();
    runtime.loops_[0].start(direct.shutdown(runtime.loops_[0].worker())).get();
    EXPECT_TRUE(runtime.loops_[0].worker().accepting());
    EXPECT_TRUE(runtime.loops_[1].worker().accepting());
}

TEST(Ruvia, independent_group_stages_configuration_before_external_loops_are_driven) {
    using namespace server_group_test;
    external_loops runtime(false);
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    group.listen(plain(port.endpoint().port()));
    group.trusted_proxies({});
    runtime.start();
    runtime.loops_[0].start(group.start(runtime.loops_[0].worker())).get();
    EXPECT_NE(request(port.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].worker())).get();
}

TEST(Ruvia, independent_single_server_installs_trusted_proxy_configuration_before_start) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server server(runtime.loops_[0], {.request_stream_ = &group_answer});
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
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto port = acceptor.local_endpoint().port();
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    runtime.loops_[0].start(group.add_connection_service(runtime.loops_[0].worker(), plain(port))).get();
    runtime.loops_[0].start(group.start(runtime.loops_[0].worker())).get();
    for (std::size_t index = 0; index != group.worker_count(); ++index) {
        asio::ip::tcp::socket peer(io);
        peer.connect(acceptor.local_endpoint());
        auto accepted = acceptor.accept();
        runtime.loops_[0].start(group.adopt(runtime.loops_[0].worker(), index, {.address_ = "127.0.0.1", .port_ = port}, accepted.release())).get();
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
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].worker())).get();
}

TEST(Ruvia, independent_group_prepares_all_and_rolls_back_failed_dynamic_addition) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation first(ports), good(ports);
    asio::ip::tcp::acceptor busy(ports, {asio::ip::address_v4::loopback(), 0});
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    group.listen(plain(first.endpoint().port()));
    runtime.loops_[0].start(group.start(runtime.loops_[0].worker())).get();
    auto bad = secure(busy.local_endpoint().port(), 0, identity);
    bad.http_ = good.endpoint().port();
    bad.http3_.reset();
    EXPECT_THROW(runtime.loops_[0].start(group.add_listener(runtime.loops_[0].worker(), std::move(bad))).get(), std::system_error);
    EXPECT_NE(request(first.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(group.add_listener(runtime.loops_[0].worker(), plain(good.endpoint().port()))).get();
    EXPECT_NE(request(good.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(group.remove_listener(runtime.loops_[0].worker(), {.address_ = "127.0.0.1", .port_ = good.endpoint().port()})).get();
    runtime.loops_[0].start(group.add_listener(runtime.loops_[0].worker(), plain(good.endpoint().port()))).get();
    EXPECT_NE(request(good.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].worker())).get();
}

TEST(Ruvia, independent_group_start_failure_preserves_external_unrelated_work) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation good(ports);
    asio::ip::tcp::acceptor busy(ports, {asio::ip::address_v4::loopback(), 0});
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    group.listen(plain(good.endpoint().port()));
    group.listen(plain(busy.local_endpoint().port()));
    EXPECT_THROW(runtime.loops_[0].start(group.start(runtime.loops_[0].worker())).get(), std::system_error);
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].worker())).get();
    const auto alive = []() -> ruvia::task<int> { co_return 42; };
    EXPECT_EQ(runtime.loops_[0].start(alive()).get(), 42);
    EXPECT_EQ(runtime.loops_[1].start(alive()).get(), 42);
    ruvia::http_server_group replacement(runtime.loops_, &answer_callbacks);
    replacement.listen(plain(good.endpoint().port()));
    runtime.loops_[0].start(replacement.start(runtime.loops_[0].worker())).get();
    EXPECT_NE(request(good.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(replacement.shutdown(runtime.loops_[0].worker())).get();
}

TEST(Ruvia, independent_group_accepts_member_worker_controls_and_coordinates_loop_retirement) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    const auto member_controls = [&]() -> ruvia::task<void> {
        const auto& caller = runtime.loops_[0].worker();
        co_await group.start(caller);
        co_await group.add_listener(caller, plain(port.endpoint().port()));
        EXPECT_FALSE(co_await group.close_connection(caller, 0));
        co_await group.remove_listener(caller, {.address_ = "127.0.0.1", .port_ = port.endpoint().port()});
        co_await group.shutdown(caller);
    };
    runtime.loops_[0].start(member_controls()).get();
    ruvia::http_server_group retiring(runtime.loops_, &answer_callbacks);
    ruvia::stop_source unrelated_stop;
    std::promise<void> sleeping, resumed;
    auto sleeping_result = sleeping.get_future();
    auto resumed_result = resumed.get_future();
    const auto unrelated = [&]() -> ruvia::task<ruvia::timer_sleep_result> {
        sleeping.set_value();
        const auto result = co_await ruvia::sleep_for(runtime.loops_[1].worker(), 1h, unrelated_stop.token());
        resumed.set_value();
        co_return result;
    };
    auto unrelated_root = runtime.loops_[1].start(unrelated());
    sleeping_result.get();
    runtime.loops_[0].start(retiring.start(runtime.loops_[0].worker())).get();
    runtime.first_attachment_.stop();
    auto finished = std::async(std::launch::async, [&] {
        runtime.loops_[1].start(retiring.shutdown(runtime.loops_[1].worker())).get();
    });
    ASSERT_EQ(finished.wait_for(5s), std::future_status::ready);
    finished.get();
    EXPECT_TRUE(runtime.loops_[1].worker().accepting());
    EXPECT_EQ(retiring.stats().active_connections_, std::size_t{0});
    EXPECT_EQ(resumed_result.wait_for(0s), std::future_status::timeout);
    unrelated_stop.request_stop();
    EXPECT_EQ(std::move(unrelated_root).get(), ruvia::timer_sleep_result::stop_requested);
}

#if !defined(_WIN32)
TEST(Ruvia, independent_group_duplicates_in_worker_order_and_rolls_back_invalid_inheritance) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group original(runtime.loops_, &answer_callbacks);
    original.listen(plain(port.endpoint().port()));
    runtime.loops_[0].start(original.start(runtime.loops_[0].worker())).get();
    auto inherited = runtime.loops_[0].start(original.duplicate_listener_sockets(runtime.loops_[0].worker(), {.address_ = "127.0.0.1", .port_ = port.endpoint().port()})).get();
    auto valid = runtime.loops_[0].start(original.duplicate_listener_sockets(runtime.loops_[0].worker(), {.address_ = "127.0.0.1", .port_ = port.endpoint().port()})).get();
    ASSERT_EQ(inherited.http_.size(), std::size_t{2});
    runtime.loops_[0].start(original.shutdown(runtime.loops_[0].worker())).get();
    ruvia::http_server_group replacement(runtime.loops_, &answer_callbacks);
    auto invalid = plain(port.endpoint().port());
    invalid.inherited_sockets_ = inherited;
    invalid.inherited_sockets_->http_.pop_back();
    const auto extra = inherited.http_.back();
    inherited.http_.pop_back();
    // The malformed config consumes its one socket; close the deliberately
    // omitted duplicate through the public native socket owner as well.
    ruvia::http_server_group rejected(runtime.loops_, &answer_callbacks);
    rejected.listen(std::move(invalid));
    EXPECT_THROW(runtime.loops_[0].start(rejected.start(runtime.loops_[0].worker())).get(), std::invalid_argument);
    runtime.loops_[0].start(rejected.shutdown(runtime.loops_[0].worker())).get();
    asio::ip::tcp::acceptor omitted(ports);
    omitted.assign(asio::ip::tcp::v4(), extra);
    omitted.close();
    auto adopted = plain(port.endpoint().port());
    adopted.inherited_sockets_ = std::move(valid);
    replacement.listen(std::move(adopted));
    runtime.loops_[0].start(replacement.start(runtime.loops_[0].worker())).get();
    EXPECT_NE(request(port.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(replacement.shutdown(runtime.loops_[0].worker())).get();
}
#endif

#if defined(RUVIA_ENABLE_HTTP3)
TEST(Ruvia, independent_group_shared_http3_endpoint_survives_repeated_migration_rebind_and_listener_replacement) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation tcp(ports);
    ruvia::test::udp_port_reservation udp(ports);
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    group.listen(secure(tcp.endpoint().port(), udp.endpoint().port(), identity));
    runtime.loops_[0].start(group.start(runtime.loops_[0].worker())).get();
    ruvia::test::http3_test_client client({asio::ip::address_v4::loopback(), udp.endpoint().port()}, identity.ca_file_, "localhost", 5s);
    auto previous = client.start_request("GET", "/worker");
    const auto initial = client.finish_response(previous);
    ASSERT_EQ(initial.status_, 200);
    ASSERT_FALSE(initial.body_.empty());
    const ruvia::test::tls_identity rotated("localhost", "DNS:localhost,IP:127.0.0.1");
    runtime.loops_[0].start(group.replace_tls_identities(runtime.loops_[0].worker(), {.address_ = "127.0.0.1", .port_ = tcp.endpoint().port()},
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
        runtime.loops_[0].start(group.remove_listener(runtime.loops_[0].worker(), {.address_ = "127.0.0.1", .port_ = tcp.endpoint().port()})).get();
        runtime.loops_[0].start(group.add_listener(runtime.loops_[0].worker(), secure(tcp.endpoint().port(), udp.endpoint().port(), identity))).get();
        ruvia::test::http3_test_client new_client({asio::ip::address_v4::loopback(), udp.endpoint().port()}, identity.ca_file_, "localhost", 5s);
        EXPECT_EQ(new_client.request("GET", "/worker").status_, 200);
        EXPECT_EQ(client.request("GET", "/worker").body_, initial.body_);
        previous += 4;
    }
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].worker())).get();
    EXPECT_EQ(group.stats().active_connections_, std::size_t{0});
}
#endif

TEST(Ruvia, independent_group_mixed_listener_preserves_transport_during_rotation_readd_and_inheritance) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto port = reservation.endpoint().port();
    const ruvia::listener_endpoint target{.address_ = "127.0.0.1", .port_ = port};
    const ruvia::test::tls_identity first("mixed-group-first", "DNS:localhost");
    const ruvia::test::tls_identity second("mixed-group-second", "DNS:localhost");
    auto config = secure(port, 0, first);
    config.http_ = port;
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    runtime.loops_[0].start(group.start(runtime.loops_[0].worker())).get();
    runtime.loops_[0].start(group.add_listener(runtime.loops_[0].worker(), config)).get();
    auto plaintext = std::async(std::launch::async, [port] { return request(port, "/proxy"); });
    EXPECT_NE(tls_request(port, first).find("127.0.0.1:direct:https"), std::string::npos);
    EXPECT_NE(plaintext.get().find("127.0.0.1:direct:http"), std::string::npos);
    runtime.loops_[0].start(group.replace_tls_identities(runtime.loops_[0].worker(), target, secure(port, 0, second).tls_)).get();
    EXPECT_NE(tls_request(port, second).find("127.0.0.1:direct:https"), std::string::npos);
    runtime.loops_[0].start(group.remove_listener(runtime.loops_[0].worker(), target)).get();
    asio::ip::tcp::socket refused(io);
    asio::error_code error;
    refused.connect(reservation.endpoint(), error);
    EXPECT_EQ(error, asio::error::connection_refused);
    config.tls_ = secure(port, 0, second).tls_;
    runtime.loops_[0].start(group.add_listener(runtime.loops_[0].worker(), config)).get();
    EXPECT_NE(request(port, "/proxy").find("127.0.0.1:direct:http"), std::string::npos);
    EXPECT_NE(tls_request(port, second).find("127.0.0.1:direct:https"), std::string::npos);
#if !defined(_WIN32)
    auto inherited = runtime.loops_[0].start(group.duplicate_listener_sockets(runtime.loops_[0].worker(), target)).get();
    EXPECT_TRUE(inherited.http_.empty());
    EXPECT_EQ(inherited.https_.size(), 2U);
#endif
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].worker())).get();
    ruvia::http_server_group successor(runtime.loops_, &answer_callbacks);
#if !defined(_WIN32)
    config.inherited_sockets_ = std::move(inherited);
#endif
    successor.listen(std::move(config));
    runtime.loops_[0].start(successor.start(runtime.loops_[0].worker())).get();
    EXPECT_NE(request(port, "/proxy").find("127.0.0.1:direct:http"), std::string::npos);
    EXPECT_NE(tls_request(port, second).find("127.0.0.1:direct:https"), std::string::npos);
    runtime.loops_[0].start(successor.shutdown(runtime.loops_[0].worker())).get();
}

TEST(Ruvia, mixed_raw_connection_services_detect_transport_after_single_and_group_adoption) {
    using namespace server_group_test;
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

    ruvia::http_server_group group(runtime.loops_, [](const ruvia::worker_ref&) { return ruvia::http_server_callbacks{.connection_ = &raw_transport_answer}; });
    runtime.loops_[0].start(group.add_connection_service(runtime.loops_[0].worker(), config)).get();
    runtime.loops_[0].start(group.start(runtime.loops_[0].worker())).get();
    for (std::size_t worker = 0; worker < group.worker_count(); ++worker) {
        for (const bool secured : {false, true}) {
            asio::ip::tcp::socket peer(io);
            peer.connect(acceptor.local_endpoint());
            auto accepted = acceptor.accept();
            runtime.loops_[0].start(group.adopt(runtime.loops_[0].worker(), worker, target, accepted.release())).get();
            if (secured) {
                asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(peer), client);
                stream.handshake(asio::ssl::stream_base::client);
                EXPECT_TRUE(request_on(stream).ends_with("tls"));
            } else {
                EXPECT_TRUE(request_on(peer).ends_with("plain"));
            }
        }
    }
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].worker())).get();
}

TEST(Ruvia, independent_group_cross_worker_add_preserves_caller_timers_and_cold_address_inputs) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    runtime.loops_[0].start(group.start(runtime.loops_[0].worker())).get();

    auto addition = [&] {
        std::string address = "127.0.0.1";
        auto config = plain(port.endpoint().port());
        config.address_ = address;
        auto pending = group.add_listener(runtime.loops_[0].worker(), std::move(config));
        address.assign(address.size(), 'x');
        return pending;
    }();
    held_work held;
    std::promise<void> blocked;
    auto blocked_result = blocked.get_future();
    ASSERT_TRUE(runtime.loops_[1].worker().post([gate = held.gate_, &blocked] {
                                              blocked.set_value();
                                              gate->acquire();
                                          })
            .accepted());
    blocked_result.get();
    auto operation = runtime.loops_[0].start(std::move(addition));
    std::promise<void> ticked;
    auto tick = ticked.get_future();
    const auto timer = [&]() -> ruvia::task<void> {
        (void)co_await ruvia::sleep_for(runtime.loops_[0].worker(), 10ms);
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
        auto pending = group.remove_listener(runtime.loops_[0].worker(),
            {.address_ = address, .port_ = port.endpoint().port()});
        address.assign(address.size(), 'x');
        return pending;
    }();
    runtime.loops_[0].start(std::move(removal)).get();
    asio::ip::tcp::socket refused(ports);
    asio::error_code error;
    refused.connect(port.endpoint(), error);
    EXPECT_EQ(error, asio::error::connection_refused);
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].worker())).get();
}

TEST(Ruvia, independent_group_tls_preparation_keeps_caller_live_and_close_cancels_publication) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    const ruvia::test::tls_identity identity("group-cancel", "DNS:localhost");
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    runtime.loops_[0].start(group.start(runtime.loops_[0].worker())).get();
    std::counting_semaphore<> occupied{0};
    held_work held(2);
    for (int index = 0; index != 2; ++index) {
        ASSERT_TRUE(runtime.pool_.submit([gate = held.gate_, &occupied] {
                                     occupied.release();
                                     gate->acquire();
                                 })
                .accepted());
    }
    occupied.acquire();
    occupied.acquire();
    auto operation = runtime.loops_[0].start(group.add_listener(runtime.loops_[0].worker(),
        secure(port.endpoint().port(), 0, identity)));
    std::promise<void> ticked;
    auto tick = ticked.get_future();
    const auto timer = [&]() -> ruvia::task<void> {
        (void)co_await ruvia::sleep_for(runtime.loops_[0].worker(), 10ms);
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
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].worker())).get();
    asio::ip::tcp::socket refused(ports);
    asio::error_code error;
    refused.connect(port.endpoint(), error);
    EXPECT_EQ(error, asio::error::connection_refused);
    EXPECT_TRUE(runtime.loops_[0].worker().accepting());
    EXPECT_TRUE(runtime.loops_[1].worker().accepting());
}

TEST(Ruvia, independent_group_cold_adoption_owns_socket_and_endpoint_before_execution) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto port = acceptor.local_endpoint().port();
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    runtime.loops_[0].start(group.add_connection_service(runtime.loops_[0].worker(), plain(port))).get();
    runtime.loops_[0].start(group.start(runtime.loops_[0].worker())).get();

    asio::ip::tcp::socket discarded(io);
    discarded.connect(acceptor.local_endpoint());
    {
        auto accepted = acceptor.accept();
        auto cold = group.adopt(runtime.loops_[0].worker(), 1,
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
        auto pending = group.adopt(runtime.loops_[0].worker(), 1,
            {.address_ = address, .port_ = port}, accepted.release());
        address.assign(address.size(), 'x');
        return pending;
    }();
    runtime.loops_[0].start(std::move(adoption)).get();
    const auto response = request_on(peer, "/");
    EXPECT_NE(response.find(" 200 "), std::string::npos);
    EXPECT_NE(response.find(std::to_string(group.worker(1).id()) + ":"), std::string::npos);
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].worker())).get();
}

#if !defined(_WIN32)
TEST(Ruvia, independent_group_discarded_cold_addition_closes_all_inherited_listener_sockets) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group original(runtime.loops_, &answer_callbacks);
    original.listen(plain(port.endpoint().port()));
    runtime.loops_[0].start(original.start(runtime.loops_[0].worker())).get();
    auto sockets = runtime.loops_[0].start(original.duplicate_listener_sockets(runtime.loops_[0].worker(),
                                               {.address_ = "127.0.0.1", .port_ = port.endpoint().port()}))
                       .get();
    ASSERT_EQ(sockets.http_.size(), 2U);
    const auto descriptors = sockets.http_;
    ruvia::http_server_group replacement(runtime.loops_, &answer_callbacks);
    runtime.loops_[0].start(replacement.start(runtime.loops_[0].worker())).get();
    {
        auto config = plain(port.endpoint().port());
        config.inherited_sockets_ = std::move(sockets);
        auto cold = replacement.add_listener(runtime.loops_[0].worker(), std::move(config));
        (void)cold;
    }
    for (const auto descriptor : descriptors) {
        errno = 0;
        EXPECT_EQ(::fcntl(descriptor, F_GETFD), -1);
        EXPECT_EQ(errno, EBADF);
    }
    EXPECT_NE(request(port.endpoint().port()).find(" 200 "), std::string::npos);
    runtime.loops_[0].start(replacement.shutdown(runtime.loops_[0].worker())).get();
    runtime.loops_[0].start(original.shutdown(runtime.loops_[0].worker())).get();
}
#endif

TEST(Ruvia, independent_group_member_retirement_cancels_pending_cross_worker_addition) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context ports;
    ruvia::test::tcp_port_reservation port(ports);
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    runtime.loops_[0].start(group.start(runtime.loops_[0].worker())).get();
    held_work held;
    std::promise<void> blocked;
    auto blocked_result = blocked.get_future();
    ASSERT_TRUE(runtime.loops_[1].worker().post([gate = held.gate_, &blocked] {
                                              blocked.set_value();
                                              gate->acquire();
                                          })
            .accepted());
    blocked_result.get();
    auto operation = runtime.loops_[0].start(group.add_listener(runtime.loops_[0].worker(),
        plain(port.endpoint().port())));
    std::promise<void> submitted;
    auto submitted_result = submitted.get_future();
    const auto observe = [&]() -> ruvia::task<void> {
        (void)co_await ruvia::sleep_for(runtime.loops_[0].worker(), 10ms);
        submitted.set_value();
    };
    auto observed = runtime.loops_[0].start(observe());
    const auto progress = submitted_result.wait_for(2s);
    runtime.second_attachment_.stop();
    held.release();
    EXPECT_EQ(progress, std::future_status::ready);
    std::move(observed).get();
    EXPECT_ANY_THROW(std::move(operation).get());
    runtime.loops_[0].start(group.shutdown(runtime.loops_[0].worker())).get();
    EXPECT_EQ(group.stats().active_connections_, 0U);
    EXPECT_TRUE(runtime.loops_[0].worker().accepting());
    asio::ip::tcp::socket refused(ports);
    asio::error_code error;
    refused.connect(port.endpoint(), error);
    EXPECT_EQ(error, asio::error::connection_refused);
}

TEST(Ruvia, independent_group_prepared_batch_defers_handlers_and_rolls_back_abandoned_commit) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io), second(io), abandoned(io), abandoned_second(io);
    std::atomic<unsigned> calls{};
    std::atomic<bool> published{};
    ruvia::http_server_group group(runtime.loops_, [&](const ruvia::worker_ref&) {
        return ruvia::http_server_callbacks{.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
            EXPECT_TRUE(published.load());
            ++calls;
            co_await group_answer(stream);
        }};
    });
    const auto caller = runtime.loops_[0].worker();
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
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation original(io), first(io);
    asio::ip::tcp::acceptor blocker(io, {asio::ip::address_v4::loopback(), 0});
    const auto blocked = blocker.local_endpoint();
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    group.listen(plain(original.endpoint().port()));
    const auto caller = runtime.loops_[0].worker();
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
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation first(io), second(io);
    const auto config = [](std::uint16_t port) {
        return ruvia::listen_config{.address_ = "127.0.0.1", .http_ = port, .dispatch_ = ruvia::listener_dispatch::connection};
    };
    ruvia::http_server_group group(runtime.loops_, [](const ruvia::worker_ref&) {
        return ruvia::http_server_callbacks{.connection_ = [](ruvia::server_connection& connection) -> ruvia::task<void> {
            while (auto bytes = co_await connection.read()) {
                co_await connection.write(*bytes);
            }
        }};
    });
    group.listen(config(first.endpoint().port()));
    group.listen(config(second.endpoint().port()));
    const auto caller = runtime.loops_[0].worker();
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
    using namespace server_group_test;
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
        ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
        group.listen(config);
        const auto caller = runtime.loops_[0].worker();
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
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation secure_port(io), other(io);
    ruvia::test::udp_port_reservation udp(io);
    const ruvia::test::tls_identity identity("forced-listener", "DNS:localhost");
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    group.listen(secure(secure_port.endpoint().port(), udp.endpoint().port(), identity));
    group.listen(plain(other.endpoint().port()));
    const auto caller = runtime.loops_[0].worker();
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
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    std::binary_semaphore removed{0};
    ruvia::http_server_group* owner = nullptr;
    ruvia::http_server_group group(runtime.loops_, [&](const ruvia::worker_ref&) {
        return ruvia::http_server_callbacks{.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
            co_await owner->remove_listener(stream.worker(),
                {.address_ = "127.0.0.1", .port_ = port.endpoint().port()}, {.close_connections_ = true});
            removed.release();
        }};
    });
    owner = &group;
    group.listen(plain(port.endpoint().port()));
    const auto caller = runtime.loops_[0].worker();
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
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io), other(io);
    ruvia::http_server_group group(runtime.loops_, &answer_callbacks);
    group.listen(plain(port.endpoint().port()));
    group.listen(plain(other.endpoint().port()));
    const auto caller = runtime.loops_[0].worker();
    runtime.loops_[0].start(group.start(caller)).get();
    asio::ip::tcp::socket peer(io);
    peer.connect(port.endpoint());
    std::string preface("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
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

TEST(Ruvia, independent_group_factory_builds_per_worker_routers_on_member_loops) {
    using namespace server_group_test;
    external_loops runtime;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto port = acceptor.local_endpoint().port();
    std::vector<std::uint64_t> built_workers;
    ruvia::http_server_group group(runtime.loops_, [&](const ruvia::worker_ref& worker) {
        EXPECT_TRUE(worker.is_current());
        built_workers.push_back(worker.id());
        auto routes = std::make_shared<ruvia::router>(worker);
        routes->add("GET", "/who", [id = worker.id()](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text(std::to_string(id));
        });
        routes->freeze();
        return ruvia::http_server_callbacks{
            .request_stream_ = [routes = std::move(routes)](ruvia::request_stream& stream) { return routes->dispatch(stream); }};
    });
    const auto caller = runtime.loops_[0].worker();
    runtime.loops_[0].start(group.add_connection_service(caller, {.address_ = "127.0.0.1", .http_ = port})).get();
    runtime.loops_[0].start(group.start(caller)).get();
    ASSERT_EQ(built_workers.size(), 2U);
    EXPECT_EQ(built_workers[0], runtime.loops_[0].worker().id());
    EXPECT_EQ(built_workers[1], runtime.loops_[1].worker().id());
    for (std::size_t index = 0; index != group.worker_count(); ++index) {
        asio::ip::tcp::socket peer(io);
        peer.connect(acceptor.local_endpoint());
        auto accepted = acceptor.accept();
        runtime.loops_[0].start(group.adopt(caller, index, {.address_ = "127.0.0.1", .port_ = port}, accepted.release())).get();
        EXPECT_TRUE(request_on(peer, "/who").ends_with("\r\n\r\n" + std::to_string(group.worker(index).id())));
    }
    runtime.loops_[0].start(group.shutdown(caller)).get();
}

namespace standalone_websocket_test {
using namespace std::chrono_literals;

struct target final {
    std::string origin_;
    std::string authority_;
    ruvia::http_client_versions versions_;
    ruvia::http_protocol_version version_;
    asio::ip::tcp::endpoint endpoint_;
};

// What the handler observed once its session ended.
struct session_record final {
    ruvia::websocket_termination termination_{};
    std::uint64_t received_{};
    std::uint64_t sent_{};
};

struct observation final {
    unsigned entered_{};
    unsigned retired_{};
    unsigned refused_{};
    unsigned exclusive_{};
    unsigned cancelled_{};
    std::string_view scenario_{"startup"};
    std::optional<ruvia::task<std::optional<ruvia::websocket_message>>> expired_read_;
    std::optional<ruvia::websocket> expired_session_;
    std::optional<ruvia::task<ruvia::websocket_accept_result>> expired_accept_;
    std::map<std::string, ruvia::websocket_rejection, std::less<>> rejections_;
    std::map<std::string, session_record, std::less<>> sessions_;

    void record(std::string_view path, const ruvia::websocket& websocket) {
        sessions_.insert_or_assign(std::string(path),
            session_record{websocket.termination(), websocket.bytes_received(), websocket.bytes_sent()});
    }
    [[nodiscard]] session_record session(std::string_view path) const {
        const auto found = sessions_.find(path);
        if (found == sessions_.end()) {
            throw std::runtime_error("WebSocket session outcome was not recorded");
        }
        return found->second;
    }
};

struct request_retirement final {
    observation& observed_;
    explicit request_retirement(observation& observed)
        : observed_(observed) {
        ++observed_.entered_;
    }
    ~request_retirement() {
        ++observed_.retired_;
    }
};

ruvia::task<void> expect_read_interrupted(
    ruvia::task<std::optional<ruvia::websocket_message>> read, std::errc expected, bool& interrupted) {
    try {
        (void)co_await std::move(read);
    } catch (const std::system_error& error) {
        interrupted = error.code() == expected;
    }
}

ruvia::task<void> read_to_end(ruvia::websocket& websocket, bool& ended) {
    ended = !(co_await websocket.read()).has_value();
}

ruvia::task<void> read_operation_to_end(ruvia::task<std::optional<ruvia::websocket_message>> operation, bool& ended) {
    ended = !(co_await std::move(operation)).has_value();
}

// Reads interrupted by their own deadline or stop request keep the session;
// close(4001) then completes the handshake under a pending read.
ruvia::task<void> close_under_pending_read(ruvia::websocket& websocket, const ruvia::worker_ref& worker) {
    const auto start = co_await websocket.read();
    EXPECT_TRUE(start && start->payload() == "start");
    bool timed_out = false;
    co_await expect_read_interrupted(websocket.read({.timeout_ = 20ms}), std::errc::timed_out, timed_out);
    bool cancelled = false;
    {
        ruvia::stop_source stop;
        ruvia::task_scope reads(worker);
        reads.spawn(expect_read_interrupted(
            websocket.read({.stop_token_ = stop.token()}), std::errc::operation_canceled, cancelled));
        stop.request_stop();
        co_await reads.join();
    }
    EXPECT_TRUE(timed_out);
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::none);
    co_await websocket.text("interrupted");
    const auto resumed = co_await websocket.read();
    EXPECT_TRUE(resumed && resumed->payload() == "resume");
    bool ended = false;
    ruvia::task_scope reads(worker);
    reads.spawn(read_to_end(websocket, ended));
    std::exception_ptr failure;
    try {
        co_await websocket.close({.code_ = ruvia::websocket_close_code::from_value(4001), .reason_ = "token expired"});
    } catch (...) {
        failure = std::current_exception();
        websocket.abort();
    }
    co_await reads.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(ended);
}

constexpr std::size_t fragment_count = 4;

[[nodiscard]] std::string fragment_payload(std::size_t index) {
    return std::string(3000, static_cast<char>('a' + index));
}

// Sends each message in fragment_count frames with a Ping between the first
// two, uncompressed and then compressed.
ruvia::task<void> send_fragmented(ruvia::websocket& websocket) {
    for (const bool compress : {false, true}) {
        auto message = websocket.begin_text({.compress_ = compress});
        bool data_lane_held = false;
        try {
            co_await websocket.text("overlap");
        } catch (const std::logic_error&) {
            data_lane_held = true;
        }
        EXPECT_TRUE(data_lane_held);
        for (std::size_t index = 0; index + 1 < fragment_count; ++index) {
            co_await message.write(fragment_payload(index));
            if (index == 0) {
                co_await websocket.ping("between");
            }
        }
        co_await message.finish(fragment_payload(fragment_count - 1));
    }
}

// The identical business coroutine runs at both real ends, irrespective of role.
ruvia::task<void> exchange_fragmented(ruvia::websocket& websocket) {
    co_await send_fragmented(websocket);
    std::string expected;
    for (std::size_t i = 0; i < fragment_count; ++i) {
        expected += fragment_payload(i);
    }
    for (unsigned i = 0; i != 2; ++i) {
        const auto message = co_await websocket.read();
        EXPECT_TRUE(message && message->payload() == expected);
    }
    co_await websocket.close();
    EXPECT_EQ(websocket.peer_close_code(), 1000);
}

// Reassembles one message from fragment frames, counting frames and the Ping.
ruvia::task<std::string> receive_fragmented(auto& peer, bool compressed, std::size_t& data_frames, std::size_t& pings) {
    std::string payload;
    for (;;) {
        auto frame = co_await peer.read_frame();
        if (!frame) {
            throw std::runtime_error("missing message fragment");
        }
        if (frame->opcode_ == 9U) {
            EXPECT_TRUE(frame->final_);
            ++pings;
            continue;
        }
        EXPECT_EQ(frame->opcode_, data_frames == 0 ? 1U : 0U);
        EXPECT_EQ(frame->compressed_, compressed && data_frames == 0);
        ++data_frames;
        payload += frame->payload_;
        if (frame->final_) {
            break;
        }
    }
    if (!compressed) {
        co_return payload;
    }
    ruvia::websocket_deflate_codecs codecs;
    std::string inflated;
    EXPECT_TRUE(codecs.inflate_message(payload, inflated, ruvia::protocol_byte_limit::unlimited()) ==
                ruvia::websocket_inflate_result::ok);
    co_return std::string(inflated);
}

ruvia::task<void> serve(ruvia::request_stream& request, observation& observed) {
    request_retirement retirement(observed);
    const auto path = request.request().path();
    if (path == "/forbidden") {
        request.draft().status(ruvia::http_status::forbidden);
        (void)co_await request.response().commit();
        co_await request.response().end();
        try {
            (void)co_await request.accept_websocket();
        } catch (const std::logic_error&) {
            ++observed.exclusive_;
        }
        co_return;
    }
    if (path == "/cold") {
        observed.expired_accept_.emplace(request.accept_websocket());
        request.draft().status(ruvia::http_status::forbidden);
        (void)co_await request.response().commit();
        co_await request.response().end();
        co_return;
    }
    ruvia::websocket_route_config config{
        .subprotocols_ = {"standalone.v1"},
        .require_subprotocol_ = true,
        .lifecycle_ = {.close_handshake_timeout_ = 40ms},
        .deflate_ = {.context_takeover_ = true}};
    if (path == "/heartbeat") {
        config.lifecycle_.heartbeat_ = {.ping_interval_ = 20ms, .pong_timeout_ = 40ms};
    }
    if (path == "/limit") {
        config.max_message_bytes_ = ruvia::protocol_byte_limit::limited(256);
    }
    if (path == "/graceful") {
        config.lifecycle_.close_handshake_timeout_ = 5s;
    }
    {
        auto discarded = request.accept_websocket(config);
    }
    const auto accepted = co_await request.accept_websocket(std::move(config));
    auto* websocket = accepted.session();
    if (websocket == nullptr) {
        ++observed.refused_;
        observed.rejections_.insert_or_assign(std::string(path), *accepted.rejection());
        co_return;
    }
    EXPECT_EQ(websocket->subprotocol(), "standalone.v1");
    try {
        (void)co_await request.accept_tunnel();
    } catch (const std::logic_error&) {
        ++observed.exclusive_;
    }
    try {
        (void)co_await request.accept_websocket();
    } catch (const std::logic_error&) {
        ++observed.exclusive_;
    }
    try {
        (void)co_await request.response().commit();
    } catch (const std::logic_error&) {
        ++observed.exclusive_;
    }
    if (path == "/limit") {
        const auto first = co_await websocket->read();
        EXPECT_TRUE(first.has_value());
        if (first) {
            EXPECT_EQ(first->payload().size(), 256U);
        }
        // 本地接收上限不能限制本会话的发送方向。
        co_await websocket->text(std::string(1024, 'r'), {.compress_ = false});
        EXPECT_FALSE((co_await websocket->read()).has_value());
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/close") {
        co_return;
    }
    if (path == "/moved") {
        observed.expired_session_.emplace(std::move(*websocket));
        co_return;
    }
    if (path == "/shared") {
        co_await exchange_fragmented(*websocket);
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/fragments") {
        co_await send_fragmented(*websocket);
        EXPECT_FALSE((co_await websocket->read()).has_value());
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/assemble") {
        while (const auto message = co_await websocket->read()) {
            co_await websocket->text(std::to_string(message->payload().size()) + (message->binary() ? ":binary" : ":text"));
        }
        co_return;
    }
    if (path == "/graceful") {
        co_await close_under_pending_read(*websocket, request.worker());
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/abort") {
        // Abort after the peer has its handshake response.
        EXPECT_TRUE((co_await websocket->read()).has_value());
        websocket->abort();
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/eof" || path == "/invalid-frame" || path == "/invalid-utf8") {
        EXPECT_FALSE((co_await websocket->read()).has_value());
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/count") {
        // Echoes one message after a Ping, then reads until the peer's Close.
        auto message = co_await websocket->read();
        if (!message) {
            throw std::runtime_error("missing counted message");
        }
        const std::string payload(message->payload());
        co_await websocket->ping("abc");
        co_await websocket->text(payload);
        EXPECT_FALSE((co_await websocket->read()).has_value());
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/fail") {
        throw std::runtime_error("private application failure");
    }
    if (path == "/heartbeat" || path == "/cancel" || path == "/host") {
        try {
            EXPECT_FALSE((co_await websocket->read()).has_value());
        } catch (const std::system_error&) {
        }
        if (path == "/heartbeat") {
            observed.record(path, *websocket);
        }
        if (request.stop_token().stop_requested()) {
            ++observed.cancelled_;
        }
        co_return;
    }
    auto message = co_await websocket->read();
    if (!message) {
        throw std::runtime_error("missing application command");
    }
    if (path == "/wire") {
        EXPECT_EQ(message->payload(), "prebuffered");
        co_await websocket->ping("probe");
        co_await websocket->text(std::string(2048, 'x'));
        auto final = co_await websocket->read();
        EXPECT_FALSE(final.has_value());
        observed.expired_read_.emplace(websocket->read());
        co_return;
    }
    EXPECT_EQ(message->opcode(), ruvia::websocket_opcode::text);
    EXPECT_EQ(message->payload(), std::string(4096, 'z'));
    co_await websocket->text("accepted:4096", {.compress_ = false});
    co_await websocket->binary(std::as_bytes(std::span(std::string_view("binary-response"))));
    co_await websocket->text(std::string(4096, 'x'));
    co_await websocket->ping("server-control");
    message = co_await websocket->read();
    if (!message) {
        throw std::runtime_error("missing binary command");
    }
    EXPECT_EQ(message->opcode(), ruvia::websocket_opcode::binary);
    EXPECT_EQ(message->payload(), std::string_view("\0\1\2\3", 4));
    co_await websocket->text("binary:4");
}

std::string masked_frame(unsigned opcode, std::string_view payload, bool final = true, bool compressed = false) {
    if (payload.size() > 65535) {
        throw std::invalid_argument("test frame payload is too large");
    }
    const std::array<unsigned char, 4> mask{0x37, 0xfa, 0x21, 0x3d};
    std::string bytes;
    bytes.push_back(static_cast<char>((final ? 0x80U : 0U) | (compressed ? 0x40U : 0U) | opcode));
    if (payload.size() <= 125) {
        bytes.push_back(static_cast<char>(0x80U | payload.size()));
    } else {
        bytes.push_back(static_cast<char>(0xfe));
        bytes.push_back(static_cast<char>(payload.size() >> 8U));
        bytes.push_back(static_cast<char>(payload.size() & 0xffU));
    }
    for (const auto byte : mask) {
        bytes.push_back(static_cast<char>(byte));
    }
    for (std::size_t index = 0; index != payload.size(); ++index) {
        bytes.push_back(static_cast<char>(static_cast<unsigned char>(payload[index]) ^ mask[index % mask.size()]));
    }
    return bytes;
}

// The byte peer deliberately has no WebSocket runtime: assertions observe the
// server's actual frame bits, automatic Pong, and deadline transport retirement.
class wire_peer final {
public:
    explicit wire_peer(asio::io_context& io)
        : socket_(io) {}
    ~wire_peer() {
        abort();
    }

    ruvia::task<void> open(ruvia::http_client& client, const target& destination, std::string_view path,
        bool prebuffer = false, bool invalid_version = false) {
        if (destination.version_ != ruvia::http_protocol_version::http11) {
            if (invalid_version) {
                throw std::logic_error("malformed extended CONNECT needs the raw wire fixture");
            }
            result_.emplace(co_await client.client().connect(destination.authority_).protocol("websocket").target(path).header("Sec-WebSocket-Version", "13").header("Sec-WebSocket-Protocol", "standalone.v1").header("Sec-WebSocket-Extensions", "permessage-deflate").open());
            auto* tunnel = result_->tunnel();
            if (tunnel == nullptr) {
                throw std::runtime_error("extended CONNECT was refused");
            }
            EXPECT_EQ(tunnel->status(), ruvia::http_status::ok);
            EXPECT_EQ(tunnel->protocol_version(), destination.version_);
            EXPECT_EQ(tunnel->header("Sec-WebSocket-Protocol"), "standalone.v1");
            EXPECT_TRUE(tunnel->header("Sec-WebSocket-Extensions").has_value());
            if (prebuffer) {
                co_await write(masked_frame(1, "prebuffered"));
            }
            co_return;
        }
        const auto connected = co_await ruvia::async_asio([&](auto completion) {
            socket_.async_connect(destination.endpoint_, std::move(completion));
        });
        if (connected.error_code()) {
            throw std::system_error(connected.error_code());
        }
        std::string head = "GET " + std::string(path) + " HTTP/1.1\r\nHost: " + destination.authority_ +
                           "\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: " +
                           (invalid_version ? "12" : "13") +
                           "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: standalone.v1\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n";
        if (prebuffer) {
            head += masked_frame(1, "prebuffered");
        }
        co_await write(head);
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
            asio::async_read_until(socket_, asio::dynamic_buffer(input_), "\r\n\r\n", std::move(completion));
        });
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        const auto response = input_.substr(0, read.result());
        if (invalid_version) {
            EXPECT_TRUE(response.starts_with("HTTP/1.1 400 "));
            EXPECT_NE(response.find("Sec-WebSocket-Version: 13"), std::string::npos);
            co_return;
        }
        EXPECT_TRUE(response.starts_with("HTTP/1.1 101 "));
        EXPECT_NE(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
        EXPECT_NE(response.find("standalone.v1"), std::string::npos);
        EXPECT_NE(response.find("permessage-deflate"), std::string::npos);
        input_.erase(0, read.result());
    }

    ruvia::task<void> write(std::string_view bytes) {
        if (result_) {
            co_await result_->tunnel()->write(bytes);
        } else {
            const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
                asio::async_write(socket_, asio::buffer(bytes), std::move(completion));
            });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
        }
    }

    struct frame final {
        unsigned opcode_;
        bool compressed_;
        std::string payload_;
        bool final_;
    };

    ruvia::task<std::optional<frame>> read_frame() {
        if (!(co_await fill(2))) {
            co_return std::nullopt;
        }
        const auto first = static_cast<unsigned char>(input_[0]);
        const auto second = static_cast<unsigned char>(input_[1]);
        EXPECT_EQ(second & 0x80U, 0U);
        std::size_t size = second & 0x7fU;
        std::size_t offset = 2;
        if (size == 126) {
            if (!(co_await fill(4))) {
                throw std::runtime_error("truncated frame length");
            }
            size = (static_cast<unsigned char>(input_[2]) << 8U) | static_cast<unsigned char>(input_[3]);
            offset = 4;
        } else if (size == 127) {
            throw std::runtime_error("unexpected huge test frame");
        }
        if (!(co_await fill(offset + size))) {
            throw std::runtime_error("truncated frame payload");
        }
        frame result{first & 0x0fU, (first & 0x40U) != 0, input_.substr(offset, size), (first & 0x80U) != 0};
        input_.erase(0, offset + size);
        co_return result;
    }

    // Ends the peer's sending direction without a Close frame.
    ruvia::task<void> finish() {
        if (result_) {
            co_await result_->tunnel()->finish();
        } else {
            std::error_code ignored;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
        }
    }

    void abort() {
        if (result_ && result_->tunnel()) {
            result_->tunnel()->abort();
        }
        ruvia::close_socket(socket_);
    }

private:
    ruvia::task<bool> fill(std::size_t bytes) {
        while (input_.size() < bytes) {
            if (result_) {
                auto chunk = co_await result_->tunnel()->text();
                if (!chunk) {
                    co_return false;
                }
                input_.append(*chunk);
            } else {
                std::array<char, 4096> storage;
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
                    socket_.async_read_some(asio::buffer(storage), std::move(completion));
                });
                if (read.result() != 0) {
                    input_.append(storage.data(), read.result());
                }
                if (read.error_code() == asio::error::eof) {
                    co_return input_.size() >= bytes;
                }
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
            }
        }
        co_return true;
    }

    asio::ip::tcp::socket socket_;
    std::optional<ruvia::http_client_tunnel_result> result_;
    std::string input_;
};

ruvia::task<void> await_retirement(ruvia::http_server& server, const observation& observed) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (observed.entered_ != observed.retired_) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("WebSocket request did not retire");
        }
        (void)co_await ruvia::sleep_for(server.worker(), 1ms);
    }
}

// Typed CONNECT senders enforce RFC 6455's client-side version requirement.
// Malformed bootstrap coverage must bypass those senders, not weaken them.
void reject_invalid_extended_connect(const target& destination, const std::filesystem::path& ca_file) {
#if defined(RUVIA_ENABLE_HTTP2)
    if (destination.version_ == ruvia::http_protocol_version::http2) {
        asio::io_context io;
        asio::ssl::context tls(asio::ssl::context::tls_client);
        tls.load_verify_file(ca_file.string());
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, tls);
        stream.set_verify_mode(asio::ssl::verify_peer);
        stream.set_verify_callback(asio::ssl::host_name_verification("127.0.0.1"));
        constexpr std::array<unsigned char, 3> alpn{2, 'h', '2'};
        if (SSL_set_alpn_protos(stream.native_handle(), alpn.data(), static_cast<unsigned int>(alpn.size())) != 0) {
            throw std::runtime_error("cannot configure malformed HTTP/2 peer ALPN");
        }
        stream.next_layer().connect(destination.endpoint_);
        stream.handshake(asio::ssl::stream_base::client);
        std::string wire(ruvia::http2_client_preface);
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        asio::write(stream, asio::buffer(wire));
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        for (;;) {
            const auto frame = ruvia::test::read_frame(stream, deadline);
            if (frame.header_.type_ == static_cast<unsigned>(ruvia::http2_frame_type::settings) &&
                (frame.header_.flags_ & 1U) == 0) {
                break;
            }
        }
        wire.clear();
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 1, 0, {});
        std::string block;
        for (const auto& field : std::array<std::pair<std::string_view, std::string_view>, 7>{{{":method", "CONNECT"}, {":protocol", "websocket"}, {":scheme", "https"},
                 {":authority", destination.authority_}, {":path", "/invalid-version"},
                 {"sec-websocket-version", "12"}, {"sec-websocket-protocol", "standalone.v1"}}}) {
            ruvia::hpack_encoder::encode_header(block, field.first, field.second);
        }
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 4, 1, block);
        asio::write(stream, asio::buffer(wire));
        block.clear();
        ruvia::hpack_decoder decoder;
        bool ended = false;
        for (;;) {
            const auto frame = ruvia::test::read_frame(stream, deadline);
            if (frame.header_.stream_id_ != 1) {
                continue;
            }
            const auto type = frame.header_.type_;
            if (type != static_cast<unsigned>(ruvia::http2_frame_type::headers) &&
                type != static_cast<unsigned>(ruvia::http2_frame_type::continuation)) {
                throw std::runtime_error("malformed HTTP/2 bootstrap did not receive response HEADERS");
            }
            if (type == static_cast<unsigned>(ruvia::http2_frame_type::headers)) {
                ended = (frame.header_.flags_ & 1U) != 0;
            }
            block.append(frame.payload_);
            if ((frame.header_.flags_ & 4U) == 0) {
                continue;
            }
            bool status = false;
            bool version = false;
            const auto error = decoder.decode(block, [&](std::string_view name, std::string_view value) {
                if (name == ":status") {
                    EXPECT_EQ(value, "400");
                    status = true;
                } else if (name == "sec-websocket-version") {
                    EXPECT_EQ(value, "13");
                    version = true;
                }
                return true;
            });
            EXPECT_FALSE(error.has_value());
            EXPECT_TRUE(status);
            EXPECT_TRUE(version);
            while (!ended) {
                const auto body = ruvia::test::read_frame(stream, deadline);
                if (body.header_.stream_id_ != 1) {
                    continue;
                }
                if (body.header_.type_ != static_cast<unsigned>(ruvia::http2_frame_type::data)) {
                    throw std::runtime_error("malformed HTTP/2 bootstrap response did not complete normally");
                }
                ended = (body.header_.flags_ & 1U) != 0;
            }
            return;
        }
    }
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    if (destination.version_ == ruvia::http_protocol_version::http3) {
        const auto port = static_cast<std::uint16_t>(std::stoul(destination.authority_.substr(destination.authority_.rfind(':') + 1)));
        // The public client validates Sec-WebSocket-Version: 13 before sending.
        ruvia::test::http3_raw_request_client peer({asio::ip::address_v4::loopback(), port}, ca_file, "127.0.0.1", 3s);
        const auto response = peer.request({{":method", "CONNECT"}, {":protocol", "websocket"}, {":scheme", "https"},
                                               {":authority", destination.authority_}, {":path", "/invalid-version"},
                                               {"sec-websocket-version", "12"}, {"sec-websocket-protocol", "standalone.v1"}},
            false);
        EXPECT_EQ(response.status_, 400);
        const auto version = response.header("sec-websocket-version");
        ASSERT_TRUE(version.has_value());
        EXPECT_EQ(*version, "13");
        return;
    }
#endif
    throw std::logic_error("malformed extended CONNECT needs HTTP/2 or HTTP/3");
}

ruvia::task<void> exercise(ruvia::http_client& client, const target& destination,
    ruvia::http_server& server, observation& observed, asio::io_context& io, const std::filesystem::path& ca_file) {
    for (unsigned mode = 0; mode != 3; ++mode) {
        observed.scenario_ = "/limit";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/limit");
        ruvia::websocket_deflate_codecs codecs;
        const auto message_frame = [&](std::string_view payload, bool compressed) {
            if (!compressed) {
                return masked_frame(1, payload);
            }
            std::string encoded;
            if (!codecs.deflate_message(payload, encoded, 6, 15)) {
                throw std::runtime_error("test message compression failed");
            }
            EXPECT_LT(encoded.size(), 256U);
            return masked_frame(1, encoded, true, true);
        };
        const auto first = message_frame(std::string(256, 'a'), mode == 1);
        co_await peer.write(first);
        const auto response = co_await peer.read_frame();
        if (!response) {
            throw std::runtime_error("missing response above the receive-only limit");
        }
        EXPECT_EQ(response->opcode_, 1U);
        EXPECT_FALSE(response->compressed_);
        EXPECT_EQ(response->payload_, std::string(1024, 'r'));
        std::string oversized;
        if (mode == 0) {
            // 只发送声明257字节的帧头；不得等待或预分配完整正文才拒绝。
            oversized.assign("\x81\xfe\x01\x01\0\0\0\0", 8);
        } else if (mode == 1) {
            // 压缩消息线面小于256字节，解压输出仍必须在本地限额处拒绝。
            oversized = message_frame(std::string(8192, 'a'), true);
        } else {
            // 每片都合法，跨分片累计消息超过256字节时拒绝。
            const std::string fragment(100, 'a');
            oversized = masked_frame(1, fragment, false);
            oversized += masked_frame(0, fragment, false);
            oversized += masked_frame(0, fragment);
        }
        co_await peer.write(oversized);
        // 协议失败允许服务器发Close后直接终结传输（RFC6455 §7.1.7）；
        // 观察真实1009帧，不启动客户端自动Close回复写入已终结的隧道。
        const auto close = co_await peer.read_frame();
        if (!close) {
            throw std::runtime_error("missing receive-limit Close");
        }
        EXPECT_EQ(close->opcode_, 8U);
        EXPECT_FALSE(close->compressed_);
        EXPECT_EQ(close->payload_, std::string("\x03\xf1", 2));
        co_await await_retirement(server, observed);
        EXPECT_TRUE(observed.session("/limit").termination_ == ruvia::websocket_termination::message_too_large);
    }
    {
        observed.scenario_ = "/shared";
        auto websocket = co_await client.client().websocket("/shared").subprotocol("standalone.v1").deflate({.compression_ = {.enabled_ = true}}).connect();
        co_await exchange_fragmented(websocket);
    }
    co_await await_retirement(server, observed);
    {
        observed.scenario_ = "/moved";
        auto websocket = co_await client.client().websocket("/moved").subprotocol("standalone.v1").connect();
        EXPECT_FALSE((co_await websocket.read()).has_value());
        co_await await_retirement(server, observed);
        bool expired = false;
        try {
            (void)co_await observed.expired_session_->read();
        } catch (const std::logic_error&) {
            expired = true;
        }
        EXPECT_TRUE(expired);
        observed.expired_session_.reset();
    }
    {
        observed.scenario_ = "/exchange";
        auto websocket = co_await client.client().websocket("/exchange").subprotocol("standalone.v1").deflate({.compression_ = {.enabled_ = true}}).connect();
        EXPECT_EQ(websocket.subprotocol(), "standalone.v1");
        co_await websocket.text(std::string(4096, 'z'));
        auto message = co_await websocket.read();
        if (!message) {
            throw std::runtime_error("missing command response");
        }
        EXPECT_EQ(message->payload(), "accepted:4096");
        message = co_await websocket.read();
        if (!message) {
            throw std::runtime_error("missing binary response");
        }
        EXPECT_EQ(message->opcode(), ruvia::websocket_opcode::binary);
        EXPECT_EQ(message->payload(), "binary-response");
        message = co_await websocket.read();
        if (!message) {
            throw std::runtime_error("missing compressed response");
        }
        EXPECT_EQ(message->payload(), std::string(4096, 'x'));
        co_await websocket.ping("client-control");
        const std::array<std::byte, 4> command{std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3}};
        co_await websocket.binary(command, {.compress_ = false});
        message = co_await websocket.read();
        if (!message) {
            throw std::runtime_error("missing binary command response");
        }
        EXPECT_EQ(message->payload(), "binary:4");
        co_await websocket.close();
        EXPECT_EQ(websocket.peer_close_code(), 1000);
    }
    co_await await_retirement(server, observed);
    {
        observed.scenario_ = "/fragments";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/fragments");
        std::string expected;
        for (std::size_t index = 0; index != fragment_count; ++index) {
            expected += fragment_payload(index);
        }
        for (const bool compressed : {false, true}) {
            std::size_t data_frames = 0;
            std::size_t pings = 0;
            const auto payload = co_await receive_fragmented(peer, compressed, data_frames, pings);
            EXPECT_EQ(payload, expected);
            EXPECT_EQ(data_frames, fragment_count);
            EXPECT_EQ(pings, 1U);
        }
        co_await peer.write(masked_frame(8, std::string("\x03\xe8", 2)));
        const auto close = co_await peer.read_frame();
        EXPECT_TRUE(close && close->opcode_ == 8U);
        co_await await_retirement(server, observed);
        const auto counted = observed.session("/fragments");
        // Two messages of fragment_count frames, the Pings and the Close echo.
        EXPECT_GT(counted.sent_, 2 * fragment_payload(0).size());
    }
    {
        observed.scenario_ = "/assemble";
        auto websocket = co_await client.client().websocket("/assemble").subprotocol("standalone.v1").deflate({.compression_ = {.enabled_ = true}}).connect();
        for (const bool compress : {false, true}) {
            const auto sent_before = websocket.bytes_sent();
            auto message = websocket.begin_binary({.compress_ = compress});
            auto moved = std::move(websocket);
            bool data_lane_held = false;
            try {
                co_await moved.binary(std::as_bytes(std::span(std::string_view("overlap"))));
            } catch (const std::logic_error&) {
                data_lane_held = true;
            }
            EXPECT_TRUE(data_lane_held);
            for (std::size_t index = 0; index + 1 < fragment_count; ++index) {
                co_await message.write(fragment_payload(index));
                if (index == 0) {
                    co_await moved.ping("between");
                }
                EXPECT_GT(moved.bytes_sent(), sent_before);
            }
            co_await message.finish(fragment_payload(fragment_count - 1));
            const auto reply = co_await moved.read();
            EXPECT_TRUE(reply && reply->payload() == std::to_string(fragment_count * fragment_payload(0).size()) + ":binary");
            websocket = std::move(moved);
        }
        {
            // An unfinished message cannot be completed later, so dropping it ends the session.
            auto abandoned = websocket.begin_text();
            co_await abandoned.write("partial");
        }
        EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::aborted);
        co_await await_retirement(server, observed);
    }
    {
        observed.scenario_ = "/graceful";
        auto original = co_await client.client().websocket("/graceful").subprotocol("standalone.v1").connect();
        auto cold_read = original.read({.timeout_ = 20ms});
        auto websocket = std::move(original);
        // The server stays silent until "start", so these client reads can
        // only end by their own deadline or stop request.
        bool timed_out = false;
        try {
            (void)co_await std::move(cold_read);
        } catch (const std::system_error& error) {
            timed_out = error.code() == std::errc::timed_out;
        }
        bool cancelled = false;
        {
            ruvia::stop_source stop;
            ruvia::task_scope reads(websocket.worker());
            reads.spawn(expect_read_interrupted(websocket.read({.stop_token_ = stop.token()}), std::errc::operation_canceled, cancelled));
            auto moved = std::move(websocket);
            stop.request_stop();
            co_await reads.join();
            websocket = std::move(moved);
        }
        EXPECT_TRUE(timed_out);
        EXPECT_TRUE(cancelled);
        EXPECT_TRUE(websocket.connected());
        co_await websocket.text("start");
        const auto interrupted = co_await websocket.read();
        EXPECT_TRUE(interrupted && interrupted->payload() == "interrupted");
        co_await websocket.text("resume");
        EXPECT_FALSE((co_await websocket.read()).has_value());
        EXPECT_EQ(websocket.peer_close_code(), 4001);
        EXPECT_EQ(websocket.peer_close_reason(), "token expired");
        EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::peer_close);
        co_await await_retirement(server, observed);
        EXPECT_TRUE(observed.session("/graceful").termination_ == ruvia::websocket_termination::local_close);
    }
    // Masked text (2 + 4 + 5), automatic Pong to Ping "abc" (2 + 4 + 3) and a
    // Close carrying 1000 (2 + 4 + 2) up; Ping (2 + 3), the echo (2 + 5) and
    // the Close echo (2 + 2) down. Deflated payloads count as compressed.
    for (const bool deflate : {false, true}) {
        observed.scenario_ = "/count";
        const std::string payload = deflate ? std::string(4096, 'c') : std::string("hello");
        auto request = client.client().websocket("/count");
        request.subprotocol("standalone.v1");
        if (deflate) {
            request.deflate({.compression_ = {.enabled_ = true}});
        }
        auto websocket = co_await request.connect();
        co_await websocket.text(payload);
        const auto echo = co_await websocket.read();
        EXPECT_TRUE(echo && echo->payload() == payload);
        co_await websocket.close();
        EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::local_close);
        co_await await_retirement(server, observed);
        const auto counted = observed.session("/count");
        EXPECT_TRUE(counted.termination_ == ruvia::websocket_termination::peer_close);
        EXPECT_EQ(websocket.bytes_sent(), counted.received_);
        EXPECT_EQ(websocket.bytes_received(), counted.sent_);
        if (deflate) {
            EXPECT_LT(websocket.bytes_sent(), payload.size());
            EXPECT_LT(websocket.bytes_received(), payload.size());
        } else {
            EXPECT_EQ(websocket.bytes_sent(), 11U + 9U + 8U);
            EXPECT_EQ(websocket.bytes_received(), 5U + 7U + 4U);
        }
    }
    for (const auto path : {"/forbidden", "/missing-subprotocol", "/cold"}) {
        observed.scenario_ = path;
        bool rejected = false;
        try {
            auto websocket = co_await client.client().websocket(path).connect();
        } catch (const ruvia::http_client_error& error) {
            rejected = true;
            EXPECT_EQ(error.code(), ruvia::make_error_code(ruvia::http_client_errc::handshake_rejected));
            EXPECT_EQ(error.status(), path == std::string_view("/missing-subprotocol") ? ruvia::http_status::bad_request : ruvia::http_status::forbidden);
        }
        EXPECT_TRUE(rejected);
    }
    EXPECT_TRUE(observed.expired_accept_.has_value());
    observed.scenario_ = "expired accept";
    if (observed.expired_accept_) {
        bool expired = false;
        try {
            (void)co_await std::move(*observed.expired_accept_);
        } catch (const std::logic_error&) {
            expired = true;
        }
        observed.expired_accept_.reset();
        EXPECT_TRUE(expired);
    }
    observed.scenario_ = "/invalid-version";
    if (destination.version_ == ruvia::http_protocol_version::http11) {
        wire_peer invalid(io);
        co_await invalid.open(client, destination, "/invalid-version", false, true);
    } else {
        auto rejection = std::async(std::launch::async, [&] {
            reject_invalid_extended_connect(destination, ca_file);
        });
        while (rejection.wait_for(0s) != std::future_status::ready) {
            (void)co_await ruvia::sleep_for(server.worker(), 1ms);
        }
        rejection.get();
    }
    co_await await_retirement(server, observed);
    {
        // The callback reads the refusal the server sends: RFC 6455 section
        // 4.4 answers an unsupported version with 400.
        const auto found = observed.rejections_.find("/invalid-version");
        EXPECT_TRUE(found != observed.rejections_.end());
        if (found != observed.rejections_.end()) {
            EXPECT_EQ(found->second.status_, ruvia::http_status::bad_request);
            EXPECT_EQ(found->second.code_, "unsupported_websocket_version");
        }
        const auto subprotocol = observed.rejections_.find("/missing-subprotocol");
        EXPECT_TRUE(subprotocol != observed.rejections_.end());
        if (subprotocol != observed.rejections_.end()) {
            EXPECT_EQ(subprotocol->second.status_, ruvia::http_status::bad_request);
            EXPECT_EQ(subprotocol->second.code_, "unsupported_websocket_subprotocol");
        }
        observed.rejections_.clear();
    }
    {
        observed.scenario_ = "/wire";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/wire", true);
        auto ping = co_await peer.read_frame();
        if (!ping) {
            throw std::runtime_error("missing server Ping");
        }
        EXPECT_EQ(ping->opcode_, 9U);
        EXPECT_EQ(ping->payload_, "probe");
        co_await peer.write(masked_frame(10, ping->payload_));
        auto compressed = co_await peer.read_frame();
        if (!compressed) {
            throw std::runtime_error("missing deflated frame");
        }
        EXPECT_EQ(compressed->opcode_, 1U);
        EXPECT_TRUE(compressed->compressed_);
        EXPECT_LT(compressed->payload_.size(), 2048U);
        co_await peer.write(masked_frame(9, "wire-control"));
        auto pong = co_await peer.read_frame();
        if (!pong) {
            throw std::runtime_error("missing automatic Pong");
        }
        EXPECT_EQ(pong->opcode_, 10U);
        EXPECT_FALSE(pong->compressed_);
        EXPECT_EQ(pong->payload_, "wire-control");
        co_await peer.write(masked_frame(8, std::string_view("\x03\xe8", 2)));
        auto close = co_await peer.read_frame();
        if (!close) {
            throw std::runtime_error("missing peer Close acknowledgement");
        }
        EXPECT_EQ(close->opcode_, 8U);
        EXPECT_EQ(close->payload_, std::string_view("\x03\xe8", 2));
    }
    co_await await_retirement(server, observed);
    EXPECT_TRUE(observed.expired_read_.has_value());
    observed.scenario_ = "expired read";
    if (observed.expired_read_) {
        bool expired = false;
        try {
            (void)co_await std::move(*observed.expired_read_);
        } catch (const std::logic_error&) {
            expired = true;
        }
        observed.expired_read_.reset();
        EXPECT_TRUE(expired);
    }
    {
        observed.scenario_ = "/fail";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/fail");
        const auto close = co_await peer.read_frame();
        if (!close) {
            throw std::runtime_error("missing failed-handler Close");
        }
        EXPECT_EQ(close->opcode_, 8U);
        EXPECT_EQ(close->payload_, std::string_view("\x03\xf3"
                                                    "internal server error",
                                       23));
        co_await peer.write(masked_frame(8, std::string_view("\x03\xf3", 2)));
    }
    co_await await_retirement(server, observed);
    {
        observed.scenario_ = "/cancel";
        std::optional<ruvia::websocket> websocket;
        websocket.emplace(co_await client.client().websocket("/cancel").subprotocol("standalone.v1").connect());
        auto cold = websocket->read();
        bool ended = false;
        ruvia::task_scope reads(client.worker());
        reads.spawn(read_operation_to_end(websocket->read(), ended));
        websocket.reset();
        co_await reads.join();
        EXPECT_TRUE(ended);
        bool expired = false;
        try {
            (void)co_await std::move(cold);
        } catch (const std::logic_error&) {
            expired = true;
        }
        EXPECT_TRUE(expired);
        co_await await_retirement(server, observed);
    }
    for (const auto path : {"/heartbeat", "/close"}) {
        observed.scenario_ = path;
        wire_peer peer(io);
        co_await peer.open(client, destination, path);
        const auto start = std::chrono::steady_clock::now();
        auto frame = co_await peer.read_frame();
        if (!frame) {
            throw std::runtime_error("missing liveness control frame");
        }
        EXPECT_EQ(frame->opcode_, path == std::string_view("/heartbeat") ? 9U : 8U);
        bool retired = false;
        try {
            retired = !(co_await peer.read_frame()).has_value();
        } catch (const std::system_error&) {
            retired = true;
        }
        EXPECT_TRUE(retired);
        EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
        co_await await_retirement(server, observed);
        if (path == std::string_view("/heartbeat")) {
            EXPECT_TRUE(observed.session(path).termination_ == ruvia::websocket_termination::timeout);
        }
    }
    {
        observed.scenario_ = "/cancel";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/cancel");
        peer.abort();
    }
    co_await await_retirement(server, observed);
    {
        observed.scenario_ = "/abort";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/abort");
        co_await peer.write(masked_frame(1, "abort"));
        co_await await_retirement(server, observed);
        EXPECT_TRUE(observed.session("/abort").termination_ == ruvia::websocket_termination::aborted);
    }
    {
        observed.scenario_ = "/eof";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/eof");
        co_await peer.finish();
        co_await await_retirement(server, observed);
        EXPECT_TRUE(observed.session("/eof").termination_ == ruvia::websocket_termination::eof);
    }
    // An unmasked client frame fails as 1002, a non-UTF-8 text message as 1007.
    for (const auto path : {"/invalid-frame", "/invalid-utf8"}) {
        observed.scenario_ = path;
        const bool unmasked = path == std::string_view("/invalid-frame");
        wire_peer peer(io);
        co_await peer.open(client, destination, path);
        co_await peer.write(unmasked ? std::string("\x81\x01x", 3) : masked_frame(1, "\xff"));
        const auto close = co_await peer.read_frame();
        if (!close) {
            throw std::runtime_error("missing protocol failure Close");
        }
        EXPECT_EQ(close->opcode_, 8U);
        EXPECT_EQ(close->payload_, unmasked ? std::string("\x03\xea", 2) : std::string("\x03\xef", 2));
        co_await await_retirement(server, observed);
        const auto failed = observed.session(path);
        EXPECT_TRUE(failed.termination_ == (unmasked ? ruvia::websocket_termination::protocol_error : ruvia::websocket_termination::invalid_payload_data));
        EXPECT_EQ(failed.received_, unmasked ? 0U : 7U);
        EXPECT_EQ(failed.sent_, 4U);
    }
}
}  // namespace standalone_websocket_test

namespace standalone_websocket_test {
void exercise_protocol(ruvia::http_client_versions protocol) {
    asio::io_context io;
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &preparation});
    ruvia::test::tcp_port_reservation plain(io), secure(io);
    ruvia::test::udp_port_reservation udp(io);
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    observation observed;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& request) {
            return serve(request, observed);
        }},
            {.connection_scan_interval_ = 5ms});
        server.listen({.address_ = "127.0.0.1", .http_ = plain.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        ruvia::listen_config tls{.address_ = "127.0.0.1", .https_ = secure.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
        tls.http3_ = udp.endpoint().port();
#endif
        server.listen(std::move(tls));
        std::exception_ptr failure;
        try {
            co_await server.start();
            std::vector<target> destinations{{"http://127.0.0.1:" + std::to_string(plain.endpoint().port()),
                "127.0.0.1:" + std::to_string(plain.endpoint().port()), ruvia::http_client_versions{.http2_ = false},
                ruvia::http_protocol_version::http11, plain.endpoint()}};
#if defined(RUVIA_ENABLE_HTTP2)
            destinations.push_back({"https://127.0.0.1:" + std::to_string(secure.endpoint().port()),
                "127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http1_ = false},
                ruvia::http_protocol_version::http2, secure.endpoint()});
#endif
#if defined(RUVIA_ENABLE_HTTP3)
            destinations.push_back({"https://127.0.0.1:" + std::to_string(udp.endpoint().port()),
                "127.0.0.1:" + std::to_string(udp.endpoint().port()), ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true},
                ruvia::http_protocol_version::http3, {}});
#endif
            for (const auto& destination : destinations) {
                if (destination.versions_ != protocol) continue;
                ruvia::http_client_config config{.base_url_ = destination.origin_, .versions_ = destination.versions_};
                config.tls_.ca_file_ = identity.ca_file_;
                ruvia::http_client client(attachment.loop(), config);
                std::exception_ptr exchange_failure;
                try {
                    co_await exercise(client, destination, server, observed, io, identity.ca_file_);
                    observed.scenario_ = "/host";
                    wire_peer pending(io);
                    co_await pending.open(client, destination, "/host");
                    // Stopping this host must wake the accepted WebSocket read,
                    // retire its borrowed facade, and join every scanner write.
                    server.close();
                    co_await server.shutdown();
                    EXPECT_EQ(observed.entered_, observed.retired_);
                    EXPECT_EQ(server.stats().active_connections_, 0U);
                } catch (...) {
                    exchange_failure = std::current_exception();
                    try {
                        std::rethrow_exception(exchange_failure);
                    } catch (const std::exception& error) {
                        std::string detail = std::string(observed.scenario_) + ": " + error.what();
                        if (const auto* client_error = dynamic_cast<const ruvia::http_client_error*>(&error)) {
                            detail += " [client code=" + std::to_string(client_error->code().value());
                            if (const auto status = client_error->status()) {
                                detail += ", status=" + std::to_string(status->value());
                            }
                            detail += "]";
                        } else if (const auto* system_error = dynamic_cast<const std::system_error*>(&error)) {
                            detail += std::string(" [") + system_error->code().category().name() +
                                      ":" + std::to_string(system_error->code().value()) + "]";
                        }
                        detail += " [entered=" + std::to_string(observed.entered_) +
                                  ", retired=" + std::to_string(observed.retired_) + "]";
                        exchange_failure = std::make_exception_ptr(std::runtime_error(std::move(detail)));
                    } catch (...) {
                        exchange_failure = std::current_exception();
                    }
                }
                co_await client.shutdown();
                if (exchange_failure) std::rethrow_exception(exchange_failure);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await server.shutdown();
        EXPECT_EQ(observed.entered_, observed.retired_);
        EXPECT_GT(observed.refused_, 0U);
        EXPECT_GE(observed.exclusive_, 4U);
        EXPECT_GT(observed.cancelled_, 0U);
        attachment.stop();
        if (failure) std::rethrow_exception(failure);
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}
}  // namespace standalone_websocket_test

TEST(Ruvia, independent_http1_websocket_handshake_frames_liveness_and_retirement) {
    using namespace standalone_websocket_test;
    standalone_websocket_test::exercise_protocol(ruvia::http_client_versions{.http2_ = false});
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, independent_http2_websocket_handshake_frames_liveness_and_retirement) {
    using namespace standalone_websocket_test;
    standalone_websocket_test::exercise_protocol(ruvia::http_client_versions{.http1_ = false});
}
#endif

#if defined(RUVIA_ENABLE_HTTP3)
TEST(Ruvia, independent_http3_websocket_handshake_frames_liveness_and_retirement) {
    using namespace standalone_websocket_test;
    standalone_websocket_test::exercise_protocol(ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true});
}
#endif
