#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/streaming.h"

#include "http_server_fixture.h"
#include "test_io_context.h"

// Multiple client connections share one application rate-limit budget.
// This executable owns its application, so its rate limit and short request
// phase timeouts cannot leak into other suites.

namespace {

constexpr std::size_t admitted_requests = 4;
constexpr auto phase_timeout = std::chrono::milliseconds(200);

[[nodiscard]] ruvia::server_config admission_server_config() {
    return {.worker_count_ = 1,
        .idle_timeout_ = phase_timeout,
        .connection_scan_interval_ = std::chrono::milliseconds(5),
        .request_header_timeout_ = phase_timeout,
        .request_body_timeout_ = phase_timeout};
}

// Error-handler calls of the request in flight; the server worker writes, the
// client coroutine reads after the response arrives.
std::atomic<int> error_handler_calls{0};

ruvia::task<ruvia::http_response> counted_error(ruvia::context& c, ruvia::http_error_info error) {
    ++error_handler_calls;
    c.draft().status(error.status());
    co_return c.text("rejected");
}

[[maybe_unused]] const bool application_configured = [] {
    ruvia::app().rate_limit(ruvia::rate_limit_config{
        .rule_ = {.max_requests_ = admitted_requests, .window_ = std::chrono::minutes(10)}});
    ruvia::app().trusted_proxies({.cidrs_ = {"127.0.0.1/32"}});
    ruvia::app().on_error(&counted_error);
    return true;
}();

class admission_routes final {
    RUVIA_ROUTES_BEGIN(admission_routes)
    RUVIA_GET("/plain", plain);
    RUVIA_POST("/upload", upload);
    RUVIA_GET("/events", events);
    RUVIA_GET("/boom", boom);
    RUVIA_GET("/limited", plain, ruvia::rate_limit<1, 600000>);
    // Answer without reading the request body.
    RUVIA_POST("/ignore", plain);
    RUVIA_POST("/stream-ignore", events);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> plain(ruvia::context& context_value) {
        co_return context_value.text("plain");
    }

    ruvia::task<ruvia::http_response> upload(ruvia::context& context_value) {
        (void)co_await context_value.req().text();
        co_return context_value.text("uploaded");
    }

    ruvia::task<ruvia::http_response> boom(ruvia::context&) {
        throw std::runtime_error("boom");
        co_return ruvia::http_response{};
    }

    ruvia::task<void> events(ruvia::context& context_value) {
        if (co_await context_value.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await context_value.stream().write("event");
    }
};

// Sends one GET on a fresh connection that the server closes after its
// response; returns everything the server wrote.
std::string exchange(asio::io_context& io, const asio::ip::tcp::endpoint& server_endpoint, std::string_view target,
    std::string_view headers) {
    asio::ip::tcp::socket socket(io);
    socket.connect(server_endpoint);
    std::string request = "GET ";
    request.append(target).append(" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n");
    request.append(headers).append("\r\n");
    asio::write(socket, asio::buffer(request));
    std::string output;
    std::error_code error;
    asio::read(socket, asio::dynamic_buffer(output), error);
    return output;
}

std::string lowercase(std::string_view text) {
    std::string lowered(text);
    for (auto& character : lowered) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return lowered;
}

// Sends `prefix` on a fresh HTTP/1.1 connection, then stalls. Returns the
// bytes the server wrote before closing, or nullopt when it is still open
// after 5 s.
std::optional<std::string> stalled_connection_output(const asio::ip::tcp::endpoint& server_endpoint,
    const asio::ip::address_v4& local_address, std::string_view prefix) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::socket socket(io);
    socket.open(asio::ip::tcp::v4());
    socket.bind({local_address, 0});
    socket.connect(server_endpoint);
    if (!prefix.empty()) {
        asio::write(socket, asio::buffer(prefix));
    }
    std::string output;
    std::error_code result;
    bool closed = false;
    asio::async_read(socket, asio::dynamic_buffer(output), [&](const std::error_code& error, std::size_t) {
        result = error;
        closed = true;
    });
    io.run_for(std::chrono::seconds(5));
    if (!closed || (result != asio::error::eof && result != asio::error::connection_reset)) {
        return std::nullopt;
    }
    return output;
}

// The status codes of the HTTP/1.1 responses in `output`, concatenated.
std::string status_codes(std::string_view output) {
    constexpr std::string_view status_line = "HTTP/1.1 ";
    std::string codes;
    for (auto position = output.find(status_line); position != std::string_view::npos;
        position = output.find(status_line, position + status_line.size())) {
        codes.append(output.substr(position + status_line.size(), 3));
    }
    return codes;
}

}  // namespace

TEST(Ruvia, http1_clients_share_406_admission_and_rate_limit_budget) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, admission_server_config());
    constexpr std::string_view refuse_every_coding = "Accept-Encoding: identity;q=0, *;q=0\r\n";
    // Each exchange runs on its own connection from the same client address.
    // A response stream commits its representation before a status can be
    // inspected, so a client that accepts no coding gets 406 first. The
    // rejection is a terminal error: the error handler runs once and its
    // answer is not negotiated again.
    for (int connection = 0; connection != 2; ++connection) {
        error_handler_calls = 0;
        const auto response = exchange(io, server.endpoint(), "/events", refuse_every_coding);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 406 "));
        EXPECT_TRUE(response.ends_with("rejected"));
        EXPECT_EQ(error_handler_calls.load(), 1);
    }
    for (int connection = 0; connection != 2; ++connection) {
        const auto response = exchange(io, server.endpoint(), "/plain", {});
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(response.ends_with("plain"));
    }
    // Every connection drew on the same client-address budget; it is spent. A
    // client that accepts no coding still learns why.
    const auto admitted_limit = "x-ratelimit-limit: " + std::to_string(admitted_requests) + "\r\n";
    for (int connection = 0; connection != 2; ++connection) {
        for (const auto headers : {std::string_view{}, refuse_every_coding}) {
            error_handler_calls = 0;
            const auto response = exchange(io, server.endpoint(), "/plain", headers);
            const auto head = lowercase(response);
            EXPECT_TRUE(response.starts_with("HTTP/1.1 429 "));
            EXPECT_TRUE(head.find("\r\nretry-after: ") != std::string::npos);
            EXPECT_TRUE(head.find("\r\n" + admitted_limit) != std::string::npos);
            EXPECT_TRUE(response.ends_with("rejected"));
            EXPECT_EQ(error_handler_calls.load(), 1);
        }
    }
    server.finish();
}

TEST(Ruvia, http1_server_closes_connections_stalled_past_request_phase_timeouts) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, admission_server_config());
    const auto loopback = asio::ip::address_v4::loopback();
    // Idle: a connection that never sends a request.
    const auto idle = stalled_connection_output(server.endpoint(), loopback, "");
    EXPECT_TRUE(idle.has_value());
    // Initial read: a request head that never completes.
    const auto head = stalled_connection_output(server.endpoint(), loopback, "GET /plain HTTP/1.1\r\nHost: a\r\n");
    EXPECT_TRUE(head.has_value());
    EXPECT_TRUE(head.value_or("").find("plain") == std::string::npos);
    // Payload read: an admitted request whose body stalls. A distinct loopback
    // source address keeps it outside the rate-limit budget of the other case.
    const auto upload = stalled_connection_output(server.endpoint(), asio::ip::make_address_v4("127.0.0.2"),
        "POST /upload HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nab");
    EXPECT_TRUE(upload.has_value());
    EXPECT_TRUE(upload.value_or("").find("uploaded") == std::string::npos);
    EXPECT_TRUE(upload.value_or("").find(" 429 ") == std::string::npos);
    server.finish();
}

TEST(Ruvia, http1_failed_handler_error_response_is_not_renegotiated_into_406) {
    // The 500 for a throwing handler carries no representation of the target
    // resource, so a refused coding must neither replace it with a 406 nor run
    // the error handler again. No compression is configured, so
    // "gzip, identity;q=0" cannot be satisfied either. Distinct loopback source
    // addresses keep these requests outside the other cases' rate-limit budgets.
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, admission_server_config());
    const std::array<std::pair<std::string_view, std::string_view>, 2> cases{{
        {"127.0.0.3", "identity;q=0, *;q=0"},
        {"127.0.0.4", "gzip, identity;q=0"},
    }};
    for (const auto& [source, refused] : cases) {
        error_handler_calls = 0;
        std::string request("GET /boom HTTP/1.1\r\nHost: a\r\nConnection: close\r\nAccept-Encoding: ");
        request.append(refused);
        request.append("\r\n\r\n");
        const auto output =
            stalled_connection_output(server.endpoint(), asio::ip::make_address_v4(std::string(source)), request);
        EXPECT_TRUE(output.has_value());
        EXPECT_TRUE(output.value_or("").starts_with("HTTP/1.1 500 "));
        EXPECT_TRUE(output.value_or("").find("rejected") != std::string::npos);
        EXPECT_EQ(error_handler_calls.load(), 1);
    }
    server.finish();
}

TEST(Ruvia, http1_rate_limits_canonicalize_forwarded_client_addresses) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, admission_server_config());
    const auto loopback = asio::ip::address_v4::loopback();
    // Each pair has its own address budget. The second spelling must consume
    // the first spelling's budget through both independently enforced rules.
    const std::array<std::pair<std::string_view, std::string_view>, 6> equivalent{{
        {"fe80::a%12", "fe80:0:0:0:0:0:0:a%12"},
        {"fe80::b%12", "FE80::B%12"},
        {"fe80::c%12", "FE80:0:0:0:0:0:0:C%12"},
        {"2001:db8:10::1", "2001:db8:10::ffff"},
        {"192.0.2.10", "::ffff:192.0.2.10"},
        {"::ffff:192.0.2.11", "192.0.2.11"},
    }};
    auto send = [&](std::string_view target, std::string_view address) {
        std::string request("GET ");
        request.append(target);
        request.append(" HTTP/1.1\r\nHost: a\r\nConnection: close\r\nX-Forwarded-For: ");
        request.append(address);
        request.append("\r\n\r\n");
        const auto output = stalled_connection_output(server.endpoint(), loopback, request);
        EXPECT_TRUE(output.has_value());
        return output.value_or("");
    };
    for (const auto& [first, alternate] : equivalent) {
        EXPECT_TRUE(send("/limited", first).starts_with("HTTP/1.1 200 "));
        const auto route_rejection = send("/limited", alternate);
        EXPECT_TRUE(route_rejection.starts_with("HTTP/1.1 429 "));
        EXPECT_TRUE(route_rejection.find("X-RateLimit-Limit: 1\r\n") != std::string::npos);
        // The two requests above also consumed two of the four app-wide
        // admissions. Spend the rest without the route-specific middleware.
        for (std::size_t i = 2; i < admitted_requests; ++i) {
            EXPECT_TRUE(send("/plain", first).starts_with("HTTP/1.1 200 "));
        }
        const auto app_rejection = send("/plain", alternate);
        EXPECT_TRUE(app_rejection.starts_with("HTTP/1.1 429 "));
        EXPECT_TRUE(app_rejection.find("X-RateLimit-Limit: 4\r\n") != std::string::npos);
    }
    // Scoped identities preserve all 128 address bits and the numeric scope,
    // and cannot collide with the separate unscoped /64 namespace.
    for (const auto address : {"fe80::a%13", "fe80::d%12", "fe80::a", "2001:db8:11::1", "192.0.2.12"}) {
        EXPECT_TRUE(send("/limited", address).starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(send("/limited", address).starts_with("HTTP/1.1 429 "));
        for (std::size_t i = 2; i < admitted_requests; ++i) {
            EXPECT_TRUE(send("/plain", address).starts_with("HTTP/1.1 200 "));
        }
        EXPECT_TRUE(send("/plain", address).starts_with("HTTP/1.1 429 "));
    }
    server.finish();
}

// A request body that arrived in full with its head costs no keep-alive when
// the response never reads it -- an unresolved target, a handler or stream
// that ignores it, or an app-wide 429 -- so the request pipelined behind it is
// still answered. A body still incomplete closes the connection instead.
// Distinct loopback source addresses give each case its own rate-limit budget.
TEST(Ruvia, http1_unread_buffered_body_keeps_the_connection) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, admission_server_config());
    const auto output_from = [&](std::string_view source, std::string_view requests) {
        const auto output =
            stalled_connection_output(server.endpoint(), asio::ip::make_address_v4(std::string(source)), requests);
        EXPECT_TRUE(output.has_value());
        return output.value_or("");
    };
    const std::string next("GET /plain HTTP/1.1\r\nHost: a\r\n\r\n");
    const std::string length_body("Content-Length: 5\r\n\r\nhello");

    EXPECT_EQ(
        status_codes(output_from("127.0.0.10", "POST /missing HTTP/1.1\r\nHost: a\r\n" + length_body + next)),
        std::string("404200"));
    EXPECT_EQ(status_codes(output_from("127.0.0.11",
                  "POST /plain HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
                  "5\r\nhello\r\n0\r\n\r\n" +
                      next)),
        std::string("405200"));
    EXPECT_EQ(
        status_codes(output_from("127.0.0.12", "POST /ignore HTTP/1.1\r\nHost: a\r\n" + length_body + next)),
        std::string("200200"));
    const auto streamed =
        output_from("127.0.0.13", "POST /stream-ignore HTTP/1.1\r\nHost: a\r\n" + length_body + next);
    EXPECT_EQ(status_codes(streamed), std::string("200200"));
    EXPECT_TRUE(streamed.find("event") != std::string::npos);

    // Body bytes still to come: the response closes the connection.
    const auto partial =
        output_from("127.0.0.14", "POST /missing HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\nab");
    EXPECT_EQ(status_codes(partial), std::string("404"));
    EXPECT_TRUE(partial.find("Connection: close\r\n") != std::string::npos);

    // The app-wide limit admits four requests; the 429s that follow keep the
    // connection while the body is in hand, and close it when it is not.
    std::string spent;
    for (std::size_t i = 0; i < admitted_requests; ++i) {
        spent.append(next);
    }
    EXPECT_EQ(
        status_codes(output_from("127.0.0.15", spent + "POST /upload HTTP/1.1\r\nHost: a\r\n" + length_body + next)),
        std::string("200200200200429429"));
    const auto limited_partial =
        output_from("127.0.0.16", spent + "POST /upload HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\nab");
    EXPECT_EQ(status_codes(limited_partial), std::string("200200200200429"));
    EXPECT_TRUE(limited_partial.find("Connection: close\r\n") != std::string::npos);
    server.finish();
}
