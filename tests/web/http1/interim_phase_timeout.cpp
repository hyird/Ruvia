#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
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

// An interim head sent from a handler first writes the pipelined responses
// held before it; over TLS that write always completes asynchronously. Once
// it and the interim head are out, the handler is timed as a handler again
// (idle_timeout), not as a write. A response-stream handler likewise: only
// its writes are timed as writes, and the wait between them is governed by
// idle_timeout. This executable owns its application, so its short write
// timeout cannot leak into other suites.

namespace interim_phase_timeout_test {

using namespace std::chrono_literals;

constexpr auto write_timeout = 100ms;
// Several write timeouts, far inside the default idle timeout.
constexpr auto handler_pause = 400ms;
// One stream write that takes many write timeouts to drain to a slow reader.
constexpr std::size_t bulk_bytes = std::size_t{8} * 1024 * 1024;
std::atomic<std::chrono::milliseconds::rep> bulk_write_duration_ms{0};

class interim_phase_routes final {
    RUVIA_ROUTES_BEGIN(interim_phase_routes)
    RUVIA_GET("/interim-phase/item", item);
    RUVIA_GET("/interim-phase/hinted", hinted);
    RUVIA_GET_STREAM("/interim-phase/stream", stream);
    RUVIA_GET_STREAM("/interim-phase/bulk", bulk);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> item(ruvia::context& c) {
        co_return c.text("item");
    }

    ruvia::task<ruvia::http_response> hinted(ruvia::context& c) {
        co_await c.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        (void)co_await ruvia::sleep_for(c.worker(), handler_pause);
        co_return c.text("hinted");
    }

    ruvia::task<void> stream(ruvia::context& c) {
        co_await c.stream().write("first-chunk");
        (void)co_await ruvia::sleep_for(c.worker(), handler_pause);
        co_await c.stream().write("second-chunk");
    }

    ruvia::task<void> bulk(ruvia::context& c) {
        const std::string bytes(bulk_bytes, 'b');
        const auto started = std::chrono::steady_clock::now();
        co_await c.stream().write(bytes);
        bulk_write_duration_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(),
            std::memory_order_relaxed);
    }
};

// Sends `requests` in one TLS record, then reads until the server closes.
// A paced read drains a full 64 KiB before pausing: sleeping after each 16 KiB
// TLS record can leave a raw socket write pending for over 100 ms while the
// kernel drains its large send queue, which legitimately expires write_timeout.
// The receive window bounds read-ahead; it does not bound the server send queue.
std::string exchange_tls(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view requests,
    std::chrono::milliseconds read_pause = 0ms, std::chrono::milliseconds initial_read_pause = 0ms) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    asio::error_code error;
    stream.next_layer().open(endpoint.protocol());
    stream.next_layer().set_option(asio::socket_base::receive_buffer_size(64 * 1024));
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    if (!error) {
        asio::write(stream, asio::buffer(requests), error);
    }
    std::string response;
    std::array<char, 64 * 1024> buffer;
    std::this_thread::sleep_for(initial_read_pause);
    while (!error) {
        const auto size = read_pause == 0ms ? stream.read_some(asio::buffer(buffer), error)
                                            : asio::read(stream, asio::buffer(buffer), error);
        response.append(buffer.data(), size);
        std::this_thread::sleep_for(read_pause);
    }
    return response;
}

}  // namespace interim_phase_timeout_test

using namespace interim_phase_timeout_test;

RUVIA_TEST(a_handler_after_its_interim_head_is_timed_as_a_handler_not_a_write) {
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.ca_file_.parent_path() / "key.pem";
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(private_key.string().c_str(), "w"), BIO_free);
        if (!output || ruvia::test::write_tls_private_key(
                           output.get(), SSL_CTX_get0_privatekey(identity.context_.native_handle())) != 1) {
            throw std::runtime_error("cannot write interim phase test private key");
        }
    }

    asio::io_context io;
    asio::ip::tcp::acceptor reservation(io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();

    auto& app = ruvia::app();
    std::binary_semaphore started{0};
    // Error handling runs while the response is prepared; it is handler work,
    // timed by idle_timeout, not by write_timeout.
    app.on_error([](ruvia::context& c, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        (void)co_await ruvia::sleep_for(c.worker(), handler_pause);
        c.status(error.status());
        co_return c.text("refused");
    });
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .write_timeout_ = write_timeout})
        .listen({
            .address_ = "127.0.0.1",
            .https_ = endpoint.port(),
            .tls_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key},
        })
        .on_start([&started] { started.release(); });
    std::exception_ptr failure;
    std::thread runner([&app, &failure] {
        try {
            app.run();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    const bool ready = started.try_acquire_for(10s);
    RUVIA_CHECK(ready);
    if (ready) {
        // The first response is held behind the second request, so the 103
        // has to flush it first.
        const auto output = exchange_tls(io, endpoint,
            "GET /interim-phase/item HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "GET /interim-phase/hinted HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        const auto item = output.find("item");
        const auto interim = output.find("HTTP/1.1 103 ");
        RUVIA_CHECK(item != std::string::npos && interim != std::string::npos && item < interim);
        RUVIA_CHECK(output.ends_with("hinted"));

        const auto streamed = exchange_tls(io, endpoint,
            "GET /interim-phase/stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        RUVIA_CHECK(streamed.find("first-chunk") != std::string::npos);
        RUVIA_CHECK(streamed.find("second-chunk") != std::string::npos);
        RUVIA_CHECK(streamed.ends_with("0\r\n\r\n"));

        // A steadily draining single write is not inactivity. Paced batches
        // keep each raw write below the timeout; the handler measurement proves
        // that the whole write still spans more than one inactivity budget.
        const auto bulk = exchange_tls(io, endpoint,
            "GET /interim-phase/bulk HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 2ms);
        RUVIA_CHECK(bulk.size() > bulk_bytes);
        RUVIA_CHECK(bulk.ends_with("0\r\n\r\n"));
        RUVIA_CHECK(bulk_write_duration_ms.load(std::memory_order_relaxed) > write_timeout.count());

        // The same response to a peer that accepts no data exhausts the send
        // queue and must time out, rather than treating a pending write as
        // progress. Only resume reading after several inactivity budgets.
        const auto stalled = exchange_tls(io, endpoint,
            "GET /interim-phase/bulk HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 0ms, handler_pause);
        RUVIA_CHECK(stalled.starts_with("HTTP/1.1 200 "));
        RUVIA_CHECK(stalled.find("\r\n\r\n") != std::string::npos);
        RUVIA_CHECK(stalled.size() < bulk_bytes);
        RUVIA_CHECK(!stalled.ends_with("0\r\n\r\n"));

        // No acceptable coding: the 406 recovery runs on_error after the route.
        const auto refused = exchange_tls(io, endpoint,
            "GET /interim-phase/item HTTP/1.1\r\nHost: localhost\r\n"
            "Accept-Encoding: identity;q=0, *;q=0\r\nConnection: close\r\n\r\n");
        RUVIA_CHECK(refused.starts_with("HTTP/1.1 406 "));
        RUVIA_CHECK(refused.ends_with("refused"));
    }
    app.stop();
    runner.join();
    RUVIA_CHECK(failure == nullptr);
}
