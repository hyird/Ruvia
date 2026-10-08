#include "http3_buffered_dispatch_fixture.h"

namespace {

ruvia::Task<void> dispatchWatchdog(const ruvia::WorkerHandle& workerHandle,
    Dispatch& dispatch, Input& input, std::uint64_t streamId, ruvia::WorkerSignal& started,
    ruvia::WorkerSignal& finished, bool& joined, ruvia::StopToken stopToken, bool& expired) {
    const auto sleep = co_await ruvia::sleepFor(workerHandle, 2s, stopToken);
    if (sleep != ruvia::TimerSleepResult::kElapsed || joined) {
        co_return;
    }
    expired = true;
    dispatch.cancel();
    (void)input.cancelRequest(streamId);
    started.notify();
    finished.notify();
}

ruvia::Task<void> exerciseCancellationAndJoin(
    Fixture& fixture, const ruvia::WorkerHandle& workerHandle,
    ruvia::testing::TestContext& ruvia_ctx) {
    fixture.options.deadline = ruvia::DeadlineConfig{.handler = 1min};
    fixture.routes.handlers.allocateErrorHeaders = true;
    fixture.routes.handlers.responseBody.assign(2048, 'e');
    ruvia::WorkerSignal started(workerHandle);
    fixture.routes.handlers.started = &started;
    feedRequest(fixture, 0, "GET", "/suspend");
    auto dispatch = fixture.makeDispatch(0, fixture.services);
    ruvia::WorkerSignal finished(workerHandle);
    ruvia::StopSource watchdogStop;
    ruvia::TaskScope tasks(workerHandle, {.resource = fixture.worker.resource()});
    Dispatch::RunStatus status{Dispatch::RunStatus::kFailed};
    bool joined = false;
    bool watchdogExpired = false;
    tasks.spawn(runOwner(dispatch, status, joined, finished));
    tasks.spawn(dispatchWatchdog(workerHandle, dispatch, fixture.input, 0, started, finished,
        joined, watchdogStop.token(), watchdogExpired));
    co_await started.wait();
    if (!watchdogExpired) {
        RUVIA_CHECK(dispatch.handlerActive());
        dispatch.cancel();
        RUVIA_CHECK(fixture.input.cancelRequest(0).status == Input::Status::kLocalCancelled);
    }
    co_await finished.wait();
    watchdogStop.requestStop();
    co_await tasks.join();

    RUVIA_CHECK(!watchdogExpired);
    RUVIA_CHECK(joined);
    RUVIA_CHECK(fixture.routes.handlers.handlerResumed);
    RUVIA_CHECK(fixture.routes.handlers.errorHandlerCalled);
    RUVIA_CHECK(status == Dispatch::RunStatus::kCancelled);
    RUVIA_CHECK(dispatch.cancellationReason() == Dispatch::CancellationReason::kExplicit);
    RUVIA_CHECK(!dispatch.handlerActive());
    RUVIA_CHECK(!dispatch.responseReady());
    RUVIA_CHECK(fixture.session.request(0) == nullptr);
    buffer::borrowed_block block;
    RUVIA_CHECK(!fixture.outbound.try_receive(block));
    Control control;
    RUVIA_CHECK(!fixture.outbound.try_receive_control(control));

    feedRequest(fixture, 4, "GET", "/large");
    auto partial = fixture.makeDispatch(4, fixture.services);
    RUVIA_CHECK(co_await partial.runHandler() == Dispatch::RunStatus::kResponseReady);
    const auto published = partial.publishStep();
    RUVIA_CHECK(published.status == Dispatch::PublishStatus::kBytesPublished);
    RUVIA_CHECK(partial.publicationDemand() == PublicationDemand::kData);
    const auto dataBlocked = partial.publishStep();
    RUVIA_CHECK(dataBlocked.status == Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(dataBlocked.blockReason == BlockReason::kData);
    partial.cancel();
    RUVIA_CHECK(partial.publicationDemand() == PublicationDemand::kLocalCancelled);
    RUVIA_CHECK(fixture.input.cancelRequest(4).status == Input::Status::kLocalCancelled);
    const auto cancelled = partial.publishStep();
    RUVIA_CHECK(cancelled.status == Dispatch::PublishStatus::kCancelled);
    RUVIA_CHECK(cancelled.blockReason == BlockReason::kNone);
    PublishedWire partialWire;
    drain_buffer(fixture.outbound, {kEpoch, kGeneration, 4}, partialWire);
    RUVIA_CHECK(!partialWire.bytes.empty());
    RUVIA_CHECK(!partialWire.finalWireBytes.has_value());
    RUVIA_CHECK(fixture.session.request(4) == nullptr);
}

ruvia::Task<void> exerciseSuspendingHandlerStop(Fixture& fixture,
    const ruvia::WorkerHandle& workerHandle, bool stopWorker,
    ruvia::testing::TestContext& ruvia_ctx) {
    fixture.options.deadline = ruvia::DeadlineConfig{.handler = stopWorker ? 1min : 50ms};
    ruvia::WorkerSignal started(workerHandle);
    ruvia::WorkerSignal finished(workerHandle);
    ruvia::StopSource watchdogStop;
    fixture.routes.handlers.started = &started;
    feedRequest(fixture, 0, "GET", "/suspend");
    auto dispatch = fixture.makeDispatch(0, fixture.services);
    ruvia::TaskScope tasks(workerHandle, {.resource = fixture.worker.resource()});
    Dispatch::RunStatus status{Dispatch::RunStatus::kFailed};
    bool joined = false;
    bool watchdogExpired = false;
    tasks.spawn(runOwner(dispatch, status, joined, finished));
    tasks.spawn(dispatchWatchdog(workerHandle, dispatch, fixture.input, 0, started, finished,
        joined, watchdogStop.token(), watchdogExpired));

    co_await started.wait();
    if (!watchdogExpired && stopWorker) {
        fixture.workerStopSource.requestStop();
    }
    co_await finished.wait();
    watchdogStop.requestStop();
    co_await tasks.join();

    RUVIA_CHECK(!watchdogExpired);
    RUVIA_CHECK(joined);
    RUVIA_CHECK(fixture.routes.handlers.handlerResumed);
    RUVIA_CHECK(fixture.routes.handlers.errorHandlerCalled);
    RUVIA_CHECK(!dispatch.handlerActive());

    RUVIA_CHECK(status == Dispatch::RunStatus::kCancelled);
    RUVIA_CHECK(!dispatch.responseReady());
    RUVIA_CHECK(dispatch.cancellationReason() ==
                (stopWorker ? Dispatch::CancellationReason::kWorkerStop
                            : Dispatch::CancellationReason::kDeadline));
    buffer::borrowed_block block;
    Control control;
    RUVIA_CHECK(!fixture.outbound.try_receive(block));
    RUVIA_CHECK(!fixture.outbound.try_receive_control(control));
}

ruvia::Task<void> warmDispatch(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    feedRequest(fixture, 0, "GET", "/large");
    auto dispatch = fixture.makeDispatch(0, fixture.services);
    RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);
    PublishedWire wire;
    publishAndDrain(dispatch, fixture, 0, wire, ruvia_ctx);
    RUVIA_CHECK(dispatch.complete());
    RUVIA_CHECK(fixture.session.request(0) == nullptr);
}

ruvia::Task<void> exercise_buffer_close_during_data(
    Fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    feedRequest(fixture, 4, "GET", "/large");
    auto dispatch = fixture.makeDispatch(4, fixture.services);
    RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);

    PublishedWire wire;
    const MessageId id{kEpoch, kGeneration, 4};
    const auto headers = dispatch.publishStep();
    RUVIA_CHECK(headers.status == Dispatch::PublishStatus::kBytesPublished);
    drain_buffer(fixture.outbound, id, wire);
    const auto dataHeader = dispatch.publishStep();
    RUVIA_CHECK(dataHeader.status == Dispatch::PublishStatus::kBytesPublished);
    drain_buffer(fixture.outbound, id, wire);

    const std::span<const char> published(wire.bytes.data(), wire.bytes.size());
    const auto headersFrame = ruvia::decodeHttp3Frame(published);
    if (!headersFrame) {
        RUVIA_CHECK(false);
        co_return;
    }
    RUVIA_CHECK_EQ(headersFrame->type, static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders));
    const auto dataFrameHeader = ruvia::decodeHttp3FrameHeader(
        published.subspan(headersFrame->encodedBytes));
    if (!dataFrameHeader) {
        RUVIA_CHECK(false);
        co_return;
    }
    RUVIA_CHECK_EQ(dataFrameHeader->type, static_cast<std::uint64_t>(ruvia::Http3FrameType::kData));
    RUVIA_CHECK(dataFrameHeader->length > 0);
    RUVIA_CHECK_EQ(headersFrame->encodedBytes + dataFrameHeader->encodedBytes, published.size());
    RUVIA_CHECK(!wire.finalWireBytes.has_value());

    const auto beforeFailure = dispatch.publishedWireBytes();
    RUVIA_CHECK(fixture.outbound.stop());
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::local_buffer_stopped);
    const auto failure = dispatch.publishStep();
    RUVIA_CHECK(failure.status == Dispatch::PublishStatus::kFailed);
    RUVIA_CHECK(failure.blockReason == BlockReason::kNone);
    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), beforeFailure);
    RUVIA_CHECK(!dispatch.complete());
    drain_buffer(fixture.outbound, id, wire);
    RUVIA_CHECK(!wire.finalWireBytes.has_value());
    Control control;
    RUVIA_CHECK(!fixture.outbound.try_receive_control(control));
    RUVIA_CHECK(fixture.session.request(4) == nullptr);
}

ruvia::Task<void> exercise_buffer_close_during_fin(
    Fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    feedRequest(fixture, 4, "GET", "/large");
    auto dispatch = fixture.makeDispatch(4, fixture.services);
    RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);

    PublishedWire wire;
    const MessageId id{kEpoch, kGeneration, 4};
    for (unsigned segment = 0; segment < 3; ++segment) {
        const auto published = dispatch.publishStep();
        RUVIA_CHECK(published.status == Dispatch::PublishStatus::kBytesPublished);
        if (published.status != Dispatch::PublishStatus::kBytesPublished) {
            co_return;
        }
        drain_buffer(fixture.outbound, id, wire);
    }

    std::size_t offset = 0;
    std::size_t headersFrames = 0;
    std::size_t dataFrames = 0;
    std::string body;
    for (unsigned frameIndex = 0; offset < wire.bytes.size() && frameIndex < 4; ++frameIndex) {
        const auto decoded = ruvia::decodeHttp3Frame(
            std::span<const char>(wire.bytes.data() + offset, wire.bytes.size() - offset));
        if (!decoded) {
            RUVIA_CHECK(false);
            co_return;
        }
        if (decoded->type == static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders)) {
            ++headersFrames;
        } else if (decoded->type == static_cast<std::uint64_t>(ruvia::Http3FrameType::kData)) {
            ++dataFrames;
            body.append(decoded->payload.data(), decoded->payload.size());
        }
        offset += decoded->encodedBytes;
    }
    RUVIA_CHECK_EQ(offset, wire.bytes.size());
    RUVIA_CHECK_EQ(headersFrames, std::size_t{1});
    RUVIA_CHECK_EQ(dataFrames, std::size_t{1});
    RUVIA_CHECK(body == fixture.routes.handlers.responseBody);
    RUVIA_CHECK(!dispatch.complete());
    RUVIA_CHECK(!wire.finalWireBytes.has_value());

    const auto controlFiller = fixture.outbound.try_send_control(
        {Control::kind::writable, id, 0});
    RUVIA_CHECK(controlAccepted(controlFiller));
    const auto blockedFin = dispatch.publishStep();
    RUVIA_CHECK(blockedFin.status == Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blockedFin.blockReason == BlockReason::kControl);

    const auto beforeFailure = dispatch.publishedWireBytes();
    RUVIA_CHECK(fixture.outbound.stop());
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::local_buffer_stopped);
    const auto failure = dispatch.publishStep();
    RUVIA_CHECK(failure.status == Dispatch::PublishStatus::kFailed);
    RUVIA_CHECK(failure.blockReason == BlockReason::kNone);
    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), beforeFailure);
    RUVIA_CHECK(!dispatch.complete());
    drain_buffer(fixture.outbound, id, wire);
    RUVIA_CHECK(!wire.finalWireBytes.has_value());
    Control control;
    RUVIA_CHECK(!fixture.outbound.try_receive_control(control));
    RUVIA_CHECK(fixture.session.request(4) == nullptr);
}

}  // namespace

RUVIA_TEST(http3BufferedDispatchCancellationStopsAndJoinsHandlerBeforeDiscardingErrorResponse) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    Fixture fixture(workerHandle, upstream);
    runWorkerTask(attachment, exerciseCancellationAndJoin(fixture, workerHandle, ruvia_ctx));
}

ruvia::Task<void> exerciseWorkerStopAndDeadline(
    const ruvia::WorkerHandle& workerHandle, ruvia::testing::TestContext& ruvia_ctx) {
    ruvia::test::CountingMemoryResource workerStopUpstream;
    {
        Fixture fixture(workerHandle, workerStopUpstream);
        co_await exerciseSuspendingHandlerStop(fixture, workerHandle, true, ruvia_ctx);
        RUVIA_CHECK(fixture.session.request(0) == nullptr);
    }
    RUVIA_CHECK_EQ(workerStopUpstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(workerStopUpstream.allocationCount(), workerStopUpstream.deallocationCount());

    ruvia::test::CountingMemoryResource deadlineUpstream;
    {
        Fixture fixture(workerHandle, deadlineUpstream);
        co_await exerciseSuspendingHandlerStop(fixture, workerHandle, false, ruvia_ctx);
        RUVIA_CHECK(fixture.session.request(0) == nullptr);
    }
    RUVIA_CHECK_EQ(deadlineUpstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(deadlineUpstream.allocationCount(), deadlineUpstream.deallocationCount());
}

ruvia::Task<void> exercisePublicationDeadlineRegistration(
    const ruvia::WorkerHandle& workerHandle, ruvia::testing::TestContext& ruvia_ctx) {
    ruvia::test::CountingMemoryResource upstream;
    Fixture fixture(workerHandle, upstream);

    fixture.options.deadline = ruvia::DeadlineConfig{.handler = 1min};
    feedRequest(fixture, 0, "GET", "/large");
    auto explicitlyCancelled = fixture.makeDispatch(0, fixture.services);
    RUVIA_CHECK(co_await explicitlyCancelled.runHandler() ==
                Dispatch::RunStatus::kResponseReady);
    std::size_t explicitCallbacks = 0;
    RUVIA_CHECK(explicitlyCancelled.registerPublicationDeadlineCallback(
        [&explicitCallbacks]() noexcept { ++explicitCallbacks; }));
    explicitlyCancelled.cancel();
    RUVIA_CHECK_EQ(explicitCallbacks, std::size_t{1});
    RUVIA_CHECK(explicitlyCancelled.cancellationReason() ==
                Dispatch::CancellationReason::kExplicit);
    RUVIA_CHECK(explicitlyCancelled.publishStep().status ==
                Dispatch::PublishStatus::kCancelled);
    const auto explicitFin = fixture.input.acceptControl({Control::kind::stream_fin,
        {kEpoch, kGeneration, 0}, requestWire(fixture.worker, "GET", "/large").size()});
    RUVIA_CHECK(explicitFin.status == Input::Status::kDuplicateFin);

    fixture.options.deadline = ruvia::DeadlineConfig{.handler = 20ms};
    feedRequest(fixture, 4, "GET", "/large");
    auto alreadyStopped = fixture.makeDispatch(4, fixture.services);
    RUVIA_CHECK(co_await alreadyStopped.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK(co_await ruvia::sleepFor(workerHandle, 60ms, fixture.workerStop) ==
                ruvia::TimerSleepResult::kElapsed);
    std::size_t stoppedCallbacks = 0;
    RUVIA_CHECK(alreadyStopped.registerPublicationDeadlineCallback(
        [&stoppedCallbacks]() noexcept { ++stoppedCallbacks; }));
    RUVIA_CHECK_EQ(stoppedCallbacks, std::size_t{1});
    RUVIA_CHECK(alreadyStopped.cancellationReason() ==
                Dispatch::CancellationReason::kDeadline);
    RUVIA_CHECK(alreadyStopped.publishStep().status == Dispatch::PublishStatus::kCancelled);
    const auto expiredFin = fixture.input.acceptControl({Control::kind::stream_fin,
        {kEpoch, kGeneration, 4}, requestWire(fixture.worker, "GET", "/large").size()});
    RUVIA_CHECK(expiredFin.status == Input::Status::kDuplicateFin);

    feedRequest(fixture, 8, "GET", "/large");
    auto completed = fixture.makeDispatch(8, fixture.services);
    RUVIA_CHECK(co_await completed.runHandler() == Dispatch::RunStatus::kResponseReady);
    std::size_t lateCallbacks = 0;
    RUVIA_CHECK(completed.registerPublicationDeadlineCallback(
        [&lateCallbacks]() noexcept { ++lateCallbacks; }));
    PublishedWire wire;
    publishAndDrain(completed, fixture, 8, wire, ruvia_ctx);
    RUVIA_CHECK(completed.complete());
    RUVIA_CHECK(co_await ruvia::sleepFor(workerHandle, 40ms, fixture.workerStop) ==
                ruvia::TimerSleepResult::kElapsed);
    RUVIA_CHECK_EQ(lateCallbacks, std::size_t{0});
    RUVIA_CHECK(completed.complete());
    RUVIA_CHECK(fixture.session.request(0) == nullptr);
    RUVIA_CHECK(fixture.session.request(4) == nullptr);
    RUVIA_CHECK(fixture.session.request(8) == nullptr);
}

ruvia::Task<void> exercise_buffer_close_stages(
    const ruvia::WorkerHandle& workerHandle, ruvia::testing::TestContext& ruvia_ctx) {
    ruvia::test::CountingMemoryResource dataUpstream;
    {
        Fixture fixture(workerHandle, dataUpstream);
        co_await warmDispatch(fixture, ruvia_ctx);
        const auto warmedPoolAllocations = dataUpstream.liveAllocations();
        co_await exercise_buffer_close_during_data(fixture, ruvia_ctx);
        // WorkerMemory may retain returned blocks in its pool until teardown.
        RUVIA_CHECK_EQ(dataUpstream.liveAllocations(), warmedPoolAllocations);
    }
    RUVIA_CHECK_EQ(dataUpstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(dataUpstream.allocationCount(), dataUpstream.deallocationCount());

    ruvia::test::CountingMemoryResource finUpstream;
    {
        Fixture fixture(workerHandle, finUpstream);
        co_await warmDispatch(fixture, ruvia_ctx);
        const auto warmedPoolAllocations = finUpstream.liveAllocations();
        co_await exercise_buffer_close_during_fin(fixture, ruvia_ctx);
        RUVIA_CHECK_EQ(finUpstream.liveAllocations(), warmedPoolAllocations);
    }
    RUVIA_CHECK_EQ(finUpstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(finUpstream.allocationCount(), finUpstream.deallocationCount());
}

RUVIA_TEST(http3BufferedDispatchCombinesWorkerStopAndHandlerDeadlineThroughRouter) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    runWorkerTask(attachment, exerciseWorkerStopAndDeadline(workerHandle, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchRegistersPublicationDeadlineInlineAndLatchesCancellation) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    runWorkerTask(attachment,
        exercisePublicationDeadlineRegistration(workerHandle, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchFailsWithoutFinWhenOutboundClosesDuringDataOrFin) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    runWorkerTask(attachment, exercise_buffer_close_stages(workerHandle, ruvia_ctx));
}
