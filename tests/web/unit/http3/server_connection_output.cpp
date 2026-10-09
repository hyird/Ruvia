#include <variant>

#include "http3_server_connection_fixture.h"

namespace {

ruvia::Task<void> signalProbe(ruvia::WorkerSignal& signal, bool& awoke) {
    co_await signal.wait();
    awoke = true;
}

ruvia::Task<bool> waitForLocalWork(Connection& connection,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stopToken) {
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        const auto state = connection.workState();
        if (state.wrongWorker) {
            co_return false;
        }
        if (state.runnable.local) {
            co_return true;
        }
        if (co_await ruvia::sleepFor(worker, 1ms, stopToken) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    co_return connection.workState().runnable.local;
}

ruvia::Task<void> exerciseLaneQueueIsolation(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    ruvia::TaskScope probes(worker, {.resource = fixture.worker.resource()});
    buffer inbound(8, 8, 4, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 70, .maxTrackedStreams = 8});

    const MessageId fillerId{kEpoch, kGeneration + 70, 12};
    RUVIA_CHECK(accepted(outbound.try_send_control(
        {Control::kind::writable, fillerId, 0})));
    const auto head = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration + 70, 4}, "HEAD", "/file");
    RUVIA_CHECK(head.status == Connection::EventStatus::kDispatched);
    co_await waitForReady(connection, 1, worker, fixture.workerStop);
    const auto headHeaders = connection.publishOne(kAllWorkLanes);
    RUVIA_CHECK(headHeaders.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK_EQ(headHeaders.streamId, std::uint64_t{4});
    RUVIA_CHECK(headHeaders.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished);
    const auto blockedControl = connection.publishOne(kAllWorkLanes);
    RUVIA_CHECK(blockedControl.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK_EQ(blockedControl.streamId, std::uint64_t{4});
    RUVIA_CHECK(blockedControl.publication.status ==
                Connection::Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blockedControl.publication.blockReason ==
                Connection::Dispatch::PublishBlockReason::kControl);

    const auto data = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration + 70, 0}, "GET", "/first");
    RUVIA_CHECK(data.status == Connection::EventStatus::kDispatched);
    co_await waitForReady(connection, 1, worker, fixture.workerStop);
    const auto blockedData = connection.publishOne(kAllWorkLanes);
    RUVIA_CHECK(blockedData.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK_EQ(blockedData.streamId, std::uint64_t{0});
    RUVIA_CHECK(blockedData.publication.status ==
                Connection::Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blockedData.publication.blockReason ==
                Connection::Dispatch::PublishBlockReason::kData);

    auto state = connection.workState();
    RUVIA_CHECK(!state.wrongWorker);
    RUVIA_CHECK(!state.runnable.data && !state.runnable.control && !state.runnable.local);
    RUVIA_CHECK(state.blocked.data && state.blocked.control);
    RUVIA_CHECK_EQ(state.blockedCount, std::size_t{2});
    RUVIA_CHECK(connection.publishOne(kAllWorkLanes).status ==
                Connection::PublishStatus::kNoReadyRequest);
    RUVIA_CHECK(connection.publishOne(kAllWorkLanes).status ==
                Connection::PublishStatus::kNoReadyRequest);

    // Consume the last work-activation notification. Parking and idle probes
    // must not manufacture repeated wakeups.
    co_await scheduler.wait();
    bool schedulerAwokeAgain = false;
    probes.spawn(signalProbe(scheduler.signal, schedulerAwokeAgain));
    const auto slept = co_await ruvia::sleepFor(worker, 5ms, fixture.workerStop);
    requireWatchdogSuccess(ruvia_ctx, slept == ruvia::TimerSleepResult::kElapsed);
    RUVIA_CHECK(!schedulerAwokeAgain);

    std::array<PublishedWire, 6> drainedWires{};
    drainDataOnly(outbound, drainedWires);
    const auto reactivated = connection.reactivateBlocked({.data = true});
    RUVIA_CHECK_EQ(reactivated, std::size_t{1});
    state = connection.workState();
    RUVIA_CHECK(state.runnable.data && state.blocked.control && !state.blocked.data);
    const auto retriedData = connection.publishOne({.data = true});
    RUVIA_CHECK(retriedData.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK_EQ(retriedData.streamId, std::uint64_t{0});
    RUVIA_CHECK(retriedData.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished);
    const auto blockedAgain = connection.publishOne({.data = true});
    RUVIA_CHECK(blockedAgain.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK_EQ(blockedAgain.streamId, std::uint64_t{0});
    RUVIA_CHECK(blockedAgain.publication.status ==
                Connection::Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blockedAgain.publication.blockReason ==
                Connection::Dispatch::PublishBlockReason::kData);
    RUVIA_CHECK(connection.publishOne({.control = true}).status ==
                Connection::PublishStatus::kNoReadyRequest);
    state = connection.workState();
    RUVIA_CHECK(state.blocked.data && state.blocked.control);
    RUVIA_CHECK(!state.runnable.control && !state.runnable.local);
    RUVIA_CHECK(!schedulerAwokeAgain);

    RUVIA_CHECK(connection.requestStop());
    RUVIA_CHECK_EQ(connection.readyRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.workState().blockedCount, std::size_t{0});
    RUVIA_CHECK_EQ(connection.requestInfo(0).status, Connection::RequestStatus::kCancelled);
    RUVIA_CHECK_EQ(connection.requestInfo(4).status, Connection::RequestStatus::kCancelled);
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));
    RUVIA_CHECK(schedulerAwokeAgain);
    co_await probes.join();
    co_await connection.join();
    std::array<PublishedWire, 6> remaining{};
    drainAll(outbound, remaining);
    simulateGlobalStopTakeover(connection);
}

ruvia::Task<void> exercisePublicationDeadlineForLane(Fixture& fixture,
    const ruvia::WorkerHandle& worker, bool controlLane, std::uint64_t generation,
    ruvia::testing::TestContext& ruvia_ctx) {
    fixture.options.deadline = ruvia::DeadlineConfig{.handler = 80ms};
    fixture.routes.handlers.handlerCalls = 0;
    if (!controlLane) {
        fixture.routes.handlers.largeResponseHeader.assign(20 * 1024, 'd');
    }

    TestActivationSignal scheduler(worker);
    buffer inbound(8, 8, 4, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});
    const MessageId id{kEpoch, generation, 0};
    if (controlLane) {
        RUVIA_CHECK(accepted(outbound.try_send_control(
            {Control::kind::writable, {kEpoch, generation, 12}, 0})));
    }

    const auto method = controlLane ? "HEAD" : "GET";
    const auto path = controlLane ? "/file" : "/first";
    const auto request = routeRequest(connection, inbound, fixture.worker, id, method, path);
    RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    co_await waitForReady(connection, 1, worker, fixture.workerStop);

    if (controlLane) {
        const auto headers = connection.publishOne(kAllWorkLanes);
        RUVIA_CHECK(headers.status == Connection::PublishStatus::kAttempted);
        RUVIA_CHECK(headers.publication.status ==
                    Connection::Dispatch::PublishStatus::kBytesPublished);
        std::array<PublishedWire, 6> discarded{};
        drainDataOnly(outbound, discarded);
    } else {
        const auto prefix = connection.publishOne(kAllWorkLanes);
        RUVIA_CHECK(prefix.status == Connection::PublishStatus::kAttempted);
        RUVIA_CHECK(prefix.publication.status ==
                    Connection::Dispatch::PublishStatus::kBytesPublished);
        RUVIA_CHECK(prefix.publication.bytesPublished > 0);
    }

    const auto blocked = connection.publishOne(kAllWorkLanes);
    RUVIA_CHECK(blocked.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK(blocked.publication.status ==
                Connection::Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blocked.publication.blockReason ==
                (controlLane ? Connection::Dispatch::PublishBlockReason::kControl
                             : Connection::Dispatch::PublishBlockReason::kData));
    auto state = connection.workState();
    RUVIA_CHECK(state.blocked.contains(controlLane ? Connection::WorkLane::kControl
                                                   : Connection::WorkLane::kData));
    RUVIA_CHECK_EQ(state.blockedCount, std::size_t{1});

    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForLocalWork(connection, worker, fixture.workerStop));
    state = connection.workState();
    RUVIA_CHECK(state.runnable.local);
    RUVIA_CHECK(!state.blocked.data && !state.blocked.control);
    const auto expired = connection.publishOne({.local = true});
    RUVIA_CHECK(expired.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK_EQ(expired.streamId, id.stream_id);
    RUVIA_CHECK(expired.publication.status ==
                Connection::Dispatch::PublishStatus::kCancelled);
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));

    RUVIA_CHECK_EQ(connection.requestInfo(0).status, Connection::RequestStatus::kCancelled);
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    RUVIA_CHECK(!connection.transportCloseRequired());
    const auto reset = connection.peekTransportIntent();
    RUVIA_CHECK(reset.has_value());
    RUVIA_CHECK(reset->token.kind == Connection::TransportIntentKind::kStreamReset);
    RUVIA_CHECK_EQ(reset->token.id.epoch, id.epoch);
    RUVIA_CHECK_EQ(reset->token.id.connection_generation, id.connection_generation);
    RUVIA_CHECK_EQ(reset->token.id.stream_id, id.stream_id);
    RUVIA_CHECK(reset->streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kRequestCancelled);

    const auto finalSize = requestWire(fixture.worker, method, path).size();
    const auto lateFin = connection.acceptControl(
        {Control::kind::stream_fin, id, static_cast<std::uint64_t>(finalSize)});
    RUVIA_CHECK(lateFin.input.status == Connection::Input::Status::kClosedStream);
    RUVIA_CHECK(!connection.transportCloseRequired());
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{1});

    std::array<PublishedWire, 6> wires{};
    drainAll(outbound, wires);
    if (!controlLane) {
        RUVIA_CHECK(!wires[0].bytes.empty());
        RUVIA_CHECK(!wires[0].finalWireBytes.has_value());
    }

    const auto sibling = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, generation, 4}, "GET", "/first");
    RUVIA_CHECK(sibling.status == Connection::EventStatus::kDispatched);
    co_await waitForReady(connection, 1, worker, fixture.workerStop);
    bool siblingPublished = false;
    for (std::size_t attempt = 0; attempt < 32 && !siblingPublished; ++attempt) {
        const auto publication = connection.publishOne(kAllWorkLanes);
        if (publication.status == Connection::PublishStatus::kNoReadyRequest) {
            break;
        }
        RUVIA_CHECK(publication.status == Connection::PublishStatus::kAttempted);
        switch (publication.publication.status) {
            case Connection::Dispatch::PublishStatus::kBytesPublished:
                drainDataOnly(outbound, wires);
                (void)connection.reactivateBlocked({.data = true});
                break;
            case Connection::Dispatch::PublishStatus::kFinPublished:
            case Connection::Dispatch::PublishStatus::kComplete:
                drainAll(outbound, wires);
                (void)connection.reactivateBlocked({.control = true});
                siblingPublished = true;
                break;
            case Connection::Dispatch::PublishStatus::kBackpressured:
                if (publication.publication.blockReason ==
                    Connection::Dispatch::PublishBlockReason::kData) {
                    drainDataOnly(outbound, wires);
                    (void)connection.reactivateBlocked({.data = true});
                } else {
                    drainAll(outbound, wires);
                    (void)connection.reactivateBlocked({.control = true});
                }
                break;
            default:
                RUVIA_CHECK(false);
                break;
        }
    }
    RUVIA_CHECK(siblingPublished);
    RUVIA_CHECK_EQ(connection.requestInfo(4).status, Connection::RequestStatus::kPublished);
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{2});
    RUVIA_CHECK(!connection.transportCloseRequired());

    RUVIA_CHECK(connection.requestStop());
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
}

ruvia::Task<void> exerciseHandlerDeadlineCancellation(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    fixture.options.deadline = ruvia::DeadlineConfig{.handler = 80ms};
    fixture.routes.handlers.handlerCalls = 0;
    TestActivationSignal scheduler(worker);
    ruvia::WorkerSignal handlerStarted(worker);
    fixture.routes.handlers.slowStarted = &handlerStarted;
    buffer inbound(4, 4, 2, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr auto generation = kGeneration + 83;
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});
    const MessageId id{kEpoch, generation, 0};
    const auto request = routeRequest(connection, inbound, fixture.worker, id, "GET", "/slow");
    RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForSlowStart(fixture, worker, fixture.workerStop));
    requireWatchdogSuccess(ruvia_ctx,
        co_await waitForTaskCount(connection, 0, worker, fixture.workerStop));

    RUVIA_CHECK_EQ(connection.requestInfo(0).status, Connection::RequestStatus::kCancelled);
    RUVIA_CHECK(connection.requestInfo(0).runStatus == Connection::Dispatch::RunStatus::kCancelled);
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    RUVIA_CHECK(!connection.transportCloseRequired());
    const auto reset = connection.peekTransportIntent();
    RUVIA_CHECK(reset.has_value());
    RUVIA_CHECK(reset->token.kind == Connection::TransportIntentKind::kStreamReset);
    RUVIA_CHECK_EQ(reset->token.id.stream_id, std::uint64_t{0});
    RUVIA_CHECK(reset->streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kRequestCancelled);
    const auto lateFin = connection.acceptControl({Control::kind::stream_fin, id,
        static_cast<std::uint64_t>(requestWire(fixture.worker, "GET", "/slow").size())});
    RUVIA_CHECK(lateFin.input.status == Connection::Input::Status::kClosedStream);
    RUVIA_CHECK(fixture.routes.handlers.slowObservedStop);
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{1});
    RUVIA_CHECK(!connection.transportCloseRequired());

    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
}

ruvia::Task<void> exerciseDeadlineActivationAndHandlerCancellation(
    Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    co_await exercisePublicationDeadlineForLane(
        fixture, worker, false, kGeneration + 81, ruvia_ctx);
    co_await exercisePublicationDeadlineForLane(
        fixture, worker, true, kGeneration + 82, ruvia_ctx);
    co_await exerciseHandlerDeadlineCancellation(fixture, worker, ruvia_ctx);
}

ruvia::Task<void> exerciseRejectionBackpressure(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(fixture.services.worker());
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr std::uint64_t generation = kGeneration + 92;
    const MessageId id{kEpoch, generation, 0};
    const MessageId filler{kEpoch, generation, 12};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 4});
    constexpr std::array expectField{ruvia::Http3FieldSectionFieldView{"expect", "custom-expectation"}};
    const auto wire = requestWire(fixture.worker, "GET", "/first", {}, expectField);
    constexpr std::array<std::byte, 1> fillerBytes{std::byte{'x'}};
    RUVIA_CHECK(accepted(outbound.try_send(filler, fillerBytes)));
    const auto rejected = acceptWireBytes(connection, inbound, id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(rejected.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});

    const auto dataBlocked = connection.publishOne({.data = true});
    RUVIA_CHECK(dataBlocked.publication.status ==
                Connection::Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(dataBlocked.publication.blockReason ==
                Connection::Dispatch::PublishBlockReason::kData);
    RUVIA_CHECK(connection.workState().blocked.data);
    std::array<PublishedWire, 6> wires{};
    drainDataOnly(outbound, wires);
    RUVIA_CHECK_EQ(connection.reactivateBlocked({.data = true}), std::size_t{1});
    const auto headers = connection.publishOne({.data = true});
    RUVIA_CHECK(headers.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished);
    RUVIA_CHECK(accepted(outbound.try_send_control({Control::kind::writable, filler, 0})));
    const auto controlBlocked = connection.publishOne({.control = true});
    RUVIA_CHECK(controlBlocked.publication.status ==
                Connection::Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(controlBlocked.publication.blockReason ==
                Connection::Dispatch::PublishBlockReason::kControl);
    RUVIA_CHECK(connection.workState().blocked.control);
    drainAll(outbound, wires);
    RUVIA_CHECK_EQ(connection.reactivateBlocked({.control = true}), std::size_t{1});
    const auto fin = connection.publishOne({.control = true});
    RUVIA_CHECK(fin.publication.status ==
                Connection::Dispatch::PublishStatus::kFinPublished);
    drainAll(outbound, wires);
    const auto response = decodeResponse(wires[0], ruvia::HttpKnownMethod::kGet,
        id.stream_id, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, std::uint16_t{417});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});
    const auto inputFin = connection.acceptControl({Control::kind::stream_fin, id,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(inputFin.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    (void)ruvia_ctx;
}

ruvia::Task<void> exerciseDynamicQpackPublication(Fixture& fixture, const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr auto generation = kGeneration + 103;
    const MessageId requestId{kEpoch, generation, 0};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker, fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .session = {.connection = {.qpackMaxTableCapacity = 256, .qpackBlockedStreams = 2, .enableConnectProtocol = true}}, .maxTrackedStreams = 8});
    const auto prefixes = ruvia::Http3LocalCriticalStreams::create({.qpackMaxTableCapacity = 256, .qpackBlockedStreams = 2});
    RUVIA_CHECK((prefixes.index() == 0));
    const auto settings = std::get<0>(prefixes).controlPrefix();
    RUVIA_CHECK(acceptWireBytes(connection, inbound, {kEpoch, generation, 2}, settings).status == Connection::EventStatus::kAccepted);
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 256, .maxBlockedStreams = 2}, fixture.worker.resource());
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{":method", "POST"}, ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"}, ruvia::Http3FieldSectionFieldView{":path", "/body"},
        ruvia::Http3FieldSectionFieldView{"content-length", "7"}, ruvia::Http3FieldSectionFieldView{"x-custom", "value"}};
    const auto section = encoder.encode(0, fields);
    RUVIA_CHECK((section.index() == 0));
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders), std::string_view(std::get<0>(section).data(), std::get<0>(section).size())) +
                      frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), "payload");
    const auto head = acceptWireBytes(connection, inbound, requestId, std::span(wire.data(), wire.size()));
    RUVIA_CHECK(head.input.status == Connection::Input::Status::kDeferredQpack);
    const auto fin = connection.acceptControl({Control::kind::stream_fin, requestId, wire.size()});
    RUVIA_CHECK(fin.input.status == Connection::Input::Status::kDeferredQpack);
    RUVIA_CHECK(!connection.canAcceptInput(0, 1));
    RUVIA_CHECK(!connection.resumeQpackInput());
    auto instructions = std::string(1, char(2));
    const auto pending = encoder.pendingEncoderOutput();
    instructions.append(pending.data(), pending.size());
    RUVIA_CHECK(acceptWireBytes(connection, inbound, {kEpoch, generation, 6}, std::span(instructions.data(), instructions.size())).status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK(connection.resumeQpackInput());
    co_await waitForReady(connection, 1, worker, fixture.workerStop);
    RUVIA_CHECK_EQ(fixture.routes.handlers.bodySeen, std::string("payload"));
    std::string encoderWire, decoderWire, responseWire;
    bool responseFin = false;
    bool sawBlocked = false;
    for (std::size_t i = 0; i < 1000 && (!responseFin || connection.workState().runnableCount || connection.workState().blockedCount); ++i) {
        const auto attempt = connection.publishOne(kAllWorkLanes);
        if (attempt.status == Connection::PublishStatus::kAttempted && attempt.publication.status == Connection::Dispatch::PublishStatus::kBackpressured) {
            sawBlocked = true;
        }
        // Occasionally keep the sole block borrowed across a publication attempt.
        buffer::borrowed_block block;
        if (outbound.try_receive(block)) {
            if (const auto* critical = block.critical()) {
                auto& destination = critical->kind == ruvia::http3_critical_stream_output::stream_kind::qpack_encoder ? encoderWire : decoderWire;
                destination.append(reinterpret_cast<const char*>(block.bytes().data()), block.bytes().size());
            } else {
                responseWire.append(reinterpret_cast<const char*>(block.bytes().data()), block.bytes().size());
            }
            if (i == 0) {
                const auto blocked = connection.publishOne(kAllWorkLanes);
                sawBlocked |= blocked.publication.status == Connection::Dispatch::PublishStatus::kBackpressured;
            }
            block.release();
        }
        Control control;
        while (outbound.try_receive_control(control)) {
            if (control.kind == Control::kind::stream_fin) {
                responseFin = true;
                RUVIA_CHECK_EQ(control.value, responseWire.size());
            }
        }
        (void)connection.reactivateBlocked({.data = true, .control = true});
    }
    RUVIA_CHECK(sawBlocked);
    RUVIA_CHECK(responseFin);
    RUVIA_CHECK(!encoderWire.empty());
    RUVIA_CHECK(!decoderWire.empty());
    ruvia::Http3Connection peer(ruvia::Http3PeerRole::kClient, fixture.worker.resource(), {.qpackMaxTableCapacity = 256, .qpackBlockedStreams = 2});
    RUVIA_CHECK(peer.registerClientRequest(0, ruvia::HttpKnownMethod::kPost).scope == ruvia::Http3ConnectionErrorScope::kNone);
    struct Received {
        std::string body;
        std::uint16_t status{};
    } received;
    const auto receive = +[](void* context, const ruvia::Http3ConnectionEvent& event) {
        auto& result = *static_cast<Received*>(context);
        if (event.kind == ruvia::Http3ConnectionEventKind::kFinalHead) {
            result.status = event.head->status;
        }
        if (event.kind == ruvia::Http3ConnectionEventKind::kBody) {
            result.body.append(event.body.data(), event.body.size());
        }
    };
    auto result = peer.feed(0, std::span(responseWire.data(), responseWire.size()), true, false, receive, &received);
    RUVIA_CHECK(result.status == ruvia::Http3ConnectionStatus::kQpackBlocked);
    const auto consumed = result.consumedBytes;
    encoderWire.insert(0, 1, char(2));
    RUVIA_CHECK(peer.feed(7, std::span(encoderWire.data(), encoderWire.size()), false, false, receive, &received).scope == ruvia::Http3ConnectionErrorScope::kNone);
    result = peer.feed(0, std::span(responseWire.data() + consumed, responseWire.size() - consumed), true, false, receive, &received);
    RUVIA_CHECK(result.scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK_EQ(received.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(received.body, std::string("payload"));
    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK(inbound.stop());
    RUVIA_CHECK(outbound.stop());
}

ruvia::Task<void> exercise_peer_input_with_held_request_credit(
    Fixture& fixture, const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal activation(worker);
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(8, 8, 8, fixture.worker.resource());
    constexpr auto generation = kGeneration + 104;
    const MessageId request_id{kEpoch, generation, 0};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, activation,
        {.epoch = kEpoch, .connectionGeneration = generation, .session = {.connection = {.qpackMaxTableCapacity = 256, .qpackBlockedStreams = 2, .enableConnectProtocol = true}}, .maxTrackedStreams = 8});
    const auto prefixes = ruvia::Http3LocalCriticalStreams::create(
        {.qpackMaxTableCapacity = 256, .qpackBlockedStreams = 2});
    RUVIA_CHECK((prefixes.index() == 0));
    RUVIA_CHECK(connection.accept_peer_stream_data({kEpoch, generation, 2},
                              std::as_bytes(std::get<0>(prefixes).controlPrefix()))
                    .status == Connection::EventStatus::kAccepted);
    ruvia::Http3QpackEncoder encoder(
        {.maxTableCapacity = 256, .maxBlockedStreams = 2}, fixture.worker.resource());
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{":method", "POST"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/body"},
        ruvia::Http3FieldSectionFieldView{"content-length", "7"},
        ruvia::Http3FieldSectionFieldView{"x-custom", "value"}};
    const auto section = encoder.encode(0, fields);
    RUVIA_CHECK((section.index() == 0));
    const auto head_wire = frame(1, {std::get<0>(section).data(), std::get<0>(section).size()});
    RUVIA_CHECK(acceptWireBytes(connection, inbound, request_id,
                    std::span(head_wire.data(), head_wire.size()))
                    .input.status == Connection::Input::Status::kDeferredQpack);
    const auto body_wire = frame(0, "payload");
    RUVIA_CHECK(inbound.try_send(request_id,
                    std::as_bytes(std::span(body_wire.data(), body_wire.size()))) == buffer::send_result::sent);
    buffer::borrowed_block held_body;
    RUVIA_CHECK(inbound.try_receive(held_body));
    RUVIA_CHECK(!connection.canAcceptInput(0, held_body.bytes().size()));
    RUVIA_CHECK(connection.acceptControl({Control::kind::stream_fin, request_id,
                                             head_wire.size() + body_wire.size()})
                    .input.status == Connection::Input::Status::kDeferredFin);
    auto instructions = std::string(1, char(2));
    const auto pending = encoder.pendingEncoderOutput();
    instructions.append(pending.data(), pending.size());
    RUVIA_CHECK(instructions.size() > 2);
    const auto held_bytes = std::string(reinterpret_cast<const char*>(held_body.bytes().data()), held_body.bytes().size());
    for (const char byte : instructions) {
        buffer::data_reservation unavailable;
        RUVIA_CHECK(inbound.reserve_data({kEpoch, generation, 6}, unavailable) == buffer::reservation_result::no_block);
        RUVIA_CHECK(connection.accept_peer_stream_data({kEpoch, generation, 6},
                                  std::as_bytes(std::span(&byte, 1)))
                        .status == Connection::EventStatus::kAccepted);
        RUVIA_CHECK_EQ(std::string(reinterpret_cast<const char*>(held_body.bytes().data()), held_body.bytes().size()), held_bytes);
    }
    RUVIA_CHECK(connection.resumeQpackInput());
    RUVIA_CHECK(connection.canAcceptInput(0, held_body.bytes().size()));
    const auto body = connection.acceptData(held_body);
    RUVIA_CHECK(body.input.status == Connection::Input::Status::kFinished);
    held_body.release();
    co_await waitForReady(connection, 1, worker, fixture.workerStop);
    RUVIA_CHECK_EQ(fixture.routes.handlers.bodySeen, std::string("payload"));
    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK(inbound.stop());
    RUVIA_CHECK(outbound.stop());
}

ruvia::Task<void> exerciseOriginPublication(Fixture& fixture, const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer inbound(1, 1, 1, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    constexpr auto generation = kGeneration + 104;
    fixture.services = fixture.services.withTlsTransport("127.0.0.1", {});
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker, fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});
    RUVIA_CHECK(routeRequest(connection, inbound, fixture.worker, {kEpoch, generation, 0}, "GET", "/advertise").status == Connection::EventStatus::kDispatched);
    co_await waitForReady(connection, 1, worker, fixture.workerStop);
    std::string controlWire, responseWire;
    bool responseFin = false;
    bool sawBlocked = false;
    for (unsigned round = 0; round != 1000 && (!responseFin || connection.workState().runnableCount || connection.workState().blockedCount); ++round) {
        const auto attempt = connection.publishOne(kAllWorkLanes);
        sawBlocked |= attempt.publication.status == Connection::Dispatch::PublishStatus::kBackpressured;
        buffer::borrowed_block block;
        if (outbound.try_receive(block)) {
            if (const auto* critical = block.critical()) {
                RUVIA_CHECK(critical->epoch == kEpoch && critical->connection_generation == generation);
                RUVIA_CHECK(critical->kind == ruvia::http3_critical_stream_output::stream_kind::control);
                controlWire.append(reinterpret_cast<const char*>(block.bytes().data()), block.bytes().size());
            } else {
                responseWire.append(reinterpret_cast<const char*>(block.bytes().data()), block.bytes().size());
            }
            const auto blocked = connection.publishOne(kAllWorkLanes);
            sawBlocked |= blocked.publication.status == Connection::Dispatch::PublishStatus::kBackpressured;
            block.release();
        }
        Control control;
        while (outbound.try_receive_control(control)) {
            if (control.kind == Control::kind::stream_fin) {
                responseFin = true;
                RUVIA_CHECK_EQ(control.value, responseWire.size());
            }
        }
        (void)connection.reactivateBlocked({.data = true, .control = true});
    }
    RUVIA_CHECK(sawBlocked && responseFin && controlWire.size() > buffer::max_block_bytes);
    ruvia::Http3Connection peer(ruvia::Http3PeerRole::kClient, fixture.worker.resource(), {.receiveOriginAdvertisements = true});
    const auto prefixes = ruvia::Http3LocalCriticalStreams::create({});
    const auto prefix = std::get<0>(prefixes).controlPrefix();
    controlWire.insert(0, prefix.data(), prefix.size());
    std::size_t receivedOrigins{};
    const auto receive = [](void* raw, const ruvia::Http3ConnectionEvent& event) {
        if (event.originAdvertisement) {
            *static_cast<std::size_t*>(raw) += event.originAdvertisement->origins.size();
        }
    };
    RUVIA_CHECK(peer.feed(3, std::span<const char>(controlWire), false, false, receive, &receivedOrigins).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK_EQ(receivedOrigins, std::size_t{40});
    RUVIA_CHECK(peer.registerClientRequest(0, ruvia::HttpKnownMethod::kGet).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(peer.feed(0, std::span<const char>(responseWire), true, false, receive, &receivedOrigins).status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK(inbound.stop());
    RUVIA_CHECK(outbound.stop());
}

}  // namespace

RUVIA_TEST(http3_server_connection_parks_rejection_on_exact_buffer_lane) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseRejectionBackpressure(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionKeepsDataAndControlBackpressureInSeparateLanes) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseLaneQueueIsolation(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionActivatesBlockedPublicationOnDeadline) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseDeadlineActivationAndHandlerCancellation(
                                      fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3_server_advertises_origins_on_control_stream_under_buffer_backpressure) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseOriginPublication(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionResumesDynamicQpackAndPublishesBothCriticalStreamsWithBackpressure) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseDynamicQpackPublication(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(http3_server_connection_peer_encoder_fragments_progress_with_all_request_credits_borrowed) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exercise_peer_input_with_held_request_credit(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
}

namespace {
ruvia::Task<void> exerciseContinueBeforeBody(Fixture& fixture, const ruvia::WorkerHandle& worker, unsigned mode, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    buffer inbound(2, 2, 2, fixture.worker.resource());
    buffer outbound(1, 1, 1, fixture.worker.resource());
    const auto generation = kGeneration + 120 + mode;
    const MessageId id{kEpoch, generation, 0};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker, fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .session = {.max_buffered_body_bytes = mode == 2 ? 3u : 16u}, .maxTrackedStreams = 8});
    const std::array fields{ruvia::Http3FieldSectionFieldView{"expect", "100-continue"}};
    const auto head = ruvia::encodeHttp3ClientRequestHead({.method = "POST", .scheme = "https", .authority = "example.test", .path = "/body", .fields = fields, .bodyLength = mode == 2 ? std::nullopt : std::optional<std::uint64_t>{mode == 3 ? 0u : 7u}}, {}, fixture.worker.resource());
    RUVIA_CHECK((head.index() == 0));
    const auto wire = frame(1, {std::get<0>(head).fieldSection.data(), std::get<0>(head).fieldSection.size()});
    const auto received = acceptWireBytes(connection, inbound, id, std::span(wire.data(), wire.size()));
    RUVIA_CHECK(received.status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    std::array<PublishedWire, 6> wires{};
    if (mode == 1) {
        RUVIA_CHECK(connection.workState().runnable.data);
        RUVIA_CHECK(connection.acceptControl({Control::kind::stream_reset, id, wire.size()}).status == Connection::EventStatus::kAccepted);
        RUVIA_CHECK_EQ(connection.publishOne(kAllWorkLanes).status, Connection::PublishStatus::kNoReadyRequest);
    } else {
        if (mode != 3) {
            const auto initial = connection.publishOne(kAllWorkLanes);
            RUVIA_CHECK(initial.publication.status == Connection::Dispatch::PublishStatus::kBytesPublished);
            drainAll(outbound, wires);
            RUVIA_CHECK(!wires[0].finalWireBytes);
            ruvia::Http3ClientResponse decoder(0, ruvia::HttpKnownMethod::kPost, fixture.worker.resource());
            DecodedResponse result;
            RUVIA_CHECK(decoder.feed(std::span(wires[0].bytes.data(), wires[0].bytes.size()), false, false, captureResponse, &result).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK_EQ(result.informationalHeads, std::size_t{1});
            RUVIA_CHECK_EQ(result.finalHeads, std::size_t{0});
        } else {
            RUVIA_CHECK_EQ(connection.publishOne(kAllWorkLanes).status, Connection::PublishStatus::kNoReadyRequest);
        }
        const auto body = mode == 3 ? std::string{} : frame(0, "payload");
        if (!body.empty()) {
            (void)acceptWireBytes(connection, inbound, id, std::span(body.data(), body.size()));
        }
        const auto finished = connection.acceptControl({Control::kind::stream_fin, id, wire.size() + body.size()});
        RUVIA_CHECK(finished.status == (mode == 2 ? Connection::EventStatus::kRejected : Connection::EventStatus::kDispatched));
        bool sawFin = false;
        for (std::size_t i = 0; i < 2000 && !sawFin; ++i) {
            const auto attempt = connection.publishOne(kAllWorkLanes);
            drainAll(outbound, wires);
            (void)connection.reactivateBlocked({.data = true, .control = true});
            sawFin = wires[0].finalWireBytes.has_value();
            if (attempt.status == Connection::PublishStatus::kNoReadyRequest) {
                co_await ruvia::sleepFor(worker, 1ms);
            }
        }
        RUVIA_CHECK(sawFin);
        const auto response = decodeResponse(wires[0], ruvia::HttpKnownMethod::kPost, 0, fixture.worker.resource());
        RUVIA_CHECK_EQ(response.status, std::uint16_t(mode == 2 ? 413 : 200));
        RUVIA_CHECK_EQ(response.informationalHeads, std::size_t(mode == 3 ? 0 : 1));
        RUVIA_CHECK_EQ(response.body, std::string(mode == 0 ? "payload" : ""));
    }
    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK(inbound.stop());
    RUVIA_CHECK(outbound.stop());
}
}  // namespace

RUVIA_TEST(http3ServerConnectionContinuesUploadBeforeBodyAndKeepsFinCountAcrossRejectionAndReset) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
        const auto worker = attachment.loop().handle();
        ruvia::test::CountingMemoryResource upstream;
        {
            Fixture fixture(worker, upstream);
            runWorkerTask(attachment, exerciseContinueBeforeBody(fixture, worker, mode, ruvia_ctx));
        }
        RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    }
}

namespace {
ruvia::Task<void> exercisePushPublication(Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::test::CountingMemoryResource& upstream, ruvia::testing::TestContext& ruvia_ctx) {
    ruvia::WorkerSignal slowStarted(worker);
    fixture.routes.handlers.slowStarted = &slowStarted;
    std::size_t warmedLiveAllocations{};
    std::string retainedPromisePath;
    std::string retainedBody;
    for (int iteration = 0; iteration != 139; ++iteration) {
        const int mode = iteration < 128 ? 0 : iteration - 128;
        fixture.routes.handlers.pushMode = mode;
        fixture.routes.handlers.pushCompleted = false;
        fixture.routes.handlers.pushAccepted = false;
        fixture.routes.handlers.slowStartedObserved = false;
        fixture.routes.handlers.slowObservedStop = false;
        fixture.routes.handlers.pushedCookie.clear();
        fixture.routes.handlers.pushedHeader.clear();
        TestActivationSignal activation(worker);
        buffer inbound(2, 2, 2, fixture.worker.resource());
        buffer outbound(1, 1, 1, fixture.worker.resource());
        Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.services, fixture.options, outbound, activation,
            {.epoch = kEpoch, .connectionGeneration = kGeneration, .maxTrackedStreams = 32});
        std::unordered_map<std::uint64_t, PublishedWire> wires;
        bool openObserved = false;
        bool cancelledActivePush = false;
        std::exception_ptr failure;
        try {
            if (mode != 1) {
                constexpr std::array<char, 6> settingsAndMax{0, 4, 0, 0xd, 1, 0};
                RUVIA_CHECK(acceptWireBytes(connection, inbound, {kEpoch, kGeneration, 2}, settingsAndMax).status == Connection::EventStatus::kAccepted);
            }
            RUVIA_CHECK(routeRequest(connection, inbound, fixture.worker, {kEpoch, kGeneration, 0}, "GET", "/push").status == Connection::EventStatus::kDispatched);
            for (std::size_t attempt = 0; attempt != 5000; ++attempt) {
                if (const auto intent = connection.peekTransportIntent()) {
                    if (intent->token.kind == Connection::TransportIntentKind::kOpenPushStream) {
                        RUVIA_CHECK(!openObserved);
                        openObserved = true;
                        RUVIA_CHECK_EQ(intent->token.id.stream_id, std::uint64_t{0});
                        RUVIA_CHECK_EQ(intent->token.id.push_id, std::optional<std::uint64_t>{0});
                        if (mode == 3) {
                            RUVIA_CHECK(connection.acceptControl({.kind = Control::kind::stream_reset,
                                                                     .id = {kEpoch, kGeneration, 0},
                                                                     .value = requestWire(fixture.worker, "GET", "/push").size()})
                                            .status == Connection::EventStatus::kStreamCancelled);
                        } else if (mode == 4) {
                            RUVIA_CHECK(connection.requestStop());
                            const auto close = connection.peekTransportIntent();
                            RUVIA_CHECK(close && close->token.kind == Connection::TransportIntentKind::kConnectionClose);
                            RUVIA_CHECK(connection.ackTransportIntent(close->token));
                        } else if (mode == 7) {
                            constexpr std::array<char, 3> cancel{3, 1, 0};
                            RUVIA_CHECK(acceptWireBytes(connection, inbound, {kEpoch, kGeneration, 2}, cancel).status == Connection::EventStatus::kAccepted);
                        }
                        auto forged = intent->token;
                        forged.id.push_id = 1;
                        RUVIA_CHECK(!connection.ackTransportIntent(forged, Connection::PushStreamOpenResult{.status = Connection::PushStreamOpenResult::Status::kOpened, .streamId = 31}));
                        RUVIA_CHECK(!connection.ackTransportIntent(intent->token));
                        RUVIA_CHECK(connection.ackTransportIntent(intent->token,
                            Connection::PushStreamOpenResult{.status = mode == 2 ? Connection::PushStreamOpenResult::Status::kUnavailable : Connection::PushStreamOpenResult::Status::kOpened,
                                .streamId = 31}));
                        RUVIA_CHECK(!connection.ackTransportIntent(intent->token, Connection::PushStreamOpenResult{}));
                    } else {
                        RUVIA_CHECK(connection.ackTransportIntent(intent->token));
                    }
                }
                if (!cancelledActivePush && mode == 9 && fixture.routes.handlers.slowStartedObserved) {
                    std::array<char, 32> priority{};
                    const auto encoded = ruvia::encodeHttp3PriorityUpdate(priority, {.elementId = 0, .push = true, .fields = {.urgency = 1, .incremental = true}});
                    RUVIA_CHECK((encoded.index() == 0));
                    RUVIA_CHECK(acceptWireBytes(connection, inbound, {kEpoch, kGeneration, 2}, std::span(priority).first(std::get<0>(encoded))).status == Connection::EventStatus::kAccepted);
                    constexpr std::array<char, 3> cancel{3, 1, 0};
                    RUVIA_CHECK(acceptWireBytes(connection, inbound, {kEpoch, kGeneration, 2}, cancel).status == Connection::EventStatus::kAccepted);
                    cancelledActivePush = true;
                } else if (!cancelledActivePush && mode == 10 && connection.requestInfo(31).status == Connection::RequestStatus::kRunning) {
                    RUVIA_CHECK(connection.acceptControl({.kind = Control::kind::stream_reset,
                                                             .id = {kEpoch, kGeneration, 31, 0}})
                                    .status == Connection::EventStatus::kStreamCancelled);
                    cancelledActivePush = true;
                }
                for (std::size_t turn = 0; turn != 8; ++turn) {
                    (void)connection.publishOne(kAllWorkLanes);
                    Control control;
                    while (outbound.try_receive_control(control)) {
                        if (control.kind == Control::kind::stream_fin) {
                            wires[control.id.stream_id].finalWireBytes = control.value;
                        }
                    }
                    buffer::borrowed_block block;
                    while (outbound.try_receive(block)) {
                        if (!block.critical()) {
                            RUVIA_CHECK_EQ(block.id().push_id, block.id().stream_id == 31 ? std::optional<std::uint64_t>{0} : std::nullopt);
                            auto bytes = block.bytes();
                            wires[block.id().stream_id].bytes.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                        }
                        block.release();
                    }
                    (void)connection.reactivateBlocked(kAllWorkLanes);
                }
                if (connection.activeTaskCount() == 0 && connection.pendingTransportIntentCount() == 0) {
                    break;
                }
                co_await ruvia::sleepFor(worker, 1ms, fixture.workerStop);
            }
            RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
            RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
            RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
            RUVIA_CHECK_EQ(openObserved, mode != 1 && mode != 5 && mode != 6);
            RUVIA_CHECK_EQ(fixture.routes.handlers.pushAccepted, mode == 0 || mode == 8 || mode == 9 || mode == 10);
            if (mode == 9 || mode == 10) {
                RUVIA_CHECK(cancelledActivePush);
                RUVIA_CHECK(!connection.transportCloseRequired());
                if (mode == 9) {
                    RUVIA_CHECK(fixture.routes.handlers.slowObservedStop);
                    RUVIA_CHECK_EQ(fixture.routes.handlers.pushedPriority.urgency, std::uint8_t{1});
                    RUVIA_CHECK(fixture.routes.handlers.pushedPriority.incremental);
                }
            }
            if (mode != 3 && mode != 4) {
                RUVIA_CHECK(fixture.routes.handlers.pushCompleted || mode == 6);
                auto& parent = wires[0];
                RUVIA_CHECK(parent.finalWireBytes.has_value());
                RUVIA_CHECK_EQ(*parent.finalWireBytes, parent.bytes.size());
                ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, fixture.worker.resource(), {.maxPushId = 0});
                RUVIA_CHECK(client.registerClientRequest(0, ruvia::HttpKnownMethod::kGet).scope == ruvia::Http3ConnectionErrorScope::kNone);
                std::unordered_map<std::uint64_t, std::string> bodies;
                auto capture = +[](void* raw, const ruvia::Http3ConnectionEvent& event) {
                    if (event.kind == ruvia::Http3ConnectionEventKind::kBody) {
                        (*static_cast<std::unordered_map<std::uint64_t, std::string>*>(raw))[event.streamId].append(event.body.data(), event.body.size());
                    }
                };
                RUVIA_CHECK(client.feed(0, std::span(parent.bytes), true, false, capture, &bodies).scope == ruvia::Http3ConnectionErrorScope::kNone);
                if (mode == 0 || mode == 8) {
                    auto& pushed = wires[31];
                    RUVIA_CHECK(pushed.finalWireBytes.has_value());
                    RUVIA_CHECK_EQ(*pushed.finalWireBytes, pushed.bytes.size());
                    RUVIA_CHECK(client.feed(31, std::span(pushed.bytes), true, false, capture, &bodies).scope == ruvia::Http3ConnectionErrorScope::kNone);
                    RUVIA_CHECK_EQ(bodies[0], "parent");
                    RUVIA_CHECK_EQ(bodies[31], mode == 8 ? "" : "/first");
                    RUVIA_CHECK_EQ(fixture.routes.handlers.pushedCookie, "pushed");
                    RUVIA_CHECK_EQ(fixture.routes.handlers.pushedHeader, "owned-header");
                    RUVIA_CHECK(client.promisedRequest(0) != nullptr);
                    RUVIA_CHECK_EQ(client.promisedRequest(0)->path, "/first");
                    if (iteration == 0) {
                        retainedPromisePath = client.promisedRequest(0)->path;
                        retainedBody = bodies[31];
                    }
                    RUVIA_CHECK_EQ(retainedPromisePath, "/first");
                    RUVIA_CHECK_EQ(retainedBody, "/first");
                } else if (mode == 9 || mode == 10) {
                    RUVIA_CHECK(client.promisedRequest(0) != nullptr);
                    RUVIA_CHECK_EQ(bodies[0], "parent");
                } else {
                    RUVIA_CHECK(client.promisedRequest(0) == nullptr);
                    if (mode != 6) {
                        RUVIA_CHECK_EQ(bodies[0], "parent");
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        (void)connection.requestStop();
        while (const auto intent = connection.peekTransportIntent()) {
            if (intent->token.kind == Connection::TransportIntentKind::kOpenPushStream) {
                (void)connection.ackTransportIntent(intent->token, Connection::PushStreamOpenResult{.status = Connection::PushStreamOpenResult::Status::kStopped});
            } else {
                (void)connection.ackTransportIntent(intent->token);
            }
        }
        co_await connection.join();
        RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
        RUVIA_CHECK(inbound.stop());
        RUVIA_CHECK(outbound.stop());
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (iteration == 96) {
            warmedLiveAllocations = upstream.liveAllocations();
        } else if (iteration > 96 && iteration < 128) {
            RUVIA_CHECK_EQ(upstream.liveAllocations(), warmedLiveAllocations);
        }
    }
}
}  // namespace

RUVIA_TEST(http3ServerConnectionPushRoutesOwnedPromisesAndSettlesOpenCancellationAndStop) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exercisePushPublication(fixture, worker, upstream, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
