#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/streaming.h"

#include "test_harness.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

namespace stream_disconnect_test {
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;
std::atomic<std::size_t> finished{}, cancelled{}, errors{}, context_cancelled{}, body_rejected{}, body_complete{};
std::atomic<std::size_t> write_entered{};
ruvia::stop_source explicit_stop;
std::atomic<bool> default_cancelled{};

struct completion final {
    ~completion() {
        finished.fetch_add(1);
    }
};

class routes final {
    RUVIA_ROUTES_BEGIN(routes)
    RUVIA_GET("/disconnect/events", events);
    RUVIA_GET("/disconnect/pipeline", pipeline);
    RUVIA_GET("/disconnect/body", body);
    RUVIA_POST("/disconnect/echo", echo);
    RUVIA_POST("/disconnect/post-sse", post_sse);
    RUVIA_GET("/disconnect/health", health);
    RUVIA_ROUTES_END

    ruvia::task<void> events(ruvia::context& c) {
        completion done;
        const auto mode = c.req().query("mode").value_or("token");
        const auto original = c.stop_token();
        auto writer = c.stream_sse();
        auto token = mode == "default" ? original : writer.cancel_on_peer_eof();
        if (mode == "explicit") {
            token = ruvia::combine_stop_tokens(token, explicit_stop.token());
        }
        co_await writer.write({.data_ = "first"});
        if (mode == "default") {
            default_cancelled.store(co_await ruvia::sleep_for(c.worker(), 200ms, token) ==
                                    ruvia::timer_sleep_result::stop_requested);
            co_await writer.write({.data_ = "default-complete"});
            co_return;
        }
        if (mode == "write" || mode == "timeout") {
            // 大块写入让关闭、写完成和写超时竞争同一个连接生命周期。
            const std::string bulk(8 * 1024 * 1024, 'x');
            write_entered.fetch_add(1);
            co_await c.stream().write(bulk);
            co_return;
        }
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
        const auto token = writer.cancel_on_peer_eof();
        co_await writer.write("pipeline-first");
        (void)co_await ruvia::sleep_for(c.worker(), 100ms, token);
        co_await writer.write("pipeline-last");
    }

    ruvia::task<void> body(ruvia::context& c) {
        auto& writer = c.stream();
        try {
            (void)writer.cancel_on_peer_eof();
        } catch (const std::logic_error&) {
            body_rejected.fetch_add(1);
        }
        if (c.req().query("early").has_value()) {
            co_await writer.write("early");
        }
        const auto received = co_await c.req().text();
        (void)writer.cancel_on_peer_eof();
        body_complete.fetch_add(1);
        co_await writer.write(received);
    }

    ruvia::task<void> post_sse(ruvia::context& c) {
        const auto received = co_await c.req().text();
        auto writer = c.stream_sse();
        co_await writer.write({.data_ = received});
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        const auto text = co_await c.req().text();
        co_return c.text(text);
    }

    ruvia::task<ruvia::http_response> health(ruvia::context& c) {
        co_return c.text("healthy");
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

// 非阻塞客户端给每个读取设置独立截止时间，回归失败不会挂住整个测试进程。
class peer final {
public:
    peer(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, bool tls)
        : context_(asio::ssl::context::tls_client),
          stream_(io, context_),
          tls_(tls) {
        context_.set_verify_mode(asio::ssl::verify_none);
        bool connected = false;
        asio::error_code connect_error;
        io.restart();
        stream_.next_layer().async_connect(endpoint, [&](const asio::error_code& ec) {
            connect_error = ec;
            connected = true;
        });
        io.run_for(2s);
        if (!connected) {
            asio::error_code ignored;
            stream_.next_layer().close(ignored);
            io.restart();
            io.run();
            throw std::runtime_error("disconnect test connect timed out");
        }
        if (connect_error) {
            throw std::system_error(connect_error);
        }
        stream_.next_layer().non_blocking(true);
        if (tls_) {
            const auto deadline = clock_type::now() + 2s;
            for (;;) {
                asio::error_code ec;
                stream_.handshake(asio::ssl::stream_base::client, ec);
                if (!ec) {
                    break;
                }
                if (!retry(ec) || clock_type::now() >= deadline) {
                    throw std::system_error(ec);
                }
                std::this_thread::sleep_for(1ms);
            }
        }
    }
    void send(std::string_view bytes) {
        const auto deadline = clock_type::now() + 2s;
        while (!bytes.empty()) {
            asio::error_code ec;
            const auto n = tls_ ? stream_.write_some(asio::buffer(bytes), ec)
                                : stream_.next_layer().write_some(asio::buffer(bytes), ec);
            bytes.remove_prefix(n);
            if (ec && (!retry(ec) || clock_type::now() >= deadline)) {
                throw std::system_error(ec);
            }
            if (ec) {
                std::this_thread::sleep_for(1ms);
            }
        }
    }
    std::string read_until(std::string_view marker, std::chrono::milliseconds timeout = 2s) {
        std::string output;
        std::array<char, 16384> bytes;
        const auto deadline = clock_type::now() + timeout;
        while (clock_type::now() < deadline) {
            asio::error_code ec;
            const auto n = tls_ ? stream_.read_some(asio::buffer(bytes), ec)
                                : stream_.next_layer().read_some(asio::buffer(bytes), ec);
            output.append(bytes.data(), n);
            if (!marker.empty() && output.find(marker) != std::string::npos) {
                return output;
            }
            if (ec && !retry(ec)) {
                return output;
            }
            if (ec) {
                std::this_thread::sleep_for(1ms);
            }
        }
        return output;
    }
    void half_close() {
        asio::error_code ignored;
        stream_.next_layer().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    }
    void request_key_update() {
        if (SSL_key_update(stream_.native_handle(), SSL_KEY_UPDATE_REQUESTED) != 1) {
            throw std::runtime_error("TLS key update failed");
        }
    }
    void close(bool notify = false, bool reset = false) {
        if (notify && tls_) {
            // 非阻塞 shutdown 会发送 close_notify，但不等待对端的关闭通知。
            asio::error_code ec;
            stream_.shutdown(ec);
            if ((SSL_get_shutdown(stream_.native_handle()) & SSL_SENT_SHUTDOWN) == 0) {
                throw std::runtime_error("TLS close_notify was not sent");
            }
            if (ec && !retry(ec) && ec != asio::error::eof && ec != asio::ssl::error::stream_truncated) {
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
    static bool retry(const asio::error_code& ec) {
        return ec == asio::error::would_block || ec == asio::error::try_again;
    }
    asio::ssl::context context_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    bool tls_;
};

std::string request(std::string_view mode) {
    return "GET /disconnect/events?mode=" + std::string(mode) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
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

RUVIA_TEST(http1_passive_stream_disconnect_is_opt_in_and_recycles_connections) {
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
    asio::ip::tcp::acceptor plain_reservation(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::acceptor tls_reservation(io, {asio::ip::address_v4::loopback(), 0});
    const auto plain_endpoint = plain_reservation.local_endpoint();
    const auto tls_endpoint = tls_reservation.local_endpoint();
    plain_reservation.close();
    tls_reservation.close();
    auto& app = ruvia::app();
    std::binary_semaphore started{0};
    app.server({.worker_count_ = 2, .idle_timeout_ = 15s, .connection_scan_interval_ = 5ms, .write_timeout_ = 150ms})
        .listen({.address_ = "127.0.0.1", .http_ = plain_endpoint.port(), .https_ = tls_endpoint.port(), .tls_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}})
        .on_error([](ruvia::context& c, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
            errors.fetch_add(1);
            std::cerr << "stream_disconnect error: " << error.code() << ": " << error.message() << '\n';
            c.status(error.status());
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
    RUVIA_CHECK(ready);
    try {
        if (ready) {
            for (const bool tls : {false, true}) {
                const auto endpoint = tls ? tls_endpoint : plain_endpoint;
                for (const auto mode : {"token", "writer"}) {
                    for (const bool notify : {false, true}) {
                        peer client(io, endpoint, tls);
                        const auto before = cancelled.load();
                        const auto context_before = context_cancelled.load();
                        client.send(request(mode));
                        RUVIA_CHECK(client.read_until("data: first").find("data: first") != std::string::npos);
                        const auto begin = clock_type::now();
                        client.close(notify);
                        RUVIA_CHECK(wait_until([&] { return cancelled.load() == before + 1; }));
                        RUVIA_CHECK(clock_type::now() - begin < 2s);
                        RUVIA_CHECK_EQ(context_cancelled.load(), context_before);
                        RUVIA_CHECK(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
                    }
                }
                peer body_client(io, endpoint, tls);
                const auto rejected_before = body_rejected.load();
                const auto complete_before = body_complete.load();
                body_client.send("GET /disconnect/body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\nConnection: close\r\n\r\n");
                RUVIA_CHECK(wait_until([&] { return body_rejected.load() == rejected_before + 1; }));
                body_client.send("hello-world");
                RUVIA_CHECK(body_client.read_until("0\r\n\r\n").find("hello-world") != std::string::npos);
                RUVIA_CHECK_EQ(body_complete.load(), complete_before + 1);
                peer post_sse_client(io, endpoint, tls);
                post_sse_client.send("POST /disconnect/post-sse HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\nConnection: close\r\n\r\n{\"id\":\"1\"}");
                const auto post_sse_output = post_sse_client.read_until("0\r\n\r\n");
                RUVIA_CHECK(post_sse_output.find("text/event-stream") != std::string::npos);
                RUVIA_CHECK(post_sse_output.find("data: {\"id\":\"1\"}") != std::string::npos);
                peer early_client(io, endpoint, tls);
                early_client.send("GET /disconnect/body?early=1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\n");
                RUVIA_CHECK(early_client.read_until("early").find("early") != std::string::npos);
                early_client.send("hello-worldGET /disconnect/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
                const auto early_output = early_client.read_until({});
                RUVIA_CHECK(early_output.find("hello-world") != std::string::npos);
                RUVIA_CHECK(early_output.find("healthy") == std::string::npos);
                RUVIA_CHECK(wait_until([&] { return app.http_stats().active_connections_ == 0; }));

                for (const std::size_t size : {std::size_t{31}, std::size_t{256 * 1024}}) {
                    peer pipeline_client(io, endpoint, tls);
                    pipeline_client.send("GET /disconnect/pipeline HTTP/1.1\r\nHost: localhost\r\n\r\n");
                    RUVIA_CHECK(pipeline_client.read_until("pipeline-first").find("pipeline-first") != std::string::npos);
                    if (tls) {
                        pipeline_client.request_key_update();
                    }
                    const std::string payload(size, 'p');
                    pipeline_client.send("POST /disconnect/echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: " +
                                         std::to_string(size) + "\r\n\r\n" + payload);
                    const auto output = pipeline_client.read_until({}, 3s);
                    if (!output.ends_with(payload)) {
                        std::cerr << "stream_disconnect pipeline: tls=" << tls << " size=" << size
                                  << " received=" << output.size() << " response=" << output.substr(0, 256) << '\n';
                    }
                    RUVIA_CHECK(output.find("pipeline-last") != std::string::npos);
                    RUVIA_CHECK(output.ends_with(payload));
                    RUVIA_CHECK(output.find("HTTP/1.1 200 ") != std::string::npos);
                }
                for (const auto mode : {"write", "timeout"}) {
                    peer racing(io, endpoint, tls);
                    const auto writes_before = write_entered.load();
                    racing.send(request(mode));
                    RUVIA_CHECK(racing.read_until("data: first").find("data: first") != std::string::npos);
                    RUVIA_CHECK(wait_until([&] { return write_entered.load() == writes_before + 1; }));
                    if (std::string_view(mode) == "timeout") {
                        RUVIA_CHECK(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
                    }
                    racing.close(false, true);
                    RUVIA_CHECK(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
                }
            }
            peer explicit_peer(io, plain_endpoint, false);
            const auto explicit_before = cancelled.load();
            const auto context_before = context_cancelled.load();
            explicit_peer.send(request("explicit"));
            RUVIA_CHECK(explicit_peer.read_until("data: first").find("data: first") != std::string::npos);
            explicit_stop.request_stop();
            RUVIA_CHECK(wait_until([&] { return cancelled.load() == explicit_before + 1; }));
            RUVIA_CHECK_EQ(context_cancelled.load(), context_before);
            explicit_peer.close();
            RUVIA_CHECK(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
            // 默认路径既不订阅 EOF，也不破坏 TCP 合法的请求方向半关闭。
            peer default_peer(io, plain_endpoint, false);
            const auto default_before = finished.load();
            default_peer.send(request("default"));
            RUVIA_CHECK(default_peer.read_until("data: first").find("data: first") != std::string::npos);
            default_peer.half_close();
            std::this_thread::sleep_for(50ms);
            RUVIA_CHECK_EQ(finished.load(), default_before);
            RUVIA_CHECK(!default_cancelled.load());
            RUVIA_CHECK(default_peer.read_until("default-complete", 800ms).find("default-complete") != std::string::npos);
            RUVIA_CHECK(!default_cancelled.load());
            default_peer.close();
            RUVIA_CHECK(wait_until([&] { return app.http_stats().active_connections_ == 0; }));

            const auto fd_baseline = fd_count();
            std::size_t fd_peak = fd_baseline, active_peak = 0;
            std::chrono::milliseconds recycle_max{0};
            constexpr std::size_t batch_size = 64, batches = 36;
            for (std::size_t batch = 0; batch < batches; ++batch) {
                std::vector<std::unique_ptr<peer>> clients;
                for (std::size_t index = 0; index < batch_size; ++index) {
                    auto client = std::make_unique<peer>(io, plain_endpoint, false);
                    client->send(request("token"));
                    RUVIA_CHECK(client->read_until("data: first").find("data: first") != std::string::npos);
                    clients.push_back(std::move(client));
                }
                active_peak = std::max(active_peak, app.http_stats().active_connections_);
                fd_peak = std::max(fd_peak, fd_count());
                const auto before = cancelled.load();
                const auto begin = clock_type::now();
                for (auto& client : clients) {
                    client->close();
                }
                RUVIA_CHECK(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
                RUVIA_CHECK_EQ(cancelled.load(), before + batch_size);
                recycle_max = std::max(recycle_max, std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - begin));
            }
            const auto fd_final = fd_count();
            RUVIA_CHECK_EQ(active_peak, batch_size);
#ifdef __linux__
            RUVIA_CHECK(fd_final <= fd_baseline);
#endif
            peer recovery(io, plain_endpoint, false);
            recovery.send("GET /disconnect/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
            RUVIA_CHECK(recovery.read_until("healthy").find("healthy") != std::string::npos);
            recovery.close();
            RUVIA_CHECK(wait_until([&] { return app.http_stats().active_connections_ == 0; }));
            std::cout << "stream_disconnect metrics: total=" << batch_size * batches << " peak_active=" << active_peak
                      << " fd_baseline=" << fd_baseline << " fd_peak=" << fd_peak << " fd_final=" << fd_final
                      << " max_recycle_ms=" << recycle_max.count() << " errors=" << errors.load()
                      << " connection_failures=" << app.http_stats().connection_failures_
                      << " refused=" << app.http_stats().connections_refused_
                      << " max_connections_per_worker=" << *ruvia::server_config{}.max_connections_per_worker_ << '\n';
            RUVIA_CHECK_EQ(app.http_stats().connections_refused_, std::size_t{0});
            RUVIA_CHECK_EQ(context_cancelled.load(), std::size_t{0});

            // 在同一个有限等待上竞争 worker stop 和 peer EOF。
            peer stopping(io, plain_endpoint, false);
            const auto before_stop = finished.load();
            stopping.send(request("token"));
            RUVIA_CHECK(stopping.read_until("data: first").find("data: first") != std::string::npos);
            app.stop();
            stopping.close();
            RUVIA_CHECK(wait_until([&] { return finished.load() == before_stop + 1; }));
        }
    } catch (...) {
        client_failure = std::current_exception();
    }
    app.stop();
    runner.join();
    RUVIA_CHECK_EQ(errors.load(), std::size_t{0});
    RUVIA_CHECK_EQ(app.http_stats().connection_failures_, std::size_t{0});
    RUVIA_CHECK_EQ(app.http_stats().accept_failures_, std::size_t{0});
    RUVIA_CHECK_EQ(app.http_stats().worker_failures_, std::size_t{0});
    if (server_failure) {
        std::rethrow_exception(server_failure);
    }
    if (client_failure) {
        std::rethrow_exception(client_failure);
    }
}
