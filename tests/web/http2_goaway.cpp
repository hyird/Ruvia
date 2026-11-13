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
#include "ruvia/http/http2_connection.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"

#include "http_server_fixture.h"

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
