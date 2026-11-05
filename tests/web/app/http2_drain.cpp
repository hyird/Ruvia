#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
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

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/socket_base.hpp>
#include <asio/write.hpp>

#include "ruvia/core/timer.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "test_harness.h"

namespace http2_drain_test {
using namespace std::chrono_literals;
inline constexpr std::size_t response_bytes = 8 * 1024 * 1024;

struct observation final {
    std::binary_semaphore started_{0};
    std::atomic<bool> entered_{};
    std::atomic<bool> release_{};
    std::atomic<bool> cancelled_{};
    std::atomic<bool> forbidden_ran_{};
    std::atomic<int> uploads_{};
    std::atomic<std::size_t> uploaded_bytes_{};
};
observation& observed() {
    static observation value;
    return value;
}

class drain_routes final {
public:
    RUVIA_ROUTES_BEGIN(drain_routes)
    RUVIA_GET("/held", held);
    RUVIA_GET("/forbidden", forbidden);
    RUVIA_GET("/stalled", stalled);
    RUVIA_GET("/brief", brief);
    RUVIA_GET("/block", block);
    RUVIA_POST("/upload", upload);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> held(ruvia::context& context) {
        auto& state = observed();
        state.entered_ = true;
        const auto stop = context.stop_token();
        while (!state.release_ && !stop.stop_requested()) {
            (void)co_await ruvia::sleep_for(context.worker(), 1ms, stop);
        }
        state.cancelled_ = stop.stop_requested();
        if (state.cancelled_) {
            co_return context.text("cancelled");
        }
        std::pmr::string body(response_bytes, 'd', context.arena());
        co_return context.text(std::move(body));
    }
    ruvia::task<ruvia::http_response> forbidden(ruvia::context& context) {
        observed().forbidden_ran_ = true;
        co_return context.text("must-not-run");
    }
    ruvia::task<ruvia::http_response> brief(ruvia::context& context) {
        co_return context.text("brief");
    }
    // Holds the worker thread itself until the test releases it, so input
    // and lifecycle controls queue up behind it in a known order.
    ruvia::task<ruvia::http_response> block(ruvia::context& context) {
        auto& state = observed();
        state.entered_ = true;
        while (!state.release_) {
            std::this_thread::sleep_for(1ms);
        }
        co_return context.text("block-done");
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

class app_run final {
public:
    explicit app_run(ruvia::application& app)
        : app_(app),
          thread_([this] {
              try {
                  app_.run();
              } catch (...) {
                  failure_ = std::current_exception();
              }
          }) {}
    ~app_run() {
        if (thread_.joinable()) {
            observed().release_ = true;
            app_.stop();
            thread_.join();
        }
    }
    void join() {
        thread_.join();
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    ruvia::application& app_;
    std::exception_ptr failure_;
    std::thread thread_;
};

template <typename predicate_type>
void await_observation(predicate_type predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("HTTP/2 drain observation timed out");
        }
        std::this_thread::sleep_for(1ms);
    }
}

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream)) {
        throw std::logic_error("invalid HTTP/2 test frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}
std::string request(std::uint32_t stream, std::string_view path, std::string_view method = "GET") {
    std::pmr::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", method);
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", path);
    std::string wire;
    // END_HEADERS, and END_STREAM unless a request body follows.
    append_frame(wire, ruvia::http2_frame_type::headers, method == "GET" ? 0x5 : 0x4, stream, block);
    return wire;
}
std::string data_frame(std::uint32_t stream, std::string_view payload, bool end_stream) {
    std::string wire;
    append_frame(wire, ruvia::http2_frame_type::data, end_stream ? 0x1 : 0x0, stream, payload);
    return wire;
}

struct frame final {
    ruvia::http2_frame_header header_;
    std::string payload_;
};
// Nonblocking reads bound every wire assertion, including a missing GOAWAY.
class peer final {
public:
    explicit peer(asio::ip::tcp::socket& socket)
        : socket_(socket) {
        socket_.non_blocking(true);
    }
    void read_exact(char* bytes, std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (count != 0) {
            asio::error_code error;
            const auto received = socket_.read_some(asio::buffer(bytes, count), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("HTTP/2 drain frame timed out");
                }
                std::this_thread::sleep_for(1ms);
                continue;
            }
            if (error) {
                throw std::system_error(error);
            }
            bytes += received;
            count -= received;
        }
    }
    frame next() {
        std::array<char, ruvia::http2_frame_header_bytes> bytes{};
        read_exact(bytes.data(), bytes.size());
        const auto header = ruvia::parse_http2_frame_header(bytes);
        if (!header || header->length_ > 64 * 1024) {
            throw std::runtime_error("invalid HTTP/2 drain response frame");
        }
        frame value{*header, std::string(header->length_, '\0')};
        read_exact(value.payload_.data(), value.payload_.size());
        return value;
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
        throw std::runtime_error("truncated HTTP/2 drain frame");
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

void check_drain(ruvia::testing::test_context& ruvia_ctx, bool deadline_expires) {
    auto& state = observed();
    state.entered_ = false;
    state.release_ = false;
    state.cancelled_ = false;
    state.forbidden_ran_ = false;
    auto& app = ruvia::app();
    register_start_hook(app);
    asio::io_context io;
    asio::ip::tcp::acceptor reservation(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();
    app.server({.worker_count_ = 1, .drain_timeout_ = deadline_expires ? 200ms : 10s, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    app_run run(app);
    if (!state.started_.try_acquire_for(3s)) {
        throw std::runtime_error("HTTP/2 drain app did not start");
    }
    asio::ip::tcp::socket socket(io);
    socket.open(endpoint.protocol());
    socket.set_option(asio::socket_base::receive_buffer_size(64 * 1024));
    socket.connect(endpoint);
    std::string opening(ruvia::http2_client_preface);
    // Permit the full response at the HTTP/2 layer so the deliberately slow
    // receiver backpressures the socket writer, not the stream send window.
    append_frame(opening, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x04\x01\x00\x00\x00", 6));
    append_frame(opening, ruvia::http2_frame_type::window_update, 0, 0, std::string_view("\x00\xff\x00\x01", 4));
    opening.append(request(1, "/held"));
    asio::write(socket, asio::buffer(opening));
    await_observation([&state] { return state.entered_.load(); });
    app.stop();
    peer client(socket);
    // RFC 9113 section 6.8: GOAWAY(2^31-1) and a PING first; the final GOAWAY
    // with the last admitted stream follows the PING ACK.
    frame goaway;
    do {
        goaway = client.next();
    } while (goaway.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
    RUVIA_CHECK_EQ(uint32(goaway.payload_) & 0x7fffffffU, 0x7fffffffU);
    RUVIA_CHECK_EQ(uint32(std::string_view(goaway.payload_).substr(4)), 0U);
    frame ping;
    do {
        ping = client.next();
    } while (ping.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::ping) || (ping.header_.flags_ & 0x1) != 0);
    std::string ack;
    append_frame(ack, ruvia::http2_frame_type::ping, 0x1, 0, ping.payload_);
    asio::write(socket, asio::buffer(ack));
    do {
        goaway = client.next();
    } while (goaway.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
    RUVIA_CHECK_EQ(uint32(goaway.payload_) & 0x7fffffffU, 1U);
    RUVIA_CHECK_EQ(uint32(std::string_view(goaway.payload_).substr(4)), 0U);
    RUVIA_CHECK(!state.cancelled_);
    if (!deadline_expires) {
        const auto forbidden_request = request(3, "/forbidden");
        asio::write(socket, asio::buffer(forbidden_request));
        frame refused;
        do {
            refused = client.next();
        } while (refused.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream) || refused.header_.stream_id_ != 3);
        RUVIA_CHECK_EQ(uint32(refused.payload_), 7U);
        RUVIA_CHECK(!state.forbidden_ran_);
        RUVIA_CHECK(!state.cancelled_);
        state.release_ = true;
        // Give the response producer a head start without consuming its body:
        // the response exceeds normal TCP send/receive buffering.
        std::this_thread::sleep_for(100ms);
        std::string body;
        bool complete = false;
        while (!complete) {
            const auto received = client.next();
            if (received.header_.stream_id_ == 1) {
                if (received.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    body.append(received.payload_);
                }
                complete = (received.header_.flags_ & 0x1) != 0;
            }
        }
        RUVIA_CHECK_EQ(body.size(), response_bytes);
        RUVIA_CHECK(std::all_of(body.begin(), body.end(), [](char byte) { return byte == 'd'; }));
        RUVIA_CHECK(!state.cancelled_);
    }
    await_observation([&client] { return client.ended(); });
    run.join();
    RUVIA_CHECK_EQ(state.cancelled_.load(), deadline_expires);
    RUVIA_CHECK(!state.forbidden_ran_);
}

frame next_of_type(peer& client, ruvia::http2_frame_type type) {
    frame value;
    do {
        value = client.next();
    } while (value.header_.type_ != static_cast<std::uint8_t>(type) ||
             (type == ruvia::http2_frame_type::ping && (value.header_.flags_ & 0x1) != 0));
    return value;
}

// Serves `config` and opens an HTTP/2 connection (preface, SETTINGS carrying
// `settings_payload` unless it is absent, then `requests`) that never
// acknowledges the server's PING.
class silent_peer_run final {
public:
    silent_peer_run(ruvia::server_config config, std::string_view requests,
        std::optional<std::string_view> settings_payload = std::string_view{})
        : app_(ruvia::app()),
          endpoint_(reserve_endpoint(io_)),
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
    static asio::ip::tcp::endpoint reserve_endpoint(asio::io_context& io) {
        asio::ip::tcp::acceptor reservation(io, {asio::ip::make_address("127.0.0.1"), 0});
        const auto endpoint = reservation.local_endpoint();
        reservation.close();
        return endpoint;
    }
    static ruvia::application& configure(
        ruvia::application& app, ruvia::server_config config, const asio::ip::tcp::endpoint& endpoint) {
        register_start_hook(app);
        app.server(std::move(config));
        app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
        return app;
    }

    asio::io_context io_;
    ruvia::application& app_;
    asio::ip::tcp::endpoint endpoint_;
    app_run run_;
    asio::ip::tcp::socket socket_;
};

// The GOAWAY(2^31-1) and drain PING a drain starts with.
void expect_round_trip_start(ruvia::testing::test_context& ruvia_ctx, peer& client) {
    const auto first = next_of_type(client, ruvia::http2_frame_type::goaway);
    RUVIA_CHECK_EQ(uint32(first.payload_) & 0x7fffffffU, 0x7fffffffU);
    (void)next_of_type(client, ruvia::http2_frame_type::ping);
}

// The final GOAWAY(NO_ERROR) with `last_stream_id`, then the server's end of
// the connection.
void expect_final_goaway_and_end(
    ruvia::testing::test_context& ruvia_ctx, peer& client, std::uint32_t last_stream_id) {
    const auto last = next_of_type(client, ruvia::http2_frame_type::goaway);
    RUVIA_CHECK_EQ(uint32(last.payload_) & 0x7fffffffU, last_stream_id);
    RUVIA_CHECK_EQ(uint32(std::string_view(last.payload_).substr(4)), 0U);
    await_observation([&client] { return client.ended(); });
}

void check_idle_timeout_drain(ruvia::testing::test_context& ruvia_ctx, bool stalled_stream) {
    auto& state = observed();
    state.entered_ = false;
    state.release_ = false;
    state.cancelled_ = false;
    silent_peer_run run(
        {.worker_count_ = 1, .idle_timeout_ = 300ms, .connection_scan_interval_ = 5ms},
        stalled_stream ? request(1, "/stalled") : std::string());
    peer client(run.socket());
    expect_round_trip_start(ruvia_ctx, client);
    // Without the ACK the drain still ends within its round-trip bound
    // (min(1 s, drain_timeout_ / 2)): a stalled stream is cancelled, and the
    // final GOAWAY precedes the close.
    expect_final_goaway_and_end(ruvia_ctx, client, stalled_stream ? 1U : 0U);
    // Ends the server's lingering close at once.
    run.socket().close();
    if (stalled_stream) {
        await_observation([&state] { return state.cancelled_.load(); });
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
    std::string wire;
    append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, opaque);
    asio::write(socket, asio::buffer(wire));
    std::vector<frame> frames;
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
    std::vector<frame> seen;
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
void expect_stream_end(ruvia::testing::test_context& ruvia_ctx, peer& client, std::uint32_t stream_id) {
    for (;;) {
        const auto value = client.next();
        RUVIA_CHECK(!is_frame(value, ruvia::http2_frame_type::goaway, 0));
        RUVIA_CHECK(!is_frame(value, ruvia::http2_frame_type::rst_stream, stream_id));
        const bool ending = is_frame(value, ruvia::http2_frame_type::headers, stream_id) ||
                            is_frame(value, ruvia::http2_frame_type::data, stream_id);
        if (ending && (value.header_.flags_ & 0x1) != 0) {
            return;
        }
    }
}

// The RST_STREAM error code the server sent for `stream_id`; also checks
// that no GOAWAY ended the connection meanwhile.
std::uint32_t reset_code(ruvia::testing::test_context& ruvia_ctx, const std::vector<frame>& frames,
    std::uint32_t stream_id) {
    std::uint32_t code = 0xffffffffU;
    for (const auto& value : frames) {
        RUVIA_CHECK(!is_frame(value, ruvia::http2_frame_type::goaway, 0));
        if (is_frame(value, ruvia::http2_frame_type::rst_stream, stream_id)) {
            code = uint32(value.payload_);
        }
    }
    return code;
}
}  // namespace http2_drain_test

RUVIA_TEST(http2_app_stop_sends_goaway_refuses_new_streams_and_completes_admitted_request) {
    http2_drain_test::check_drain(ruvia_ctx, false);
}
RUVIA_TEST(http2_app_stop_cancels_admitted_request_at_drain_deadline) {
    http2_drain_test::check_drain(ruvia_ctx, true);
}

// RFC 9113 section 6.8 requires only "at least one round-trip time" before the
// final GOAWAY: a peer that never acknowledges the drain PING neither withholds
// the final GOAWAY nor holds stop until drain_timeout_.
RUVIA_TEST(http2_app_stop_sends_final_goaway_without_ping_ack) {
    using namespace http2_drain_test;
    silent_peer_run run({.worker_count_ = 1, .drain_timeout_ = 6s, .connection_scan_interval_ = 5ms}, {});
    peer client(run.socket());
    // The server's SETTINGS shows the session runs before stop begins.
    (void)next_of_type(client, ruvia::http2_frame_type::settings);
    const auto stop_started = std::chrono::steady_clock::now();
    run.app().stop();
    expect_round_trip_start(ruvia_ctx, client);
    expect_final_goaway_and_end(ruvia_ctx, client, 0);
    // Ends the server's lingering close at once.
    run.socket().close();
    run.join();
    RUVIA_CHECK(std::chrono::steady_clock::now() - stop_started < 3s);
}

// An idle-timeout drain is bounded like a stop's: the final GOAWAY and an
// orderly end follow within the round-trip bound, not a second idle timeout
// that closes the socket without them.
RUVIA_TEST(http2_idle_timeout_drain_ends_with_final_goaway) {
    http2_drain_test::check_idle_timeout_drain(ruvia_ctx, false);
}
RUVIA_TEST(http2_idle_timeout_drain_cancels_stalled_stream_after_final_goaway) {
    http2_drain_test::check_idle_timeout_drain(ruvia_ctx, true);
}

// RFC 9113 section 6.8: "Endpoints SHOULD always send a GOAWAY frame before
// closing a connection" -- also when the peer's end of input ends the session.
RUVIA_TEST(http2_peer_end_of_input_gets_final_goaway_before_close) {
    using namespace http2_drain_test;
    silent_peer_run run({.worker_count_ = 1, .connection_scan_interval_ = 5ms}, {});
    peer client(run.socket());
    (void)next_of_type(client, ruvia::http2_frame_type::settings);
    run.socket().shutdown(asio::ip::tcp::socket::shutdown_send);
    expect_final_goaway_and_end(ruvia_ctx, client, 0);
    run.socket().close();
    run.app().stop();
    run.join();
}

// Pingora's h2_idle_timeout: idleness runs from the end of the last stream,
// and PINGs (with the server's ACK writes) open no stream, so they do not
// renew it.
RUVIA_TEST(http2_idle_timeout_runs_from_last_stream_end_despite_pings) {
    using namespace http2_drain_test;
    silent_peer_run run(
        {.worker_count_ = 1, .idle_timeout_ = 300ms, .connection_scan_interval_ = 5ms}, request(1, "/brief"));
    peer client(run.socket());
    expect_stream_end(ruvia_ctx, client, 1);
    const auto frames = keep_busy_until(client, run.socket(), {}, [](const frame& value) {
        return is_frame(value, ruvia::http2_frame_type::goaway, 0);
    });
    RUVIA_CHECK_EQ(uint32(frames.back().payload_) & 0x7fffffffU, 0x7fffffffU);
    expect_final_goaway_and_end(ruvia_ctx, client, 1);
    run.socket().close();
    run.app().stop();
    run.join();
}

// request_body_timeout_ times each stream's body: a stalled body is reset
// even while another stream's DATA keeps the connection busy, and only that
// stream ends.
RUVIA_TEST(http2_request_body_timeout_resets_only_the_stalled_stream) {
    using namespace http2_drain_test;
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
    RUVIA_CHECK_EQ(reset_code(ruvia_ctx, frames, 1), 8U);
    RUVIA_CHECK_EQ(reset_code(ruvia_ctx, frames, 3), 0xffffffffU);
    const auto finish = data_frame(3, {}, true);
    asio::write(run.socket(), asio::buffer(finish));
    expect_stream_end(ruvia_ctx, client, 3);
    RUVIA_CHECK_EQ(observed().uploads_.load(), 1);
    RUVIA_CHECK(observed().uploaded_bytes_.load() != 0);
    run.socket().close();
    run.app().stop();
    run.join();
}

// write_timeout_ bounds a stream's wait for peer flow-control window, as
// Pingora's h2 write timeout does: a peer that never opens the window gets
// that stream reset, while the connection keeps serving.
RUVIA_TEST(http2_send_window_wait_is_bounded_by_write_timeout) {
    using namespace http2_drain_test;
    // SETTINGS_INITIAL_WINDOW_SIZE = 0.
    silent_peer_run run({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .write_timeout_ = 300ms},
        request(1, "/brief"), std::string_view("\x00\x04\x00\x00\x00\x00", 6));
    peer client(run.socket());
    const auto frames = keep_busy_until(client, run.socket(), {}, [](const frame& value) {
        return is_frame(value, ruvia::http2_frame_type::rst_stream, 1);
    });
    RUVIA_CHECK_EQ(reset_code(ruvia_ctx, frames, 1), 8U);
    std::string reopened;
    append_frame(reopened, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x04\x00\x00\xff\xff", 6));
    reopened.append(request(3, "/brief"));
    asio::write(run.socket(), asio::buffer(reopened));
    expect_stream_end(ruvia_ctx, client, 3);
    run.socket().close();
    run.app().stop();
    run.join();
}

// Until the client preface and its SETTINGS arrive the connection is not
// ready: the request-header deadlines bound it, not idle_timeout_.
RUVIA_TEST(http2_readiness_is_bounded_by_header_completion_timeout) {
    using namespace http2_drain_test;
    silent_peer_run run({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .header_completion_timeout_ = 300ms},
        {}, std::nullopt);
    peer client(run.socket());
    expect_round_trip_start(ruvia_ctx, client);
    expect_final_goaway_and_end(ruvia_ctx, client, 0);
    run.socket().close();
    run.app().stop();
    run.join();
}

// An HTTP/2 session that starts once the drain began -- its cleartext
// preface had arrived when the drain cancelled the connection's read -- still
// gets the drain's GOAWAY, and the request it already sent is served. The
// worker is held in an HTTP/1 handler while the preface arrives and stop()
// posts the drain, so the drain runs before the read is performed.
RUVIA_TEST(http2_session_started_during_drain_gets_goaway) {
    using namespace http2_drain_test;
    auto& state = observed();
    state.entered_ = false;
    state.release_ = false;
    auto& app = ruvia::app();
    register_start_hook(app);
    asio::io_context io;
    asio::ip::tcp::acceptor reservation(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();
    app.server({.worker_count_ = 1, .drain_timeout_ = 30s, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    app_run run(app);
    if (!state.started_.try_acquire_for(3s)) {
        throw std::runtime_error("HTTP/2 drain app did not start");
    }
    asio::ip::tcp::socket session(io);
    session.connect(endpoint);
    await_observation([&app] { return app.http_stats().active_connections_ == 1; });
    asio::ip::tcp::socket blocker(io);
    blocker.connect(endpoint);
    asio::write(blocker, asio::buffer(std::string_view("GET /block HTTP/1.1\r\nHost: localhost\r\n\r\n")));
    await_observation([&state] { return state.entered_.load(); });

    std::string opening(ruvia::http2_client_preface);
    append_frame(opening, ruvia::http2_frame_type::settings, 0, 0, {});
    opening.append(request(1, "/brief"));
    asio::write(session, asio::buffer(opening));
    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    // Lets the lifecycle thread post the drain behind the held handler.
    std::this_thread::sleep_for(100ms);
    state.release_ = true;

    peer client(session);
    bool drain_goaway = false;
    bool final_goaway = false;
    bool body_ended = false;
    std::string body;
    while (!final_goaway || !body_ended) {
        const auto received = client.next();
        if (received.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway)) {
            const auto last_stream = uint32(received.payload_) & 0x7fffffffU;
            if (last_stream == 0x7fffffffU) {
                drain_goaway = true;
            } else {
                RUVIA_CHECK_EQ(last_stream, 1U);
                final_goaway = true;
            }
        } else if (received.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
                   received.header_.stream_id_ == 1) {
            body.append(received.payload_);
            body_ended = (received.header_.flags_ & 0x1) != 0;
        }
    }
    RUVIA_CHECK(drain_goaway);
    RUVIA_CHECK_EQ(body, std::string("brief"));
    // Ends the server's lingering closes at once.
    session.close();
    blocker.close();
    run.join();
    RUVIA_CHECK(std::chrono::steady_clock::now() - stop_started < 10s);
}
