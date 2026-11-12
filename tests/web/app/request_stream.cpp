#include "ruvia/web/request_stream.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
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
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/middleware.h"

#include "app_run.h"
#include "raw_http_peer.h"
#include "tcp_port_reservation.h"
#include "test_io_context.h"
#include "test_tls_identity.h"
#include "udp_port_reservation.h"

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
    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next_value) {
        ++middleware_runs();
        context.draft().header("x-middleware", "ran");
        co_await next_value();
    }
};

class ping_controller final {
public:
    RUVIA_ROUTES_BEGIN(ping_controller)
    RUVIA_GET("/ping", ping);
    RUVIA_ROUTES_END

    // Large enough for the application's compression policy.
    ruvia::task<ruvia::http_response> ping(ruvia::context& context) {
        std::string body;
        for (int index = 0; index < 512; ++index) {
            body.append("pong");
        }
        co_return context.text(body);
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

// Echoes the tunnel until the peer ends its direction, then ends its own.
ruvia::task<void> echo_tunnel(ruvia::http_tunnel& tunnel) {
    while (auto chunk = co_await tunnel.read()) {
        co_await tunnel.write(std::string_view(*chunk));
    }
    co_await tunnel.finish();
}

// One callback serves every protocol: it reads the whole request body, then
// streams it back with the byte count as a trailer; CONNECT and HTTP/1.1
// Upgrade become echo tunnels, except a CONNECT to refused_authority. A few
// paths fail before or after the response is committed.
ruvia::task<void> serve(ruvia::request_stream& stream) {
    const auto& request = stream.request();
    if (stream.tunnel_requested()) {
        if (request.target() == refused_authority) {
            stream.draft().status(ruvia::http_status::forbidden);
            stream.draft().header("x-denied", "yes");
            EXPECT_THROW((void)stream.body(), std::logic_error);
            (void)co_await stream.response().commit();
            co_await stream.response().write(std::string_view("denied"));
            co_await stream.response().end();
            co_return;
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
    stream.draft().header("x-connection-id", std::to_string(stream.conn().id()));
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

ruvia::task<void> exercise(ruvia::http_client_handle client, ruvia::http_protocol_version version, std::string label) {
    const auto payload = large_payload();
    {
        auto response = co_await client.post("/echo")
                            .header("origin", "https://site.test")
                            .header("accept-encoding", "gzip")
                            .body(payload)
                            .send();
        EXPECT_EQ(response.status().value(), 200) << label;
        EXPECT_TRUE(response.protocol_version() == version) << label;
        EXPECT_FALSE(response.header("x-middleware").has_value()) << label;
        EXPECT_FALSE(response.header("access-control-allow-origin").has_value()) << label;
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
    {
        auto refused = co_await client.connect(refused_authority).open();
        auto* response = refused.response();
        EXPECT_TRUE(response != nullptr) << label;
        if (response != nullptr) {
            EXPECT_EQ(response->status().value(), 403) << label;
            EXPECT_EQ(response->header("x-denied").value_or(""), "yes") << label;
            const auto body = co_await response->text();
            EXPECT_EQ(body.text(), "denied") << label;
        }
    }
}

struct client_target final {
    std::string base_url_;
    ruvia::http_client_protocol protocol_;
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
            config.protocol_ = target.protocol_;
            config.connect_timeout_ = 5s;
            config.request_timeout_ = 20s;
            config.tls_.ca_file_ = ca_file;
            owner.emplace(loop, config);
            co_await exercise(owner->client(), target.version_, target.label_);
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

using namespace request_stream_test;

// A request stream listener needs on_request_stream at run(); ALPN lists stay
// reserved for connection listeners.
TEST(Ruvia, request_stream_configuration_is_validated) {
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
// reported. An HTTP/1.1 Upgrade becomes a tunnel after 101. The routes
// listener of the same application keeps its middleware, CORS and
// compression policy; none of them, nor on_error, touches a request stream.
TEST(Ruvia, request_stream_listeners_serve_every_protocol_beside_routes) {
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
#endif
    const ruvia::listen_config routes_listener{.address_ = "127.0.0.1", .http_ = routes_endpoint.port()};

    {
        std::lock_guard lock(failures_mutex());
        failures().clear();
    }
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({stream_listener, routes_listener})
        .use<marking_middleware>()
        .cors(ruvia::cors_config{})
        .compression(ruvia::compression_config{})
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
        {plain_url, ruvia::http_client_protocol::http1_only, ruvia::http_protocol_version::http11, "HTTP/1.1"},
        {tls_url, ruvia::http_client_protocol::http1_only, ruvia::http_protocol_version::http11, "HTTP/1.1 over TLS"},
    };
#if defined(RUVIA_ENABLE_HTTP2)
    targets.push_back(
        {plain_url, ruvia::http_client_protocol::http2_only, ruvia::http_protocol_version::http2, "h2c"});
    targets.push_back({tls_url, ruvia::http_client_protocol::http2_only, ruvia::http_protocol_version::http2, "h2"});
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    targets.push_back({"https://localhost:" + std::to_string(http3_port), ruvia::http_client_protocol::http3_only,
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
        root.get();
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
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
            return failures_with(failure_message) + failures_with(silent_message) + failures_with(abort_message) ==
                   3 * targets.size();
        },
        10s);
    app.stop();
    runner.join();
    app.on_connection_failure(nullptr);
    EXPECT_EQ(middleware_runs().load(), 1);
    EXPECT_EQ(error_handler_runs().load(), 0);
    EXPECT_EQ(failures_with(failure_message), targets.size());
    EXPECT_EQ(failures_with(silent_message), targets.size());
    EXPECT_EQ(failures_with(abort_message), targets.size());
    std::lock_guard lock(failures_mutex());
    EXPECT_EQ(failures().size(), 3 * targets.size());
}
