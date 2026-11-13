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

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "http_server_fixture.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include <asio/ip/udp.hpp>

#include "http3_test_client.h"
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

using namespace listener_sockets_test;

#if !defined(_WIN32)
// listen() owns the sockets it is handed: the next run() serves on them
// instead of binding -- here a port bound without SO_REUSEPORT, which the
// server could not have bound itself -- and later runs bind. A socket that
// does not serve its service, a socket given twice or one for a disabled
// service closes every socket of that call; a socket count unlike the worker
// count fails run(), which consumed and closed them.
TEST(Ruvia, listen_adopts_inherited_tcp_sockets_and_closes_rejected_ones) {
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
