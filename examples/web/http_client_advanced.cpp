// Standalone HTTP client covering streaming uploads, trailers, body readers,
// interim heads, redirects, push, advertisements, result budgets, and QUIC migration.
// First run ruvia_example_http_features. Run this executable with 1, 2, or 3.
// Protocol 1/2 uses localhost:8093; RUVIA_CLIENT_TLS=true selects TLS port 8444.
// Protocol 3 requires that server's TLS setup and RUVIA_TLS_CA for its local CA.
// Optional HTTP/3 flags: RUVIA_QUIC_V2=true, RUVIA_EARLY_DATA=true,
// RUVIA_MIGRATE=true (validate a new local UDP endpoint, then download through
// the same connection; Ruvia servers allow client migration by default).
// Successful runs print uploaded=12 and the downloaded chunks. Upload trailers
// are supported on HTTP/2 and HTTP/3; HTTP/1 prints trailers=0.

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>

#include <asio/ip/udp.hpp>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/Timer.h"
#include "ruvia/http/HttpClientRedirect.h"
#include "ruvia/web/Dotenv.h"
#include "ruvia/web/HttpClient.h"

#include "environment.h"

namespace {

ruvia::Task<void> follow_redirect(ruvia::HttpClient& client, const ruvia::HttpClientConfig& config) {
    std::pmr::unsynchronized_pool_resource memory;
    const auto origin = config.scheme == ruvia::HttpScheme::kHttps
                            ? ruvia::HttpOriginView::https({.host = config.host, .port = config.port})
                            : ruvia::HttpOriginView::http({.host = config.host, .port = config.port});
    std::pmr::string target("/features/redirect", &memory);
    for (unsigned hop = 0; hop != 4; ++hop) {
        auto response = co_await client.send({.target = target});
        if (!ruvia::isHttpClientRedirectStatus(response.status())) {
            auto bytes = co_await response.body().readAll();
            std::cout << "redirected bytes=" << bytes.bytes().size() << '\n';
            co_return;
        }
        const auto location = response.header("location");
        if (!location) {
            throw std::runtime_error("redirect has no Location");
        }
        auto destination = ruvia::resolveHttpClientRedirectTarget(origin,
            {.currentTarget = target, .location = *location, .resource = &memory});
        const auto* resolved = destination.resolved();
        if (!resolved || resolved->crossOrigin()) {
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

ruvia::Task<void> migrate(ruvia::HttpClient& client, ruvia::EventLoop loop) {
    // Migration requires a concrete address and nonzero port in the existing
    // path's address family. Reserve an available loopback port for this demo,
    // then release it so the client can bind its replacement socket there.
    asio::ip::udp::socket reservation(loop.ioContext(),
        {asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();
    auto migration = client.start_quic_path_migration(endpoint);
    if (migration.status == ruvia::quic_migration_status::rejected) {
        // A peer can disable active migration. Rejection leaves the current
        // connection usable, as the download below demonstrates.
        std::cout << "QUIC migration rejected; continuing on the current path\n";
        co_return;
    }
    for (unsigned attempt = 0; migration.status == ruvia::quic_migration_status::started && attempt != 100; ++attempt) {
        if (co_await ruvia::sleepFor(client.worker(), std::chrono::milliseconds(20)) == ruvia::TimerSleepResult::kStopRequested) {
            break;
        }
        const auto state = client.path_migration(migration.id);
        if (!state) {
            throw std::runtime_error("migration state retired");
        }
        migration = *state;
    }
    if (migration.status == ruvia::quic_migration_status::started) {
        // Cancelling a submitted migration closes that connection and its
        // requests. shutdown() below still joins its transport work.
        (void)client.cancel_quic_path_migration(migration.id);
        throw std::runtime_error("migration validation did not finish");
    }
    if (migration.status != ruvia::quic_migration_status::validated) {
        throw std::runtime_error("migration rejected or failed");
    }
    std::cout << "QUIC path validated\n";
}

ruvia::Task<ruvia::HttpClientResponseBytes> exercise(
    ruvia::EventLoop loop, ruvia::HttpClientConfig config, bool migrate_path) {
    // Construction performs no I/O. A client owns one fixed origin and binds
    // to this loop; its first request lazily establishes the connection.
    ruvia::HttpClient client(loop, config,
        {.maxRetainedBytes = 1024 * 1024, .max_in_flight_bytes = 4 * 1024 * 1024});
    std::optional<ruvia::HttpClientResponseBytes> retained;
    std::exception_ptr failure;
    try {
        auto bounded = client.withOptions({.timeout = std::chrono::seconds(5)});
        {
            const std::array<ruvia::HttpHeaderView, 2> headers{{
                {"content-type", "application/octet-stream"},
                {"trailer", "X-Upload-Complete"},
            }};
            // With no contentLength, HTTP/1 uses chunks; HTTP/2/3 use DATA frames.
            // A 100-continue expectation lets a peer reject the head first.
            const bool upload_trailers = config.protocol != ruvia::HttpClientProtocol::kHttp1Only;
            std::cout << "upload\n";
            auto exchange = co_await bounded.openRequest(
                {.method = "POST", .target = "/features/upload", .headers = std::span(headers).first(upload_trailers ? 2 : 1)},
                {.expectation = ruvia::HttpClientRequestExpectation::kContinue, .maxChunkBytes = 1024});
            co_await exchange.body().write("hello ");
            co_await exchange.body().write("world!");
            const std::array<ruvia::HttpHeaderView, 1> trailers{{{"X-Upload-Complete", "yes"}}};
            co_await exchange.body().end(upload_trailers ? std::span<const ruvia::HttpHeaderView>(trailers) : std::span<const ruvia::HttpHeaderView>{});
            auto response = co_await exchange.response();
            while (const auto text = co_await response.body().text()) {
                // Each view expires at the next body operation. Do not retain
                // it or assume an entire UTF-8 character is in one chunk.
                std::cout << *text;
            }
        }
        {
            std::cout << "informational response and push\n";
            auto response = co_await bounded.send({.target = "/features/info"});
            std::cout << "interim heads=" << response.informationalResponses().size() << '\n';
            if (config.protocol != ruvia::HttpClientProtocol::kHttp1Only) {
                response.reprioritize({.urgency = 1, .incremental = true});
            }
            while (co_await response.body().read()) {
            }
            while (auto push = client.nextPush()) {
                std::cout << "push=" << push->request().path << '\n';
                auto pushed = co_await push->response();
                auto content = co_await pushed.body().readAll(4096);
                std::cout << "pushed bytes=" << content.bytes().size() << '\n';
            }
            while (auto advertisement = client.nextAdvertisement()) {
                std::cout << "advertisement: origins=" << (advertisement->origins() != nullptr)
                          << " alt-svc=" << (advertisement->alternativeService() != nullptr) << '\n';
            }
        }
        co_await follow_redirect(client, config);
        {
            // TLS rejection of 0-RTT is replayed in 1-RTT by the transport.
            // HTTP 425 remains an application response. Never opt writes in.
            auto response = co_await bounded.send({.target = "/features/safe", .replay_safe = true});
            std::cout << "local early data=" << response.header("X-Local-Early-Data").value_or("0") << '\n';
            while (co_await response.body().read()) {
            }
        }
        if (migrate_path) {
            co_await migrate(client, loop);
        }
        {
            auto response = co_await bounded.send({.target = "/features/download"});
            // read(), text(), readAll(), and pipeTo() consume ONE linear body
            // reader. readAll() collects only its unread remainder, with both
            // a per-call limit and a total retained-result budget.
            retained.emplace(co_await response.body().readAll(4096));
            if (response.trailer("X-Example-Complete") != "yes") {
                throw std::runtime_error("missing response trailer");
            }
        }
        std::cout << "completed requests=" << client.stats().completedRequests << '\n';
    } catch (...) {
        failure = std::current_exception();
    }
    // Response metadata, pushes, advertisements, and borrowed chunks must be
    // destroyed on this loop. HttpClientResponseBytes is the owned exception:
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
        if (protocol != "1" && protocol != "2" && protocol != "3") {
            throw std::invalid_argument("usage: ruvia_example_http_client_advanced [1|2|3]");
        }
        const example::environment env;
        const bool tls = protocol == "3" || env.get<bool>("RUVIA_CLIENT_TLS").value_or(false);
        const bool migration = protocol == "3" && env.get<bool>("RUVIA_MIGRATE").value_or(false);
        ruvia::HttpClientConfig config{
            .scheme = tls ? ruvia::HttpScheme::kHttps : ruvia::HttpScheme::kHttp,
            .host = "localhost",
            .port = tls ? 8444 : 8093,
            .protocol = protocol == "3"   ? ruvia::HttpClientProtocol::kHttp3Only
                        : protocol == "2" ? ruvia::HttpClientProtocol::kHttp2Only
                                          : ruvia::HttpClientProtocol::kHttp1Only,
            .initial_quic_version = env.get<bool>("RUVIA_QUIC_V2").value_or(false) ? ruvia::quic_version::v2 : ruvia::quic_version::v1,
            .http3_early_data = env.get<bool>("RUVIA_EARLY_DATA").value_or(false),
            .advertisements = {.receiveOrigins = tls, .receiveAlternativeServices = tls},
            .push = {.enabled = true},
            .receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend,
            .caFile = std::string(env.get("RUVIA_TLS_CA").value_or("")),
        };
        ruvia::EventLoopPool loops({.loopCount = 1});
        loops.start();
        auto done = loops.loop(0).start(exercise(loops.loop(0), std::move(config), migration));
        auto body = done.get();
        loops.stop();
        loops.join();
        const auto bytes = body.bytes();
        std::cout.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
