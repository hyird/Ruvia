#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <string>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/Socket.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/Http2Framing.h"

#include "http2/Http2SansIoSession.h"
#include "http2_sansio_session_fixture.h"
#include "memory_resource_fixture.h"
#include "router/RouterImpl.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct ConnectObservation {
    std::string received;
    bool sawEof{};
    bool stable{true};
    bool returnEarly{};
};
ruvia::Task<void> echoTunnel(void* raw, ruvia::Context& context) {
    auto& observation = *static_cast<ConnectObservation*>(raw);
    if (observation.returnEarly) {
        co_return;
    }
    std::optional<std::pmr::string> retained;
    auto& tunnel = context.tunnel();
    {
        auto cold = tunnel.read();
    }
    while (auto chunk = co_await tunnel.read()) {
        observation.received.append(*chunk);
        if (!retained) {
            retained.emplace(*chunk, context.pool());
        }
        auto output = tunnel.write(std::string_view(*chunk));
        chunk->assign("changed-input");
        co_await std::move(output);
        observation.stable = observation.stable && retained->find_first_not_of('t') == std::string_view::npos;
    }
    observation.sawEof = true;
    co_await tunnel.finish();
}
ruvia::Task<ruvia::HttpResponse> afterTunnel(void*, ruvia::Context& context) {
    co_return context.text("sibling");
}
ruvia::Task<void> serveConnect(asio::ip::tcp::acceptor& acceptor, const ruvia::WorkerHandle& worker,
    const ruvia::detail::RouteTable& routes, ruvia::WorkerMemory& memory) {
    auto accepted = co_await ruvia::asyncAsio<asio::ip::tcp::socket>([&](auto completion) { acceptor.async_accept(std::move(completion)); });
    if (accepted.errorCode()) {
        throw std::system_error(accepted.errorCode());
    }
    auto socket = std::move(accepted.result());
    ruvia::test::Http2SansIoSessionFixture fixture;
    ruvia::ConnectionScanner scanner(worker, {.scanInterval = std::chrono::milliseconds(5)});
    ruvia::ConnectionScanner::Guard guard(&scanner, fixture.scannerEntry, socket);
    scanner.start();
    co_await ruvia::detail::runHttp2SansIoSession(socket, routes, memory, fixture.context(fixture.services(worker)));
    scanner.stop();
}
ruvia::Task<void> flushPeer(asio::ip::tcp::socket& socket, ruvia::Http2Connection& connection, std::pmr::memory_resource* resource) {
    std::pmr::string bytes(resource);
    while (connection.wantsWrite()) {
        bytes.clear();
        (void)connection.takeOutputBatch(16384, bytes);
        const auto completion = co_await ruvia::asyncAsio<std::size_t>([&](auto handler) { asio::async_write(socket, asio::buffer(bytes), std::move(handler)); });
        if (completion.errorCode()) {
            throw std::system_error(completion.errorCode());
        }
    }
}
}  // namespace
RUVIA_TEST(http2ConnectRoutesEchoLargeDuplexStreamsAndLeaveSiblingRequestsAvailable) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    ruvia::test::CountingMemoryResource allocations;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        const auto& worker = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::closeSocket(socket); } });
        ruvia::WorkerMemory memory(allocations);
        ruvia::detail::Router router;
        auto& routes = ruvia::detail::RouterImpl::from(router);
        ConnectObservation observation;
        routes.registerTunnelRoute({}, std::pmr::string("target.test:443"), {&observation, &echoTunnel}, {}, {},
            {.peerTransportFinTimeout = std::chrono::milliseconds(40)});
        routes.registerTunnelRoute("test-tunnel", std::pmr::string("/udp/:host/:port"), {&observation, &echoTunnel}, {}, {});
        routes.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/after"), {nullptr, &afterTunnel}, ruvia::detail::RequestBodyMode::kBuffered, {}, {});
        routes.finalize();
        ruvia::TaskScope tasks(worker);
        tasks.spawn(serveConnect(acceptor, worker, routes.routeTable(), memory));
        try {
            const auto connected = co_await ruvia::asyncAsio([&](auto handler) { socket.async_connect(acceptor.local_endpoint(), std::move(handler)); });
            if (connected.errorCode()) {
                throw std::system_error(connected.errorCode());
            }
            auto peer = ruvia::Http2Connection::client({.resource = memory.resource()});
            std::array<char, 16384> input{};
            std::string receivedWire;
            auto pump = [&]() -> ruvia::Task<void> {
                const auto read = co_await ruvia::asyncAsio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.errorCode()) {
                    throw std::system_error(read.errorCode());
                }
                receivedWire.append(input.data(), read.result());
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::Http2FeedResult::kProtocolFailure) {
                    throw std::runtime_error("CONNECT peer protocol failure");
                }
            };
            co_await flushPeer(socket, peer, memory.resource());
            while (!peer.receivedPeerSettings()) {
                co_await pump();
                while (auto event = peer.nextEvent()) {
                    if (peer.connectionError()) {
                        throw std::runtime_error("CONNECT SETTINGS failure");
                    }
                }
                co_await flushPeer(socket, peer, memory.resource());
            }
            for (unsigned round = 0; round != 3; ++round) {
                observation.returnEarly = round == 2;
                const auto request = round != 1 ? peer.submitRequestHead(ruvia::Http2ConnectRequestHeadView{.authority = "TARGET.TEST:443"})
                                                : peer.submitRequestHead(ruvia::Http2ExtendedConnectRequestHeadView{.protocol = "test-tunnel", .scheme = "https", .authority = "proxy.test", .target = "/udp/target.test/443"});
                if (!request.submitted()) {
                    throw std::runtime_error("CONNECT request submission rejected");
                }
                const auto id = request.submitted()->streamId();
                co_await flushPeer(socket, peer, memory.resource());
                bool established = false;
                while (!established) {
                    co_await pump();
                    while (auto event = peer.nextEvent()) {
                        if (const auto* head = event->responseHead()) {
                            RUVIA_CHECK(head->streamId() == id);
                            RUVIA_CHECK(head->head().status() == ruvia::http_status::kOk);
                            RUVIA_CHECK(std::ranges::none_of(head->head().headers(), [](const auto& header) { return header.name() == "content-length"; }));
                            established = true;
                        }
                        if (event->streamClosed() || peer.connectionError()) {
                            throw std::runtime_error("CONNECT handshake reset");
                        }
                    }
                    co_await flushPeer(socket, peer, memory.resource());
                }
                const std::string payload(100003, 't');
                if (round == 2) {
                    bool reset = false;
                    while (!reset) {
                        co_await pump();
                        while (auto event = peer.nextEvent()) {
                            if (event->streamClosed()) {
                                reset = true;
                            }
                            RUVIA_CHECK(!peer.connectionError());
                        }
                        co_await flushPeer(socket, peer, memory.resource());
                    }
                    bool sentFin = false;
                    for (std::size_t offset = 0; offset + ruvia::kHttp2FrameHeaderBytes <= receivedWire.size();) {
                        const auto header = ruvia::parseHttp2FrameHeader(std::span<const char>(receivedWire.data() + offset, receivedWire.size() - offset));
                        if (!header || header->length > receivedWire.size() - offset - ruvia::kHttp2FrameHeaderBytes) {
                            break;
                        }
                        sentFin = sentFin || (header->streamId == id && header->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kData) && (header->flags & 1U) != 0);
                        offset += ruvia::kHttp2FrameHeaderBytes + header->length;
                    }
                    RUVIA_CHECK(sentFin);
                    continue;
                }
                const auto submitted = peer.submitData(id, payload, ruvia::Http2EndStream::kEndStream);
                RUVIA_CHECK(submitted == ruvia::Http2DataSubmitStatus::kAccepted || submitted == ruvia::Http2DataSubmitStatus::kQueued);
                co_await flushPeer(socket, peer, memory.resource());
                std::string echoed;
                bool ended = false;
                while (!ended) {
                    co_await pump();
                    while (auto event = peer.nextEvent()) {
                        if (auto* data = event->tunnelData()) {
                            RUVIA_CHECK(data->streamId() == id);
                            echoed.append(data->bytes());
                            (void)peer.acknowledge(data->takeCredit());
                        }
                        if (const auto* end = event->tunnelEnd()) {
                            RUVIA_CHECK(end->streamId() == id);
                            ended = true;
                        }
                        if (event->streamClosed() || peer.connectionError()) {
                            throw std::runtime_error("CONNECT stream reset");
                        }
                    }
                    co_await flushPeer(socket, peer, memory.resource());
                }
                RUVIA_CHECK(echoed == payload);
                RUVIA_CHECK(observation.sawEof && observation.stable);
                RUVIA_CHECK(observation.received == payload);
                observation.received.clear();
                observation.sawEof = false;
            }
            auto sibling = peer.submitRequestHead(ruvia::Http2RegularRequestHeadView{.authority = "proxy.test", .target = "/after"});
            if (!sibling.submitted()) {
                throw std::runtime_error("sibling request failed");
            }
            co_await flushPeer(socket, peer, memory.resource());
            bool ended = false;
            std::string body;
            while (!ended) {
                co_await pump();
                while (auto event = peer.nextEvent()) {
                    if (auto* bytes = event->messageBodyChunk()) {
                        body.append(bytes->bytes());
                    }
                    if (event->messageEnd()) {
                        ended = true;
                    }
                }
                co_await flushPeer(socket, peer, memory.resource());
            }
            RUVIA_CHECK(body == "sibling");
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::closeSocket(socket);
        acceptor.close();
        (void)watchdog.cancel();
        co_await tasks.join();
        RUVIA_CHECK(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(allocations.liveAllocations(), std::size_t{0});
}
