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
#include <system_error>
#include <thread>
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
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_server.h"
#include "ruvia/web/middleware.h"

#include "app_run.h"
#include "raw_http_peer.h"
#include "tcp_port_reservation.h"
#include "test_io_context.h"
#include "test_tls_identity.h"
#include "udp_port_reservation.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include "ruvia/http/http3_connection.h"

#include "http3_test_client.h"
#endif
#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"

#include "http2_wire_fixture.h"
#endif

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

std::atomic<bool> refusal_read{false};
std::atomic<bool> refusal_returned{false};
std::atomic<unsigned> proxy_chunks_written{0};
std::atomic<unsigned> reset_chunks_written{0};
std::atomic<unsigned> reset_finished{0};
constexpr unsigned proxy_chunk_count = 512;
constexpr std::string_view reset_failure = "streamed refusal cancelled by peer";

ruvia::task<void> await_refusal_reader(const ruvia::worker_handle& worker, ruvia::stop_token stop) {
    for (int attempt = 0; attempt != 5000 && !refusal_read.load() && !stop.stop_requested(); ++attempt) {
        (void)co_await ruvia::sleep_for(worker, 1ms, stop);
    }
    EXPECT_TRUE(refusal_read.load());
}

// A refusal handler sets refusal_returned only after its response ended on
// the wire, which the client can observe first. Each exchange waits for its
// own handler to return, so a late store cannot leak into the next exchange.
ruvia::task<void> await_refusal_return(const ruvia::worker_handle& worker, const std::string& label) {
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

ruvia::task<void> exercise(ruvia::http_client_handle client, ruvia::http_protocol_version version,
    std::string label, ruvia::worker_handle worker) {
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
            co_await exercise(owner->client(), target.version_, target.label_, loop.handle());
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
// reported. An HTTP/1.1 Upgrade becomes a tunnel after 101. The host's
// server_config CORS and compression policies cover both listeners; the routes
// listener's middleware and on_error never touch a request stream.
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
    stream_listener.http3_early_data_ = true;
#endif
    const ruvia::listen_config routes_listener{.address_ = "127.0.0.1", .http_ = routes_endpoint.port()};
    const auto registered_client_url = "http://127.0.0.1:" + std::to_string(routes_endpoint.port());

    {
        std::lock_guard lock(failures_mutex());
        failures().clear();
    }
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2, .compression_ = ruvia::compression_config{}, .cors_ = ruvia::cors_config{}})
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
        EXPECT_EQ(initial.request("GET", "/early").header("x-early-data").value_or(std::pmr::string{}), "no");
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
        EXPECT_EQ(response.header("x-early-data").value_or(std::pmr::string{}), "yes");
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
        std::pmr::string wire(ruvia::http2_client_preface, ruvia::process_resource());
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        std::uint32_t id = 1;
        for (const auto path : {"/state-none", "/state-zero", "/state-unknown"}) {
            std::pmr::string block(ruvia::process_resource());
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
        const auto worker = attachment.loop().handle();
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
        const auto stop_pending_read = [&](const ruvia::worker_handle& worker, auto read) -> ruvia::task<void> {
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
