#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "ruvia/core/timer.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/websocket.h"

#include "tcp_port_reservation.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

namespace stream_disconnect_test {
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;
std::atomic<std::size_t> finished{}, cancelled{}, errors{}, context_cancelled{}, body_cancelled{}, body_complete{};
std::atomic<std::size_t> pipeline_cancelled{};
std::atomic<std::size_t> write_entered{};
// Upgraded writes to a peer that stops reading, and how many of them failed.
std::atomic<std::size_t> stalled_upgrade_writes{}, stalled_upgrade_failures{};
// More than loopback socket buffers absorb, so the write stays blocked.
constexpr std::size_t stalled_upgrade_bytes = std::size_t{16} << 20;
ruvia::stop_source explicit_stop;

struct completion final {
    ~completion() {
        finished.fetch_add(1);
    }
};

class routes final {
    RUVIA_ROUTES_BEGIN(routes)
    RUVIA_GET("/disconnect/events", events);
    RUVIA_GET("/disconnect/pipeline", pipeline);
    RUVIA_GET("/disconnect/pipeline-wait", pipeline_wait);
    RUVIA_GET("/disconnect/body", body);
    RUVIA_POST("/disconnect/echo", echo);
    RUVIA_POST("/disconnect/post-sse", post_sse);
    RUVIA_GET("/disconnect/health", health);
    RUVIA_GET("/disconnect/slow", slow);
    RUVIA_GET("/disconnect/h2-bulk", h2_bulk);
    RUVIA_GET_WS("/disconnect/websocket-stall", websocket_stall);
    RUVIA_CONNECT("stall.test:443", tunnel_stall);
    RUVIA_ROUTES_END

    ruvia::task<void> events(ruvia::context& c) {
        completion done;
        const auto mode = c.req().query("mode").value_or("token");
        const auto original = c.stop_token();
        auto writer = c.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        const ruvia::stop_group wait_stop(original, mode == "explicit" ? explicit_stop.token() : ruvia::stop_token{});
        const auto token = wait_stop.token();
        co_await writer.write({.data_ = "first"});
        if (mode == "write" || mode == "timeout") {
            // 大块写入让关闭、写完成和写超时竞争同一个连接生命周期。
            const std::string bulk(8 * 1024 * 1024, 'x');
            write_entered.fetch_add(1);
            co_await c.stream().write(bulk);
            co_return;
        }
        // 空闲等待：既无写入也无 writer.sleep()，只有读侧终止能及时唤醒。
        const auto result = mode == "writer" ? co_await writer.sleep(10s)
                                             : co_await ruvia::sleep_for(c.worker(), 10s, token);
        if (result == ruvia::timer_sleep_result::stop_requested) {
            cancelled.fetch_add(1);
        }
        if (original.stop_requested()) {
            context_cancelled.fetch_add(1);
        }
    }

    ruvia::task<void> pipeline(ruvia::context& c) {
        auto& writer = c.stream();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("pipeline-first");
        if (co_await ruvia::sleep_for(c.worker(), 100ms, c.stop_token()) ==
            ruvia::timer_sleep_result::stop_requested) {
            pipeline_cancelled.fetch_add(1);
        }
        co_await writer.write("pipeline-last");
    }

    // A streamed request pipelined behind another: an idle wait on its stop.
    ruvia::task<void> pipeline_wait(ruvia::context& c) {
        auto& writer = c.stream();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("wait-first");
        if (co_await ruvia::sleep_for(c.worker(), 150ms, c.stop_token()) ==
            ruvia::timer_sleep_result::stop_requested) {
            pipeline_cancelled.fetch_add(1);
        }
        co_await writer.write("wait-last");
    }

    // The request body completes only after the handler starts; detection
    // begins with the read that completes it, early commit or not.
    ruvia::task<void> body(ruvia::context& c) {
        auto& writer = c.stream();
        const bool early = c.req().query("early").has_value();
        if (early) {
            if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
                static_cast<void>(co_await c.req().text());
                co_return;
            }
            co_await writer.write("early");
        }
        const auto received = co_await c.req().text();
        body_complete.fetch_add(1);
        if (!early && co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write(received);
        if (c.req().query("wait").has_value() &&
            co_await ruvia::sleep_for(c.worker(), 10s, c.stop_token()) == ruvia::timer_sleep_result::stop_requested) {
            body_cancelled.fetch_add(1);
        }
    }

    ruvia::task<void> post_sse(ruvia::context& c) {
        const auto received = co_await c.req().text();
        auto writer = c.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write({.data_ = received});
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        const auto text = co_await c.req().text();
        co_return c.text(text);
    }

    ruvia::task<ruvia::http_response> health(ruvia::context& c) {
        co_return c.text("healthy");
    }

    // A buffered response that leaves only after 100ms, so a client can end
    // its request direction before the server writes anything.
    ruvia::task<ruvia::http_response> slow(ruvia::context& c) {
        (void)co_await ruvia::sleep_for(c.worker(), 100ms);
        co_return c.text("slow-done");
    }

    ruvia::task<ruvia::http_response> h2_bulk(ruvia::context& c) {
        co_return c.text(std::pmr::string(stalled_upgrade_bytes, 'b', c.allocator<char>()));
    }

    // The WebSocket route keeps its default lifecycle: no heartbeat.
    ruvia::task<void> websocket_stall(ruvia::context& c) {
        stalled_upgrade_writes.fetch_add(1);
        try {
            co_await c.websocket().binary(std::string(stalled_upgrade_bytes, 'w'));
        } catch (const std::system_error&) {
            stalled_upgrade_failures.fetch_add(1);
        }
    }

    ruvia::task<void> tunnel_stall(ruvia::context& c) {
        stalled_upgrade_writes.fetch_add(1);
        try {
            co_await c.tunnel().write(std::string(stalled_upgrade_bytes, 't'));
        } catch (const std::system_error&) {
            stalled_upgrade_failures.fetch_add(1);
        }
    }
};

template <typename predicate>
bool wait_until(predicate ready, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = clock_type::now() + timeout;
    while (!ready() && clock_type::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return ready();
}

#if defined(RUVIA_ENABLE_HTTP2)
void append_http2_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags, std::uint32_t stream,
    std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream)) {
        throw std::logic_error("invalid HTTP/2 test frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

void append_u32(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

// A prior-knowledge HTTP/2 connection that opens every flow-control window to
// its maximum, so the server's output is limited only by the socket, then
// sends one request on stream 1: a large buffered GET or a CONNECT tunnel.
std::string http2_request_without_reading(bool tunnel) {
    constexpr std::uint32_t max_window = 0x7fffffffU;
    std::string wire(ruvia::http2_client_preface);
    std::string settings{0x00, 0x04};
    append_u32(settings, max_window);
    append_http2_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
    std::string increment;
    append_u32(increment, max_window - 65535U);
    append_http2_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, increment);
    std::pmr::unsynchronized_pool_resource resource;
    std::pmr::string block(&resource);
    if (tunnel) {
        ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
        ruvia::hpack_encoder::encode_header(block, ":authority", "stall.test:443");
        append_http2_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    } else {
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(block, ":path", "/disconnect/h2-bulk");
        append_http2_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    }
    return wire;
}

#endif

// 异步客户端给每次 I/O 设置独立截止时间，回归失败不会挂住测试进程。
class peer final {
public:
    peer(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, bool tls)
        : io_(io),
          context_(asio::ssl::context::tls_client),
          stream_(io, context_),
          tls_(tls) {
        context_.set_verify_mode(asio::ssl::verify_none);
        bool connected = false;
        asio::error_code connect_error;
        stream_.next_layer().async_connect(endpoint, [&](const asio::error_code& ec) {
            connect_error = ec;
            connected = true;
        });
        if (!wait_for_completion(connected, 2s)) {
            throw std::runtime_error("disconnect test connect timed out");
        }
        if (connect_error) {
            throw std::system_error(connect_error);
        }
        if (tls_) {
            bool handshaken = false;
            asio::error_code handshake_error;
            stream_.async_handshake(asio::ssl::stream_base::client, [&](asio::error_code ec) {
                handshake_error = ec;
                handshaken = true;
            });
            if (!wait_for_completion(handshaken, 2s)) {
                throw std::runtime_error("disconnect test TLS handshake timed out");
            }
            if (handshake_error) {
                throw std::system_error(handshake_error);
            }
        }
    }
    void send(std::string_view bytes) {
        bool written = false;
        asio::error_code error;
        const auto complete = [&](asio::error_code ec, std::size_t) {
            error = ec;
            written = true;
        };
        if (tls_) {
            asio::async_write(stream_, asio::buffer(bytes), complete);
        } else {
            asio::async_write(stream_.next_layer(), asio::buffer(bytes), complete);
        }
        if (!wait_for_completion(written, 2s)) {
            throw std::runtime_error("disconnect test write timed out");
        }
        if (error) {
            throw std::system_error(error);
        }
    }
    std::string read_until(std::string_view marker, std::chrono::milliseconds timeout = 2s) {
        std::string output;
        std::array<char, 16384> bytes;
        const auto deadline = clock_type::now() + timeout;
        while (clock_type::now() < deadline) {
            asio::error_code ec;
            std::size_t n = 0;
            bool read = false;
            const auto complete = [&](asio::error_code error, std::size_t count) {
                ec = error;
                n = count;
                read = true;
            };
            if (tls_) {
                stream_.async_read_some(asio::buffer(bytes), complete);
            } else {
                stream_.next_layer().async_read_some(asio::buffer(bytes), complete);
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_type::now());
            const bool completed = wait_for_completion(read, std::max(remaining, 0ms));
            output.append(bytes.data(), n);
            if (!marker.empty() && output.find(marker) != std::string::npos) {
                return output;
            }
            if (!completed || ec) {
                return output;
            }
        }
        return output;
    }
    void half_close() {
        asio::error_code ignored;
        stream_.next_layer().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    }
    // TLS 1.3 half-close: close_notify ends only the sending direction. The
    // shutdown then waits for the server's close_notify; cancel that read so
    // the response stays readable. Callers send it while the server stays
    // quiet for far longer than the 20ms the close_notify write needs.
    void send_close_notify() {
        asio::cancellation_signal cancel;
        bool completed = false;
        asio::error_code error;
        stream_.async_shutdown(asio::bind_cancellation_slot(cancel.slot(), [&](asio::error_code ec) {
            error = ec;
            completed = true;
        }));
        io_.restart();
        io_.run_for(20ms);
        if (!completed) {
            cancel.emit(asio::cancellation_type::terminal);
            wait_for_completion(completed, 2s);
        }
        if ((SSL_get_shutdown(stream_.native_handle()) & SSL_SENT_SHUTDOWN) == 0) {
            throw std::runtime_error("TLS close_notify was not sent");
        }
        // The server may close first: answering with its close_notify, or,
        // having abandoned a lone request, without one.
        if (error && error != asio::error::operation_aborted && error != asio::error::eof &&
            error != asio::ssl::error::stream_truncated) {
            throw std::system_error(error);
        }
    }
    // A clean end of the request direction: TCP FIN, or TLS close_notify. A
    // bare TCP FIN under TLS is a truncation instead (half_close()).
    void end_requests() {
        if (tls_) {
            send_close_notify();
        } else {
            half_close();
        }
    }
    void request_key_update() {
        if (SSL_key_update(stream_.native_handle(), SSL_KEY_UPDATE_REQUESTED) != 1) {
            throw std::runtime_error("TLS key update failed");
        }
    }
    void close(bool notify = false, bool reset = false) {
        if (notify && tls_) {
            asio::error_code ec;
            bool shutdown = false;
            stream_.async_shutdown([&](asio::error_code error) {
                ec = error;
                shutdown = true;
            });
            const bool completed = wait_for_completion(shutdown, 2s);
            if ((SSL_get_shutdown(stream_.native_handle()) & SSL_SENT_SHUTDOWN) == 0) {
                throw std::runtime_error("TLS close_notify was not sent");
            }
            if (completed && ec && ec != asio::error::eof && ec != asio::ssl::error::stream_truncated) {
                throw std::system_error(ec);
            }
        }
        asio::error_code ignored;
        if (reset) {
            stream_.next_layer().set_option(asio::socket_base::linger(true, 0), ignored);
        }
        stream_.next_layer().close(ignored);
    }

private:
    bool wait_for_completion(bool& completed, std::chrono::milliseconds timeout) {
        io_.restart();
        io_.run_for(timeout);
        if (completed) {
            return true;
        }
        // 关闭后排完组合操作，回调和借用缓冲都先于本次调用返回而退役。
        asio::error_code ignored;
        stream_.next_layer().close(ignored);
        io_.restart();
        io_.run();
        return false;
    }
    asio::io_context& io_;
    asio::ssl::context context_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    bool tls_;
};

void check_pipeline_upload(asio::io_context& io,
    const asio::ip::tcp::endpoint& endpoint, bool tls, std::size_t size, bool defer_body) {
    peer client(io, endpoint, tls);
    client.send("GET /disconnect/pipeline HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(client.read_until("pipeline-first").find("pipeline-first") != std::string::npos);
    if (tls) {
        client.request_key_update();
    }
    const std::string payload(size, 'p');
    const auto prefix = defer_body ? size / 2 : size;
    client.send("POST /disconnect/echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: " +
                std::to_string(size) + "\r\n\r\n" + payload.substr(0, prefix));
    std::string output;
    if (defer_body) {
        // 前一响应结束后，EOF 监测器必须 join；剩余上传由后续请求接管。
        output = client.read_until("pipeline-last");
        client.send(payload.substr(prefix));
    }
    output += client.read_until({}, 3s);
    if (!output.ends_with(payload)) {
        std::cerr << "stream_disconnect pipeline: tls=" << tls << " size=" << size << " deferred=" << defer_body
                  << " received=" << output.size() << " response=" << output.substr(0, 256) << '\n';
    }
    EXPECT_TRUE(output.find("pipeline-last") != std::string::npos);
    EXPECT_TRUE(output.ends_with(payload));
    EXPECT_TRUE(output.find("HTTP/1.1 200 ") != std::string::npos);
}

// RFC 9112 section 9.6: a FIN behind pipelined requests closes only the
// request direction. The stream in progress and every request received before
// the FIN -- a streamed one idling on its stop included -- complete; then the
// server closes without reading that end again as abandonment.
void check_pipeline_half_close(asio::io_context& io,
    const asio::ip::tcp::endpoint& endpoint, bool tls, bool trailing_buffered) {
    peer client(io, endpoint, tls);
    const auto cancelled_before = pipeline_cancelled.load();
    client.send("GET /disconnect/pipeline HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(client.read_until("pipeline-first").find("pipeline-first") != std::string::npos);
    // The streamed request is either the last one before the FIN, with no byte
    // behind it, or followed by a buffered one.
    client.send(std::string("GET /disconnect/pipeline-wait HTTP/1.1\r\nHost: localhost\r\n\r\n") +
                (trailing_buffered ? "GET /disconnect/health HTTP/1.1\r\nHost: localhost\r\n\r\n" : ""));
    // Under TLS the TLS engine then reports the end to every later read at
    // once, with no transport read: a pipelined stream must not re-read it.
    client.end_requests();
    // Read to the server's close: no Connection: close was requested, so only
    // the request direction's end can close it before the 3s read deadline.
    const auto begin = clock_type::now();
    const auto output = client.read_until({}, 3s);
    EXPECT_TRUE(clock_type::now() - begin < 2s);
    const auto first_end = output.find("pipeline-last");
    const auto waited_end = output.find("wait-last");
    const auto health = output.find("healthy");
    EXPECT_TRUE(first_end != std::string::npos);
    EXPECT_TRUE(waited_end != std::string::npos && waited_end > first_end);
    EXPECT_TRUE(output.find("0\r\n\r\n", waited_end) != std::string::npos);
    if (trailing_buffered) {
        EXPECT_TRUE(health != std::string::npos && health > waited_end);
    } else {
        EXPECT_TRUE(health == std::string::npos);
    }
    EXPECT_EQ(pipeline_cancelled.load(), cancelled_before);
}

std::string request(std::string_view mode) {
    return "GET /disconnect/events?mode=" + std::string(mode) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

// How the client ends a pipelined burst whose last request is a streamed
// response idling on its stop.
enum class pipeline_end { immediate,
    after_last_started,
    truncated };

// A request received behind another one -- whatever route answered that one,
// and whenever the end arrives -- treats a clean end of the request direction
// as a half-close: it completes, then the server closes. A TLS truncation
// (TCP FIN without close_notify) still ends it as abandonment.
void check_pipelined_stream_end(asio::io_context& io,
    const asio::ip::tcp::endpoint& endpoint, bool tls, std::string_view leading_requests,
    std::string_view leading_marker, pipeline_end end) {
    peer client(io, endpoint, tls);
    const auto cancelled_before = pipeline_cancelled.load();
    client.send(std::string(leading_requests) + "GET /disconnect/pipeline-wait HTTP/1.1\r\nHost: localhost\r\n\r\n");
    std::string output;
    if (end == pipeline_end::after_last_started) {
        // The streamed request already runs, its predecessors done: the end
        // arrives with nothing buffered behind any request.
        output = client.read_until("wait-first");
        EXPECT_TRUE(output.find("wait-first") != std::string::npos);
        client.end_requests();
    } else if (end == pipeline_end::truncated) {
        client.half_close();
    } else {
        client.end_requests();
    }
    const auto begin = clock_type::now();
    output += client.read_until({}, 3s);
    EXPECT_TRUE(clock_type::now() - begin < 2s);
    const auto leading_end = output.find(leading_marker);
    EXPECT_TRUE(leading_end != std::string::npos);
    const auto waited_end = output.find("wait-last");
    if (end == pipeline_end::truncated) {
        // The stream is abandoned: stopped in its wait, or failing its first
        // write when the truncation is seen before it.
        EXPECT_TRUE(waited_end == std::string::npos);
        return;
    }
    EXPECT_TRUE(waited_end != std::string::npos && waited_end > leading_end);
    EXPECT_TRUE(output.find("0\r\n\r\n", waited_end) != std::string::npos);
    EXPECT_EQ(pipeline_cancelled.load(), cancelled_before);
}

// A FIN (or close_notify) with no request before or behind the stream is the
// client leaving (Go net/http, Pingora, nginx): the request stops and the
// stream truncates.
void check_single_stream_end(asio::io_context& io,
    const asio::ip::tcp::endpoint& endpoint, bool tls) {
    peer client(io, endpoint, tls);
    const auto before = cancelled.load();
    const auto context_before = context_cancelled.load();
    client.send(request("token"));
    EXPECT_TRUE(client.read_until("data: first").find("data: first") != std::string::npos);
    client.end_requests();
    EXPECT_TRUE(wait_until([&] { return cancelled.load() == before + 1; }));
    EXPECT_EQ(context_cancelled.load(), context_before + 1);
    EXPECT_TRUE(client.read_until("0\r\n\r\n", 500ms).find("0\r\n\r\n") == std::string::npos);
    client.close();
}

std::size_t fd_count() {
#ifdef __linux__
    std::error_code ec;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end; it.increment(ec)) {
        ++count;
    }
    return count;
#else
    return 0;
#endif
}
}  // namespace stream_disconnect_test

using namespace stream_disconnect_test;

TEST(Ruvia, http1_stream_peer_close_stops_request_and_recycles_connections) {
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.ca_file_.parent_path() / "key.pem";
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(private_key.string().c_str(), "w"), BIO_free);
        if (!output || ruvia::test::write_tls_private_key(output.get(),
                           SSL_CTX_get0_privatekey(identity.context_.native_handle())) != 1) {
            throw std::runtime_error("cannot write disconnect test private key");
        }
    }
    asio::io_context io;
    ruvia::test::tcp_port_reservation plain_reservation(io);
    ruvia::test::tcp_port_reservation tls_reservation(io);
    const auto plain_endpoint = plain_reservation.endpoint();
    const auto tls_endpoint = tls_reservation.endpoint();
    auto& app = ruvia::app();
    std::binary_semaphore started{0};
    app.server({.worker_count_ = 2, .idle_timeout_ = 15s, .connection_scan_interval_ = 5ms, .write_timeout_ = 150ms})
        .listen({.address_ = "127.0.0.1", .http_ = plain_endpoint.port(), .https_ = tls_endpoint.port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}}})
        .on_error([](ruvia::context& c, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
            errors.fetch_add(1);
            std::cerr << "stream_disconnect error: " << error.code() << ": " << error.message() << '\n';
            c.draft().status(error.status());
            co_return c.text("unexpected-error");
        })
        .on_start([&started] { started.release(); });
    std::exception_ptr server_failure;
    std::thread runner([&] {
        try {
            app.run();
        } catch (...) {
            server_failure = std::current_exception();
        }
    });
    std::exception_ptr client_failure;
    const bool ready = started.try_acquire_for(10s);
    EXPECT_TRUE(ready);
    try {
        if (ready) {
            for (const bool tls : {false, true}) {
                const auto endpoint = tls ? tls_endpoint : plain_endpoint;
                // An idle SSE (no write, no writer.sleep()) observes the client
                // leaving through context::stop_token(): FIN, TLS close_notify, or RST.
                struct close_kind final {
                    bool notify_;
                    bool reset_;
                };
                for (const auto mode : {"token", "writer"}) {
                    for (const auto kind : {close_kind{false, false}, close_kind{true, false}, close_kind{false, true}}) {
                        peer client(io, endpoint, tls);
                        const auto before = cancelled.load();
                        const auto context_before = context_cancelled.load();
                        client.send(request(mode));
                        EXPECT_TRUE(client.read_until("data: first").find("data: first") != std::string::npos);
                        const auto begin = clock_type::now();
                        client.close(kind.notify_, kind.reset_);
                        EXPECT_TRUE(wait_until([&] { return cancelled.load() == before + 1; }));
                        EXPECT_TRUE(clock_type::now() - begin < 2s);
                        EXPECT_EQ(context_cancelled.load(), context_before + 1);
                        EXPECT_TRUE(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
                    }
                }
                peer body_client(io, endpoint, tls);
                const auto complete_before = body_complete.load();
                body_client.send("GET /disconnect/body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\nConnection: close\r\n\r\n");
                std::this_thread::sleep_for(20ms);
                body_client.send("hello-world");
                EXPECT_TRUE(body_client.read_until("0\r\n\r\n").find("hello-world") != std::string::npos);
                EXPECT_EQ(body_complete.load(), complete_before + 1);
                // Committed before its body arrived: the read completing the
                // body starts detection, so the idle wait still ends on close.
                peer waiting_body_client(io, endpoint, tls);
                const auto body_cancelled_before = body_cancelled.load();
                waiting_body_client.send("GET /disconnect/body?early=1&wait=1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\n");
                EXPECT_TRUE(waiting_body_client.read_until("early").find("early") != std::string::npos);
                waiting_body_client.send("hello-world");
                EXPECT_TRUE(waiting_body_client.read_until("hello-world").find("hello-world") != std::string::npos);
                const auto body_close_begin = clock_type::now();
                waiting_body_client.close();
                EXPECT_TRUE(wait_until([&] { return body_cancelled.load() == body_cancelled_before + 1; }));
                EXPECT_TRUE(clock_type::now() - body_close_begin < 2s);
                peer post_sse_client(io, endpoint, tls);
                post_sse_client.send("POST /disconnect/post-sse HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\nConnection: close\r\n\r\n{\"id\":\"1\"}");
                const auto post_sse_output = post_sse_client.read_until("0\r\n\r\n");
                EXPECT_TRUE(post_sse_output.find("text/event-stream") != std::string::npos);
                EXPECT_TRUE(post_sse_output.find("data: {\"id\":\"1\"}") != std::string::npos);
                peer early_client(io, endpoint, tls);
                early_client.send("GET /disconnect/body?early=1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\n");
                EXPECT_TRUE(early_client.read_until("early").find("early") != std::string::npos);
                early_client.send("hello-worldGET /disconnect/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
                const auto early_output = early_client.read_until({});
                EXPECT_TRUE(early_output.find("hello-world") != std::string::npos);
                EXPECT_TRUE(early_output.find("healthy") == std::string::npos);
                EXPECT_TRUE(wait_until([&] { return app.http_stats().active_connections_ == 0; }));

                const auto pipeline_cancelled_before = pipeline_cancelled.load();
                for (const std::size_t size : {std::size_t{31}, std::size_t{256 * 1024}}) {
                    check_pipeline_upload(io, endpoint, tls, size, false);
                    if (tls) {
                        check_pipeline_upload(io, endpoint, true, size, true);
                    }
                }
                // Pipelined bytes arriving during the stream are retained, not
                // read as the client leaving.
                EXPECT_EQ(pipeline_cancelled.load(), pipeline_cancelled_before);
                for (const bool trailing_buffered : {false, true}) {
                    check_pipeline_half_close(io, endpoint, tls, trailing_buffered);
                }
                constexpr std::string_view slow_request = "GET /disconnect/slow HTTP/1.1\r\nHost: localhost\r\n\r\n";
                const std::string two_buffered =
                    std::string(slow_request) + "GET /disconnect/health HTTP/1.1\r\nHost: localhost\r\n\r\n";
                // [buffered, streamed] and [buffered, buffered, streamed]: no
                // monitor ran before the stream, which still completes.
                check_pipelined_stream_end(io, endpoint, tls, slow_request, "slow-done", pipeline_end::immediate);
                check_pipelined_stream_end(io, endpoint, tls, two_buffered, "healthy", pipeline_end::immediate);
                // [streamed, streamed] with the end after the first completed.
                check_pipelined_stream_end(io, endpoint, tls,
                    "GET /disconnect/pipeline HTTP/1.1\r\nHost: localhost\r\n\r\n", "pipeline-last",
                    pipeline_end::after_last_started);
                if (tls) {
                    check_pipelined_stream_end(io, endpoint, tls, slow_request, "slow-done", pipeline_end::truncated);
                }
                check_single_stream_end(io, endpoint, tls);
                EXPECT_TRUE(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
                for (const auto mode : {"write", "timeout"}) {
                    peer racing(io, endpoint, tls);
                    const auto writes_before = write_entered.load();
                    racing.send(request(mode));
                    EXPECT_TRUE(racing.read_until("data: first").find("data: first") != std::string::npos);
                    EXPECT_TRUE(wait_until([&] { return write_entered.load() == writes_before + 1; }));
                    if (std::string_view(mode) == "timeout") {
                        EXPECT_TRUE(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
                    }
                    racing.close(false, true);
                    EXPECT_TRUE(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
                }
                // write_timeout_ bounds every write of an upgraded connection
                // as well: a WebSocket without heartbeat or a CONNECT tunnel
                // whose peer stops reading is closed once its write makes no
                // progress, long before idle_timeout_.
                for (const auto upgrade : {std::string_view("GET /disconnect/websocket-stall HTTP/1.1\r\nHost: localhost\r\n"
                                                            "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
                                                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"),
                         std::string_view("CONNECT stall.test:443 HTTP/1.1\r\nHost: stall.test:443\r\n\r\n")}) {
                    peer stalled(io, endpoint, tls);
                    const auto writes_before = stalled_upgrade_writes.load();
                    const auto failures_before = stalled_upgrade_failures.load();
                    stalled.send(upgrade);
                    const auto head = stalled.read_until("\r\n\r\n");
                    EXPECT_TRUE(head.starts_with(upgrade.starts_with("GET") ? "HTTP/1.1 101 " : "HTTP/1.1 200 "));
                    EXPECT_TRUE(wait_until([&] { return stalled_upgrade_writes.load() == writes_before + 1; }));
                    EXPECT_TRUE(wait_until([&] { return app.http_stats().active_connections_ == 0; }, 5s));
                    EXPECT_TRUE(wait_until([&] { return stalled_upgrade_failures.load() == failures_before + 1; }));
                    stalled.close(false, true);
                }
            }
#if defined(RUVIA_ENABLE_HTTP2)
            // The HTTP/2 reader keeps its connection idle (or long_lived for
            // a tunnel) while the writer is blocked; write_timeout_ still
            // closes a connection whose peer stopped reading.
            for (const bool tunnel : {false, true}) {
                peer stalled(io, plain_endpoint, false);
                const auto writes_before = stalled_upgrade_writes.load();
                const auto failures_before = stalled_upgrade_failures.load();
                stalled.send(http2_request_without_reading(tunnel));
                if (tunnel) {
                    EXPECT_TRUE(wait_until([&] { return stalled_upgrade_writes.load() == writes_before + 1; }));
                }
                EXPECT_TRUE(wait_until([&] { return app.http_stats().active_connections_ == 0; }, 5s));
                if (tunnel) {
                    EXPECT_TRUE(wait_until([&] { return stalled_upgrade_failures.load() == failures_before + 1; }));
                }
                stalled.close(false, true);
            }
#endif
            peer explicit_peer(io, plain_endpoint, false);
            const auto explicit_before = cancelled.load();
            const auto explicit_context_before = context_cancelled.load();
            explicit_peer.send(request("explicit"));
            EXPECT_TRUE(explicit_peer.read_until("data: first").find("data: first") != std::string::npos);
            explicit_stop.request_stop();
            EXPECT_TRUE(wait_until([&] { return cancelled.load() == explicit_before + 1; }));
            EXPECT_EQ(context_cancelled.load(), explicit_context_before);
            explicit_peer.close();
            EXPECT_TRUE(wait_until([&] { return app.http_stats().active_connections_ == 0; }));

            const auto fd_baseline = fd_count();
            std::size_t fd_peak = fd_baseline, active_peak = 0;
            std::chrono::milliseconds recycle_max{0};
            constexpr std::size_t batch_size = 64, batches = 36;
            for (std::size_t batch = 0; batch < batches; ++batch) {
                std::vector<std::unique_ptr<peer>> clients;
                for (std::size_t index = 0; index < batch_size; ++index) {
                    auto client = std::make_unique<peer>(io, plain_endpoint, false);
                    client->send(request("token"));
                    EXPECT_TRUE(client->read_until("data: first").find("data: first") != std::string::npos);
                    clients.push_back(std::move(client));
                }
                active_peak = std::max(active_peak, app.http_stats().active_connections_);
                fd_peak = std::max(fd_peak, fd_count());
                const auto before = cancelled.load();
                const auto context_before = context_cancelled.load();
                const auto begin = clock_type::now();
                for (auto& client : clients) {
                    client->close();
                }
                EXPECT_TRUE(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
                EXPECT_EQ(cancelled.load(), before + batch_size);
                EXPECT_EQ(context_cancelled.load(), context_before + batch_size);
                recycle_max = std::max(recycle_max, std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - begin));
            }
            const auto fd_final = fd_count();
            EXPECT_EQ(active_peak, batch_size);
#ifdef __linux__
            EXPECT_TRUE(fd_final <= fd_baseline);
#endif
            peer recovery(io, plain_endpoint, false);
            recovery.send("GET /disconnect/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
            EXPECT_TRUE(recovery.read_until("healthy").find("healthy") != std::string::npos);
            recovery.close();
            EXPECT_TRUE(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
            std::cout << "stream_disconnect metrics: total=" << batch_size * batches << " peak_active=" << active_peak
                      << " fd_baseline=" << fd_baseline << " fd_peak=" << fd_peak << " fd_final=" << fd_final
                      << " max_recycle_ms=" << recycle_max.count() << " errors=" << errors.load()
                      << " connection_failures=" << app.http_stats().connection_failures_
                      << " refused=" << app.http_stats().connections_refused_
                      << " max_connections_per_worker=" << *ruvia::server_config{}.max_connections_per_worker_ << '\n';
            EXPECT_EQ(app.http_stats().connections_refused_, std::size_t{0});

            // 在同一个有限等待上竞争 worker stop 和 peer EOF。
            peer stopping(io, plain_endpoint, false);
            const auto before_stop = finished.load();
            stopping.send(request("token"));
            EXPECT_TRUE(stopping.read_until("data: first").find("data: first") != std::string::npos);
            app.stop();
            stopping.close();
            EXPECT_TRUE(wait_until([&] { return finished.load() == before_stop + 1; }));
        }
    } catch (...) {
        client_failure = std::current_exception();
    }
    app.stop();
    runner.join();
    EXPECT_EQ(errors.load(), std::size_t{0});
    EXPECT_EQ(app.http_stats().connection_failures_, std::size_t{0});
    EXPECT_EQ(app.http_stats().accept_failures_, std::size_t{0});
    EXPECT_EQ(app.http_stats().worker_failures_, std::size_t{0});
    if (server_failure) {
        std::rethrow_exception(server_failure);
    }
    if (client_failure) {
        std::rethrow_exception(client_failure);
    }
}
