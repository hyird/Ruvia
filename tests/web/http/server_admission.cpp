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

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/streaming.h"

#include "http_server_fixture.h"
#include "test_harness.h"
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
    c.status(error.status());
    co_return c.text("rejected");
}

[[maybe_unused]] const bool application_configured = [] {
    ruvia::app().rate_limit(ruvia::rate_limit_config{
        .rule_ = {.max_requests_ = admitted_requests, .window_ = std::chrono::minutes(10)}});
    ruvia::app().on_error(&counted_error);
    return true;
}();

class admission_routes final {
    RUVIA_ROUTES_BEGIN(admission_routes)
    RUVIA_GET("/plain", plain);
    RUVIA_POST("/upload", upload);
    RUVIA_GET("/events", events);
    RUVIA_GET("/boom", boom);
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
        co_await context_value.stream().write("event");
    }
};

ruvia::task<std::string> collect_text(ruvia::http_client_response& response) {
    const auto bytes_value = co_await response.body().read_all();
    const auto view = bytes_value.bytes();
    co_return std::string(reinterpret_cast<const char*>(view.data()), view.size());
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

}  // namespace

RUVIA_TEST(http1_clients_share_406_admission_and_rate_limit_budget) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io, admission_server_config());
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client first(attachment.loop(), {.scheme_ = ruvia::http_scheme::http,
                                                        .host_ = "127.0.0.1",
                                                        .port_ = server.endpoint().port()});
        ruvia::http_client second(attachment.loop(), {.scheme_ = ruvia::http_scheme::http,
                                                         .host_ = "127.0.0.1",
                                                         .port_ = server.endpoint().port()});
        const std::array<ruvia::http_client*, 2> clients{&first, &second};
        const std::array<ruvia::http_header_view, 1> refuse_every_coding{{{"Accept-Encoding", "identity;q=0, *;q=0"}}};
        std::exception_ptr failure;
        try {
            // A response stream commits its representation before a status can
            // be inspected, so a client that accepts no coding gets 406 first.
            // The rejection is a terminal error: the error handler runs once
            // and its answer is not negotiated again.
            for (auto* client : clients) {
                error_handler_calls = 0;
                auto response = co_await client->handle().send({.target_ = "/events", .headers_ = refuse_every_coding});
                RUVIA_CHECK(response.status() == ruvia::http_status::not_acceptable);
                RUVIA_CHECK((co_await collect_text(response)) == "rejected");
                RUVIA_CHECK_EQ(error_handler_calls.load(), 1);
            }
            for (auto* client : clients) {
                auto response = co_await client->handle().send({.target_ = "/plain"});
                RUVIA_CHECK(response.status() == ruvia::http_status::ok);
                RUVIA_CHECK((co_await collect_text(response)) == "plain");
            }
            // Both connections drew on the same client-address budget; it is
            // spent. A client that accepts no coding still learns why.
            const std::array<std::span<const ruvia::http_header_view>, 2> header_sets{
                std::span<const ruvia::http_header_view>{}, refuse_every_coding};
            for (auto* client : clients) {
                for (const auto headers : header_sets) {
                    error_handler_calls = 0;
                    auto response = co_await client->handle().send({.target_ = "/plain", .headers_ = headers});
                    RUVIA_CHECK(response.status() == ruvia::http_status::too_many_requests);
                    RUVIA_CHECK(response.header("Retry-After").has_value());
                    RUVIA_CHECK((co_await collect_text(response)) == "rejected");
                    RUVIA_CHECK_EQ(error_handler_calls.load(), 1);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await first.shutdown();
        co_await second.shutdown();
        server.finish();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http1_server_closes_connections_stalled_past_request_phase_timeouts) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, admission_server_config());
    const auto loopback = asio::ip::address_v4::loopback();
    // Idle: a connection that never sends a request.
    const auto idle = stalled_connection_output(server.endpoint(), loopback, "");
    RUVIA_CHECK(idle.has_value());
    // Initial read: a request head that never completes.
    const auto head = stalled_connection_output(server.endpoint(), loopback, "GET /plain HTTP/1.1\r\nHost: a\r\n");
    RUVIA_CHECK(head.has_value());
    RUVIA_CHECK(head.value_or("").find("plain") == std::string::npos);
    // Payload read: an admitted request whose body stalls. A distinct loopback
    // source address keeps it outside the rate-limit budget of the other case.
    const auto upload = stalled_connection_output(server.endpoint(), asio::ip::make_address_v4("127.0.0.2"),
        "POST /upload HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nab");
    RUVIA_CHECK(upload.has_value());
    RUVIA_CHECK(upload.value_or("").find("uploaded") == std::string::npos);
    RUVIA_CHECK(upload.value_or("").find(" 429 ") == std::string::npos);
    server.finish();
}

RUVIA_TEST(http1_failed_handler_error_response_is_not_renegotiated_into_406) {
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
        RUVIA_CHECK(output.has_value());
        RUVIA_CHECK(output.value_or("").starts_with("HTTP/1.1 500 "));
        RUVIA_CHECK(output.value_or("").find("rejected") != std::string::npos);
        RUVIA_CHECK_EQ(error_handler_calls.load(), 1);
    }
    server.finish();
}
