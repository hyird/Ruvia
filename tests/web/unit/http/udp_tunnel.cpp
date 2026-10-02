#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/web/HttpClient.h"
#include "ruvia/web/HttpUdpTunnel.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/NativeAcceptedSocketTicket.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace {
ruvia::Task<void> udpEcho(void*, ruvia::Context& context) {
    ruvia::HttpUdpTunnel udp(context.tunnel().capsules());
    while (auto datagram = co_await udp.read()) {
        co_await udp.send(datagram->payload());
    }
    co_await udp.finish();
}
ruvia::Task<ruvia::HttpResponse> udpSibling(void*, ruvia::Context& context) {
    co_return context.text("sibling");
}
ruvia::Task<void> forwardConnections(asio::ip::tcp::acceptor& acceptor, ruvia::detail::WebWorkerRuntime& server) {
    for (;;) {
        auto accepted = co_await ruvia::asyncAsio<asio::ip::tcp::socket>([&](auto done) { acceptor.async_accept(std::move(done)); });
        if (accepted.errorCode() == asio::error::operation_aborted) {
            co_return;
        }
        if (accepted.errorCode()) {
            throw std::system_error(accepted.errorCode());
        }
        auto socket = std::move(accepted.result());
        std::error_code error;
        const auto native = socket.release(error);
        if (error) {
            throw std::system_error(error);
        }
        ruvia::detail::NativeAcceptedSocketTicket ticket(asio::ip::tcp::v4(), 0, native);
        if (!server.networkSubmission().post([&server, ticket = std::move(ticket)]() mutable {
                                           server.acceptTransferredConnection(std::move(ticket));
                                       })
                .accepted()) {
            throw std::runtime_error("UDP tunnel socket dispatch rejected");
        }
    }
}
}  // namespace

RUVIA_TEST(http_udp_tunnel_negotiates_http1_upgrade_and_http2_extended_connect_and_owns_results) {
    for (const auto protocol : {ruvia::HttpClientProtocol::kHttp1Only, ruvia::HttpClientProtocol::kHttp2Only}) {
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io);
        ruvia::detail::Router router;
        auto& routes = ruvia::detail::RouterImpl::from(router);
        routes.registerTunnelRoute("connect-udp", std::pmr::string("/udp/:host/:port"), {nullptr, udpEcho}, {}, {});
        routes.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/sibling"), {nullptr, udpSibling}, ruvia::detail::RequestBodyMode::kBuffered, {}, {});
        routes.finalize();
        ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), routes.routeTable());
        server.start();
        std::exception_ptr failure;
        auto run = [&]() -> ruvia::Task<void> {
            const auto worker = attachment.loop().handle();
            asio::ip::tcp::acceptor source(io, {asio::ip::address_v4::loopback(), 0});
            ruvia::TaskScope forwarding(worker);
            forwarding.spawn(forwardConnections(source, server));
            std::optional<ruvia::HttpUdpDatagram> retained;
            {
                ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1", .port = source.local_endpoint().port(), .connectionCount = 1, .requestTimeout = std::chrono::seconds(5), .maxResponseBytes = 16384, .protocol = protocol});
                try {
                    std::string target = "/udp/target.test/443";
                    auto operation = client.openUdpTunnel({.target = target}, {.maxChunkBytes = 1024});
                    target.assign("mutated");
                    auto result = co_await std::move(operation);
                    if (!result.tunnel()) {
                        throw std::runtime_error("CONNECT-UDP rejected");
                    }
                    RUVIA_CHECK(result.tunnel()->status().value() == (protocol == ruvia::HttpClientProtocol::kHttp1Only ? 101 : 200));
                    RUVIA_CHECK(result.tunnel()->header("capsule-protocol") == "?1");
                    RUVIA_CHECK(!result.tunnel()->header("content-length"));
                    if (protocol == ruvia::HttpClientProtocol::kHttp1Only) {
                        RUVIA_CHECK(result.tunnel()->header("upgrade") == "connect-udp");
                    } else {
                        RUVIA_CHECK(!result.tunnel()->header("connection"));
                    }
                    auto udp = std::move(*result.tunnel()).udp();
                    std::string payload(16003, 'u');
                    auto send = udp.send(payload);
                    payload.assign("mutated");
                    co_await std::move(send);
                    retained = co_await udp.read();
                    RUVIA_CHECK(retained && retained->payload().size() == 16003);
                    RUVIA_CHECK(retained && std::ranges::all_of(retained->payload(), [](std::byte value) { return value == std::byte{'u'}; }));
                    co_await udp.send("");
                    auto empty = co_await udp.read();
                    RUVIA_CHECK(empty && empty->payload().empty());
                    co_await udp.finish();
                    RUVIA_CHECK(!(co_await udp.read()));
                    auto rejected = co_await client.openUdpTunnel({.target = "/missing"});
                    RUVIA_CHECK(!rejected.tunnel() && rejected.response());
                    if (!rejected.response()) {
                        throw std::runtime_error("missing UDP tunnel rejection");
                    }
                    RUVIA_CHECK(rejected.response()->status() == ruvia::http_status::kNotFound);
                    auto denial = co_await rejected.response()->body().readAll();
                    RUVIA_CHECK(!denial.bytes().empty());
                    if (protocol == ruvia::HttpClientProtocol::kHttp1Only) {
                        const ruvia::HttpHeaderView malformed[]{{"Connection", "Upgrade"}, {"Upgrade", "connect-udp"}};
                        auto bad = co_await client.send({.target = "/udp/target.test/443", .headers = malformed});
                        RUVIA_CHECK(bad.status() == ruvia::http_status::kBadRequest);
                        auto errorBody = co_await bad.body().readAll();
                        RUVIA_CHECK(!errorBody.bytes().empty());
                    }
                    {
                        auto second = co_await client.openUdpTunnel({.target = "/udp/target.test/443"});
                        if (!second.tunnel()) {
                            throw std::runtime_error("second CONNECT-UDP rejected");
                        }
                        auto blocked = std::move(*second.tunnel()).udp();
                        bool failed{};
                        ruvia::TaskScope reads(worker);
                        auto receive = [&]() -> ruvia::Task<void> {
                            try {
                                (void)co_await blocked.read();
                            } catch (const ruvia::HttpClientError&) {
                                failed = true;
                            }
                        };
                        reads.spawn(receive());
                        co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1));
                        blocked.abort();
                        co_await reads.join();
                        RUVIA_CHECK(failed);
                    }
                    auto sibling = co_await client.send({.target = "/sibling"});
                    std::string body;
                    while (auto bytes = co_await sibling.body().text()) {
                        body.append(*bytes);
                    }
                    RUVIA_CHECK(body == "sibling");
                } catch (...) {
                    failure = std::current_exception();
                }
                co_await client.shutdown();
            }
            RUVIA_CHECK(retained && retained->payload().size() == 16003);
            retained.reset();
            std::error_code ignored;
            source.close(ignored);
            try {
                co_await forwarding.join();
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
        server.stop();
        server.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
}
