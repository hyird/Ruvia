#include "http3_buffered_dispatch_fixture.h"

namespace {

ruvia::Task<void> exerciseRouteAndPublish(
    Fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{"x-input", "present"},
        ruvia::Http3FieldSectionFieldView{"cookie", "session=one"},
        ruvia::Http3FieldSectionFieldView{"cookie", "other=two"}};
    feedRequest(fixture, 0, "POST", "/items", "payload", fields);
    auto dispatch = fixture.makeDispatch(
        0, fixture.services.withTlsTransport("127.0.0.1"));
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kNotReady);
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{0});
    RUVIA_CHECK(co_await dispatch.prepare() == Dispatch::PrepareStatus::kPrepared);
    RUVIA_CHECK(co_await dispatch.prepare() == Dispatch::PrepareStatus::kAlreadyPrepared);
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kNotReady);
    const auto notReady = dispatch.publishStep();
    RUVIA_CHECK(notReady.status == Dispatch::PublishStatus::kNotReady);
    RUVIA_CHECK(notReady.blockReason == BlockReason::kNone);
    RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{1});
    RUVIA_CHECK(fixture.routes.handlers.requestReadCorrectly);
    bool unexpectedNoDeadlineCallback = false;
    RUVIA_CHECK(!dispatch.registerPublicationDeadlineCallback(
        [&unexpectedNoDeadlineCallback]() noexcept { unexpectedNoDeadlineCallback = true; }));
    RUVIA_CHECK(!unexpectedNoDeadlineCallback);
    RUVIA_CHECK(dispatch.responseReady());
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);

    const auto allocations = fixture.upstream.allocationCount();
    const auto returns = fixture.upstream.deallocationCount();
    const auto liveAllocations = fixture.upstream.liveAllocations();
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);
    }
    RUVIA_CHECK_EQ(fixture.upstream.allocationCount(), allocations);
    RUVIA_CHECK_EQ(fixture.upstream.deallocationCount(), returns);
    RUVIA_CHECK_EQ(fixture.upstream.liveAllocations(), liveAllocations);
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{1});
    PublicationDemand foreignDemand{PublicationDemand::kData};
    std::thread foreignWorker([&] { foreignDemand = dispatch.publicationDemand(); });
    foreignWorker.join();
    RUVIA_CHECK(foreignDemand == PublicationDemand::kWrongWorker);

    PublishedWire wire;
    publishAndDrain(dispatch, fixture, 0, wire, ruvia_ctx, true);
    RUVIA_CHECK(wire.identityMatched);
    RUVIA_CHECK_EQ(wire.finalWireBytes.value_or(0), wire.bytes.size());
    RUVIA_CHECK_EQ(wire.finalWireBytes.value_or(0), dispatch.publishedWireBytes());
    DecodedResponse response;
    const auto decoded = decodePublished(wire, ruvia::HttpKnownMethod::kPost, 0,
        fixture.worker.resource(), response);
    RUVIA_CHECK(decoded.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(response.finalHeads, std::size_t{1});
    RUVIA_CHECK_EQ(response.status, std::uint16_t{200});
    RUVIA_CHECK(response.dispatchHeader == "buffered");
    RUVIA_CHECK(response.body == "buffered-h3-ok");
    RUVIA_CHECK_EQ(response.messageEnds, std::size_t{1});
    RUVIA_CHECK(dispatch.complete());
    RUVIA_CHECK(fixture.session.request(0) == nullptr);
}

ruvia::Task<void> exerciseHeadAndFile(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    feedRequest(fixture, 0, "HEAD", "/file");
    auto head = fixture.makeDispatch(0, fixture.services);
    RUVIA_CHECK(co_await head.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK(head.publicationDemand() == PublicationDemand::kData);
    const auto headHeaders = head.publishStep();
    RUVIA_CHECK(headHeaders.status == Dispatch::PublishStatus::kBytesPublished);
    RUVIA_CHECK(head.publicationDemand() == PublicationDemand::kControl);
    PublishedWire headWire;
    publishAndDrain(head, fixture, 0, headWire, ruvia_ctx);
    DecodedResponse headResponse;
    const auto headDecoded = decodePublished(headWire, ruvia::HttpKnownMethod::kHead, 0,
        fixture.worker.resource(), headResponse);
    RUVIA_CHECK(headDecoded.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(headResponse.finalHeads, std::size_t{1});
    RUVIA_CHECK_EQ(headResponse.contentLength.value_or(0), std::uint64_t{5});
    RUVIA_CHECK_EQ(headResponse.bodyEvents, std::size_t{0});
    RUVIA_CHECK(head.complete());

    feedRequest(fixture, 4, "GET", "/file");
    auto file = fixture.makeDispatch(4, fixture.services);
    RUVIA_CHECK(co_await file.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK(file.failure() == nullptr);
    PublishedWire unavailableWire;
    publishAndDrain(file, fixture, 4, unavailableWire, ruvia_ctx);
    DecodedResponse unavailable;
    RUVIA_CHECK(decodePublished(unavailableWire, ruvia::HttpKnownMethod::kGet, 4, fixture.worker.resource(), unavailable).status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(unavailable.status, std::uint16_t{503});

    feedRequest(fixture, 8, "GET", "/empty-file");
    auto emptyFile = fixture.makeDispatch(8, fixture.services);
    RUVIA_CHECK(co_await emptyFile.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK(emptyFile.publicationDemand() == PublicationDemand::kData);
    const auto emptyFileHeaders = emptyFile.publishStep();
    RUVIA_CHECK(emptyFileHeaders.status == Dispatch::PublishStatus::kBytesPublished);
    RUVIA_CHECK(emptyFile.publicationDemand() == PublicationDemand::kControl);
    PublishedWire emptyFileWire;
    publishAndDrain(emptyFile, fixture, 8, emptyFileWire, ruvia_ctx);
    DecodedResponse emptyFileResponse;
    const auto emptyFileDecoded = decodePublished(emptyFileWire, ruvia::HttpKnownMethod::kGet, 8,
        fixture.worker.resource(), emptyFileResponse);
    RUVIA_CHECK(emptyFileDecoded.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(emptyFileResponse.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(emptyFileResponse.contentLength.value_or(1), std::uint64_t{0});
    RUVIA_CHECK_EQ(emptyFileResponse.bodyEvents, std::size_t{0});
    RUVIA_CHECK(emptyFileResponse.body.empty());
    const auto emptyFileFrame = ruvia::decodeHttp3Frame(
        std::span<const char>(emptyFileWire.bytes.data(), emptyFileWire.bytes.size()));
    RUVIA_CHECK(emptyFileFrame.has_value());
    if (emptyFileFrame) {
        RUVIA_CHECK_EQ(emptyFileFrame->type,
            static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders));
        RUVIA_CHECK_EQ(emptyFileFrame->encodedBytes, emptyFileWire.bytes.size());
    }
    RUVIA_CHECK(emptyFile.complete());
}

ruvia::Task<void> exerciseColdAndError(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    feedRequest(fixture, 0, "POST", "/items", "payload");
    {
        auto cold = fixture.makeDispatch(0, fixture.services);
        {
            auto task = cold.prepare();
        }
        {
            auto task = cold.runHandler();
        }
    }
    auto stillReady = fixture.session.acquireRequest(0);
    RUVIA_CHECK(stillReady.has_value());
    stillReady.reset();
    RUVIA_CHECK(fixture.session.release(0));

    feedRequest(fixture, 4, "GET", "/throw");
    auto failure = fixture.makeDispatch(4, fixture.services);
    RUVIA_CHECK(co_await failure.prepare() == Dispatch::PrepareStatus::kPrepared);
    RUVIA_CHECK(co_await failure.prepare() == Dispatch::PrepareStatus::kAlreadyPrepared);
    RUVIA_CHECK(co_await failure.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK(fixture.routes.handlers.errorHandlerCalled);
    PublishedWire wire;
    publishAndDrain(failure, fixture, 4, wire, ruvia_ctx);
    DecodedResponse response;
    const auto decoded = decodePublished(
        wire, ruvia::HttpKnownMethod::kGet, 4, fixture.worker.resource(), response);
    RUVIA_CHECK(decoded.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(response.status, std::uint16_t{500});
    RUVIA_CHECK(response.errorHeader == "used");
    RUVIA_CHECK(response.body == "handled-error");
}

ruvia::Task<void> exerciseEscapingFailure(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    fixture.routes.handlers.throwFromErrorHandler = true;
    feedRequest(fixture, 0, "GET", "/throw");
    {
        auto dispatch = fixture.makeDispatch(0, fixture.services);
        RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);
        PublishedWire wire;
        publishAndDrain(dispatch, fixture, 0, wire, ruvia_ctx);
        DecodedResponse response;
        RUVIA_CHECK(decodePublished(wire, ruvia::HttpKnownMethod::kGet, 0,
                        fixture.worker.resource(), response)
                        .status == ruvia::Http3ClientResponseStatus::kMessageEnd);
        RUVIA_CHECK_EQ(response.status, std::uint16_t{500});
        RUVIA_CHECK(fixture.session.request(0) == nullptr);
    }
#ifndef _WIN32
    // MSVC may satisfy this request from the worker pool without reaching
    // the upstream allocator.
    fixture.routes.handlers.throwFromErrorHandler = false;
    feedRequest(fixture, 4, "GET", "/large");
    {
        auto dispatch = fixture.makeDispatch(4, fixture.services);
        fixture.allocations.reject = true;
        const auto result = co_await dispatch.runHandler();
        fixture.allocations.reject = false;
        RUVIA_CHECK(result == Dispatch::RunStatus::kFailed);
        RUVIA_CHECK(dispatch.failure() != nullptr && !dispatch.handlerActive());
        RUVIA_CHECK(fixture.session.request(4) == nullptr);
    }
#endif
    buffer::borrowed_block block;
    Control control;
    RUVIA_CHECK(!fixture.outbound.try_receive(block) && !fixture.outbound.try_receive_control(control));
    RUVIA_CHECK(!fixture.session.terminated());
}

ruvia::Task<void> exerciseRepeatedRequestMemory(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    fixture.routes.handlers.responseBody.assign(32 * 1024, 'r');
    feedRequest(fixture, 100, "POST", "/items", "payload");
    auto retained = fixture.session.acquireRequest(100);
    RUVIA_CHECK(retained.has_value());
    if (!retained) {
        co_return;
    }
    const auto retainedBody = retained->request().request().bodyBytes();
    const auto retainedHeader = retained->request().request().header("host");
    std::size_t warmedLiveAllocations{};

    for (std::uint64_t index = 0; index < 8; ++index) {
        const auto streamId = index * 4;
        feedRequest(fixture, streamId, "GET", "/large");
        auto dispatch = fixture.makeDispatch(streamId, fixture.services);
        RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);
        PublishedWire wire;
        publishAndDrain(dispatch, fixture, streamId, wire, ruvia_ctx);
        RUVIA_CHECK(dispatch.complete());
        RUVIA_CHECK_EQ(wire.finalWireBytes.value_or(0), wire.bytes.size());
        if (index == 0) {
            warmedLiveAllocations = fixture.upstream.liveAllocations();
        } else {
            RUVIA_CHECK_EQ(fixture.upstream.liveAllocations(), warmedLiveAllocations);
        }
        const auto body = retained->request().request().bodyBytes();
        RUVIA_CHECK(body.size() == retainedBody.size());
        RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()) ==
                    "payload");
        RUVIA_CHECK(retained->request().request().header("host") == retainedHeader);
    }

    retained.reset();
    RUVIA_CHECK(fixture.session.release(100));
}

ruvia::Task<void> exerciseEarlyProvenanceAndReplayPolicy(
    Fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    const std::array earlyHeader{ruvia::Http3FieldSectionFieldView{"early-data", "1"}};
    feedRequest(fixture, 0, "GET", "/throw", {}, earlyHeader, true);
    auto rejected = fixture.makeDispatch(0, fixture.services, {}, true);
    RUVIA_CHECK(co_await rejected.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{0});
    RUVIA_CHECK_EQ(fixture.routes.handlers.replay_safe_middleware_calls, std::size_t{0});
    PublishedWire rejectedWire;
    publishAndDrain(rejected, fixture, 0, rejectedWire, ruvia_ctx);
    DecodedResponse rejectedResponse;
    const auto rejectedResult = decodePublished(rejectedWire,
        ruvia::HttpKnownMethod::kGet, 0, fixture.worker.resource(), rejectedResponse);
    RUVIA_CHECK(rejectedResult.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(rejectedResponse.status, std::uint16_t{425});
    RUVIA_CHECK(rejected.complete());

    feedRequest(fixture, 4, "GET", "/throw", {}, earlyHeader, false);
    auto spoofed = fixture.makeDispatch(4, fixture.services, {}, false);
    RUVIA_CHECK(co_await spoofed.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{1});
    RUVIA_CHECK(!fixture.routes.handlers.early_data_info.received_from_early_data());
    RUVIA_CHECK(fixture.routes.handlers.early_data_info.upstream_declared_early_data());
    PublishedWire spoofedWire;
    publishAndDrain(spoofed, fixture, 4, spoofedWire, ruvia_ctx);
    RUVIA_CHECK(spoofed.complete());

    feedRequest(fixture, 8, "GET", "/early-safe", {}, {}, true);
    auto safe = fixture.makeDispatch(8, fixture.services, {}, true);
    RUVIA_CHECK(co_await safe.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{2});
    RUVIA_CHECK_EQ(fixture.routes.handlers.replay_safe_middleware_calls, std::size_t{1});
    RUVIA_CHECK(fixture.routes.handlers.early_data_info.received_from_early_data());
    RUVIA_CHECK(!fixture.routes.handlers.early_data_info.upstream_declared_early_data());
    PublishedWire safeWire;
    publishAndDrain(safe, fixture, 8, safeWire, ruvia_ctx);
    DecodedResponse safeResponse;
    const auto safeResult = decodePublished(safeWire,
        ruvia::HttpKnownMethod::kGet, 8, fixture.worker.resource(), safeResponse);
    RUVIA_CHECK(safeResult.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(safeResponse.status, std::uint16_t{200});
    RUVIA_CHECK(safe.complete());
}

ruvia::Task<void> exerciseWebSocketHandshakeFailures(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    for (const auto& [streamId, version, expectedVersion, expectedCode, requestEnded] : {
             std::tuple<std::uint64_t, std::string_view, std::string_view,
                 std::string_view, bool>{
                 0, "12", "13", "websocket_version_unsupported", false},
             {4, "", "", "invalid_websocket_handshake", false},
             {8, "12", "13", "websocket_version_unsupported", true}}) {
        feedWebSocketRequest(fixture.session, fixture.worker, streamId, version);
        if (requestEnded) {
            const auto fin = fixture.session.feed(streamId, {}, true);
            RUVIA_CHECK(fin.scope == ruvia::Http3ConnectionErrorScope::kNone);
        }
        auto dispatch = fixture.makeDispatch(streamId, fixture.services);
        RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);
        RUVIA_CHECK(fixture.routes.handlers.errorHandlerCalled);
        RUVIA_CHECK_EQ(fixture.routes.handlers.errorCode, expectedCode);
        PublishedWire wire;
        publishAndDrain(dispatch, fixture, streamId, wire, ruvia_ctx);
        DecodedResponse response;
        const auto decoded = decodePublished(wire, ruvia::HttpKnownMethod::kConnect,
            streamId, fixture.worker.resource(), response);
        RUVIA_CHECK(decoded.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
        RUVIA_CHECK_EQ(response.status, std::uint16_t{400});
        RUVIA_CHECK_EQ(response.errorHeader, "used");
        RUVIA_CHECK_EQ(response.websocketVersionHeader, expectedVersion);
        RUVIA_CHECK(response.connectionHeader.empty());
        RUVIA_CHECK(response.upgradeHeader.empty());
        RUVIA_CHECK(response.websocketAcceptHeader.empty());
        RUVIA_CHECK_EQ(response.body, "handled-error");
        if (!requestEnded) {
            RUVIA_CHECK(fixture.session.request(streamId) != nullptr);
            const auto fin = fixture.session.feed(streamId, {}, true);
            RUVIA_CHECK(fin.scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(fixture.session.release(streamId));
        }
        RUVIA_CHECK(fixture.session.request(streamId) == nullptr);
        fixture.routes.handlers.errorHandlerCalled = false;
    }
}

ruvia::Task<void> exercise_buffered_recovery_coding(Fixture& fixture, bool web_socket,
    ruvia::testing::TestContext& ruvia_ctx) {
    auto& state = fixture.routes.handlers;
    state.responseBody.assign(2048, 'a');
    state.error_body.assign(2048, 'e');
    state.response_no_transform = true;
    state.first_error_no_transform = web_socket;
    for (unsigned mode = 0; mode != 4; ++mode) {
        state.error_handler_calls = 0;
        state.errorCode.clear();
        state.error_no_transform = mode == 1;
        fixture.options.compression.emplace();
        if (mode == 2) {
            fixture.options.compression.reset();
        }
        const auto accept_encoding = mode == 3 ? "identity;q=0, *;q=0" : "gzip, identity;q=0";
        const std::uint64_t stream_id = mode * 4;
        if (web_socket) {
            feedWebSocketRequest(fixture.session, fixture.worker, stream_id, "12", accept_encoding);
        } else {
            const std::array fields{ruvia::Http3FieldSectionFieldView{"accept-encoding", accept_encoding}};
            feedRequest(fixture, stream_id, "GET", "/large", {}, fields);
        }
        auto dispatch = fixture.makeDispatch(stream_id, fixture.services);
        RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);
        RUVIA_CHECK_EQ(state.error_handler_calls, web_socket ? std::size_t{2} : std::size_t{1});
        RUVIA_CHECK_EQ(state.errorCode, std::string("not_acceptable"));
        PublishedWire wire;
        publishAndDrain(dispatch, fixture, stream_id, wire, ruvia_ctx);
        DecodedResponse response;
        RUVIA_CHECK(decodePublished(wire, web_socket ? ruvia::HttpKnownMethod::kConnect : ruvia::HttpKnownMethod::kGet,
                        stream_id, fixture.worker.resource(), response)
                        .status == ruvia::Http3ClientResponseStatus::kMessageEnd);
        RUVIA_CHECK_EQ(response.status, std::uint16_t{406});
        RUVIA_CHECK_EQ(response.finalHeads, std::size_t{1});
        RUVIA_CHECK_EQ(response.errorHeader, std::string("used"));
        if (!web_socket && mode == 0) {
            RUVIA_CHECK_EQ(response.content_encoding, std::string("gzip"));
            const auto decoded_body = ruvia::decodeHttpContent(ruvia::HttpContentCoding::kGzip, response.body,
                {.maxDecodedBytes = state.error_body.size(), .resource = fixture.worker.resource()});
            RUVIA_CHECK(decoded_body.decoded() != nullptr);
            if (const auto* content = decoded_body.decoded()) {
                RUVIA_CHECK_EQ(content->bytes(), state.error_body);
            }
        } else {
            RUVIA_CHECK(response.content_encoding.empty());
            RUVIA_CHECK_EQ(response.body, state.error_body);
        }
        if (web_socket) {
            RUVIA_CHECK(fixture.session.request(stream_id) != nullptr);
            const auto fin = fixture.session.feed(stream_id, {}, true);
            RUVIA_CHECK(fin.scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(fixture.session.release(stream_id));
        }
        RUVIA_CHECK(fixture.session.request(stream_id) == nullptr);
    }
}

}  // namespace

RUVIA_TEST(http3BufferedDispatchRejectsUntrustedAndUnsafeEarlyRequestsBeforeMiddleware) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker_handle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    Fixture fixture(worker_handle, upstream);
    runWorkerTask(attachment, exerciseEarlyProvenanceAndReplayPolicy(fixture, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchRoutesBodyAndPublishesBoundedWireResponse) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    Fixture fixture(workerHandle, upstream);
    runWorkerTask(attachment, exerciseRouteAndPublish(fixture, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchSupportsHeadFileMetadataAndRejectsFilePayload) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    Fixture fixture(workerHandle, upstream);
    runWorkerTask(attachment, exerciseHeadAndFile(fixture, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchColdTasksDoNotLeaseAndRouterErrorsUseErrorHandler) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    Fixture fixture(workerHandle, upstream);
    runWorkerTask(attachment, exerciseColdAndError(fixture, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchErrorHandlerFallbackAndAllocationFailureReleaseStorage) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(workerHandle, upstream);
        runWorkerTask(attachment, exerciseEscapingFailure(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3BufferedDispatchReturnsRepeatedRequestMemoryAndPreservesLeasedSibling) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(workerHandle, upstream);
        runWorkerTask(attachment, exerciseRepeatedRequestMemory(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3_buffered_recovery_preserves_ordinary_and_websocket_coding_policies) {
    for (const bool web_socket : {false, true}) {
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
        const auto worker = attachment.loop().handle();
        ruvia::test::CountingMemoryResource resource;
        {
            Fixture fixture(worker, resource);
            runWorkerTask(attachment, exercise_buffered_recovery_coding(fixture, web_socket, ruvia_ctx));
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
    }
}

ruvia::Task<void> exercise_buffered_recovery_cancellation(Fixture& fixture,
    const ruvia::WorkerHandle& worker, bool web_socket, ruvia::testing::TestContext& ruvia_ctx) {
    auto& state = fixture.routes.handlers;
    ruvia::WorkerSignal error_started(worker);
    state.error_started = &error_started;
    state.suspend_error_call = web_socket ? 2 : 1;
    state.response_no_transform = true;
    state.first_error_no_transform = web_socket;
    const auto accept_encoding = "gzip, identity;q=0";
    if (web_socket) {
        feedWebSocketRequest(fixture.session, fixture.worker, 0, "12", accept_encoding);
    } else {
        const std::array fields{ruvia::Http3FieldSectionFieldView{"accept-encoding", accept_encoding}};
        feedRequest(fixture, 0, "GET", "/large", {}, fields);
    }
    auto dispatch = fixture.makeDispatch(0, fixture.services);
    ruvia::TaskScope tasks(worker, {.resource = fixture.worker.resource()});
    ruvia::WorkerSignal finished(worker);
    auto status = Dispatch::RunStatus::kFailed;
    bool joined = false;
    tasks.spawn(runOwner(dispatch, status, joined, finished));
    co_await error_started.wait();
    RUVIA_CHECK(dispatch.handlerActive());
    dispatch.cancel();
    co_await finished.wait();
    co_await tasks.join();
    RUVIA_CHECK(joined && status == Dispatch::RunStatus::kCancelled);
    RUVIA_CHECK(!dispatch.handlerActive() && !dispatch.responseReady());
    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), std::uint64_t{0});
    buffer::borrowed_block block;
    Control control;
    RUVIA_CHECK(!fixture.outbound.try_receive(block) && !fixture.outbound.try_receive_control(control));
    RUVIA_CHECK(fixture.session.request(0) == nullptr);
}

RUVIA_TEST(http3_buffered_recovery_cancellation_joins_error_handler_without_publishing) {
    for (const bool web_socket : {false, true}) {
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
        const auto worker = attachment.loop().handle();
        ruvia::test::CountingMemoryResource resource;
        {
            Fixture fixture(worker, resource);
            runWorkerTask(attachment, exercise_buffered_recovery_cancellation(fixture, worker, web_socket, ruvia_ctx));
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
    }
}

RUVIA_TEST(http3BufferedDispatchWebSocketHandshakeFailureAppliesRequiredHeaders) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(workerHandle, upstream);
        fixture.executor = attachment.loop().executor();
        runWorkerTask(attachment, exerciseWebSocketHandshakeFailures(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
