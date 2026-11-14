#include <gtest/gtest.h>

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
#include <openssl/ssl.h>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"

#include "http_server_fixture.h"
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
    c.draft().status(error.status());
    co_return c.text("rejected");
}

using ruvia::test::exchange;

}  // namespace auto_https_redirect_test

using namespace auto_https_redirect_test;

TEST(Ruvia, auto_https_redirect_points_cleartext_requests_at_the_https_service) {
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.private_key_file();

    asio::io_context io;
    ruvia::test::tcp_port_reservation http_reservation(io);
    ruvia::test::tcp_port_reservation https_reservation(io);
    const auto http_endpoint = http_reservation.endpoint();
    const auto https_endpoint = https_reservation.endpoint();

    auto& app = ruvia::app();
    static std::binary_semaphore started{0};
    app.on_error(&counted_error);
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms})
        .listen({
            .address_ = "127.0.0.1",
            .http_ = http_endpoint.port(),
            .https_ = https_endpoint.port(),
            .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}},
            .auto_https_redirect_ = true,
        })
        .on_start([] {
        started.release();
    });
    ruvia::test::app_run runner(app);
    const bool ready = started.try_acquire_for(10s);
    EXPECT_TRUE(ready);
    if (ready) {
        const auto https_port = std::string(std::to_string(https_endpoint.port()));

        const auto redirected = exchange(io, http_endpoint, "GET /docs/page?lang=en HTTP/1.1\r\nHost: example.test:8080\r\n\r\n");
        EXPECT_TRUE(redirected.starts_with("HTTP/1.1 308 "));
        EXPECT_TRUE(redirected.find(std::string("\r\nLocation: https://example.test:") + https_port + "/docs/page?lang=en\r\n") != std::string::npos);

        // RFC 9112 §3.2.2: an absolute-form target names the host, so an
        // HTTP/1.0 request needs no Host field to be redirected.
        const auto absolute = exchange(io, http_endpoint, "GET http://example.com/x?y HTTP/1.0\r\n\r\n");
        EXPECT_TRUE(absolute.starts_with("HTTP/1.1 308 "));
        EXPECT_TRUE(absolute.find(std::string("\r\nLocation: https://example.com:") + https_port + "/x?y\r\n") != std::string::npos);

        error_handler_calls = 0;
        const auto refused = exchange(io, http_endpoint, "GET /docs/page HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: identity;q=0, *;q=0\r\n\r\n");
        EXPECT_TRUE(refused.starts_with("HTTP/1.1 406 "));
        EXPECT_TRUE(refused.find("Location:") == std::string::npos);
        EXPECT_TRUE(refused.ends_with("rejected"));
        EXPECT_EQ(error_handler_calls.load(), 1);
    }
    app.stop();
    runner.join();
}
