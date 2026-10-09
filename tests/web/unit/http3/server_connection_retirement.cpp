#include <variant>

#include "http3_server_connection_fixture.h"

namespace {

ruvia::Task<void> join_connection_and_notify(Connection& connection,
    ruvia::WorkerSignal& started, bool& joined) {
    started.notify();
    co_await connection.join();
    joined = true;
}

ruvia::Task<void> exercise_join_while_active(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    ruvia::WorkerSignal handlerStarted(worker);
    ruvia::WorkerSignal releaseHandler(worker);
    ruvia::WorkerSignal joinStarted(worker);
    fixture.routes.handlers.heldStarted = &handlerStarted;
    fixture.routes.handlers.heldRelease = &releaseHandler;
    buffer inbound(4, 4, 4, fixture.worker.resource());
    buffer outbound(2, 2, 2, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 29, .maxTrackedStreams = 8});

    const auto request = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration + 29, 0}, "GET", "/held");
    RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    co_await handlerStarted.wait();
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{1});
    RUVIA_CHECK(connection.requestStop());

    bool joined = false;
    ruvia::TaskScope joinScope(worker, {.resource = fixture.worker.resource()});
    joinScope.spawn(join_connection_and_notify(connection, joinStarted, joined));
    co_await joinStarted.wait();
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{1});
    RUVIA_CHECK(!joined);

    releaseHandler.notify();
    co_await joinScope.join();
    RUVIA_CHECK(joined);
    RUVIA_CHECK(fixture.routes.handlers.heldHandlerFinished);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    simulateGlobalStopTakeover(connection);
}

ruvia::Task<void> exerciseResetCancellation(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    ruvia::WorkerSignal slowStarted(worker);
    fixture.routes.handlers.slowStarted = &slowStarted;
    buffer inbound(4, 4, 4, fixture.worker.resource());
    buffer outbound(2, 2, 2, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 1, .maxTrackedStreams = 8});

    const auto wire = requestWire(fixture.worker, "GET", "/slow");
    const auto request = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration + 1, 0}, "GET", "/slow");
    if (request.status != Connection::EventStatus::kDispatched) {
        throw std::runtime_error("slow handler was not dispatched");
    }
    const bool slowStartedInTime =
        co_await waitForSlowStart(fixture, worker, fixture.workerStop);
    RUVIA_CHECK(slowStartedInTime);
    if (!slowStartedInTime) {
        RUVIA_CHECK(connection.requestStop());
        requireWatchdogSuccess(ruvia_ctx,
            co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));
        co_await connection.join();
        simulateGlobalStopTakeover(connection);
        co_return;
    }

    const MessageId id{kEpoch, kGeneration + 1, 0};
    if (!accepted(inbound.try_send_control({.kind = Control::kind::stream_reset,
            .id = id,
            .value = wire.size()}))) {
        throw std::runtime_error("HTTP/3 test RESET buffer is full");
    }
    Control reset;
    if (!inbound.try_receive_control(reset)) {
        throw std::runtime_error("HTTP/3 test RESET control is missing");
    }
    const auto cancelled = connection.acceptControl(reset);
    RUVIA_CHECK(cancelled.status == Connection::EventStatus::kStreamCancelled);
    RUVIA_CHECK(cancelled.input.status == Connection::Input::Status::kReset);
    RUVIA_CHECK(!cancelled.connectionCloseRequired);
    RUVIA_CHECK(!connection.transportCloseRequired());
    RUVIA_CHECK_EQ(connection.requestInfo(0).status, Connection::RequestStatus::kCancelled);
    RUVIA_CHECK_EQ(connection.readyRequestCount(), std::size_t{0});

    const bool retiredAfterReset =
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop);
    requireWatchdogSuccess(ruvia_ctx, retiredAfterReset);
    RUVIA_CHECK_EQ(connection.activeRequestCount(), std::size_t{0});
    RUVIA_CHECK(fixture.routes.handlers.slowObservedStop);

    RUVIA_CHECK(connection.requestStop());
    RUVIA_CHECK(connection.transportCloseRequired());
    const auto localRetirement = connection.acceptControl(
        {.kind = Control::kind::stream_reset, .id = id, .value = wire.size()});
    RUVIA_CHECK(localRetirement.status == Connection::EventStatus::kAdmissionClosed);
    RUVIA_CHECK(localRetirement.input.status == Connection::Input::Status::kStopped);
    RUVIA_CHECK(localRetirement.input.status != Connection::Input::Status::kReset);
    RUVIA_CHECK(localRetirement.connectionCloseRequired);
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK(fixture.routes.handlers.slowObservedStop);
    RUVIA_CHECK_EQ(connection.activeRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    buffer::borrowed_block unexpected;
    Control unexpectedControl;
    RUVIA_CHECK(!outbound.try_receive(unexpected));
    RUVIA_CHECK(!outbound.try_receive_control(unexpectedControl));
}

ruvia::Task<void> exercisePartialPublishStop(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer inbound(4, 4, 4, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 2, .maxTrackedStreams = 8});

    const auto request = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration + 2, 0}, "GET", "/first");
    RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    co_await waitForReady(connection, 1, worker, fixture.workerStop);
    const auto published = connection.publishOne(kAllWorkLanes);
    RUVIA_CHECK(published.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK(published.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished);

    RUVIA_CHECK(connection.requestStop());
    RUVIA_CHECK(!connection.requestStop());
    RUVIA_CHECK(connection.transportCloseRequired());
    RUVIA_CHECK(connection.publishOne(kAllWorkLanes).status ==
                Connection::PublishStatus::kNoReadyRequest);
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK_EQ(connection.requestInfo(0).status, Connection::RequestStatus::kCancelled);
    RUVIA_CHECK_EQ(connection.activeRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});

    std::array<PublishedWire, 6> wires{};
    drainAll(outbound, wires);
    RUVIA_CHECK(!wires[0].bytes.empty());
    RUVIA_CHECK(!wires[0].finalWireBytes.has_value());
}

ruvia::Task<void> exerciseResetIntentMergePolicy(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr auto generation = kGeneration + 89;
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});
    constexpr std::uint64_t streamId = 0;
    using TestAccess = ruvia::detail::Http3ServerConnectionResetIntentTestAccess;

    RUVIA_CHECK(TestAccess::enqueueLocal(connection, streamId));
    const auto local = connection.peekTransportIntent();
    RUVIA_CHECK(local.has_value());
    if (!local) {
        throw std::runtime_error("local reset fixture intent was not retained");
    }
    RUVIA_CHECK(local->streamResetErrorCode == ruvia::Http3ConnectionErrorCode::kRequestCancelled);
    RUVIA_CHECK(TestAccess::enqueueLocal(connection, streamId));
    RUVIA_CHECK(connection.peekTransportIntent()->token == local->token);

    RUVIA_CHECK(TestAccess::enqueueProtocol(connection, streamId,
        ruvia::Http3ConnectionErrorCode::kMessageError));
    const auto protocol = connection.peekTransportIntent();
    RUVIA_CHECK(protocol.has_value());
    if (!protocol) {
        throw std::runtime_error("protocol reset fixture intent was not retained");
    }
    RUVIA_CHECK(protocol->streamResetErrorCode == ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK_EQ(protocol->token.id.epoch, local->token.id.epoch);
    RUVIA_CHECK_EQ(protocol->token.id.connection_generation,
        local->token.id.connection_generation);
    RUVIA_CHECK_EQ(protocol->token.id.stream_id, local->token.id.stream_id);
    RUVIA_CHECK(protocol->token.sequence != local->token.sequence);
    RUVIA_CHECK(connection.ackTransportIntent(local->token));
    RUVIA_CHECK(connection.peekTransportIntent()->token == protocol->token);

    RUVIA_CHECK(TestAccess::enqueueProtocol(connection, streamId,
        ruvia::Http3ConnectionErrorCode::kExcessiveLoad));
    RUVIA_CHECK(TestAccess::enqueueLocal(connection, streamId));
    RUVIA_CHECK(TestAccess::enqueueProtocol(connection, streamId,
        ruvia::Http3ConnectionErrorCode::kMessageError));
    const auto unchanged = connection.peekTransportIntent();
    RUVIA_CHECK(unchanged.has_value());
    RUVIA_CHECK(unchanged->token == protocol->token);
    RUVIA_CHECK(unchanged->streamResetErrorCode == ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK(connection.ackTransportIntent(protocol->token));
    RUVIA_CHECK(!connection.ackTransportIntent(protocol->token));
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    RUVIA_CHECK(!connection.transportRetired());

    RUVIA_CHECK(connection.requestStop());
    simulateGlobalStopTakeover(connection);
    co_await connection.join();
}

ruvia::Task<void> exercisePersistentProtocolResetIntent(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr auto generation = kGeneration + 90;
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});
    const MessageId id{kEpoch, generation, 0};
    const MessageId queuedControlId{kEpoch, generation, 12};
    RUVIA_CHECK(accepted(outbound.try_send_control(
        {Control::kind::writable, queuedControlId, 0})));

    const std::array<ruvia::Http3FieldSectionFieldView, 4> invalidFields{{{":method", "GET"}, {":scheme", "https"}, {"x-before-path", "bad"}, {":path", "/"}}};
    const auto section = ruvia::encodeHttp3FieldSection(invalidFields, fixture.worker.resource());
    if ((section.index() != 0)) {
        throw std::runtime_error("HTTP/3 malformed-field fixture encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(std::get<0>(section).data(), std::get<0>(section).size()));
    {
        const auto failure = acceptWireBytes(connection, inbound, id, wire);
        RUVIA_CHECK(failure.status == Connection::EventStatus::kProtocolError);
        RUVIA_CHECK(failure.input.status == Connection::Input::Status::kProtocolError);
        RUVIA_CHECK(failure.input.protocol.scope == ruvia::Http3ConnectionErrorScope::kStream);
        RUVIA_CHECK(failure.input.protocol.code == ruvia::Http3ConnectionErrorCode::kMessageError);
        RUVIA_CHECK(!failure.connectionCloseRequired);
    }

    RUVIA_CHECK(!connection.transportCloseRequired());
    RUVIA_CHECK_EQ(connection.requestInfo(0).status, Connection::RequestStatus::kProtocolError);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    const auto intent = connection.peekTransportIntent();
    RUVIA_CHECK(intent.has_value());
    if (!intent) {
        throw std::runtime_error("HTTP/3 stream protocol reset intent was not retained");
    }
    RUVIA_CHECK(intent->token.kind == Connection::TransportIntentKind::kStreamReset);
    RUVIA_CHECK_EQ(intent->token.id.stream_id, std::uint64_t{0});
    RUVIA_CHECK(intent->streamResetErrorCode == ruvia::Http3ConnectionErrorCode::kMessageError);

    // A late duplicate cannot change the persistent intent or its token.
    const auto duplicate = acceptWireBytes(connection, inbound, id, wire);
    RUVIA_CHECK(duplicate.input.status == Connection::Input::Status::kClosedStream);
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    RUVIA_CHECK(connection.peekTransportIntent()->token == intent->token);

    const Control resetControl{.kind = Control::kind::stream_reset,
        .id = intent->token.id,
        .stream_reset_error_code = intent->streamResetErrorCode};
    RUVIA_CHECK(outbound.try_send_control(resetControl) == buffer::control_result::full);
    RUVIA_CHECK(connection.peekTransportIntent()->token == intent->token);
    Control queuedControl;
    RUVIA_CHECK(outbound.try_receive_control(queuedControl));
    RUVIA_CHECK(queuedControl.kind == Control::kind::writable);
    RUVIA_CHECK(!outbound.has_pending());
    const auto sent = outbound.try_send_control(resetControl);
    RUVIA_CHECK(sent == buffer::control_result::sent);
    RUVIA_CHECK_EQ(resetControl.value, std::uint64_t{0});
    RUVIA_CHECK(connection.ackTransportIntent(intent->token));
    RUVIA_CHECK(!connection.ackTransportIntent(intent->token));
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    RUVIA_CHECK(!connection.transportRetired());
    Control receivedReset;
    RUVIA_CHECK(outbound.try_receive_control(receivedReset));
    RUVIA_CHECK(receivedReset.kind == Control::kind::stream_reset);
    RUVIA_CHECK_EQ(receivedReset.value, std::uint64_t{0});
    RUVIA_CHECK(receivedReset.stream_reset_error_code == ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK(!outbound.has_pending());
    const auto afterHandoff = acceptWireBytes(connection, inbound, id, wire);
    RUVIA_CHECK(afterHandoff.input.status == Connection::Input::Status::kClosedStream);
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    RUVIA_CHECK(!connection.transportRetired());

    RUVIA_CHECK(connection.requestStop());
    simulateGlobalStopTakeover(connection);
    co_await connection.join();
}

ruvia::Task<void> exerciseStreamExcessiveLoadResetIntent(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr auto generation = kGeneration + 91;
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch,
            .connectionGeneration = generation,
            .session = {.maxLiveStreams = 200},
            .maxTrackedStreams = 200});
    constexpr std::array<char, 1> partialData{0};
    Connection::EventResult result;
    for (std::uint64_t streamId = 0; streamId <= 128 * 4; streamId += 4) {
        result = acceptWireBytes(connection, inbound,
            {kEpoch, generation, streamId}, partialData);
        if (streamId < 128 * 4) {
            RUVIA_CHECK(result.status == Connection::EventStatus::kAccepted);
            RUVIA_CHECK(result.input.status == Connection::Input::Status::kFed);
        }
    }
    RUVIA_CHECK(result.status == Connection::EventStatus::kProtocolError);
    RUVIA_CHECK(result.input.status == Connection::Input::Status::kProtocolError);
    RUVIA_CHECK(result.input.protocol.scope == ruvia::Http3ConnectionErrorScope::kStream);
    RUVIA_CHECK(result.input.protocol.code == ruvia::Http3ConnectionErrorCode::kExcessiveLoad);
    RUVIA_CHECK(!result.connectionCloseRequired);
    RUVIA_CHECK(!connection.transportCloseRequired());
    RUVIA_CHECK_EQ(connection.requestInfo(128 * 4).status,
        Connection::RequestStatus::kProtocolError);
    const auto intent = connection.peekTransportIntent();
    RUVIA_CHECK(intent.has_value());
    if (!intent) {
        throw std::runtime_error("HTTP/3 stream excessive-load reset intent was not retained");
    }
    RUVIA_CHECK(intent->token.kind == Connection::TransportIntentKind::kStreamReset);
    RUVIA_CHECK(intent->streamResetErrorCode == ruvia::Http3ConnectionErrorCode::kExcessiveLoad);
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});

    RUVIA_CHECK(connection.requestStop());
    simulateGlobalStopTakeover(connection);
    co_await connection.join();
}

ruvia::Task<Connection::TransportIntentToken> createPeerLimitIntent(
    Connection& connection, buffer& inbound, ruvia::WorkerMemory& worker,
    std::uint64_t epoch, std::uint64_t generation, const ruvia::WorkerHandle& workerHandle,
    const ruvia::StopToken& stopToken, ruvia::testing::TestContext& ruvia_ctx) {
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
    const MessageId controlId{epoch, generation, 2};
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

    const auto request = routeRequest(connection, inbound, worker,
        {epoch, generation, 0}, "GET", "/first");
    RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    const bool taskRetired =
        co_await waitForTaskCount(connection, 0, workerHandle, stopToken);
    requireWatchdogSuccess(ruvia_ctx, taskRetired);
    const auto intent = connection.peekTransportIntent();
    RUVIA_CHECK(intent.has_value());
    if (!intent) {
        throw std::runtime_error("HTTP/3 peer-limit reset intent was not retained");
    }
    RUVIA_CHECK(intent->token.kind == Connection::TransportIntentKind::kStreamReset);
    co_return intent->token;
}

ruvia::Task<void> exerciseStaleIntentTokens(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    constexpr std::uint64_t oldGeneration = kGeneration + 50;
    TestActivationSignal oldScheduler(worker);
    buffer oldInbound(4, 4, 4, fixture.worker.resource());
    buffer oldOutbound(2, 2, 2, fixture.worker.resource());
    Connection oldOwner(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, oldOutbound, oldScheduler,
        {.epoch = kEpoch, .connectionGeneration = oldGeneration, .maxTrackedStreams = 8});
    const auto oldToken = co_await createPeerLimitIntent(oldOwner, oldInbound,
        fixture.worker, kEpoch, oldGeneration, worker, fixture.workerStop, ruvia_ctx);
    RUVIA_CHECK(oldOwner.requestStop());
    co_await oldOwner.join();
    simulateGlobalStopTakeover(oldOwner);

    constexpr std::array<std::pair<std::uint64_t, std::uint64_t>, 2> newIdentities{{
        {kEpoch + 1, oldGeneration},
        {kEpoch, oldGeneration + 1},
    }};
    for (const auto& [epoch, generation] : newIdentities) {
        TestActivationSignal scheduler(worker);
        buffer inbound(4, 4, 4, fixture.worker.resource());
        buffer outbound(2, 2, 2, fixture.worker.resource());
        Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.services, fixture.options, outbound, scheduler,
            {.epoch = epoch, .connectionGeneration = generation, .maxTrackedStreams = 8});
        const auto currentToken = co_await createPeerLimitIntent(connection, inbound,
            fixture.worker, epoch, generation, worker, fixture.workerStop, ruvia_ctx);

        RUVIA_CHECK_EQ(currentToken.id.stream_id, oldToken.id.stream_id);
        RUVIA_CHECK_EQ(currentToken.sequence, oldToken.sequence);
        RUVIA_CHECK(!connection.ackTransportIntent(oldToken));
        RUVIA_CHECK(connection.peekTransportIntent()->token == currentToken);

        RUVIA_CHECK(connection.requestStop());
        RUVIA_CHECK(!connection.requestStop());
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{2});
        const auto closeIntent = connection.peekTransportIntent();
        RUVIA_CHECK(closeIntent.has_value());
        RUVIA_CHECK(closeIntent->token.kind == Connection::TransportIntentKind::kConnectionClose);
        RUVIA_CHECK(closeIntent->closeReason == Connection::TransportCloseReason::kLocalStop);
        RUVIA_CHECK(!connection.transportRetired());
        RUVIA_CHECK(connection.ackTransportIntent(closeIntent->token));
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
        RUVIA_CHECK(!connection.transportRetired());
        RUVIA_CHECK(connection.ackTransportIntent(currentToken));
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
        RUVIA_CHECK(connection.confirmTransportRetired(
            {.epoch = epoch, .connectionGeneration = generation}));
        RUVIA_CHECK(connection.transportRetired());
        co_await connection.join();
    }
}

ruvia::Task<void> exerciseGlobalStopWithUnsentReset(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer inbound(4, 4, 4, fixture.worker.resource());
    buffer outbound(2, 2, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 60, .maxTrackedStreams = 8});
    const MessageId queuedControlId{kEpoch, kGeneration + 60, 12};
    RUVIA_CHECK(accepted(outbound.try_send_control(
        {Control::kind::writable, queuedControlId, 0})));

    const auto resetToken = co_await createPeerLimitIntent(connection, inbound,
        fixture.worker, kEpoch, kGeneration + 60, worker, fixture.workerStop, ruvia_ctx);
    RUVIA_CHECK(outbound.try_send_control(
                    {Control::kind::stream_reset, resetToken.id, 0}) == buffer::control_result::full);
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});

    RUVIA_CHECK(connection.requestStop());
    RUVIA_CHECK(connection.transportCloseRequired());
    RUVIA_CHECK(!connection.transportRetired());
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{2});
    const bool tasksJoined =
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop);
    requireWatchdogSuccess(ruvia_ctx, tasksJoined);
    co_await connection.join();
    RUVIA_CHECK_EQ(connection.activeRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK(!connection.transportRetired());
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{2});

    const auto closeIntent = connection.peekTransportIntent();
    RUVIA_CHECK(closeIntent.has_value());
    RUVIA_CHECK(closeIntent->token.kind == Connection::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(connection.takeOverTransportRetirement(
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 60}));
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{2});
    RUVIA_CHECK(connection.ackTransportIntent(closeIntent->token));
    const auto resetAfterClose = connection.peekTransportIntent();
    RUVIA_CHECK(resetAfterClose.has_value());
    RUVIA_CHECK(resetAfterClose->token.kind == Connection::TransportIntentKind::kStreamReset);
    RUVIA_CHECK(connection.ackTransportIntent(resetAfterClose->token));
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    RUVIA_CHECK(!connection.transportRetired());
}

ruvia::Task<void> exerciseRetirementIntentAckOrder(
    Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal activation(worker);
    using TestAccess = ruvia::detail::Http3ServerConnectionResetIntentTestAccess;
    for (const bool retireBeforeAck : std::array{false, true}) {
        buffer outbound(1, 1, 1, fixture.worker.resource());
        const auto generation = kGeneration + (retireBeforeAck ? 171 : 170);
        Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.services, fixture.options, outbound, activation,
            {.epoch = kEpoch,
                .connectionGeneration = generation,
                .maxTrackedStreams = 8});
        RUVIA_CHECK(TestAccess::enqueueLocal(connection, 0));
        RUVIA_CHECK(connection.requestStop());
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{2});
        if (retireBeforeAck) {
            RUVIA_CHECK(connection.confirmTransportRetired(
                {.epoch = kEpoch, .connectionGeneration = generation}));
        }
        const auto close = connection.peekTransportIntent();
        RUVIA_CHECK(close.has_value());
        RUVIA_CHECK(close->token.kind == Connection::TransportIntentKind::kConnectionClose);
        RUVIA_CHECK(connection.ackTransportIntent(close->token));
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
        const auto reset = connection.peekTransportIntent();
        RUVIA_CHECK(reset.has_value());
        RUVIA_CHECK(reset->token.kind == Connection::TransportIntentKind::kStreamReset);
        RUVIA_CHECK_EQ(reset->token.id.stream_id, std::uint64_t{0});
        if (!retireBeforeAck) {
            RUVIA_CHECK(connection.confirmTransportRetired(
                {.epoch = kEpoch, .connectionGeneration = generation}));
        }
        RUVIA_CHECK(connection.peekTransportIntent()->token == reset->token);
        RUVIA_CHECK(connection.ackTransportIntent(reset->token));
        RUVIA_CHECK(!connection.ackTransportIntent(reset->token));
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
        RUVIA_CHECK(connection.transportRetired());
        RUVIA_CHECK(outbound.stop());
    }
    co_return;
}

ruvia::Task<void> exerciseSchedulerSettlesSupersededReset(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    using Scheduler = ruvia::detail::http3_ready_scheduler;
    using State = ruvia::detail::http3_connection_state;
    using TestAccess = ruvia::detail::Http3ServerConnectionResetIntentTestAccess;
    Scheduler scheduler(worker, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    State state;
    const ruvia::detail::http3_connection_identity identity{kEpoch + 180, kGeneration + 180};
    RUVIA_CHECK(state.reserve(scheduler, identity.epoch, identity.connection_generation) ==
                State::status::changed);
    const auto registration = state.registration();
    RUVIA_CHECK(registration.has_value());
    if (!registration) {
        throw std::runtime_error("scheduler local reset fixture slot unavailable");
    }
    RUVIA_CHECK(state.bind(identity) == State::status::changed);
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, registration->activation,
        {.epoch = identity.epoch,
            .connectionGeneration = identity.connection_generation,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(state.attach_handler(identity, connection) == State::status::changed);
    struct ExecutionTrace final {
        State* state;
        Connection* connection;
        std::size_t executions{};
        bool saw_unsettled{};
        std::optional<Connection::TransportIntentToken> executed_token;
    } trace{&state, &connection};
    state.set_transport_executor({&trace, [](void* context, ruvia::detail::http3_connection_identity executing_identity, const Connection::TransportIntent& intent) noexcept -> State::intent_execution_result {
                                      auto& executed = *static_cast<ExecutionTrace*>(context);
                                      ++executed.executions;
                                      executed.saw_unsettled = executed.connection->pendingTransportIntentCount() != 0;
                                      executed.executed_token = intent.token;
                                      if (intent.token.kind == Connection::TransportIntentKind::kConnectionClose &&
                                          executed.state->mark_transport_retired(executing_identity) != State::status::changed) {
                                          std::terminate();
                                      }
                                      return {.outcome = State::execution_outcome::executed};
                                  }});
    RUVIA_CHECK(TestAccess::enqueueLocal(connection, 0));
    const auto offered = scheduler.step();
    RUVIA_CHECK(offered.kind == Scheduler::step_kind::transport_intent);
    RUVIA_CHECK(offered.intent.token.kind == Connection::TransportIntentKind::kStreamReset);
    RUVIA_CHECK_EQ(trace.executions, std::size_t{0});
    RUVIA_CHECK(TestAccess::enqueueProtocol(connection, 0,
        ruvia::Http3ConnectionErrorCode::kMessageError));
    const auto current = connection.peekTransportIntent();
    RUVIA_CHECK(current.has_value());
    RUVIA_CHECK(current->token.sequence != offered.intent.token.sequence);
    RUVIA_CHECK(scheduler.step().kind == Scheduler::step_kind::idle);
    const auto first_execution = state.execute_intent(identity, offered.intent);
    RUVIA_CHECK(first_execution.completed());
    RUVIA_CHECK(trace.saw_unsettled);
    RUVIA_CHECK(trace.executed_token == offered.intent.token);
    RUVIA_CHECK(connection.peekTransportIntent()->token == current->token);
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token,
        offered.intent.token, first_execution.push_stream));
    const auto replacement = scheduler.step();
    RUVIA_CHECK(replacement.kind == Scheduler::step_kind::transport_intent);
    RUVIA_CHECK(replacement.intent.token == current->token);
    const auto replacement_execution = state.execute_intent(identity, replacement.intent);
    RUVIA_CHECK(replacement_execution.completed());
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token,
        replacement.intent.token, replacement_execution.push_stream));
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});

    RUVIA_CHECK(connection.requestStop());
    const auto close = scheduler.step();
    RUVIA_CHECK(close.kind == Scheduler::step_kind::transport_intent);
    RUVIA_CHECK(close.intent.token.kind == Connection::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(state.start_worker_draining(identity) == State::status::changed);
    const auto close_execution = state.execute_intent(identity, close.intent);
    RUVIA_CHECK(close_execution.outcome == State::execution_outcome::transport_retired);
    RUVIA_CHECK(state.transport_retired());
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token,
        close.intent.token, close_execution.push_stream));
    co_await connection.join();
    RUVIA_CHECK(state.mark_worker_finalized(identity) == State::status::changed);
    RUVIA_CHECK(state.retire(identity) == State::status::changed);
    RUVIA_CHECK(state.ready_to_destroy());
    RUVIA_CHECK_EQ(trace.executions, std::size_t{3});
    RUVIA_CHECK(outbound.stop());
}

}  // namespace

RUVIA_TEST(http3_server_connection_starts_join_while_handler_is_active) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exercise_join_while_active(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionMergesPendingResetCodesWithFreshTokens) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseResetIntentMergePolicy(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionRetirementKeepsExactIntentDebtsAcrossAckOrder) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseRetirementIntentAckOrder(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionSchedulerSettlesSupersededResetBeforeClose) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseSchedulerSettlesSupersededReset(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionPersistsCoreStreamResetCode) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exercisePersistentProtocolResetIntent(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionPersistsStreamExcessiveLoadResetCode) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseStreamExcessiveLoadResetIntent(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionResetAfterFinCancelsAndJoinsHandler) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseResetCancellation(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionStopAfterPartialPublishClosesAdmissionAndJoins) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exercisePartialPublishStop(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionRejectsStaleIntentTokensAndPrioritizesClose) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseStaleIntentTokens(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionGlobalStopTakesOverUnsentResetAfterJoin) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseGlobalStopWithUnsentReset(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
