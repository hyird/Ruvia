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
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"

#include "http_server_fixture.h"
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
