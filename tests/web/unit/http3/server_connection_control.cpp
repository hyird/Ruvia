#include "http3_server_connection_fixture.h"

namespace {

ruvia::Task<void> exerciseUnknownUniFinOrder(Fixture& fixture,
    const ruvia::WorkerHandle& worker, std::uint64_t generation, bool finFirst,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer inbound(4, 4, 4, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});

    constexpr std::uint64_t unknownStreamId = 2;
    constexpr std::array<char, 1> unknownStreamType{static_cast<char>(0x21)};
    const MessageId unknownId{kEpoch, generation, unknownStreamId};
    const auto unknownBytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(unknownStreamType.data()), unknownStreamType.size());
    if (!accepted(inbound.try_send(unknownId, unknownBytes)) ||
        !accepted(inbound.try_send_control(
            {Control::kind::stream_fin, unknownId, unknownStreamType.size()}))) {
        throw std::runtime_error("HTTP/3 unknown-unidirectional fixture buffer is full");
    }

    const auto acceptUnknownData = [&]() {
        buffer::borrowed_block block;
        if (!inbound.try_receive(block)) {
            throw std::runtime_error("HTTP/3 unknown-unidirectional data is missing");
        }
        auto result = connection.acceptData(block);
        block.release();
        return result;
    };
    const auto acceptUnknownFin = [&]() {
        Control fin;
        if (!inbound.try_receive_control(fin)) {
            throw std::runtime_error("HTTP/3 unknown-unidirectional FIN is missing");
        }
        return connection.acceptControl(fin);
    };

    Connection::EventResult completed;
    if (finFirst) {
        const auto deferred = acceptUnknownFin();
        RUVIA_CHECK(deferred.status == Connection::EventStatus::kAccepted);
        RUVIA_CHECK(deferred.input.status == Connection::Input::Status::kDeferredFin);
        RUVIA_CHECK(!deferred.connectionCloseRequired);
        completed = acceptUnknownData();
    } else {
        const auto fed = acceptUnknownData();
        RUVIA_CHECK(fed.status == Connection::EventStatus::kAccepted);
        RUVIA_CHECK(fed.input.status == Connection::Input::Status::kFed);
        RUVIA_CHECK(!fed.connectionCloseRequired);
        completed = acceptUnknownFin();
    }
    RUVIA_CHECK(completed.status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK(completed.input.status == Connection::Input::Status::kFinished);
    RUVIA_CHECK(!completed.connectionCloseRequired);
    RUVIA_CHECK(!connection.stopped());
    RUVIA_CHECK(!connection.transportCloseRequired());
    RUVIA_CHECK_EQ(connection.requestInfo(unknownStreamId).status,
        Connection::RequestStatus::kUnknown);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.trackedRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});

    const auto normal = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, generation, 0}, "GET", "/first");
    RUVIA_CHECK(normal.status == Connection::EventStatus::kDispatched);
    std::array<PublishedWire, 6> wires{};
    constexpr std::array<std::uint64_t, 1> normalId{0};
    co_await publishGroup(connection, outbound, wires, normalId,
        worker, fixture.workerStop, false, ruvia_ctx);
    const auto response = decodeResponse(wires[0], ruvia::HttpKnownMethod::kGet,
        0, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.finalHeads, std::size_t{1});
    RUVIA_CHECK_EQ(response.messageEnds, std::size_t{1});
    RUVIA_CHECK(response.body == "/first");

    RUVIA_CHECK(connection.requestStop());
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
}

ruvia::Task<void> exerciseCriticalUniFin(Fixture& fixture,
    const ruvia::WorkerHandle& worker, std::uint64_t generation,
    std::uint64_t streamId, char streamType,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer inbound(2, 2, 2, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});

    const MessageId id{kEpoch, generation, streamId};
    const std::array<char, 1> streamTypeBytes{streamType};
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(streamTypeBytes.data()), streamTypeBytes.size());
    if (!accepted(inbound.try_send(id, bytes))) {
        throw std::runtime_error("HTTP/3 critical-unidirectional fixture buffer is full");
    }
    buffer::borrowed_block block;
    if (!inbound.try_receive(block)) {
        throw std::runtime_error("HTTP/3 critical-unidirectional data is missing");
    }
    const auto fed = connection.acceptData(block);
    block.release();
    RUVIA_CHECK(fed.status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK(fed.input.status == Connection::Input::Status::kFed);

    const auto closed = connection.acceptControl(
        {Control::kind::stream_fin, id, streamTypeBytes.size()});
    RUVIA_CHECK(closed.status == Connection::EventStatus::kProtocolError);
    RUVIA_CHECK(closed.input.status == Connection::Input::Status::kProtocolError);
    RUVIA_CHECK(closed.input.protocol.code == ruvia::Http3ConnectionErrorCode::kClosedCriticalStream);
    RUVIA_CHECK(closed.connectionCloseRequired);
    RUVIA_CHECK(connection.transportCloseRequired());
    const auto closeIntent = connection.peekTransportIntent();
    RUVIA_CHECK(closeIntent.has_value());
    RUVIA_CHECK(closeIntent->token.kind == Connection::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(closeIntent->closeReason ==
                Connection::TransportCloseReason::kConnectionProtocolError);
    RUVIA_CHECK(closeIntent->connectionErrorCode ==
                ruvia::Http3ConnectionErrorCode::kClosedCriticalStream);
    const auto persistentClose = connection.acceptControl(
        {Control::kind::stream_fin, id, streamTypeBytes.size()});
    RUVIA_CHECK(persistentClose.status == Connection::EventStatus::kAdmissionClosed);
    RUVIA_CHECK(persistentClose.connectionCloseRequired);
    RUVIA_CHECK(persistentClose.input.status == Connection::Input::Status::kStopped);
    RUVIA_CHECK(connection.peekTransportIntent()->token == closeIntent->token);

    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
}

ruvia::Task<void> exerciseUnknownUniFinOrders(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    co_await exerciseUnknownUniFinOrder(
        fixture, worker, kGeneration + 20, false, ruvia_ctx);
    co_await exerciseUnknownUniFinOrder(
        fixture, worker, kGeneration + 21, true, ruvia_ctx);
}

ruvia::Task<void> exerciseCriticalUniFins(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    co_await exerciseCriticalUniFin(
        fixture, worker, kGeneration + 30, 2, static_cast<char>(0x00), ruvia_ctx);
    co_await exerciseCriticalUniFin(
        fixture, worker, kGeneration + 31, 6, static_cast<char>(0x02), ruvia_ctx);
    co_await exerciseCriticalUniFin(
        fixture, worker, kGeneration + 32, 10, static_cast<char>(0x03), ruvia_ctx);
}

}  // namespace

RUVIA_TEST(http3ServerConnectionIgnoresUnknownClientUniStreamsInEitherFinOrder) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseUnknownUniFinOrders(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionRejectsFinOnPeerCriticalUniStreams) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseCriticalUniFins(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
