// Standalone HTTP/1 and HTTP/2 client: streaming uploads, trailers, body readers,
// interim heads, redirects, push, advertisements, and retained result budgets.
// First run ruvia_example_http_features, then this executable with 1 or 2.
// RUVIA_CLIENT_TLS=true selects TLS on localhost:8444 instead of HTTP on port 8093.
// Set RUVIA_TLS_CA to trust the example server certificate.

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/event_loop_pool.h"
#include "ruvia/http/http_client_redirect.h"
#include "ruvia/web/dotenv.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_client.h"

#include "environment.h"

namespace {

ruvia::task<void> follow_redirect(ruvia::http_client& client, const ruvia::http_client_config& config) {
    std::pmr::unsynchronized_pool_resource memory;
    const auto origin = config.scheme_ == ruvia::http_scheme::https
                            ? ruvia::http_origin_view::https({.host_ = config.host_, .port_ = config.port_})
                            : ruvia::http_origin_view::http({.host_ = config.host_, .port_ = config.port_});
    std::pmr::string target("/features/redirect", &memory);
    const auto requests = client.handle();
    for (unsigned hop = 0; hop != 4; ++hop) {
        auto response = co_await requests.send({.target_ = target});
        if (!ruvia::is_http_client_redirect_status(response.status())) {
            auto bytes_value = co_await response.body().read_all();
            std::cout << "redirected bytes=" << bytes_value.bytes().size() << '\n';
            co_return;
        }
        const auto location = response.header("location");
        if (!location) {
            throw std::runtime_error("redirect has no Location");
        }
        auto destination = ruvia::resolve_http_client_redirect_target(origin,
            {.current_target_ = target, .location_ = *location, .resource_ = &memory});
        const auto* resolved = destination.resolved();
        if (!resolved || resolved->cross_origin()) {
            // Origins are fixed at startup. Following another origin requires
            // a preconfigured client and stripping origin-specific credentials.
            throw std::runtime_error("this example only follows same-origin redirects");
        }
        target.assign(resolved->target());
        // Drain the old response before reusing its HTTP/1 connection. Abandoning
        // a partially consumed HTTP/1 response closes that connection instead.
        while (co_await response.body().read()) {
        }
    }
    throw std::runtime_error("redirect hop limit exceeded");
}

ruvia::task<ruvia::http_client_response_bytes> exercise(
    ruvia::event_loop loop, ruvia::http_client_config config) {
    // Construction performs no I/O. A client owns one fixed origin and binds
    // to this loop; its first request lazily establishes the connection.
    ruvia::http_client client(loop, config,
        {.max_retained_bytes_ = 1024 * 1024, .max_in_flight_bytes_ = 4 * 1024 * 1024});
    std::optional<ruvia::http_client_response_bytes> retained;
    std::exception_ptr failure;
    try {
        auto bounded = client.with_options({.timeout_ = std::chrono::seconds(5)});
        {
            const std::array<ruvia::http_header_view, 1> headers{{
                {"content-type", "application/octet-stream"},
            }};
            // With no content_length, HTTP/1 uses chunked transfer coding;
            // HTTP/2 carries the same body and trailers in DATA and HEADERS.
            // A 100-continue expectation lets a peer reject the head first.
            std::cout << "upload\n";
            auto exchange_value = co_await bounded.open_request(
                {.method_ = "POST", .target_ = "/features/upload", .headers_ = headers},
                {.expectation_ = ruvia::http_client_request_expectation::continue_value, .max_chunk_bytes_ = 1024});
            co_await exchange_value.body().write("hello ");
            co_await exchange_value.body().write("world!");
            const std::array<ruvia::http_header_view, 1> trailers{{{"X-Upload-Complete", "yes"}}};
            co_await exchange_value.body().end(trailers);
            auto response = co_await exchange_value.response();
            while (const auto text = co_await response.body().text()) {
                // Each view expires at the next body operation. Do not retain
                // it or assume an entire UTF-8 character is in one chunk.
                std::cout << *text;
            }
        }
        {
            std::cout << "informational response and push\n";
            auto response = co_await bounded.send({.target_ = "/features/info"});
            std::cout << "interim heads=" << response.informational_responses().size() << '\n';
            if (response.protocol_version() == ruvia::http_protocol_version::http2) {
                response.reprioritize({.urgency_ = 1, .incremental_ = true});
            }
            while (co_await response.body().read()) {
            }
            while (auto push = client.next_push()) {
                std::cout << "push=" << push->request().path_ << '\n';
                auto pushed = co_await push->response();
                auto content = co_await pushed.body().read_all(4096);
                std::cout << "pushed bytes=" << content.bytes().size() << '\n';
            }
            while (auto advertisement = client.next_advertisement()) {
                std::cout << "advertisement: origins=" << (advertisement->origins() != nullptr)
                          << " alt-svc=" << (advertisement->alternative_service() != nullptr) << '\n';
            }
        }
        co_await follow_redirect(client, config);
        {
            auto response = co_await bounded.send({.target_ = "/features/safe"});
            while (co_await response.body().read()) {
            }
        }
        {
            auto response = co_await bounded.send({.target_ = "/features/download"});
            // read(), text(), read_all(), and pipe_to() consume ONE linear body
            // reader. read_all() collects only its unread remainder, with both
            // a per-call limit and a total retained-result budget.
            retained.emplace(co_await response.body().read_all(4096));
            if (response.trailer("X-Example-Complete") != "yes") {
                throw std::runtime_error("missing response trailer");
            }
        }
        std::cout << "completed requests=" << client.stats().completed_requests_ << '\n';
    } catch (...) {
        failure = std::current_exception();
    }
    // Response metadata, pushes, advertisements, and borrowed chunks must be
    // destroyed on this loop. http_client_response_bytes is the owned exception:
    // it can survive the client and the loop and can cross threads.
    co_await client.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
    co_return std::move(*retained);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::cout << std::unitbuf;
        const std::string_view protocol = argc > 1 ? argv[1] : "1";
        if (protocol != "1" && protocol != "2") {
            throw std::invalid_argument("usage: ruvia_example_http_client_advanced [1|2]");
        }
        const example::environment env;
        const bool tls = env.get<bool>("RUVIA_CLIENT_TLS").value_or(false);
        ruvia::http_client_config config{
            .scheme_ = tls ? ruvia::http_scheme::https : ruvia::http_scheme::http,
            .host_ = "localhost",
            .port_ = tls ? 8444 : 8093,
            .protocol_ = protocol == "2" ? ruvia::http_client_protocol::http2_only
                                         : ruvia::http_client_protocol::http1_only,
            .advertisements_ = {.receive_origins_ = tls, .receive_alternative_services_ = tls},
            .push_ = {.enabled_ = true},
            .tls_ = {.ca_file_ = std::string(env.get("RUVIA_TLS_CA").value_or(""))},
            .received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send,
        };
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        loops.start();
        auto done = loops.loop(0).start(exercise(loops.loop(0), std::move(config)));
        auto body = done.get();
        loops.stop();
        loops.join();
        const auto bytes_value = body.bytes();
        std::cout.write(reinterpret_cast<const char*>(bytes_value.data()), static_cast<std::streamsize>(bytes_value.size()));
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
