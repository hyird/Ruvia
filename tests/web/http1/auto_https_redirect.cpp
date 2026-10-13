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
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/error.h"

#include "test_harness.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

// A cleartext listener with auto_https_redirect_ answers every request with a
// permanent redirect to the HTTPS service and closes. A client that accepts
// no content coding cannot take the redirect's representation, so it gets a
// 406 from the application's error handler, called once. This executable owns
// its application, so its listeners and error handler cannot leak into other
// suites.

namespace auto_https_redirect_test {

using namespace std::chrono_literals;

std::atomic<int> error_handler_calls{0};

ruvia::task<ruvia::http_response> counted_error(ruvia::context& c, ruvia::http_error_info error) {
    ++error_handler_calls;
    c.status(error.status());
    co_return c.text("rejected");
}

// Sends `request` on a fresh cleartext connection, then reads until the server closes.
std::string exchange(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view request) {
    asio::ip::tcp::socket socket(io);
    asio::error_code error;
    socket.connect(endpoint, error);
    if (!error) {
        asio::write(socket, asio::buffer(request), error);
    }
    std::string response;
    std::array<char, 1024> buffer;
    while (!error) {
        const auto size = socket.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    return response;
}

}  // namespace auto_https_redirect_test

using namespace auto_https_redirect_test;

RUVIA_TEST(auto_https_redirect_points_cleartext_requests_at_the_https_service) {
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.ca_file_.parent_path() / "key.pem";
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(private_key.string().c_str(), "w"), BIO_free);
        if (!output || ruvia::test::write_tls_private_key(
                           output.get(), SSL_CTX_get0_privatekey(identity.context_.native_handle())) != 1) {
            throw std::runtime_error("cannot write auto-HTTPS redirect test private key");
        }
    }

    asio::io_context io;
    // Both reservations are held at once so the two ports differ.
    asio::ip::tcp::acceptor http_reservation(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::acceptor https_reservation(io, {asio::ip::address_v4::loopback(), 0});
    const auto http_endpoint = http_reservation.local_endpoint();
    const auto https_endpoint = https_reservation.local_endpoint();
    http_reservation.close();
    https_reservation.close();

    auto& app = ruvia::app();
    std::binary_semaphore started{0};
    app.on_error(&counted_error);
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms})
        .listen({
            .address_ = "127.0.0.1",
            .http_ = http_endpoint.port(),
            .https_ = https_endpoint.port(),
            .tls_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key},
            .auto_https_redirect_ = true,
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
        const auto https_port = std::to_string(https_endpoint.port());

        const auto redirected = exchange(io, http_endpoint,
            "GET /docs/page?lang=en HTTP/1.1\r\nHost: example.test:8080\r\n\r\n");
        RUVIA_CHECK(redirected.starts_with("HTTP/1.1 308 "));
        RUVIA_CHECK(redirected.find("\r\nLocation: https://example.test:" + https_port + "/docs/page?lang=en\r\n") !=
                    std::string::npos);

        // RFC 9112 §3.2.2: an absolute-form target names the host, so an
        // HTTP/1.0 request needs no Host field to be redirected.
        const auto absolute = exchange(io, http_endpoint, "GET http://example.com/x?y HTTP/1.0\r\n\r\n");
        RUVIA_CHECK(absolute.starts_with("HTTP/1.0 308 "));
        RUVIA_CHECK(absolute.find("\r\nLocation: https://example.com:" + https_port + "/x?y\r\n") != std::string::npos);

        error_handler_calls = 0;
        const auto refused = exchange(io, http_endpoint,
            "GET /docs/page HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: identity;q=0, *;q=0\r\n\r\n");
        RUVIA_CHECK(refused.starts_with("HTTP/1.1 406 "));
        RUVIA_CHECK(refused.find("Location:") == std::string::npos);
        RUVIA_CHECK(refused.ends_with("rejected"));
        RUVIA_CHECK_EQ(error_handler_calls.load(), 1);
    }
    app.stop();
    runner.join();
    RUVIA_CHECK(failure == nullptr);
}
