#include "http3_server_connection_fixture.h"

namespace ruvia::testing {
Http3DatagramReceiveStatus plan_connect_datagram_for_peer(
    detail::http3_connection_identity identity,
    std::optional<detail::http3_stream_control> marker,
    std::uint64_t accepted_wire_bytes, std::span<const char> bytes) {
    using Access = detail::http3_connection_driver_test_access;
    std::pmr::monotonic_buffer_resource resource;
    auto connection = Access::make_connection(&resource, identity);
    const auto datagram = decodeHttp3Datagram(bytes);
    if (!datagram) {
        throw std::runtime_error("CONNECT datagram fixture received invalid wire bytes");
    }
    (void)Access::add_request_stream(connection, datagram->streamId);
    if (marker && Access::accept_tunnel_established(
                      connection, *marker, accepted_wire_bytes) != Access::tunnel_result::accepted) {
        throw std::runtime_error("CONNECT datagram fixture received invalid establishment");
    }
    return Access::plan_received_datagram(connection, *datagram);
}
}  // namespace ruvia::testing

namespace {

Connection::EventResult acceptTunnelHead(Connection& connection, buffer& inbound,
    ruvia::WorkerMemory& worker, MessageId id) {
    const auto wire = web_socket_request_wire(worker, "13");
    const auto sent = inbound.try_send(id,
        std::as_bytes(std::span(wire.data(), wire.size())));
    if (!accepted(sent)) {
        throw std::runtime_error("HTTP/3 WebSocket request buffer is full");
    }
    buffer::borrowed_block block;
    if (!inbound.try_receive(block)) {
        throw std::runtime_error("HTTP/3 WebSocket request block is missing");
    }
    auto result = connection.acceptData(block);
    block.release();
    return result;
}

ruvia::Task<bool> waitForWebSocketStart(Fixture& fixture,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stopToken) {
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        if (fixture.routes.handlers.webSocketStartedObserved) {
            co_return true;
        }
        if (co_await ruvia::sleepFor(worker, 1ms, stopToken) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    co_return fixture.routes.handlers.webSocketStartedObserved;
}

ruvia::Task<void> exerciseWebSocketFinCancellation(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx,
    bool requestStopAfterFin, bool peerReset, bool deadline = false) {
    if (deadline) {
        fixture.options.deadline = ruvia::DeadlineConfig{.handler = 250ms};
    }
    TestActivationSignal scheduler(worker);
    ruvia::WorkerSignal started(worker);
    fixture.routes.handlers.webSocketStartedObserved = false;
    fixture.routes.handlers.webSocketHandlerFinished = false;
    fixture.routes.handlers.webSocketStarted = &started;
    buffer inbound(8, 8, 8, fixture.worker.resource());
    buffer outbound(8, 8, 8, fixture.worker.resource());
    const auto generation = kGeneration + (requestStopAfterFin ? 90 : peerReset ? 91
                                                                                : 92);
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8, .connectionScanner = &fixture.scanner, .executor = fixture.executor});
    const MessageId id{kEpoch, generation, 0};
    const std::array requestFields{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/socket"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", "13"}};
    const auto encodedHead = ruvia::encodeHttp3FieldSection(requestFields, fixture.worker.resource());
    RUVIA_CHECK(encodedHead.has_value());
    const auto requestHead = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(encodedHead->data(), encodedHead->size()));
    const auto request = acceptTunnelHead(connection, inbound, fixture.worker, id);
    RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    for (std::size_t attempt = 0;
        attempt < 2000 && !fixture.routes.handlers.webSocketStartedObserved; ++attempt) {
        if (connection.readyRequestCount() != 0) {
            const auto publication = connection.publishOne(kAllWorkLanes);
            RUVIA_CHECK(publication.status == Connection::PublishStatus::kAttempted);
            std::array<PublishedWire, 6> handshake{};
            drainAll(outbound, handshake);
            (void)connection.reactivateBlocked({.data = true, .control = true});
        } else if (co_await ruvia::sleepFor(worker, 1ms, fixture.workerStop) !=
                   ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForWebSocketStart(fixture, worker, fixture.workerStop));

    constexpr std::array<char, 8> maskedClose{
        static_cast<char>(0x88), static_cast<char>(0x82), char{0x11}, char{0x22},
        char{0x33}, char{0x44}, static_cast<char>(0x12), static_cast<char>(0xca)};
    const auto tunnelData = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData),
        std::string_view(maskedClose.data(), maskedClose.size()));
    const auto acceptedClose = acceptWireBytes(connection, inbound, id,
        std::span<const char>(tunnelData.data(), tunnelData.size()));
    RUVIA_CHECK(acceptedClose.status == Connection::EventStatus::kAccepted);
    bool localFinPublished = false;
    std::array<PublishedWire, 6> tunnelWire{};
    for (std::size_t attempt = 0; attempt < 2000 && !localFinPublished; ++attempt) {
        if (connection.readyRequestCount() == 0) {
            if (co_await ruvia::sleepFor(worker, 1ms, fixture.workerStop) !=
                ruvia::TimerSleepResult::kElapsed) {
                break;
            }
            continue;
        }
        const auto publication = connection.publishOne(kAllWorkLanes);
        if (publication.status == Connection::PublishStatus::kNoReadyRequest) {
            (void)co_await ruvia::sleepFor(worker, 1ms, fixture.workerStop);
            continue;
        }
        if (publication.status != Connection::PublishStatus::kAttempted) {
            break;
        }
        localFinPublished =
            publication.publication.status == Connection::Dispatch::PublishStatus::kFinPublished ||
            publication.publication.status == Connection::Dispatch::PublishStatus::kComplete;
        drainAll(outbound, tunnelWire);
        (void)connection.reactivateBlocked({.data = true, .control = true});
    }
    if (!localFinPublished) {
        RUVIA_CHECK(connection.requestStop());
        requireWatchdogSuccess(ruvia_ctx,
            co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));
        co_await connection.join();
        simulateGlobalStopTakeover(connection);
        RUVIA_CHECK(localFinPublished);
        co_return;
    }
    RUVIA_CHECK(localFinPublished);
    RUVIA_CHECK(connection.requestInfo(id.stream_id).status == Connection::RequestStatus::kPublished);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{1});

    if (requestStopAfterFin) {
        RUVIA_CHECK(connection.requestStop());
    } else if (peerReset) {
        const auto reset = connection.acceptControl({Control::kind::stream_reset, id,
            static_cast<std::uint64_t>(tunnelData.size() + requestHead.size())});
        RUVIA_CHECK(reset.status == Connection::EventStatus::kStreamCancelled);
    } else if (!deadline) {
        const auto peerFin = connection.acceptControl({Control::kind::stream_fin, id,
            static_cast<std::uint64_t>(tunnelData.size() + requestHead.size())});
        RUVIA_CHECK(peerFin.status == Connection::EventStatus::kAccepted);
        RUVIA_CHECK(peerFin.input.status == Connection::Input::Status::kFinished);
    }
    const bool joined =
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop);
    requireWatchdogSuccess(ruvia_ctx, joined);
    if (deadline) {
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
        const auto retirement = connection.peekTransportIntent();
        RUVIA_CHECK(retirement.has_value());
        if (!retirement) {
            throw std::runtime_error("deadline after FIN did not retain stream retirement intent");
        }
        RUVIA_CHECK(retirement->token.kind == Connection::TransportIntentKind::kStreamReset);
        RUVIA_CHECK_EQ(retirement->token.id.stream_id, id.stream_id);
    } else if (peerReset) {
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    }
    if (deadline) {
        const auto lateFin = connection.acceptControl({Control::kind::stream_fin, id,
            static_cast<std::uint64_t>(requestHead.size() + tunnelData.size())});
        RUVIA_CHECK(lateFin.input.status == Connection::Input::Status::kClosedStream);
        std::array<PublishedWire, 6> discarded{};
        drainAll(outbound, discarded);
        const MessageId siblingId{kEpoch, generation, 4};
        const auto sibling = routeRequest(connection, inbound, fixture.worker, siblingId,
            "GET", "/first");
        RUVIA_CHECK(sibling.status == Connection::EventStatus::kDispatched);
        co_await waitForReady(connection, 1, worker, fixture.workerStop);
        bool siblingPublished = false;
        for (std::size_t attempt = 0; attempt < 32 && !siblingPublished; ++attempt) {
            const auto publication = connection.publishOne(kAllWorkLanes);
            RUVIA_CHECK(publication.status == Connection::PublishStatus::kAttempted);
            if (publication.publication.status ==
                    Connection::Dispatch::PublishStatus::kFinPublished ||
                publication.publication.status ==
                    Connection::Dispatch::PublishStatus::kComplete) {
                siblingPublished = true;
            }
            drainAll(outbound, discarded);
            (void)connection.reactivateBlocked({.data = true, .control = true});
        }
        RUVIA_CHECK(siblingPublished);
        RUVIA_CHECK(connection.requestInfo(4).status == Connection::RequestStatus::kPublished);
        const auto retirement = connection.peekTransportIntent();
        RUVIA_CHECK(retirement.has_value());
        if (!retirement || retirement->token.kind != Connection::TransportIntentKind::kStreamReset ||
            retirement->token.id.stream_id != id.stream_id) {
            throw std::runtime_error("deadline reset intent changed after sibling publication");
        }
        RUVIA_CHECK(connection.ackTransportIntent(retirement->token));
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    }
    if (requestStopAfterFin) {
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
        RUVIA_CHECK(connection.peekTransportIntent()->token.kind ==
                    Connection::TransportIntentKind::kConnectionClose);
    }
    if (!connection.stopped()) {
        RUVIA_CHECK(connection.requestStop());
    }
    co_await connection.join();
    std::array<PublishedWire, 6> wires{};
    drainAll(outbound, wires);
    simulateGlobalStopTakeover(connection);
}

ruvia::Task<void> exerciseWebSocketFinCancellationVariants(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    fixture.scanner.start();
    co_await exerciseWebSocketFinCancellation(fixture, worker, ruvia_ctx, false, false);
    co_await exerciseWebSocketFinCancellation(fixture, worker, ruvia_ctx, false, true);
    co_await exerciseWebSocketFinCancellation(fixture, worker, ruvia_ctx, true, false);
    co_await exerciseWebSocketFinCancellation(fixture, worker, ruvia_ctx, false, false, true);
    fixture.scanner.stop();
}

}  // namespace

RUVIA_TEST(http3_worker_tunnel_body_timeout_waits_for_accepted_handshake_barrier) {
    using Access = ruvia::detail::http3_connection_driver_test_access;
    using Identity = ruvia::detail::http3_connection_identity;
    using Control = ruvia::detail::http3_stream_control;
    std::pmr::monotonic_buffer_resource resource;
    const Identity identity{
        .epoch = 7, .connection_generation = 11};
    auto makeControl = [](std::uint64_t streamId) {
        return Control{
            .kind = Control::kind::tunnel_established,
            .id = {.epoch = 7, .connection_generation = 11, .stream_id = streamId},
            .value = 100};
    };

    auto finFirst = Access::make_connection(&resource, identity);
    auto& finStream = Access::add_request_stream(finFirst, 4);
    auto& finSibling = Access::add_request_stream(finFirst, 8);
    Access::note_peer_fin(finFirst, 4);
    Access::complete_input_terminal(finFirst, 4);
    RUVIA_CHECK(finStream.input_terminal);
    RUVIA_CHECK(!finStream.body_timeout_applies());
    RUVIA_CHECK(finSibling.body_timeout_applies());
    RUVIA_CHECK(Access::accept_tunnel_established(finFirst, makeControl(4), 99) ==
                Access::tunnel_result::accepted);
    RUVIA_CHECK_EQ(Access::pending_handshakes(finFirst), std::size_t{1});
    RUVIA_CHECK(!finStream.body_timeout_applies());
    RUVIA_CHECK(finSibling.body_timeout_applies());
    RUVIA_CHECK(!Access::confirm_tunnel_established(finFirst, 4, 99));
    RUVIA_CHECK(Access::confirm_tunnel_established(finFirst, 4, 100));
    RUVIA_CHECK_EQ(Access::pending_handshakes(finFirst), std::size_t{0});
    RUVIA_CHECK(Access::accept_tunnel_established(finFirst, makeControl(4), 100) ==
                Access::tunnel_result::protocol_failure);
    auto wrongIdentity = makeControl(8);
    ++wrongIdentity.id.epoch;
    RUVIA_CHECK(Access::accept_tunnel_established(finFirst, wrongIdentity, 100) ==
                Access::tunnel_result::protocol_failure);
    RUVIA_CHECK(finSibling.body_timeout_applies());

    auto markerFirst = Access::make_connection(&resource, identity);
    auto& markerStream = Access::add_request_stream(markerFirst, 12);
    auto& markerSibling = Access::add_request_stream(markerFirst, 16);
    RUVIA_CHECK(Access::accept_tunnel_established(markerFirst, makeControl(12), 99) ==
                Access::tunnel_result::accepted);
    RUVIA_CHECK_EQ(Access::pending_handshakes(markerFirst), std::size_t{1});
    Access::note_peer_fin(markerFirst, 12);
    Access::complete_input_terminal(markerFirst, 12);
    RUVIA_CHECK(markerStream.input_terminal);
    RUVIA_CHECK(!markerStream.body_timeout_applies());
    RUVIA_CHECK(markerSibling.body_timeout_applies());
    RUVIA_CHECK(Access::confirm_tunnel_established(markerFirst, 12, 100));
    RUVIA_CHECK_EQ(Access::pending_handshakes(markerFirst), std::size_t{0});

    ruvia::test::CountingMemoryResource resetResource;
    {
        auto resetConnection = Access::make_connection(&resetResource, identity);
        auto& resetStream = Access::add_request_stream(resetConnection, 20);
        auto& resetSibling = Access::add_request_stream(resetConnection, 24);
        Access::install_frame_tracker(resetStream, &resetResource);
        RUVIA_CHECK(Access::accept_tunnel_established(resetConnection, makeControl(20), 99) ==
                    Access::tunnel_result::accepted);
        RUVIA_CHECK_EQ(Access::pending_handshakes(resetConnection), std::size_t{1});
        const auto deallocationsBeforeReset = resetResource.deallocationCount();
        const auto liveAllocationsBeforeReset = resetResource.liveAllocations();
        Access::note_input_reset(resetConnection, 20);
        Access::complete_input_terminal(resetConnection, 20);
        RUVIA_CHECK(resetStream.input_terminal);
        RUVIA_CHECK(resetStream.input_reset);
        RUVIA_CHECK(!resetStream.frame_tracker);
        // The tracker can own more than one PMR allocation on MSVC; verify
        // actual release rather than assuming its object is the only block.
        RUVIA_CHECK(resetResource.deallocationCount() > deallocationsBeforeReset);
        RUVIA_CHECK(resetResource.liveAllocations() < liveAllocationsBeforeReset);
        RUVIA_CHECK(!resetStream.body_timeout_applies());
        RUVIA_CHECK_EQ(Access::pending_handshakes(resetConnection), std::size_t{0});
        RUVIA_CHECK(Access::accept_tunnel_established(
                        resetConnection, makeControl(20), 100) ==
                    Access::tunnel_result::ignored_terminal);
        RUVIA_CHECK_EQ(Access::pending_handshakes(resetConnection), std::size_t{0});
        RUVIA_CHECK(!Access::closing(resetConnection));
        RUVIA_CHECK(Access::accept_tunnel_established(
                        resetConnection, makeControl(20), 100) ==
                    Access::tunnel_result::ignored_terminal);
        RUVIA_CHECK_EQ(Access::pending_handshakes(resetConnection), std::size_t{0});
        RUVIA_CHECK(!Access::closing(resetConnection));
        auto staleIdentity = makeControl(20);
        ++staleIdentity.id.connection_generation;
        RUVIA_CHECK(Access::accept_tunnel_established(
                        resetConnection, staleIdentity, 100) ==
                    Access::tunnel_result::protocol_failure);
        RUVIA_CHECK(resetSibling.body_timeout_applies());
        RUVIA_CHECK_EQ(Access::identity(resetConnection).connection_generation,
            identity.connection_generation);
    }
    RUVIA_CHECK_EQ(resetResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resetResource.allocationCount(), resetResource.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionWaitsForTunnelPeerFinAfterLocalOutputFin) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        fixture.executor = attachment.loop().executor();
        runWorkerTask(attachment,
            exerciseWebSocketFinCancellationVariants(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
