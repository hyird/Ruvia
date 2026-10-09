#include <variant>

#include "http3_server_connection_fixture.h"

namespace {

ruvia::Task<void> exerciseSuccessfulConnection(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    constexpr std::array<std::uint64_t, 4> firstIds{0, 4, 8, 12};
    constexpr std::array<std::uint64_t, 2> secondIds{16, 20};
    constexpr std::array expectField{ruvia::Http3FieldSectionFieldView{"expect", "custom-expectation"}};

    TestActivationSignal scheduler(worker);
    buffer inbound(8, 8, 8, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = kGeneration, .maxTrackedStreams = 32});

    const auto stale = connection.acceptControl(
        {Control::kind::stream_fin, {kEpoch + 1, kGeneration, 12}, 0});
    RUVIA_CHECK(stale.status == Connection::EventStatus::kInputRejected);
    RUVIA_CHECK(stale.input.status == Connection::Input::Status::kForeignEpoch);

    const auto first = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration, firstIds[0]}, "GET", "/first");
    RUVIA_CHECK(first.status == Connection::EventStatus::kDispatched);
    const auto taskCountBeforeDuplicate = connection.activeTaskCount();
    const auto firstWireSize = requestWire(fixture.worker, "GET", "/first").size();
    const auto duplicateFin = connection.acceptControl(
        {Control::kind::stream_fin, {kEpoch, kGeneration, firstIds[0]}, firstWireSize});
    RUVIA_CHECK(duplicateFin.input.status == Connection::Input::Status::kDuplicateFin);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), taskCountBeforeDuplicate);

    const auto second = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration, firstIds[1]}, "POST", "/body", "sibling-body");
    RUVIA_CHECK(second.status == Connection::EventStatus::kDispatched);
    const auto third = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration, firstIds[2]}, "GET", "/throw");
    RUVIA_CHECK(third.status == Connection::EventStatus::kDispatched);
    const auto rejected = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration, 12}, "GET", "/first", {}, expectField);
    RUVIA_CHECK(rejected.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK(rejected.rejection == Connection::Session::Rejection::kExpectationUnsupported);
    RUVIA_CHECK_EQ(connection.requestInfo(12).status, Connection::RequestStatus::kRejected);

    std::array<PublishedWire, 6> wires{};
    co_await publishGroup(connection, outbound, wires, firstIds,
        worker, fixture.workerStop, true, ruvia_ctx);
    const auto rejectedResponse = decodeResponse(wires[wireIndex(12)],
        ruvia::HttpKnownMethod::kGet, 12, fixture.worker.resource());
    RUVIA_CHECK_EQ(rejectedResponse.status, std::uint16_t{417});
    RUVIA_CHECK_EQ(rejectedResponse.messageEnds, std::size_t{1});

    const auto fourth = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration, secondIds[0]}, "GET", "/first");
    RUVIA_CHECK(fourth.status == Connection::EventStatus::kDispatched);
    const auto fifth = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration, secondIds[1]}, "POST", "/body", "later-body");
    RUVIA_CHECK(fifth.status == Connection::EventStatus::kDispatched);
    co_await publishGroup(connection, outbound, wires, secondIds,
        worker, fixture.workerStop, false, ruvia_ctx);

    RUVIA_CHECK(fixture.routes.handlers.bodySeen == "later-body");
    RUVIA_CHECK_EQ(connection.requestInfo(0).status, Connection::RequestStatus::kPublished);
    RUVIA_CHECK_EQ(connection.requestInfo(4).status, Connection::RequestStatus::kPublished);
    RUVIA_CHECK_EQ(connection.requestInfo(8).status, Connection::RequestStatus::kPublished);
    RUVIA_CHECK_EQ(connection.trackedRequestCount(), std::size_t{6});

    RUVIA_CHECK(connection.requestStop());
    RUVIA_CHECK(connection.transportCloseRequired());
    {
        auto coldJoin = connection.join();
        static_cast<void>(coldJoin);
    }
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK_EQ(connection.activeRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});

    constexpr std::array<std::uint64_t, 5> publishedIds{0, 4, 8, 16, 20};
    for (const auto streamId : publishedIds) {
        const auto method = streamId == 4 || streamId == 20
                                ? ruvia::HttpKnownMethod::kPost
                                : ruvia::HttpKnownMethod::kGet;
        const auto response = decodeResponse(wires[wireIndex(streamId)], method,
            streamId, fixture.worker.resource());
        RUVIA_CHECK_EQ(response.finalHeads, std::size_t{1});
        RUVIA_CHECK_EQ(response.messageEnds, std::size_t{1});
        if (streamId == 8) {
            RUVIA_CHECK_EQ(response.status, std::uint16_t{500});
        } else {
            RUVIA_CHECK_EQ(response.status, std::uint16_t{200});
        }
        if (streamId == 0 || streamId == 16) {
            RUVIA_CHECK(response.body == "/first");
        } else if (streamId == 4) {
            RUVIA_CHECK(response.body == "sibling-body");
        } else if (streamId == 20) {
            RUVIA_CHECK(response.body == "later-body");
        }
    }
}

ruvia::Task<void> exerciseEarlyRejectionBeforeRequestFin(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(fixture.services.worker());
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr std::uint64_t generation = kGeneration + 91;
    const MessageId id{kEpoch, generation, 0};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 4});
    constexpr std::array expectField{ruvia::Http3FieldSectionFieldView{"expect", "custom-expectation"}};
    const auto wire = requestWire(fixture.worker, "GET", "/first", {}, expectField);
    const auto rejected = acceptWireBytes(connection, inbound, id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(rejected.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK(rejected.rejection == Connection::Session::Rejection::kExpectationUnsupported);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    RUVIA_CHECK(connection.workState().runnable.data);

    std::array<PublishedWire, 6> wires{};
    const auto headers = connection.publishOne({.data = true});
    RUVIA_CHECK(headers.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK(headers.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished);
    drainDataOnly(outbound, wires);
    const auto fin = connection.publishOne({.control = true});
    RUVIA_CHECK(fin.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK(fin.publication.status ==
                Connection::Dispatch::PublishStatus::kFinPublished);
    drainAll(outbound, wires);
    const auto response = decodeResponse(wires[0], ruvia::HttpKnownMethod::kGet,
        id.stream_id, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, std::uint16_t{417});
    RUVIA_CHECK_EQ(response.messageEnds, std::size_t{1});
    RUVIA_CHECK_EQ(connection.requestInfo(id.stream_id).status,
        Connection::RequestStatus::kPublished);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{1});
    const auto lateBody = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), "late");
    const auto discarded = acceptWireBytes(connection, inbound, id,
        std::span<const char>(lateBody.data(), lateBody.size()));
    RUVIA_CHECK(discarded.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.readyRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});

    const auto inputFin = connection.acceptControl({Control::kind::stream_fin, id,
        static_cast<std::uint64_t>(wire.size() + lateBody.size())});
    RUVIA_CHECK(inputFin.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    (void)ruvia_ctx;
}

enum class receive_retirement { fin,
    reset,
    stop };

ruvia::Task<void> exercise_web_socket_rejection_receive_retirement(Fixture& fixture,
    const ruvia::WorkerHandle& worker, receive_retirement retirement,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    ruvia::detail::Http3ServerBodyBudget budget(64);
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    const MessageId id{kEpoch, kGeneration + 98, 0};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler, budget,
        {.epoch = id.epoch, .connectionGeneration = id.connection_generation, .maxTrackedStreams = 4});
    const auto request_head = web_socket_request_wire(fixture.worker, "12");
    const auto admitted = acceptWireBytes(connection, inbound, id,
        std::span<const char>(request_head.data(), request_head.size()));
    RUVIA_CHECK(admitted.status == Connection::EventStatus::kDispatched);
    co_await waitForReady(connection, 1, worker, fixture.workerStop);
    std::array<PublishedWire, 6> wires{};
    constexpr std::array<std::uint64_t, 1> rejected_stream{0};
    co_await publishGroup(connection, outbound, wires, rejected_stream,
        worker, fixture.workerStop, false, ruvia_ctx);
    const auto response = decodeResponse(wires[0], ruvia::HttpKnownMethod::kConnect,
        id.stream_id, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, std::uint16_t{400});
    RUVIA_CHECK_EQ(response.finalHeads, std::size_t{1});
    RUVIA_CHECK_EQ(response.messageEnds, std::size_t{1});
    RUVIA_CHECK(!connection.transportCloseRequired());
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});

    const auto late_body = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), "late");
    const auto body = acceptWireBytes(connection, inbound, id,
        std::span<const char>(late_body.data(), late_body.size()));
    RUVIA_CHECK(body.status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK_EQ(budget.used(), std::size_t{4});
    RUVIA_CHECK(!fixture.routes.handlers.webSocketStartedObserved);

    const MessageId sibling_id{id.epoch, id.connection_generation, 4};
    const auto sibling = routeRequest(connection, inbound, fixture.worker, sibling_id, "GET", "/first");
    RUVIA_CHECK(sibling.status == Connection::EventStatus::kDispatched);
    co_await waitForReady(connection, 1, worker, fixture.workerStop);
    constexpr std::array<std::uint64_t, 1> sibling_stream{4};
    co_await publishGroup(connection, outbound, wires, sibling_stream,
        worker, fixture.workerStop, false, ruvia_ctx);
    const auto sibling_response = decodeResponse(wires[1], ruvia::HttpKnownMethod::kGet,
        sibling_id.stream_id, fixture.worker.resource());
    RUVIA_CHECK_EQ(sibling_response.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(sibling_response.body, "/first");
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{1});
    RUVIA_CHECK_EQ(budget.used(), std::size_t{4});

    if (retirement != receive_retirement::stop) {
        const auto receive_end = connection.acceptControl({retirement == receive_retirement::fin ? Control::kind::stream_fin : Control::kind::stream_reset,
            id, static_cast<std::uint64_t>(request_head.size() + late_body.size())});
        RUVIA_CHECK(receive_end.status == Connection::EventStatus::kAccepted);
        RUVIA_CHECK(receive_end.input.status == (retirement == receive_retirement::fin
                                                        ? Connection::Input::Status::kFinished
                                                        : Connection::Input::Status::kReset));
        RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        RUVIA_CHECK(!connection.transportCloseRequired());
    }
    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
}

ruvia::Task<void> exercisePeerLimitZeroRejection(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(fixture.services.worker());
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr std::uint64_t generation = kGeneration + 94;
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 4});
    std::array<char, 64> settingsPayload{};
    ruvia::Http3Settings settings;
    settings.maxFieldSectionSize = 0;
    const auto settingsSize = ruvia::encodeHttp3Settings(settingsPayload, settings);
    if ((settingsSize.index() != 0)) {
        throw std::runtime_error("HTTP/3 peer setting encoding failed");
    }
    std::string controlWire(1, '\0');
    controlWire += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kSettings),
        std::string_view(settingsPayload.data(), std::get<0>(settingsSize)));
    const auto control = acceptWireBytes(connection, inbound,
        {kEpoch, generation, 2}, std::span<const char>(controlWire.data(), controlWire.size()));
    RUVIA_CHECK(control.status == Connection::EventStatus::kAccepted);

    constexpr std::array expectField{ruvia::Http3FieldSectionFieldView{"expect", "custom-expectation"}};
    const MessageId id{kEpoch, generation, 0};
    const auto wire = requestWire(fixture.worker, "GET", "/first", {}, expectField);
    const auto rejected = acceptWireBytes(connection, inbound, id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(rejected.status == Connection::EventStatus::kRejected);
    std::array<PublishedWire, 6> wires{};
    const auto headers = connection.publishOne({.data = true});
    RUVIA_CHECK(headers.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished);
    drainDataOnly(outbound, wires);
    const auto fin = connection.publishOne({.control = true});
    RUVIA_CHECK(fin.publication.status ==
                Connection::Dispatch::PublishStatus::kFinPublished);
    drainAll(outbound, wires);
    const auto response = decodeResponse(wires[0], ruvia::HttpKnownMethod::kGet,
        id.stream_id, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, std::uint16_t{417});
    RUVIA_CHECK(!connection.peekTransportIntent().has_value());
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});
    const auto inputFin = connection.acceptControl({Control::kind::stream_fin, id,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(inputFin.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    (void)ruvia_ctx;
}

ruvia::Task<void> exerciseRejectedBodyStatus(Fixture& fixture,
    std::uint64_t generation, ruvia::detail::Http3SansIoSessionLimits limits,
    Connection::Session::Rejection expectedRejection, std::uint16_t expectedStatus,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(fixture.services.worker());
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    const MessageId id{kEpoch, generation, 0};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .session = limits, .maxTrackedStreams = 4});
    const auto wire = requestWire(fixture.worker, "POST", "/body", "abcdef");
    const auto rejected = acceptWireBytes(connection, inbound, id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(rejected.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK(rejected.rejection == expectedRejection);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    std::array<PublishedWire, 6> wires{};
    const auto head = connection.publishOne({.data = true});
    RUVIA_CHECK(head.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished);
    drainDataOnly(outbound, wires);
    const auto fin = connection.publishOne({.control = true});
    RUVIA_CHECK(fin.publication.status ==
                Connection::Dispatch::PublishStatus::kFinPublished);
    drainAll(outbound, wires);
    const auto response = decodeResponse(wires[0], ruvia::HttpKnownMethod::kPost,
        id.stream_id, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, expectedStatus);
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{1});
    const auto inputFin = connection.acceptControl({Control::kind::stream_fin, id,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(inputFin.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    (void)ruvia_ctx;
}

ruvia::Task<void> exerciseBodyRejectionStatuses(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    co_await exerciseRejectedBodyStatus(fixture, kGeneration + 95,
        {.max_buffered_body_bytes = 4, .maxLiveStreams = 4, .maxBufferedBytesInFlight = 32},
        Connection::Session::Rejection::kBodyTooLarge, 413, ruvia_ctx);
    co_await exerciseRejectedBodyStatus(fixture, kGeneration + 96,
        {.max_buffered_body_bytes = 1024, .maxLiveStreams = 4, .maxBufferedBytesInFlight = 2},
        Connection::Session::Rejection::kInFlightBodyCapacity, 503, ruvia_ctx);
}

ruvia::Task<void> exerciseUnsupportedConnect(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(fixture.services.worker());
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr std::uint64_t generation = kGeneration + 97;
    const MessageId id{kEpoch, generation, 0};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 4});
    auto encoded = ruvia::encodeHttp3ClientRequestHead(
        {.method = "CONNECT", .authority = "localhost:443"}, {}, fixture.worker.resource());
    if ((encoded.index() != 0)) {
        throw std::runtime_error("HTTP/3 CONNECT request-head encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(std::get<0>(encoded).fieldSection.data(), std::get<0>(encoded).fieldSection.size()));
    const auto rejected = acceptWireBytes(connection, inbound, id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(rejected.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK(rejected.rejection == Connection::Session::Rejection::kConnectUnsupported);
    std::array<PublishedWire, 6> wires{};
    const auto head = connection.publishOne({.data = true});
    RUVIA_CHECK(head.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished);
    drainDataOnly(outbound, wires);
    const auto fin = connection.publishOne({.control = true});
    RUVIA_CHECK(fin.publication.status ==
                Connection::Dispatch::PublishStatus::kFinPublished);
    drainAll(outbound, wires);
    const auto response = decodeResponse(wires[0], ruvia::HttpKnownMethod::kConnect,
        id.stream_id, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, std::uint16_t{501});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});
    const auto inputFin = connection.acceptControl({Control::kind::stream_fin, id,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(inputFin.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    (void)ruvia_ctx;
}

ruvia::Task<void> exerciseProtocolErrorAfterRejectedHead(Fixture& fixture,
    std::uint64_t generation, bool sameBlock, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(fixture.services.worker());
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    const MessageId id{kEpoch, generation, 0};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 4});
    constexpr std::array expectField{ruvia::Http3FieldSectionFieldView{"expect", "custom-expectation"}};
    const auto head = requestWire(fixture.worker, "GET", "/first", {}, expectField);
    const auto forbidden = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kSettings), {});
    if (!sameBlock) {
        const auto rejected = acceptWireBytes(connection, inbound, id,
            std::span<const char>(head.data(), head.size()));
        RUVIA_CHECK(rejected.status == Connection::EventStatus::kRejected);
        RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{1});
        RUVIA_CHECK_EQ(connection.readyRequestCount(), std::size_t{1});
    }
    const auto bytes = sameBlock ? head + forbidden : forbidden;
    const auto error = acceptWireBytes(connection, inbound, id,
        std::span<const char>(bytes.data(), bytes.size()));
    RUVIA_CHECK(error.status == Connection::EventStatus::kProtocolError);
    RUVIA_CHECK(error.input.protocol.code == ruvia::Http3ConnectionErrorCode::kFrameUnexpected);
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.readyRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    buffer::borrowed_block output;
    Control outputControl;
    RUVIA_CHECK(!outbound.try_receive(output));
    RUVIA_CHECK(!outbound.try_receive_control(outputControl));
    if (!connection.stopped()) {
        RUVIA_CHECK(connection.requestStop());
    }
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    (void)ruvia_ctx;
}

ruvia::Task<void> exerciseRejectionProtocolErrorOrders(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    co_await exerciseProtocolErrorAfterRejectedHead(
        fixture, kGeneration + 98, true, ruvia_ctx);
    co_await exerciseProtocolErrorAfterRejectedHead(
        fixture, kGeneration + 99, false, ruvia_ctx);
}

ruvia::Task<void> exerciseRejectionResetAndStop(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(fixture.services.worker());
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr std::uint64_t generation = kGeneration + 93;
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 4});
    constexpr std::array expectField{ruvia::Http3FieldSectionFieldView{"expect", "custom-expectation"}};
    const auto wire = requestWire(fixture.worker, "GET", "/first", {}, expectField);
    const MessageId resetId{kEpoch, generation, 0};
    const auto first = acceptWireBytes(connection, inbound, resetId,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(first.status == Connection::EventStatus::kRejected);
    const auto reset = connection.acceptControl({.kind = Control::kind::stream_reset,
        .id = resetId,
        .value = wire.size()});
    RUVIA_CHECK(reset.status == Connection::EventStatus::kStreamCancelled);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.readyRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.requestInfo(0).status, Connection::RequestStatus::kCancelled);

    const auto deferredRequestHead = ruvia::encodeHttp3ClientRequestHead({.method = "GET",
                                                                             .scheme = "https",
                                                                             .authority = "example.test",
                                                                             .path = "/first",
                                                                             .fields = expectField,
                                                                             .bodyLength = 4},
        {}, fixture.worker.resource());
    RUVIA_CHECK((deferredRequestHead.index() == 0));
    if ((deferredRequestHead.index() != 0)) {
        co_return;
    }
    const auto headWire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(std::get<0>(deferredRequestHead).fieldSection.data(), std::get<0>(deferredRequestHead).fieldSection.size()));
    const auto bodyWire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), "late");
    const auto pendingWire = headWire + bodyWire;
    const MessageId deferredId{kEpoch, generation, 12};
    const auto deferredHead = acceptWireBytes(connection, inbound, deferredId,
        std::span<const char>(headWire.data(), headWire.size()));
    RUVIA_CHECK(deferredHead.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{1});
    RUVIA_CHECK(ruvia::detail::Http3ServerConnectionResetIntentTestAccess::enqueueLocal(
        connection, deferredId.stream_id));
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    RUVIA_CHECK(accepted(inbound.try_send(deferredId, std::as_bytes(
                                                          std::span<const char>(bodyWire.data(), bodyWire.size())))));
    RUVIA_CHECK(accepted(inbound.try_send_control({.kind = Control::kind::stream_reset,
        .id = deferredId,
        .value = pendingWire.size(),
        .stream_reset_error_code = ruvia::Http3ConnectionErrorCode::kRequestRejected})));
    Control deferredReset;
    RUVIA_CHECK(inbound.try_receive_control(deferredReset));
    const auto deferred = connection.acceptControl(deferredReset);
    RUVIA_CHECK(deferred.status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK(deferred.input.status == Connection::Input::Status::kDeferredReset);
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    buffer::borrowed_block finalData;
    RUVIA_CHECK(inbound.try_receive(finalData));
    const auto cancelledByData = connection.acceptData(finalData);
    finalData.release();
    RUVIA_CHECK(cancelledByData.status == Connection::EventStatus::kStreamCancelled);
    RUVIA_CHECK(cancelledByData.input.status == Connection::Input::Status::kReset);
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{0});

    const MessageId publishedId{kEpoch, generation, 4};
    const auto second = acceptWireBytes(connection, inbound, publishedId,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(second.status == Connection::EventStatus::kRejected);
    const auto headers = connection.publishOne({.data = true});
    RUVIA_CHECK(headers.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished);
    const auto fin = connection.publishOne({.control = true});
    RUVIA_CHECK(fin.publication.status ==
                Connection::Dispatch::PublishStatus::kFinPublished);
    std::array<PublishedWire, 6> wires{};
    drainAll(outbound, wires);
    const auto response = decodeResponse(wires[wireIndex(4)],
        ruvia::HttpKnownMethod::kGet, 4, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, std::uint16_t{417});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});
    const auto lateReset = connection.acceptControl({.kind = Control::kind::stream_reset,
        .id = publishedId,
        .value = wire.size()});
    RUVIA_CHECK(lateReset.status == Connection::EventStatus::kStreamCancelled);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.requestInfo(4).status, Connection::RequestStatus::kPublished);

    const MessageId pendingId{kEpoch, generation, 8};
    const auto third = acceptWireBytes(connection, inbound, pendingId,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(third.status == Connection::EventStatus::kRejected);
    const auto pendingHead = connection.publishOne({.data = true});
    RUVIA_CHECK(pendingHead.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished);
    RUVIA_CHECK(connection.requestStop());
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.readyRequestCount(), std::size_t{0});
    drainAll(outbound, wires);
    RUVIA_CHECK(!wires[wireIndex(8)].finalWireBytes.has_value());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    (void)ruvia_ctx;
}

ruvia::Task<void> exerciseRejectionIndexCapacity(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(fixture.services.worker());
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr std::uint64_t generation = kGeneration + 100;
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 1});
    constexpr std::array expectField{ruvia::Http3FieldSectionFieldView{"expect", "custom-expectation"}};
    const auto wire = requestWire(fixture.worker, "GET", "/first", {}, expectField);
    const MessageId firstId{kEpoch, generation, 0};
    const auto first = acceptWireBytes(connection, inbound, firstId,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(first.status == Connection::EventStatus::kRejected);
    const auto fin = connection.acceptControl({Control::kind::stream_fin, firstId,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(fin.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.trackedRequestCount(), std::size_t{1});
    const MessageId secondId{kEpoch, generation, 4};
    const auto second = acceptWireBytes(connection, inbound, secondId,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(second.connectionCloseRequired);
    RUVIA_CHECK(connection.transportCloseRequired());
    RUVIA_CHECK_EQ(connection.trackedRequestCount(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.readyRequestCount(), std::size_t{0});
    const auto close = connection.peekTransportIntent();
    RUVIA_CHECK(close.has_value());
    if (close) {
        RUVIA_CHECK(close->token.kind == Connection::TransportIntentKind::kConnectionClose);
    }
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    (void)ruvia_ctx;
}

ruvia::Task<void> exerciseSharedBodyBudget(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    ruvia::detail::Http3ServerBodyBudget budget(8);
    TestActivationSignal firstScheduler(worker);
    TestActivationSignal secondScheduler(worker);
    ruvia::WorkerSignal slowStarted(worker);
    fixture.routes.handlers.slowStarted = &slowStarted;
    buffer firstInbound(4, 4, 4, fixture.worker.resource());
    buffer firstOutbound(2, 2, 2, fixture.worker.resource());
    buffer secondInbound(4, 4, 4, fixture.worker.resource());
    buffer secondOutbound(2, 2, 2, fixture.worker.resource());
    Connection first(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, firstOutbound, firstScheduler, budget,
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 10, .maxTrackedStreams = 8});
    Connection second(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, secondOutbound, secondScheduler, budget,
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 11, .maxTrackedStreams = 8});

    const auto held = routeRequest(first, firstInbound, fixture.worker,
        {kEpoch, kGeneration + 10, 0}, "POST", "/slow-body", "123456");
    RUVIA_CHECK(held.status == Connection::EventStatus::kDispatched);
    const bool handlerStarted = co_await waitForSlowStart(fixture, worker, fixture.workerStop);
    RUVIA_CHECK(handlerStarted);
    requireWatchdogSuccess(ruvia_ctx, handlerStarted);
    RUVIA_CHECK(fixture.routes.handlers.bodySeen == "123456");
    RUVIA_CHECK_EQ(budget.used(), std::size_t{6});
    RUVIA_CHECK_EQ(first.activeRequestCount(), std::size_t{1});

    const auto overBudget = routeRequest(second, secondInbound, fixture.worker,
        {kEpoch, kGeneration + 11, 0}, "POST", "/body", "xyz");
    RUVIA_CHECK(overBudget.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK(overBudget.rejection ==
                Connection::Session::Rejection::kWorkerBodyBudgetExhausted);
    RUVIA_CHECK_EQ(budget.used(), std::size_t{6});
    const auto refusalHead = second.publishOne({.data = true});
    RUVIA_CHECK(refusalHead.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished);
    const auto refusalFin = second.publishOne({.control = true});
    RUVIA_CHECK(refusalFin.publication.status ==
                Connection::Dispatch::PublishStatus::kFinPublished);
    std::array<PublishedWire, 6> refusedWires{};
    drainAll(secondOutbound, refusedWires);
    const auto refusedResponse = decodeResponse(refusedWires[0],
        ruvia::HttpKnownMethod::kPost, 0, fixture.worker.resource());
    RUVIA_CHECK_EQ(refusedResponse.status, std::uint16_t{503});
    RUVIA_CHECK_EQ(second.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK_EQ(budget.used(), std::size_t{6});

    RUVIA_CHECK(first.requestStop());
    RUVIA_CHECK_EQ(budget.used(), std::size_t{6});
    const auto localRetirement = first.acceptControl(
        {Control::kind::stream_reset, {kEpoch, kGeneration + 10, 0}, 0});
    RUVIA_CHECK(localRetirement.status == Connection::EventStatus::kAdmissionClosed);
    RUVIA_CHECK(localRetirement.input.status == Connection::Input::Status::kStopped);
    RUVIA_CHECK(localRetirement.input.status != Connection::Input::Status::kReset);
    RUVIA_CHECK(localRetirement.connectionCloseRequired);

    const bool firstTasksJoined =
        co_await waitForTaskCount(first, 0, worker, fixture.workerStop);
    requireWatchdogSuccess(ruvia_ctx, firstTasksJoined);
    RUVIA_CHECK(fixture.routes.handlers.slowObservedStop);
    RUVIA_CHECK_EQ(first.activeRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
    co_await first.join();
    simulateGlobalStopTakeover(first);

    RUVIA_CHECK(second.requestStop());
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForTaskCount(second, 0, worker, fixture.workerStop));
    co_await second.join();
    simulateGlobalStopTakeover(second);
    RUVIA_CHECK_EQ(second.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
}

ruvia::Task<void> exerciseHeaderLimitConnectionClose(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr auto generation = kGeneration + 92;
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});
    const std::string largeValue(70 * 1024, 'h');
    const std::array<ruvia::Http3FieldSectionFieldView, 4> fields{{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"x-large", largeValue}}};
    const auto section = ruvia::encodeHttp3FieldSection(fields, fixture.worker.resource());
    if ((section.index() != 0)) {
        throw std::runtime_error("HTTP/3 oversized-field fixture encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(std::get<0>(section).data(), std::get<0>(section).size()));
    const auto failure = acceptWireBytes(connection, inbound,
        {kEpoch, generation, 0}, wire);
    RUVIA_CHECK(failure.status == Connection::EventStatus::kProtocolError);
    RUVIA_CHECK(failure.input.status == Connection::Input::Status::kProtocolError);
    RUVIA_CHECK(failure.input.protocol.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(failure.input.protocol.code == ruvia::Http3ConnectionErrorCode::kExcessiveLoad);
    RUVIA_CHECK(failure.connectionCloseRequired);
    RUVIA_CHECK(connection.transportCloseRequired());
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    const auto close = connection.peekTransportIntent();
    RUVIA_CHECK(close.has_value());
    if (!close) {
        throw std::runtime_error("HTTP/3 oversized-header close intent was not retained");
    }
    RUVIA_CHECK(close->token.kind == Connection::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(close->closeReason == Connection::TransportCloseReason::kConnectionProtocolError);
    RUVIA_CHECK(close->connectionErrorCode == ruvia::Http3ConnectionErrorCode::kExcessiveLoad);
    RUVIA_CHECK_EQ(connection.requestInfo(0).status, Connection::RequestStatus::kUnknown);

    simulateGlobalStopTakeover(connection);
    co_await connection.join();
}

ruvia::Task<void> exerciseInputFailureCloseCodes(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal capacityScheduler(worker);
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer capacityOutbound(1, 1, 1, fixture.worker.resource());
    constexpr auto capacityGeneration = kGeneration + 93;
    Connection capacity(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, capacityOutbound, capacityScheduler,
        {.epoch = kEpoch, .connectionGeneration = capacityGeneration, .maxTrackedStreams = 1});
    constexpr std::array<char, 1> partialData{0};
    const auto first = acceptWireBytes(capacity, inbound,
        {kEpoch, capacityGeneration, 0}, partialData);
    RUVIA_CHECK(first.input.status == Connection::Input::Status::kFed);
    const auto exhausted = acceptWireBytes(capacity, inbound,
        {kEpoch, capacityGeneration, 4}, partialData);
    RUVIA_CHECK(exhausted.status == Connection::EventStatus::kInputRejected);
    RUVIA_CHECK(exhausted.input.status == Connection::Input::Status::kCapacityExhausted);
    RUVIA_CHECK(exhausted.input.protocol.code == ruvia::Http3ConnectionErrorCode::kNoError);
    const auto capacityClose = capacity.peekTransportIntent();
    RUVIA_CHECK(capacityClose.has_value());
    RUVIA_CHECK(capacityClose->token.kind == Connection::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(capacityClose->closeReason == Connection::TransportCloseReason::kInputCapacityExhausted);
    RUVIA_CHECK(capacityClose->connectionErrorCode == ruvia::Http3ConnectionErrorCode::kExcessiveLoad);
    simulateGlobalStopTakeover(capacity);
    co_await capacity.join();

    TestActivationSignal finalSizeScheduler(worker);
    buffer finalSizeOutbound(1, 1, 1, fixture.worker.resource());
    constexpr auto finalSizeGeneration = kGeneration + 94;
    Connection finalSize(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, finalSizeOutbound, finalSizeScheduler,
        {.epoch = kEpoch, .connectionGeneration = finalSizeGeneration, .maxTrackedStreams = 8});
    const auto fed = acceptWireBytes(finalSize, inbound,
        {kEpoch, finalSizeGeneration, 0}, partialData);
    RUVIA_CHECK(fed.input.status == Connection::Input::Status::kFed);
    const auto badFin = finalSize.acceptControl({Control::kind::stream_fin,
        {kEpoch, finalSizeGeneration, 0}, 0});
    RUVIA_CHECK(badFin.status == Connection::EventStatus::kInputRejected);
    RUVIA_CHECK(badFin.input.status == Connection::Input::Status::kFinalSizeError);
    RUVIA_CHECK(badFin.input.protocol.code == ruvia::Http3ConnectionErrorCode::kNoError);
    const auto finalSizeClose = finalSize.peekTransportIntent();
    RUVIA_CHECK(finalSizeClose.has_value());
    RUVIA_CHECK(finalSizeClose->token.kind == Connection::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(finalSizeClose->closeReason == Connection::TransportCloseReason::kFinalSizeError);
    RUVIA_CHECK(finalSizeClose->connectionErrorCode == ruvia::Http3ConnectionErrorCode::kInternalError);
    simulateGlobalStopTakeover(finalSize);
    co_await finalSize.join();
}

ruvia::Task<void> exercisePeerLimitRejection(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::test::CountingMemoryResource& upstream,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer inbound(4, 4, 4, fixture.worker.resource());
    buffer outbound(2, 2, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 40, .maxTrackedStreams = 8});

    const MessageId queuedControlId{kEpoch, kGeneration + 40, 12};
    RUVIA_CHECK(accepted(outbound.try_send_control(
        {Control::kind::writable, queuedControlId, 0})));

    std::array<char, 64> settingsPayload{};
    ruvia::Http3Settings settings;
    settings.maxFieldSectionSize = 0;
    const auto settingsSize = ruvia::encodeHttp3Settings(settingsPayload, settings);
    if ((settingsSize.index() != 0)) {
        throw std::runtime_error("HTTP/3 peer-settings fixture encoding failed");
    }
    std::string controlWire(1, '\0');
    controlWire += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kSettings),
        std::string_view(settingsPayload.data(), std::get<0>(settingsSize)));
    const MessageId controlId{kEpoch, kGeneration + 40, 2};
    const auto controlBytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(controlWire.data()), controlWire.size());
    if (!accepted(inbound.try_send(controlId, controlBytes))) {
        throw std::runtime_error("HTTP/3 peer-settings fixture buffer is full");
    }
    buffer::borrowed_block controlBlock;
    if (!inbound.try_receive(controlBlock)) {
        throw std::runtime_error("HTTP/3 peer-settings data is missing");
    }
    const auto settingsResult = connection.acceptData(controlBlock);
    controlBlock.release();
    RUVIA_CHECK(settingsResult.status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK(settingsResult.input.status == Connection::Input::Status::kFed);

    const auto request = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration + 40, 0}, "GET", "/first");
    RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{1});
    const auto allocationCountBeforeIntent = upstream.allocationCount();
    const auto attempt = connection.publishOne(kAllWorkLanes);
    RUVIA_CHECK(attempt.status == Connection::PublishStatus::kNoReadyRequest);
    RUVIA_CHECK_EQ(connection.requestInfo(0).status, Connection::RequestStatus::kFailed);
    RUVIA_CHECK(!connection.transportCloseRequired());
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), allocationCountBeforeIntent);
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{1});

    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));
    RUVIA_CHECK_EQ(connection.activeRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    const auto resetIntent = connection.peekTransportIntent();
    RUVIA_CHECK(resetIntent.has_value());
    RUVIA_CHECK(resetIntent->token.kind == Connection::TransportIntentKind::kStreamReset);
    RUVIA_CHECK_EQ(resetIntent->token.id.stream_id, std::uint64_t{0});
    RUVIA_CHECK_EQ(resetIntent->token.id.epoch, kEpoch);
    RUVIA_CHECK_EQ(resetIntent->token.id.connection_generation, kGeneration + 40);
    RUVIA_CHECK(resetIntent->streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kRequestCancelled);

    const Control resetControl{.kind = Control::kind::stream_reset,
        .id = resetIntent->token.id,
        .stream_reset_error_code = resetIntent->streamResetErrorCode};
    RUVIA_CHECK(outbound.try_send_control(resetControl) == buffer::control_result::full);
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    RUVIA_CHECK(connection.peekTransportIntent()->token == resetIntent->token);

    Control queuedControl;
    RUVIA_CHECK(outbound.try_receive_control(queuedControl));
    RUVIA_CHECK(queuedControl.kind == Control::kind::writable);
    RUVIA_CHECK(!outbound.try_receive_control(queuedControl));
    RUVIA_CHECK(!outbound.has_pending());

    const auto resetSend = outbound.try_send_control(resetControl);
    RUVIA_CHECK(accepted(resetSend));
    RUVIA_CHECK(resetSend == buffer::control_result::sent);
    RUVIA_CHECK(resetControl.value == 0);
    const auto allocationCountBeforeAck = upstream.allocationCount();
    RUVIA_CHECK(connection.ackTransportIntent(resetIntent->token));
    RUVIA_CHECK(!connection.ackTransportIntent(resetIntent->token));
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    RUVIA_CHECK(!connection.peekTransportIntent().has_value());
    RUVIA_CHECK_EQ(upstream.allocationCount(), allocationCountBeforeAck);

    Control sentReset;
    RUVIA_CHECK(outbound.try_receive_control(sentReset));
    RUVIA_CHECK(sentReset.kind == Control::kind::stream_reset);
    RUVIA_CHECK(sentReset.value == 0);
    RUVIA_CHECK(!outbound.try_receive_control(sentReset));
    RUVIA_CHECK(!outbound.has_pending());

    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
}

}  // namespace

RUVIA_TEST(http3ServerConnectionRoutesAndFairlyPublishesBufferedRequests) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseSuccessfulConnection(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionPublishesRejectionBeforeRequestFin) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseEarlyRejectionBeforeRequestFin(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3_web_socket_rejection_fin_keeps_receive_lifetime_independent) {
    for (const auto retirement : {receive_retirement::fin, receive_retirement::reset, receive_retirement::stop}) {
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
        const auto worker = attachment.loop().handle();
        ruvia::test::CountingMemoryResource upstream;
        {
            Fixture fixture(worker, upstream);
            runWorkerTask(attachment, exercise_web_socket_rejection_receive_retirement(fixture, worker, retirement, ruvia_ctx));
        }
        RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
    }
}

RUVIA_TEST(http3ServerConnectionPublishesMinimalRejectionWithPeerFieldLimitZero) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exercisePeerLimitZeroRejection(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionRetiresRejectedResetAndUnfinishedStop) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseRejectionResetAndStop(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionPublishesBodyAndConnectionBudgetRejections) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseBodyRejectionStatuses(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionPublishesUnsupportedConnectStatus) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseUnsupportedConnect(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionPrioritizesProtocolErrorAfterRejectedHead) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseRejectionProtocolErrorOrders(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionBoundsRejectedStreamIndexCapacity) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseRejectionIndexCapacity(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionKeepsHeaderLimitAsConnectionError) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseHeaderLimitConnectionClose(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionMapsLocalInputFailuresToTransportCodes) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseInputFailureCloseCodes(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionSharesBodyBudgetUntilStoppedLeaseJoins) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseSharedBodyBudget(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3_server_connection_retires_peer_limit_encoding_rejection) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exercisePeerLimitRejection(fixture, worker, upstream, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
