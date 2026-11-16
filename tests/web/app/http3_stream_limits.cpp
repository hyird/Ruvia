#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "app.h"
#include "context.h"
#include "controller.h"
#include "http3_test_client.h"
#include "http_header.h"
#include "http_request_head_view.h"
#include "http_server_fixture.h"
#include "middleware.h"
#include "streaming.h"
#include "worker_timer.h"
// Per-stream and per-connection limits of the HTTP/3 server session, end to
// end over loopback UDP: inbound buffer budgets, receive-credit return to a
// slow consumer and to a refused CONNECT's optimistic bytes, the per-stream
// request-body and output-wait deadlines, and the idle-timeout drain. Each
// case re-runs the process application with its
// own server_config on one listener.

namespace http3_stream_limits_test {
using namespace std::chrono_literals;

constexpr auto client_timeout = 5s;
// The inbound budget of the budget cases, and a body four times as large.
constexpr std::size_t inbound_budget_bytes = 64 * 1024;
constexpr std::size_t oversized_body_bytes = 4 * inbound_budget_bytes;
// Tunnel datagrams that outgrow the inbound budget together while staying
// under the stream's own bound of 64 stored datagrams.
constexpr std::size_t datagram_payload_bytes = 1100;
constexpr std::size_t datagram_count = 60;
// The slow-consumer cases: a request stream's receive window (and streamed
// backlog bound), the connection's receive window (its inbound budget), and
// content three times the connection window.
constexpr std::size_t slow_stream_window_bytes = 64 * 1024;
constexpr std::size_t slow_connection_window_bytes = 1024 * 1024;
constexpr std::size_t slow_content_bytes = 3 * slow_connection_window_bytes + 7;
// Larger than the test client's 1 MiB stream receive window plus the server's
// 256 KiB per-stream output budget.
constexpr std::size_t large_response_bytes = 2 * 1024 * 1024;

struct observation final {
    std::atomic<int> buffered_bodies_{};
    std::atomic<int> streamed_bodies_{};
    std::atomic<bool> release_held_{};
    std::atomic<std::size_t> held_view_bytes_{};
    std::atomic<bool> held_view_intact_{};
    std::atomic<std::size_t> datagrams_{};
    std::atomic<std::size_t> datagram_bytes_{};
    std::atomic<int> uploads_{};
    std::atomic<std::size_t> uploaded_bytes_{};

    void reset() noexcept {
        buffered_bodies_ = 0;
        streamed_bodies_ = 0;
        release_held_ = false;
        held_view_bytes_ = 0;
        held_view_intact_ = false;
        datagrams_ = 0;
        datagram_bytes_ = 0;
        uploads_ = 0;
        uploaded_bytes_ = 0;
    }
};
observation& observed() {
    static observation value;
    return value;
}

std::span<const std::byte> byte_view(std::string_view value) {
    return std::as_bytes(std::span(value.data(), value.size()));
}

// Lets the peer fill the receive windows and fall silent before the handler
// consumes anything.
ruvia::task<void> hold_off(ruvia::context& context) {
    if (co_await ruvia::sleep_for(context.worker(), 200ms) != ruvia::timer_sleep_result::elapsed) {
        throw std::runtime_error("test hold-off sleep interrupted");
    }
}

// Parks a handler until the test has seen the server take in what it sent,
// so the content waits against the inbound budget whatever the scheduling.
ruvia::task<void> await_release(ruvia::context& context) {
    const auto deadline = std::chrono::steady_clock::now() + client_timeout;
    while (!observed().release_held_.load()) {
        if (std::chrono::steady_clock::now() >= deadline || co_await ruvia::sleep_for(context.worker(), 1ms, context.stop_token()) != ruvia::timer_sleep_result::elapsed) {
            throw std::runtime_error("held handler was not released");
        }
    }
}

// The CONNECT authority no route accepts: its refusal is answered by the
// unmatched-request chain, which refused_connect_hold parks.
constexpr std::string_view refused_connect_authority = "refused-tunnel.test:443";

// Holds the refusal of refused_connect_authority until the test released it,
// so the optimistic tunnel bytes reach a stream whose response is pending.
// Every other request passes straight through.
struct refused_connect_hold final {
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> invoke(ruvia::context& context, ruvia::next& next) {
        if (context.req().known_method() == ruvia::http_known_method::connect && context.req().authority() == refused_connect_authority) {
            co_await await_release(context);
        }
        co_await next();
    }
};

class stream_limit_routes final {
public:
    RUVIA_ROUTES_BEGIN(stream_limit_routes)
    RUVIA_POST("/inbound-buffered", inbound_buffered);
    RUVIA_POST("/inbound-stream", inbound_stream, ruvia::stream_body);
    RUVIA_POST("/inbound-held-view", inbound_held_view, ruvia::stream_body);
    RUVIA_CONNECT("held-tunnel.test:443", held_tunnel);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/held.test/53/", held_udp);
    RUVIA_POST("/slow-upload", slow_upload, ruvia::stream_body);
    RUVIA_CONNECT("slow-tunnel.test:443", slow_tunnel);
    RUVIA_POST("/upload", upload);
    RUVIA_POST("/bounded-upload", upload, ruvia::body_limit<16>);
    RUVIA_GET("/brief", brief);
    RUVIA_GET("/large", large);
    RUVIA_ROUTES_END

    // Runs only with a complete buffered body.
    ruvia::task<ruvia::http_response> inbound_buffered(ruvia::context& context) {
        const auto body = co_await context.req().text();
        ++observed().buffered_bodies_;
        co_return context.text(std::string(std::to_string(body.size())));
    }
    // Reads nothing until the test has seen the whole upload acknowledged.
    ruvia::task<ruvia::http_response> inbound_stream(ruvia::context& context) {
        co_await await_release(context);
        auto& reader = context.request_body();
        std::size_t received = 0;
        while (const auto chunk = co_await reader.read()) {
            received += chunk->size();
        }
        // The body reached its end without a failure.
        ++observed().streamed_bodies_;
        co_return context.text(std::string(std::to_string(received)));
    }
    // Keeps the view of its first read while the rest of the body exhausts
    // the inbound budget, then reads again.
    ruvia::task<ruvia::http_response> inbound_held_view(ruvia::context& context) {
        auto& reader = context.request_body();
        const auto first = co_await reader.read();
        if (!first || first->empty()) {
            throw std::runtime_error("held view request carried no content");
        }
        const std::string_view view(reinterpret_cast<const char*>(first->data()), first->size());
        const std::string copy(view);
        observed().held_view_bytes_ = view.size();
        co_await await_release(context);
        observed().held_view_intact_ = view == copy;
        // The body failed meanwhile: this read reports the exhausted budget.
        (void)co_await reader.read();
        co_return context.text("unreachable");
    }
    // Established and never read: tunnel bytes wait against the inbound
    // budget until the stream is cancelled.
    ruvia::task<void> held_tunnel(ruvia::context& context) {
        (void)context.tunnel();
        const auto deadline = std::chrono::steady_clock::now() + client_timeout;
        while (std::chrono::steady_clock::now() < deadline && co_await ruvia::sleep_for(context.worker(), 1ms, context.stop_token()) == ruvia::timer_sleep_result::elapsed) {
        }
    }
    // Reads its UDP packets only once the test has sent them all.
    ruvia::task<void> held_udp(ruvia::context& context) {
        auto tunnel = context.tunnel().udp();
        co_await await_release(context);
        std::size_t packets = 0;
        std::size_t bytes = 0;
        while (const auto packet = co_await tunnel.read()) {
            ++packets;
            bytes += packet->text().size();
        }
        observed().datagram_bytes_ = bytes;
        observed().datagrams_ = packets;
        co_await tunnel.finish();
    }
    ruvia::task<ruvia::http_response> slow_upload(ruvia::context& context) {
        co_await hold_off(context);
        auto& reader = context.request_body();
        std::size_t received = 0;
        while (const auto chunk = co_await reader.read()) {
            received += chunk->size();
        }
        co_return context.text(std::string(std::to_string(received)));
    }
    ruvia::task<void> slow_tunnel(ruvia::context& context) {
        // Established first: the peer sends tunnel bytes only after the 2xx.
        auto& tunnel = context.tunnel();
        co_await hold_off(context);
        std::size_t received = 0;
        while (const auto chunk = co_await tunnel.read()) {
            received += chunk->size();
        }
        co_await tunnel.write(std::string(std::to_string(received)));
        co_await tunnel.finish();
    }
    ruvia::task<ruvia::http_response> upload(ruvia::context& context) {
        const auto body = co_await context.req().text();
        observed().uploaded_bytes_ = body.size();
        ++observed().uploads_;
        co_return context.text("uploaded");
    }
    ruvia::task<ruvia::http_response> brief(ruvia::context& context) {
        co_return context.text("brief");
    }
    ruvia::task<ruvia::http_response> large(ruvia::context& context) {
        co_return context.text(std::string(large_response_bytes, 'x'));
    }
};

// The process application's one HTTPS + HTTP/3 listener and its start hook,
// both retained across runs.
class listener final {
public:
    [[nodiscard]] static listener& shared() {
        static listener instance;
        return instance;
    }

    listener(const listener&) = delete;
    listener& operator=(const listener&) = delete;

    [[nodiscard]] bool await_start() {
        return started_.try_acquire_for(10s);
    }
    [[nodiscard]] ruvia::test::http3_test_client connect() {
        return ruvia::test::http3_test_client(udp_.endpoint(), identity_.ca_file_, "localhost", client_timeout);
    }
    [[nodiscard]] ruvia::test::http3_raw_request_client connect_raw() {
        return ruvia::test::http3_raw_request_client(udp_.endpoint(), identity_.ca_file_, "localhost", client_timeout);
    }

private:
    listener() :
        tcp_(io_),
        udp_(io_),
        identity_("localhost") {
        ruvia::app()
            .use<refused_connect_hold>()
            .listen({
                .address_ = "127.0.0.1",
                .https_ = tcp_.endpoint().port(),
                .http3_ = udp_.endpoint().port(),
                .tls_ = {.identities_ = ruvia::tls_fixed_identities{.primary_ = {.certificate_chain_file_ = identity_.ca_file_, .private_key_file_ = identity_.private_key_file()}}},
            })
            .on_start([this] {
            started_.release();
        });
    }

    asio::io_context io_;
    ruvia::test::tcp_port_reservation tcp_;
    ruvia::test::udp_port_reservation udp_;
    ruvia::test::tls_identity identity_;
    std::binary_semaphore started_{0};
};

// One run of the application with `config` on a single worker, so the worker
// inbound budget belongs to the case's connections alone. Stopped by finish()
// or, when a case fails early, by its destructor, which first releases the
// handlers the case still holds: a failed assertion or a throwing client
// never leaves one parked.
class served_case final {
public:
    explicit served_case(ruvia::server_config config) :
        listener_(listener::shared()),
        run_(ruvia::app().workers({.worker_count_ = 1}).server(std::move(config))) {
        if (!listener_.await_start()) {
            throw std::runtime_error("HTTP/3 stream limit app did not start");
        }
    }

    [[nodiscard]] ruvia::test::http3_test_client connect() {
        if (const auto failure = run_.failure()) {
            std::rethrow_exception(failure);
        }
        return listener_.connect();
    }
    [[nodiscard]] ruvia::test::http3_raw_request_client connect_raw() {
        if (const auto failure = run_.failure()) {
            std::rethrow_exception(failure);
        }
        return listener_.connect_raw();
    }
    // Call after every client of the case is gone.
    void finish() {
        ruvia::app().request_stop();
        run_.join();
    }
    ~served_case() {
        observed().release_held_ = true;
    }
    served_case(const served_case&) = delete;
    served_case& operator=(const served_case&) = delete;

private:
    listener& listener_;
    ruvia::test::app_run run_;
};

// A buffered body four times the inbound budget: 503, and the handler never
// runs with part of it.
void expect_buffered_body_refused(ruvia::test::http3_test_client& client) {
    const std::string payload(oversized_body_bytes, 'h');
    const auto response = client.request("POST", "/inbound-buffered", {}, payload);
    EXPECT_EQ(response.status_, 503);
    EXPECT_NE(response.body_.find("inbound_buffer_limit"), std::string::npos);
    EXPECT_EQ(observed().buffered_bodies_.load(), 0);
}

// Drives `client` until a server-side observation holds. No packet announces
// it, so the wait polls in short steps.
template <typename predicate_type>
[[nodiscard]] bool drive_until(ruvia::test::http3_test_client& client, predicate_type done) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!done()) {
        if (client.closed() || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        (void)client.wait_until(done, 10ms);
    }
    return true;
}

}  // namespace http3_stream_limits_test

using namespace http3_stream_limits_test;

// AGENTS.md §4: request content past the connection's inbound buffer budget
// is answered with 503, as for HTTP/1 and HTTP/2. The budget is also the
// connection's QUIC receive window, so only a buffered body -- whose credit
// returns as it is stored -- can outgrow it.
TEST(Ruvia, http3_request_body_connection_inbound_budget_exhaustion_answers_with_503) {
    observed().reset();
    served_case server({.max_inbound_buffer_bytes_per_connection_ = inbound_budget_bytes});
    {
        auto client = server.connect();
        expect_buffered_body_refused(client);
        // The refusal ends the request, not the connection.
        EXPECT_EQ(client.request("GET", "/brief").status_, 200);
    }
    server.finish();
}

// The worker budget bounds buffered and streamed bodies alike. The streamed
// body's handler reads only after the server acknowledged every byte, so the
// unread content must outgrow the budget; its read then fails with the
// budget's std::bad_alloc (503) instead of delivering a shortened body.
TEST(Ruvia, http3_request_body_worker_inbound_budget_exhaustion_answers_with_503) {
    auto& state = observed();
    state.reset();
    served_case server({.max_inbound_buffer_bytes_per_worker_ = inbound_budget_bytes});
    {
        auto client = server.connect();
        expect_buffered_body_refused(client);

        const std::string payload(oversized_body_bytes, 's');
        const auto stream = client.start_request("POST", "/inbound-stream", {}, payload);
        const bool delivered = client.wait_until([&client, stream] {
            return client.unacknowledged_data_bytes(stream) == 0;
        }, client_timeout);
        state.release_held_ = true;
        EXPECT_TRUE(delivered);
        const auto response = client.finish_response(stream);
        EXPECT_EQ(response.status_, 503);
        EXPECT_NE(response.body_.find("inbound_buffer_limit"), std::string::npos);
        EXPECT_EQ(state.streamed_bodies_.load(), 0);

        EXPECT_EQ(client.request("GET", "/brief").status_, 200);
    }
    server.finish();
}

// The view of a streamed body read stays valid until the next read starts
// (body_reader::read). Exhausting the inbound budget meanwhile fails the body
// and frees the content nobody read, not the chunk the handler still holds.
TEST(Ruvia, http3_inbound_budget_exhaustion_keeps_the_last_body_view_valid) {
    auto& state = observed();
    state.reset();
    served_case server({.max_inbound_buffer_bytes_per_worker_ = inbound_budget_bytes});
    {
        auto client = server.connect();
        const auto stream = client.start_streaming_request("POST", "/inbound-held-view");
        const std::string first(1024, 'v');
        client.send_stream_data(stream, byte_view(first));
        EXPECT_TRUE(drive_until(client, [&state] {
            return state.held_view_bytes_.load() != 0;
        }));
        const std::string rest(oversized_body_bytes, 'r');
        client.send_stream_data(stream, byte_view(rest), true);
        const bool delivered = client.wait_until([&client, stream] {
            return client.unacknowledged_data_bytes(stream) == 0;
        }, client_timeout);
        state.release_held_ = true;
        EXPECT_TRUE(delivered);
        const auto response = client.finish_response(stream);
        EXPECT_EQ(response.status_, 503);
        EXPECT_NE(response.body_.find("inbound_buffer_limit"), std::string::npos);
        EXPECT_TRUE(state.held_view_intact_.load());
    }
    server.finish();
}

// Tunnel bytes have no response left to report a refusal: when a tunnel's
// unread content exhausts the inbound budget the stream is cancelled, as on
// HTTP/2, instead of losing bytes silently.
TEST(Ruvia, http3_tunnel_inbound_budget_exhaustion_cancels_the_stream) {
    observed().reset();
    served_case server({.max_inbound_buffer_bytes_per_worker_ = inbound_budget_bytes});
    {
        auto client = server.connect();
        const auto tunnel = client.start_tunnel(ruvia::http_request_head_view{.method_ = "CONNECT", .authority_ = "held-tunnel.test:443"});
        EXPECT_EQ(client.wait_for_response_head(tunnel).status_, 200);
        // Within the client's output budget: the write never waits on the
        // stream it is about to lose.
        const std::string payload(2 * inbound_budget_bytes, 't');
        client.send_stream_data(tunnel, byte_view(payload));
        const auto reply = client.finish_response(tunnel);
        EXPECT_EQ(reply.reset_error_, std::optional(ruvia::http3_error_code::request_cancelled));
        EXPECT_EQ(client.request("GET", "/brief").status_, 200);
    }
    server.finish();
}

// A tunnel's stored HTTP Datagrams are charged to the inbound budget like
// its bytes. One that does not fit is dropped (RFC 9221 permits loss), so a
// reader that starts late receives fewer than were sent, never more than the
// budget holds, and the tunnel itself is unaffected.
TEST(Ruvia, http3_tunnel_datagrams_count_against_the_inbound_budget) {
    auto& state = observed();
    state.reset();
    served_case server({.max_inbound_buffer_bytes_per_worker_ = inbound_budget_bytes});
    {
        auto client = server.connect();
        constexpr std::array udp_fields{ruvia::http_header_view{"capsule-protocol", "?1"}};
        const ruvia::http_request_head_view udp_head{.method_ = "CONNECT", .protocol_ = "connect-udp", .authority_ = "localhost", .target_ = "/.well-known/masque/udp/held.test/53/", .headers_ = udp_fields};
        const auto tunnel = client.start_tunnel(udp_head);
        EXPECT_EQ(client.wait_for_response_head(tunnel).status_, 200);
        // Context ID 0, then the UDP payload (RFC 9298 §5).
        const std::string packet = std::string(1, '\0') + std::string(datagram_payload_bytes, 'd');
        for (std::size_t sent = 0; sent != datagram_count; ++sent) {
            client.send_datagram(tunnel, byte_view(packet));
            // A round trip on another stream, whatever its answer: the
            // server has taken in the datagram sent before it.
            (void)client.request("GET", "/brief");
        }
        state.release_held_ = true;
        client.send_stream_data(tunnel, {}, true);
        const auto reply = client.finish_response(tunnel);
        EXPECT_FALSE(reply.reset_error_.has_value());
        const auto stored = state.datagrams_.load();
        EXPECT_GT(stored, std::size_t{0});
        EXPECT_LT(stored, datagram_count);
        EXPECT_LE(state.datagram_bytes_.load(), inbound_budget_bytes);
    }
    server.finish();
}

// Receive credit returns as the application consumes (RFC 9000 §4.1): content
// larger than the stream and connection receive windows completes although
// its consumer starts only after the windows filled and the peer fell silent,
// so no further peer input prompts the credit.
TEST(Ruvia, http3_slow_body_and_tunnel_consumers_return_receive_credit_without_peer_input) {
    observed().reset();
    served_case server({.max_buffered_body_bytes_ = slow_stream_window_bytes, .max_inbound_buffer_bytes_per_connection_ = slow_connection_window_bytes});
    {
        auto client = server.connect();
        const std::string payload(slow_content_bytes, 'w');
        const auto expected = std::string(std::to_string(payload.size()));

        const auto tunnel = client.start_tunnel(ruvia::http_request_head_view{.method_ = "CONNECT", .authority_ = "slow-tunnel.test:443"});
        EXPECT_EQ(client.wait_for_response_head(tunnel).status_, 200);
        client.send_stream_data(tunnel, byte_view(payload), true);
        const auto tunnel_reply = client.finish_response(tunnel);
        EXPECT_EQ(tunnel_reply.body_, expected);
        EXPECT_FALSE(tunnel_reply.reset_error_.has_value());

        const auto upload_reply = client.request("POST", "/slow-upload", {}, payload);
        EXPECT_EQ(upload_reply.status_, 200);
        EXPECT_EQ(upload_reply.body_, expected);
        EXPECT_FALSE(upload_reply.reset_error_.has_value());
    }
    server.finish();
}

// request_body_timeout_ times each stream's body: a stalled body is reset
// with H3_REQUEST_CANCELLED even while another stream's DATA keeps the
// connection busy, and only that stream ends.
TEST(Ruvia, http3_request_body_timeout_resets_only_the_stalled_stream) {
    auto& state = observed();
    state.reset();
    served_case server({.connection_scan_interval_ = 5ms, .request_body_timeout_ = 300ms});
    {
        auto client = server.connect();
        const auto stalled = client.start_streaming_request("POST", "/upload");
        client.send_stream_data(stalled, byte_view("a"));
        const auto busy = client.start_streaming_request("POST", "/upload");
        std::size_t busy_bytes = 0;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!client.request_closed(stalled)) {
            ASSERT_FALSE(client.closed());
            ASSERT_TRUE(std::chrono::steady_clock::now() < deadline);
            client.send_stream_data(busy, byte_view("b"));
            ++busy_bytes;
            (void)client.wait_until([&client, stalled] {
                return client.request_closed(stalled);
            }, 50ms);
        }
        const auto reset = client.finish_response(stalled);
        EXPECT_EQ(reset.status_, 0);
        EXPECT_EQ(reset.reset_error_, std::optional(ruvia::http3_error_code::request_cancelled));

        EXPECT_FALSE(client.request_closed(busy));
        client.send_stream_data(busy, {}, true);
        const auto served = client.finish_response(busy);
        EXPECT_EQ(served.status_, 200);
        EXPECT_EQ(served.body_, "uploaded");
        EXPECT_FALSE(served.reset_error_.has_value());
        EXPECT_EQ(state.uploads_.load(), 1);
        EXPECT_EQ(state.uploaded_bytes_.load(), busy_bytes);
    }
    server.finish();
}

// write_timeout_ bounds a stream's wait for output capacity: a peer that
// stops granting flow-control credit leaves the response unacknowledged, and
// that stream is reset with H3_REQUEST_CANCELLED while the connection keeps
// serving.
TEST(Ruvia, http3_output_wait_is_bounded_by_write_timeout) {
    observed().reset();
    served_case server({.connection_scan_interval_ = 5ms, .write_timeout_ = 300ms});
    {
        auto client = server.connect();
        // Unread response content holds its receive credit.
        const auto withheld = client.start_request("GET", "/large");
        EXPECT_TRUE(client.wait_until([&client, withheld] {
            return client.request_closed(withheld);
        }, 3s));
        const auto reset = client.finish_response(withheld);
        EXPECT_EQ(reset.status_, 200);
        EXPECT_EQ(reset.reset_error_, std::optional(ruvia::http3_error_code::request_cancelled));
        EXPECT_LT(reset.body_.size(), large_response_bytes);

        const auto sibling = client.request("GET", "/brief");
        EXPECT_EQ(sibling.status_, 200);
        EXPECT_EQ(sibling.body_, "brief");
    }
    server.finish();
}

// The 413 of a body past its route limit and the 417 of an unsupported
// expectation carry their own diagnostic text: each rejection owns its
// message across the error handler's suspension. The raw client sends the
// heads verbatim: http3_connection refuses to emit an unsupported Expect.
TEST(Ruvia, http3_request_rejections_answer_with_their_complete_error_text) {
    observed().reset();
    served_case server({});
    {
        auto client = server.connect_raw();
        client.start_request({{":method", "POST"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/bounded-upload"}}, std::string(64, 'b'));
        const auto oversized = client.finish_response();
        EXPECT_EQ(oversized.status_, 413);
        EXPECT_NE(oversized.body_.find(R"("detail":"request body is too large","code":"request_body_too_large")"), std::string::npos);
        client.start_request({{":method", "POST"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/upload"}, {"expect", "unknown-extension"}}, "four");
        const auto unsupported = client.finish_response();
        EXPECT_EQ(unsupported.status_, 417);
        EXPECT_NE(unsupported.body_.find(R"("detail":"unsupported Expect header","code":"unsupported_expectation")"), std::string::npos);
        EXPECT_EQ(observed().uploads_.load(), 0);
    }
    server.finish();
}

// A connection timeout drains the connection (RFC 9114 §5.2) instead of
// cutting it: the shutdown notice, the final GOAWAY within the round-trip
// bound, then CONNECTION_CLOSE(H3_NO_ERROR). Idleness runs from the end of
// the last stream: the client's QUIC PINGs open no stream and do not renew it.
TEST(Ruvia, http3_idle_timeout_drain_ends_with_final_goaway) {
    observed().reset();
    served_case server({.idle_timeout_ = 300ms, .connection_scan_interval_ = 5ms});
    {
        auto client = server.connect();
        client.keep_alive(50ms);
        const auto last = client.start_request("GET", "/brief");
        const auto response = client.finish_response(last);
        EXPECT_EQ(response.status_, 200);
        EXPECT_TRUE(client.goaway_ids().empty());

        EXPECT_TRUE(client.wait_until([&client] {
            return client.closed();
        }, 3s));
        const auto& goaways = client.goaway_ids();
        ASSERT_FALSE(goaways.empty());
        EXPECT_FALSE(goaways.front().has_value());
        EXPECT_EQ(goaways.size(), std::size_t{2});
        EXPECT_TRUE(goaways.back().has_value() && *goaways.back() > last);
        EXPECT_EQ(client.application_error(), std::optional(ruvia::http3_error_code::no_error));
        EXPECT_FALSE(client.transport_error().has_value());
    }
    server.finish();
}

// The client stays silent after its response and advertises a QUIC idle
// timeout (30 s) far above the server's HTTP one: the HTTP idle drain, not a
// silent QUIC idle close, must end the connection.
TEST(Ruvia, http3_idle_timeout_drain_precedes_quic_idle_close_for_silent_client) {
    observed().reset();
    served_case server({.idle_timeout_ = 300ms, .connection_scan_interval_ = 5ms});
    {
        auto client = server.connect();
        const auto last = client.start_request("GET", "/brief");
        const auto response = client.finish_response(last);
        EXPECT_EQ(response.status_, 200);

        EXPECT_TRUE(client.wait_until([&client] {
            return client.closed();
        }, 3s));
        const auto& goaways = client.goaway_ids();
        ASSERT_EQ(goaways.size(), std::size_t{2});
        EXPECT_FALSE(goaways.front().has_value());
        EXPECT_TRUE(goaways.back().has_value() && *goaways.back() > last);
        EXPECT_EQ(client.application_error(), std::optional(ruvia::http3_error_code::no_error));
        EXPECT_FALSE(client.transport_error().has_value());
    }
    server.finish();
}

// RFC 9114 §4.4: a client may send tunnel bytes before the CONNECT response.
// For a CONNECT no route accepts they have no reader: they are dropped and
// their credit returns, so more than the inbound budget passes while the
// refusal is pending, the refusal is still delivered instead of an
// H3_REQUEST_CANCELLED reset, and the connection serves on.
TEST(Ruvia, http3_refused_connect_drops_optimistic_tunnel_bytes_and_answers) {
    observed().reset();
    served_case server({.max_inbound_buffer_bytes_per_connection_ = inbound_budget_bytes});
    {
        auto client = server.connect_raw();
        client.start_request({{":method", "CONNECT"}, {":authority", refused_connect_authority}}, std::string(oversized_body_bytes, 't'));
        // Acknowledged past the connection receive window (the inbound
        // budget) only if the server returned the dropped bytes' credit.
        EXPECT_TRUE(client.wait_until([&client] {
            return client.acknowledged_bytes() >= oversized_body_bytes || client.response_head_received() || client.reset_error().has_value();
        }));
        EXPECT_GE(client.acknowledged_bytes(), oversized_body_bytes);
        EXPECT_FALSE(client.response_head_received());
        EXPECT_FALSE(client.reset_error().has_value());

        observed().release_held_ = true;
        const auto refusal = client.finish_response();
        EXPECT_GE(refusal.status_, 400);
        EXPECT_LT(refusal.status_, 500);
        EXPECT_NE(refusal.reset_error_, std::optional(ruvia::http3_error_code::request_cancelled));

        const auto next = client.request({{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/brief"}});
        EXPECT_EQ(next.status_, 200);
        EXPECT_EQ(next.body_, "brief");
    }
    server.finish();
}
