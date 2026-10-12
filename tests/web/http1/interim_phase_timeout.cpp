#include <array>
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
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "test_harness.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

// An interim head sent from a handler first writes the pipelined responses
// held before it; over TLS that write always completes asynchronously. Once
// it and the interim head are out, the handler is timed as a handler again
// (idle_timeout), not as a write. This executable owns its application, so
// its short write timeout cannot leak into other suites.

namespace interim_phase_timeout_test {

using namespace std::chrono_literals;

constexpr auto write_timeout = 100ms;
// Several write timeouts, far inside the default idle timeout.
constexpr auto handler_pause = 400ms;

class interim_phase_routes final {
    RUVIA_ROUTES_BEGIN(interim_phase_routes)
    RUVIA_GET("/interim-phase/item", item);
    RUVIA_GET("/interim-phase/hinted", hinted);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> item(ruvia::context& c) {
        co_return c.text("item");
    }

    ruvia::task<ruvia::http_response> hinted(ruvia::context& c) {
        co_await c.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        (void)co_await ruvia::sleep_for(c.worker(), handler_pause);
        co_return c.text("hinted");
    }
};

// Sends `requests` in one TLS record, then reads until the server closes.
std::string exchange_tls(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view requests) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    if (!error) {
        asio::write(stream, asio::buffer(requests), error);
    }
    std::string response;
    std::array<char, 1024> buffer;
    while (!error) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
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
    }
    app.stop();
    runner.join();
    RUVIA_CHECK(failure == nullptr);
}
