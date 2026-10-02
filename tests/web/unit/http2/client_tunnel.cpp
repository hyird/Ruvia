#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/HttpClient.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"
#include "ruvia/web/detail/router/RouterImpl.h"

#include "http2_sansio_session_fixture.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct TunnelObservation {
    std::string bytes;
    bool finishFirst{};
    bool eof{};
};
ruvia::Task<void> clientTunnelEcho(void* raw, ruvia::Context& c) {
    auto& observation = *static_cast<TunnelObservation*>(raw);
    observation.bytes.clear();
    observation.eof = false;
    auto& tunnel = c.tunnel();
    if (observation.finishFirst) {
        co_await tunnel.write("greeting");
        co_await tunnel.finish();
    }
    while (auto bytes = co_await tunnel.read()) {
        observation.bytes.append(*bytes);
        if (!observation.finishFirst) {
            co_await tunnel.write(std::string_view(*bytes));
        }
    }
    observation.eof = true;
    co_await tunnel.finish();
}
ruvia::Task<ruvia::HttpResponse> clientTunnelSibling(void*, ruvia::Context& c) {
    co_return c.text("sibling");
}
ruvia::Task<void> serveClientTunnel(asio::ip::tcp::acceptor& acceptor, const ruvia::WorkerHandle& worker, const ruvia::detail::RouteTable& routes, ruvia::WorkerMemory& memory) {
    auto accepted = co_await ruvia::asyncAsio<asio::ip::tcp::socket>([&](auto h) { acceptor.async_accept(std::move(h)); });
    if (accepted.errorCode()) {
        throw std::system_error(accepted.errorCode());
    }
    auto socket = std::move(accepted.result());
    ruvia::test::Http2SansIoSessionFixture fixture;
    co_await ruvia::detail::runHttp2SansIoSession(socket, routes, memory, fixture.context(fixture.services(worker)));
}
}  // namespace
RUVIA_TEST(http2_client_tunnel_owns_cold_input_half_closes_and_preserves_siblings) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    ruvia::test::CountingMemoryResource allocation;
    std::exception_ptr failure;
    std::optional<ruvia::HttpClientTunnel> retained_tunnel;
    auto run = [&]() -> ruvia::Task<void> {
        const auto& worker = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
        ruvia::WorkerMemory memory(allocation);
        TunnelObservation observation;
        ruvia::detail::Router router;
        auto& routes = ruvia::detail::RouterImpl::from(router);
        routes.registerTunnelRoute({}, std::pmr::string("target.test:443"), {&observation, clientTunnelEcho}, {}, {});
        routes.registerTunnelRoute("test-protocol", std::pmr::string("/tunnel"), {&observation, clientTunnelEcho}, {}, {});
        routes.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/sibling"), {nullptr, clientTunnelSibling}, ruvia::detail::RequestBodyMode::kBuffered, {}, {});
        routes.finalize();
        ruvia::TaskScope tasks(worker);
        tasks.spawn(serveClientTunnel(acceptor, worker, routes.routeTable(), memory));
        ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1", .port = acceptor.local_endpoint().port(), .connectionCount = 1, .requestTimeout = std::chrono::seconds(5), .maxResponseBytes = 16384, .protocol = ruvia::HttpClientProtocol::kHttp2Only});
        try {
            for (unsigned round = 0; round != 4; ++round) {
                observation.finishFirst = round >= 2;
                std::string authority = round == 1 ? "proxy.test" : "target.test:443";
                auto cold = client.openTunnel({.authority = authority, .protocol = round == 1 ? "test-protocol" : "", .target = round == 1 ? "/tunnel" : ""});
                authority.assign("mutated");
                auto result = co_await std::move(cold);
                RUVIA_CHECK(result.response() == nullptr && result.tunnel() != nullptr);
                if (!result.tunnel()) {
                    throw std::runtime_error("CONNECT was rejected");
                }
                auto tunnel = std::move(*result.tunnel());
                RUVIA_CHECK(tunnel.status() == ruvia::http_status::kOk);
                RUVIA_CHECK(!tunnel.header("content-length"));
                std::string echoed;
                const auto receive = [&]() -> ruvia::Task<void> {
                    while (auto bytes = co_await tunnel.read()) {
                        echoed.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                };
                if (observation.finishFirst) {
                    co_await receive();
                    RUVIA_CHECK(echoed == "greeting");
                }
                ruvia::TaskScope readers(worker);
                if (round == 3) {
                    tunnel.abort();
                    bool half_closed_write_rejected = false;
                    try {
                        auto late_write = tunnel.write("late");
                        static_cast<void>(late_write);
                    } catch (const ruvia::HttpClientError& error) {
                        half_closed_write_rejected = error.code() == ruvia::HttpClientError::Code::kCancelled;
                    }
                    RUVIA_CHECK(half_closed_write_rejected);
                    co_await readers.join();
                    auto sibling = co_await client.send({.target = "/sibling"});
                    std::string sibling_body;
                    while (auto bytes = co_await sibling.body().text()) {
                        sibling_body.append(*bytes);
                    }
                    RUVIA_CHECK(sibling_body == "sibling");
                    retained_tunnel.emplace(std::move(tunnel));
                    continue;
                }
                if (!observation.finishFirst) {
                    readers.spawn(receive());
                }
                std::string payload(100003, 't');
                for (std::size_t offset = 0; offset != payload.size();) {
                    const auto count = std::min<std::size_t>(16384, payload.size() - offset);
                    auto output = tunnel.write(std::string_view(payload).substr(offset, count));
                    bool busy = false;
                    try {
                        (void)tunnel.finish();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    RUVIA_CHECK(busy);
                    co_await std::move(output);
                    offset += count;
                }
                co_await tunnel.finish();
                co_await readers.join();
                if (!observation.finishFirst) {
                    RUVIA_CHECK(echoed == payload);
                }
                auto sibling = co_await client.send({.target = "/sibling"});
                std::string siblingBody;
                while (auto bytes = co_await sibling.body().text()) {
                    siblingBody.append(*bytes);
                }
                RUVIA_CHECK(siblingBody == "sibling");
                RUVIA_CHECK(observation.bytes == payload && observation.eof);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (retained_tunnel) {
            retained_tunnel->abort();
            retained_tunnel->abort();
            bool late_write_rejected = false;
            try {
                auto late_write = retained_tunnel->write("late");
                static_cast<void>(late_write);
            } catch (const ruvia::HttpClientError&) {
                late_write_rejected = true;
            }
            RUVIA_CHECK(late_write_rejected);
            retained_tunnel.reset();
        }
        std::error_code ignored;
        acceptor.close(ignored);
        co_await tasks.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(allocation.liveAllocations(), std::size_t{0});
}

namespace {
ruvia::Task<void> capsuleEcho(void*, ruvia::Context& c) {
    auto stream = c.tunnel().capsules();
    while (auto capsule = co_await stream.read()) {
        co_await stream.write(capsule->type(), capsule->payload());
    }
    co_await stream.finish();
}
}  // namespace
RUVIA_TEST(http2_client_capsule_stream_retains_results_and_cold_operations_after_shutdown) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    ruvia::test::CountingMemoryResource allocation;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        const auto& worker = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
        ruvia::WorkerMemory memory(allocation);
        ruvia::detail::Router router;
        auto& routes = ruvia::detail::RouterImpl::from(router);
        routes.registerTunnelRoute("test-capsules", std::pmr::string("/capsules"), {nullptr, capsuleEcho}, {}, {});
        routes.finalize();
        ruvia::TaskScope tasks(worker);
        tasks.spawn(serveClientTunnel(acceptor, worker, routes.routeTable(), memory));
        std::optional<ruvia::HttpCapsule> retained;
        std::unique_ptr<ruvia::ScopedOperation<void>> cold;
        {
            ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1", .port = acceptor.local_endpoint().port(), .connectionCount = 1, .requestTimeout = std::chrono::seconds(5), .maxResponseBytes = 16384, .protocol = ruvia::HttpClientProtocol::kHttp2Only});
            try {
                auto result = co_await client.openTunnel({.authority = "proxy.test", .protocol = "test-capsules", .target = "/capsules"}, {.maxChunkBytes = 1024});
                if (!result.tunnel()) {
                    throw std::runtime_error("capsule CONNECT rejected");
                }
                {
                    auto stream = std::move(*result.tunnel()).capsules();
                    std::string bytes(4099, 'c');
                    auto write = stream.write(0x123456789ULL, bytes);
                    bytes.assign("mutated");
                    co_await std::move(write);
                    retained = co_await stream.read();
                    RUVIA_CHECK(retained && retained->type() == 0x123456789ULL && retained->payload() == std::string(4099, 'c'));
                    co_await stream.write(0, "");
                    auto empty = co_await stream.read();
                    RUVIA_CHECK(empty && empty->type() == 0 && empty->payload().empty());
                    co_await stream.finish();
                    RUVIA_CHECK(!(co_await stream.read()));
                    // Discarding a cold read expires its scope without running it.
                    auto pending = stream.read();
                }
                auto second = co_await client.openTunnel({.authority = "proxy.test", .protocol = "test-capsules", .target = "/capsules"});
                if (!second.tunnel()) {
                    throw std::runtime_error("second capsule CONNECT rejected");
                }
                {
                    auto stream = std::move(*second.tunnel()).capsules();
                    cold.reset(new ruvia::ScopedOperation<void>(stream.write(12, std::string(4099, 'x'))));
                }
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        RUVIA_CHECK(retained && retained->payload() == std::string(4099, 'c'));
        cold.reset();
        retained.reset();
        std::error_code ignored;
        acceptor.close(ignored);
        try {
            co_await tasks.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(allocation.liveAllocations(), std::size_t{0});
}
