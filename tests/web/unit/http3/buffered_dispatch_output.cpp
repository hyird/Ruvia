#include <variant>

#include "http3_buffered_dispatch_fixture.h"

namespace {

ruvia::Task<void> exerciseBackpressure(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    fixture.routes.handlers.responseBody.assign(48 * 1024, 'z');
    feedRequest(fixture, 0, "GET", "/large");
    auto dispatch = fixture.makeDispatch(0, fixture.services);
    RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);
    PublishedWire wire;
    const MessageId id{kEpoch, kGeneration, 0};

    const auto first = dispatch.publishStep();
    RUVIA_CHECK(first.status == Dispatch::PublishStatus::kBytesPublished);
    RUVIA_CHECK(first.blockReason == BlockReason::kNone);
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);
    const auto firstSize = dispatch.publishedWireBytes();
    const auto blockedData = dispatch.publishStep();
    RUVIA_CHECK(blockedData.status == Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blockedData.blockReason == BlockReason::kData);
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);
    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), firstSize);
    RUVIA_CHECK(drainDataOnly(fixture.outbound, id, wire) == 1);

    // Keep the independent control lane full until the response cursor reaches FIN.
    const auto filler = fixture.outbound.try_send_control(
        {Control::kind::writable, id, 0});
    RUVIA_CHECK(controlAccepted(filler));
    bool blockedFin = false;
    std::size_t attempts = 0;
    while (!blockedFin && !dispatch.complete() && ++attempts < 10000) {
        const auto demand = dispatch.publicationDemand();
        RUVIA_CHECK(demand == PublicationDemand::kData ||
                    demand == PublicationDemand::kControl);
        const auto result = dispatch.publishStep();
        if (result.status == Dispatch::PublishStatus::kBytesPublished) {
            RUVIA_CHECK(demand == PublicationDemand::kData);
            RUVIA_CHECK(result.blockReason == BlockReason::kNone);
            RUVIA_CHECK(result.bytesPublished <= buffer::max_block_bytes);
            RUVIA_CHECK(drainDataOnly(fixture.outbound, id, wire) == 1);
        } else if (result.status == Dispatch::PublishStatus::kBackpressured) {
            const auto received = drainDataOnly(fixture.outbound, id, wire);
            if (received == 0) {
                blockedFin = true;
                RUVIA_CHECK(demand == PublicationDemand::kControl);
                RUVIA_CHECK(result.blockReason == BlockReason::kControl);
                RUVIA_CHECK(!dispatch.complete());
            } else {
                RUVIA_CHECK(demand == PublicationDemand::kData);
                RUVIA_CHECK(result.blockReason == BlockReason::kData);
            }
        } else {
            RUVIA_CHECK(result.blockReason == BlockReason::kNone);
            RUVIA_CHECK(result.status != Dispatch::PublishStatus::kFailed);
            if (result.status == Dispatch::PublishStatus::kFinPublished) {
                break;
            }
        }
    }
    RUVIA_CHECK(blockedFin);
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kControl);
    const auto beforeFin = dispatch.publishedWireBytes();
    Control queuedFiller;
    RUVIA_CHECK(fixture.outbound.try_receive_control(queuedFiller));
    RUVIA_CHECK(queuedFiller.kind == Control::kind::writable);

    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kControl);
    const auto fin = dispatch.publishStep();
    RUVIA_CHECK(fin.status == Dispatch::PublishStatus::kFinPublished);
    RUVIA_CHECK(fin.blockReason == BlockReason::kNone);
    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), beforeFin);
    drain_buffer(fixture.outbound, id, wire);
    RUVIA_CHECK(dispatch.complete());
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kLocalComplete);
    RUVIA_CHECK_EQ(wire.bytes.size(), beforeFin);
    RUVIA_CHECK_EQ(wire.finalWireBytes.value_or(0), beforeFin);
    DecodedResponse response;
    const auto decoded = decodePublished(
        wire, ruvia::HttpKnownMethod::kGet, 0, fixture.worker.resource(), response);
    RUVIA_CHECK(decoded.status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(response.body.size(), std::size_t{48 * 1024});
    RUVIA_CHECK(std::ranges::all_of(response.body, [](char value) { return value == 'z'; }));
    const auto complete = dispatch.publishStep();
    RUVIA_CHECK(complete.status == Dispatch::PublishStatus::kComplete);
    RUVIA_CHECK(complete.blockReason == BlockReason::kNone);

    feedRequest(fixture, 4, "GET", "/empty-file");
    auto cancelledAtControl = fixture.makeDispatch(4, fixture.services);
    RUVIA_CHECK(co_await cancelledAtControl.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK(cancelledAtControl.publishStep().status ==
                Dispatch::PublishStatus::kBytesPublished);
    RUVIA_CHECK(cancelledAtControl.publicationDemand() == PublicationDemand::kControl);
    const auto controlFiller = fixture.outbound.try_send_control(
        {Control::kind::writable, {kEpoch, kGeneration, 4}, 0});
    RUVIA_CHECK(controlAccepted(controlFiller));
    const auto blockedControl = cancelledAtControl.publishStep();
    RUVIA_CHECK(blockedControl.status == Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blockedControl.blockReason == BlockReason::kControl);
    RUVIA_CHECK(cancelledAtControl.publicationDemand() == PublicationDemand::kControl);
    cancelledAtControl.cancel();
    RUVIA_CHECK(cancelledAtControl.publicationDemand() ==
                PublicationDemand::kLocalCancelled);
    RUVIA_CHECK(cancelledAtControl.publishStep().status ==
                Dispatch::PublishStatus::kCancelled);
    Control preservedFiller;
    RUVIA_CHECK(fixture.outbound.try_receive_control(preservedFiller));
    RUVIA_CHECK(preservedFiller.kind == Control::kind::writable);
    PublishedWire cancelledWire;
    drain_buffer(fixture.outbound, {kEpoch, kGeneration, 4}, cancelledWire);
    RUVIA_CHECK(!cancelledWire.finalWireBytes.has_value());
    RUVIA_CHECK(fixture.session.request(4) == nullptr);

    feedRequest(fixture, 8, "GET", "/large");
    auto stopAfterBlock = fixture.makeDispatch(8, fixture.services);
    RUVIA_CHECK(co_await stopAfterBlock.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK(stopAfterBlock.publicationDemand() == PublicationDemand::kData);
    const auto queued = stopAfterBlock.publishStep();
    RUVIA_CHECK(queued.status == Dispatch::PublishStatus::kBytesPublished);
    RUVIA_CHECK(queued.blockReason == BlockReason::kNone);
    RUVIA_CHECK(stopAfterBlock.publicationDemand() == PublicationDemand::kData);
    const auto blockedBeforeStop = stopAfterBlock.publishStep();
    RUVIA_CHECK(blockedBeforeStop.status == Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blockedBeforeStop.blockReason == BlockReason::kData);
    RUVIA_CHECK(stopAfterBlock.publicationDemand() == PublicationDemand::kData);
    const auto beforeStop = stopAfterBlock.publishedWireBytes();
    RUVIA_CHECK(fixture.outbound.stop());
    const auto allocationsAtStop = fixture.upstream.allocationCount();
    const auto returnsAtStop = fixture.upstream.deallocationCount();
    const auto liveAtStop = fixture.upstream.liveAllocations();
    RUVIA_CHECK(stopAfterBlock.publicationDemand() ==
                PublicationDemand::local_buffer_stopped);
    RUVIA_CHECK_EQ(fixture.upstream.allocationCount(), allocationsAtStop);
    RUVIA_CHECK_EQ(fixture.upstream.deallocationCount(), returnsAtStop);
    RUVIA_CHECK_EQ(fixture.upstream.liveAllocations(), liveAtStop);
    const auto stopped = stopAfterBlock.publishStep();
    RUVIA_CHECK(stopped.status == Dispatch::PublishStatus::kFailed);
    RUVIA_CHECK(stopped.blockReason == BlockReason::kNone);
    RUVIA_CHECK_EQ(stopAfterBlock.publishedWireBytes(), beforeStop);
}

ruvia::Task<void> publishStandardResponseAndMeasure(Fixture& fixture, std::uint64_t streamId,
    std::size_t& decodedFieldSectionSize, std::size_t& encodedFieldSectionSize,
    ruvia::testing::TestContext& ruvia_ctx) {
    feedRequest(fixture, streamId, "GET", "/large");
    auto dispatch = fixture.makeDispatch(streamId, fixture.services);
    RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);
    PublishedWire wire;
    publishAndDrain(dispatch, fixture, streamId, wire, ruvia_ctx);
    DecodedResponse response;
    RUVIA_CHECK(decodePublished(wire, ruvia::HttpKnownMethod::kGet, streamId,
                    fixture.worker.resource(), response)
                    .status == ruvia::Http3ClientResponseStatus::kMessageEnd);
    RUVIA_CHECK_EQ(response.finalHeads, std::size_t{1});
    decodedFieldSectionSize = response.decodedFieldSectionSize;
    const auto headers = ruvia::decodeHttp3Frame(
        std::span<const char>(wire.bytes.data(), wire.bytes.size()));
    RUVIA_CHECK((headers.index() == 0));
    if ((headers.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(headers).type,
            static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders));
        encodedFieldSectionSize = std::get<0>(headers).payload.size();
    }
    RUVIA_CHECK(fixture.session.request(streamId) == nullptr);
}

ruvia::Task<void> exerciseUnlimitedPeerFieldSectionSize(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    RUVIA_CHECK(!fixture.session.peerMaxFieldSectionSize().has_value());
    std::size_t decodedSize{};
    std::size_t encodedSize{};
    co_await publishStandardResponseAndMeasure(fixture, 0, decodedSize, encodedSize, ruvia_ctx);
    RUVIA_CHECK(decodedSize > encodedSize);
    RUVIA_CHECK(!fixture.session.peerMaxFieldSectionSize().has_value());

    feedPeerSettings(fixture, std::nullopt);
    RUVIA_CHECK(!fixture.session.peerMaxFieldSectionSize().has_value());
    co_await publishStandardResponseAndMeasure(fixture, 4, decodedSize, encodedSize, ruvia_ctx);
    RUVIA_CHECK(decodedSize > encodedSize);
    RUVIA_CHECK(!fixture.session.peerMaxFieldSectionSize().has_value());
}

ruvia::Task<void> exercise_nonbuffered_peer_refusal(
    const ruvia::WorkerHandle& worker, ruvia::test::CountingMemoryResource& memory,
    ruvia::testing::TestContext& ruvia_ctx) {
    for (unsigned mode = 0; mode != 4; ++mode) {
        Fixture fixture(worker, memory);
        feedPeerSettings(fixture, 0);
        fixture.routes.handlers.sendInterim = mode == 1 || mode == 2;
        if (mode == 3) {
            const std::array fields{
                ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
                ruvia::Http3FieldSectionFieldView{":authority", "backend.test:443"}};
            const auto encoded = ruvia::encodeHttp3FieldSection(fields, fixture.worker.resource());
            RUVIA_CHECK((encoded.index() == 0));
            const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
                std::string_view(std::get<0>(encoded).data(), std::get<0>(encoded).size()));
            RUVIA_CHECK(fixture.session.feed(0, wire).scope == ruvia::Http3ConnectionErrorScope::kNone);
        } else {
            feedRequest(fixture, 0, "GET", mode == 2 ? "/large" : "/stream");
        }
        auto dispatch = fixture.makeDispatch(0, fixture.services);
        RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::peer_field_section_limit);
        // The Router invokes its error handler and constructs its fallback
        // response in every refusal mode. Even though that response is ready,
        // the peer limit forbids publishing a replacement HEADERS section.
        RUVIA_CHECK(fixture.routes.handlers.errorHandlerCalled);
        RUVIA_CHECK(fixture.routes.handlers.error_handler_response_ready);
        RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kLocalPeerLimitRejected);
        RUVIA_CHECK(dispatch.publishStep().status == Dispatch::PublishStatus::kPeerLimitRejected);
        RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), std::uint64_t{0});
        RUVIA_CHECK(fixture.session.request(0) == nullptr);
        RUVIA_CHECK_EQ(fixture.session.activeStreamCount(), std::size_t{0});
        RUVIA_CHECK(!fixture.session.terminated());
        buffer::borrowed_block block;
        Control control;
        RUVIA_CHECK(!fixture.outbound.try_receive(block));
        RUVIA_CHECK(!fixture.outbound.try_receive_control(control));
    }
}

ruvia::Task<void> exerciseDecodedPeerFieldSectionLimit(
    const ruvia::WorkerHandle& workerHandle,
    ruvia::test::CountingMemoryResource& referenceMemory,
    ruvia::test::CountingMemoryResource& exactLimitMemory,
    ruvia::test::CountingMemoryResource& belowLimitMemory,
    ruvia::testing::TestContext& ruvia_ctx) {
    std::size_t decodedSize{};
    std::size_t encodedSize{};
    {
        Fixture reference(workerHandle, referenceMemory);
        co_await publishStandardResponseAndMeasure(reference, 0, decodedSize, encodedSize, ruvia_ctx);
    }
    RUVIA_CHECK(decodedSize > encodedSize);
    RUVIA_CHECK(decodedSize > 0);

    {
        Fixture exactLimit(workerHandle, exactLimitMemory);
        feedPeerSettings(exactLimit, decodedSize);
        RUVIA_CHECK(exactLimit.session.peerMaxFieldSectionSize() == decodedSize);
        feedRequest(exactLimit, 0, "GET", "/large");
        auto dispatch = exactLimit.makeDispatch(0, exactLimit.services);
        RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);
        PublishedWire wire;
        publishAndDrain(dispatch, exactLimit, 0, wire, ruvia_ctx);
        DecodedResponse response;
        RUVIA_CHECK(decodePublished(wire, ruvia::HttpKnownMethod::kGet, 0,
                        exactLimit.worker.resource(), response)
                        .status == ruvia::Http3ClientResponseStatus::kMessageEnd);
        RUVIA_CHECK_EQ(response.decodedFieldSectionSize, decodedSize);
        RUVIA_CHECK(dispatch.complete());
    }

    {
        Fixture belowLimit(workerHandle, belowLimitMemory);
        feedPeerSettings(belowLimit, decodedSize - 1);
        RUVIA_CHECK(belowLimit.session.peerMaxFieldSectionSize() == decodedSize - 1);
        feedRequest(belowLimit, 0, "GET", "/large");
        auto dispatch = belowLimit.makeDispatch(0, belowLimit.services);
        RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::peer_field_section_limit);
        const auto rejected = dispatch.publishStep();
        RUVIA_CHECK(rejected.status == Dispatch::PublishStatus::kPeerLimitRejected);
        RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), std::uint64_t{0});
        RUVIA_CHECK(dispatch.failure() == nullptr);
        RUVIA_CHECK(!dispatch.responseReady());
        RUVIA_CHECK(belowLimit.session.request(0) == nullptr);
        RUVIA_CHECK_EQ(belowLimit.session.activeStreamCount(), std::size_t{0});
        RUVIA_CHECK(!belowLimit.session.terminated());
        const auto repeated = dispatch.publishStep();
        RUVIA_CHECK(repeated.status == Dispatch::PublishStatus::kPeerLimitRejected);
        buffer::borrowed_block block;
        Control control;
        RUVIA_CHECK(!belowLimit.outbound.try_receive(block));
        RUVIA_CHECK(!belowLimit.outbound.try_receive_control(control));
    }
}

ruvia::Task<void> exerciseLatePeerFieldSectionLimit(
    const ruvia::WorkerHandle& workerHandle,
    ruvia::test::CountingMemoryResource& beforeHandoffMemory,
    ruvia::test::CountingMemoryResource& committedMemory,
    ruvia::testing::TestContext& ruvia_ctx) {
    {
        Fixture beforeHandoff(workerHandle, beforeHandoffMemory);
        feedRequest(beforeHandoff, 0, "GET", "/large");
        auto dispatch = beforeHandoff.makeDispatch(0, beforeHandoff.services);
        RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);
        const MessageId fillerId{kEpoch, kGeneration, 100};
        const std::array<std::byte, 1> fillerBytes{std::byte{0x5a}};
        RUVIA_CHECK(sendAccepted(beforeHandoff.outbound.try_send(fillerId, fillerBytes)));
        RUVIA_CHECK(controlAccepted(beforeHandoff.outbound.try_send_control(
            {Control::kind::writable, fillerId, 0})));
        feedPeerSettings(beforeHandoff, 0);
        RUVIA_CHECK(beforeHandoff.session.peerMaxFieldSectionSize() == 0);
        const auto allocations = beforeHandoffMemory.allocationCount();
        const auto returns = beforeHandoffMemory.deallocationCount();
        RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kLocalPeerLimitRejected);
        RUVIA_CHECK_EQ(beforeHandoffMemory.allocationCount(), allocations);
        RUVIA_CHECK_EQ(beforeHandoffMemory.deallocationCount(), returns);
        const auto rejected = dispatch.publishStep();
        RUVIA_CHECK(rejected.status == Dispatch::PublishStatus::kPeerLimitRejected);
        RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), std::uint64_t{0});
        RUVIA_CHECK(dispatch.failure() == nullptr);
        RUVIA_CHECK(beforeHandoff.session.request(0) == nullptr);
        RUVIA_CHECK_EQ(beforeHandoff.session.activeStreamCount(), std::size_t{0});
        RUVIA_CHECK(!beforeHandoff.session.terminated());
        buffer::borrowed_block preservedBlock;
        RUVIA_CHECK(beforeHandoff.outbound.try_receive(preservedBlock));
        RUVIA_CHECK(preservedBlock.id().stream_id == fillerId.stream_id);
        RUVIA_CHECK(preservedBlock.bytes().size() == fillerBytes.size());
        RUVIA_CHECK(preservedBlock.bytes().front() == fillerBytes.front());
        preservedBlock.release();
        Control preservedControl;
        RUVIA_CHECK(beforeHandoff.outbound.try_receive_control(preservedControl));
        RUVIA_CHECK(preservedControl.kind == Control::kind::writable);
        RUVIA_CHECK(preservedControl.id.stream_id == fillerId.stream_id);

        feedRequest(beforeHandoff, 4, "GET", "/large");
        auto cancelled = beforeHandoff.makeDispatch(4, beforeHandoff.services);
        RUVIA_CHECK(co_await cancelled.prepare() == Dispatch::PrepareStatus::kPrepared);
        cancelled.cancel();
        RUVIA_CHECK(co_await cancelled.runHandler() == Dispatch::RunStatus::kCancelled);
        RUVIA_CHECK(cancelled.publicationDemand() == PublicationDemand::kLocalCancelled);
        RUVIA_CHECK(cancelled.publishStep().status == Dispatch::PublishStatus::kCancelled);
        RUVIA_CHECK(beforeHandoff.session.request(4) == nullptr);
        RUVIA_CHECK_EQ(beforeHandoff.session.activeStreamCount(), std::size_t{0});

        buffer::borrowed_block block;
        Control control;
        RUVIA_CHECK(!beforeHandoff.outbound.try_receive(block));
        RUVIA_CHECK(!beforeHandoff.outbound.try_receive_control(control));
    }

    {
        Fixture committed(workerHandle, committedMemory);
        committed.routes.handlers.largeResponseHeader.assign(20 * 1024, 'h');
        feedRequest(committed, 0, "GET", "/large");
        auto dispatch = committed.makeDispatch(0, committed.services);
        RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);

        PublishedWire wire;
        const MessageId id{kEpoch, kGeneration, 0};
        const auto prefix = dispatch.publishStep();
        RUVIA_CHECK(prefix.status == Dispatch::PublishStatus::kBytesPublished);
        RUVIA_CHECK_EQ(prefix.bytesPublished, buffer::max_block_bytes);
        RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), buffer::max_block_bytes);
        drain_buffer(committed.outbound, id, wire);
        RUVIA_CHECK_EQ(wire.bytes.size(), buffer::max_block_bytes);
        feedPeerSettings(committed, 0);
        RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);

        publishAndDrain(dispatch, committed, 0, wire, ruvia_ctx);
        RUVIA_CHECK(dispatch.complete());
        DecodedResponse response;
        RUVIA_CHECK(decodePublished(wire, ruvia::HttpKnownMethod::kGet, 0,
                        committed.worker.resource(), response)
                        .status == ruvia::Http3ClientResponseStatus::kMessageEnd);
        RUVIA_CHECK_EQ(response.finalHeads, std::size_t{1});
        RUVIA_CHECK_EQ(response.messageEnds, std::size_t{1});
        RUVIA_CHECK_EQ(response.largeResponseHeaderBytes, std::size_t{20 * 1024});
        RUVIA_CHECK(response.body == committed.routes.handlers.responseBody);

        feedRequest(committed, 4, "GET", "/large");
        auto nextRequest = committed.makeDispatch(4, committed.services);
        RUVIA_CHECK(co_await nextRequest.runHandler() == Dispatch::RunStatus::peer_field_section_limit);
        RUVIA_CHECK(nextRequest.publicationDemand() ==
                    PublicationDemand::kLocalPeerLimitRejected);
        const auto rejected = nextRequest.publishStep();
        RUVIA_CHECK(rejected.status == Dispatch::PublishStatus::kPeerLimitRejected);
        RUVIA_CHECK_EQ(nextRequest.publishedWireBytes(), std::uint64_t{0});
        RUVIA_CHECK(committed.session.request(4) == nullptr);
        RUVIA_CHECK_EQ(committed.session.activeStreamCount(), std::size_t{0});
        RUVIA_CHECK(!committed.session.terminated());

        feedRequest(committed, 8, "GET", "/large");
        auto shuttingDown = committed.makeDispatch(8, committed.services);
        RUVIA_CHECK(co_await shuttingDown.runHandler() == Dispatch::RunStatus::peer_field_section_limit);
        RUVIA_CHECK(committed.outbound.stop());
        // A later buffer shutdown does not overwrite the already committed
        // encoding refusal or require a second retirement of its request.
        RUVIA_CHECK(shuttingDown.publishStep().status == Dispatch::PublishStatus::kPeerLimitRejected);
        RUVIA_CHECK(committed.session.request(8) == nullptr);
        RUVIA_CHECK_EQ(committed.session.activeStreamCount(), std::size_t{0});

        buffer::borrowed_block block;
        Control control;
        RUVIA_CHECK(!committed.outbound.try_receive(block));
        RUVIA_CHECK(!committed.outbound.try_receive_control(control));
    }
}

ruvia::Task<void> exerciseAsyncResponse(Fixture& fixture, const ruvia::WorkerHandle& worker,
    std::uint64_t streamId, std::string_view path, bool cancel, ruvia::testing::TestContext& ruvia_ctx) {
    feedRequest(fixture, streamId, "GET", path);
    TunnelCallbacksState callbacks(worker, fixture.scanner);
    auto dispatch = fixture.makeDispatch(streamId, fixture.services, callbacks.callbacks());
    {
        auto cold = dispatch.runHandler();
    }
    ruvia::WorkerSignal finished(worker);
    ruvia::TaskScope tasks(worker, {.resource = fixture.worker.resource()});
    Dispatch::RunStatus status{Dispatch::RunStatus::kFailed};
    bool joined = false;
    tasks.spawn(runOwner(dispatch, status, joined, finished));
    PublishedWire wire;
    const MessageId id{kEpoch, kGeneration, streamId};
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool backpressured = false;
    while (!joined && std::chrono::steady_clock::now() < deadline) {
        const auto step = dispatch.publishStep();
        if (step.status == Dispatch::PublishStatus::kBytesPublished) {
            const auto blocked = dispatch.publishStep();
            backpressured = backpressured || blocked.status == Dispatch::PublishStatus::kBackpressured;
            if (cancel) {
                dispatch.cancel();
            }
        }
        drain_buffer(fixture.outbound, id, wire);
        co_await ruvia::sleepFor(worker, 1ms);
    }
    if (!joined) {
        dispatch.cancel();
    }
    co_await tasks.join();
    RUVIA_CHECK(joined);
    RUVIA_CHECK(fixture.routes.handlers.streamRetainedStable);
    if (cancel || fixture.routes.handlers.streamThrowAfterWrite) {
        RUVIA_CHECK(status == Dispatch::RunStatus::kCancelled);
        RUVIA_CHECK(!wire.finalWireBytes.has_value());
    } else {
        RUVIA_CHECK(status == Dispatch::RunStatus::kOutputComplete);
        RUVIA_CHECK(dispatch.complete());
        DecodedResponse response;
        RUVIA_CHECK(decodePublished(wire, ruvia::HttpKnownMethod::kGet, streamId, fixture.worker.resource(), response).status == ruvia::Http3ClientResponseStatus::kMessageEnd);
        RUVIA_CHECK_EQ(response.status,
            path == "/multipart-file" ? std::uint16_t{206} : std::uint16_t{200});
        if (path == "/multipart-file") {
            const auto& content = fixture.routes.handlers.responseBody;
            const auto separator = std::string("\r\n--h3_test_boundary\r\nContent-Type: text/plain\r\n");
            const std::string expected =
                "--h3_test_boundary\r\nContent-Type: text/plain\r\nContent-Range: bytes 0-19999/65539\r\n\r\n" +
                content.substr(0, 20000) + separator +
                "Content-Range: bytes 30000-65538/65539\r\n\r\n" + content.substr(30000) +
                "\r\n--h3_test_boundary--\r\n";
            RUVIA_CHECK_EQ(response.body, expected);
            RUVIA_CHECK_EQ(response.contentLength.value_or(0), expected.size());
            RUVIA_CHECK(response.contentType.starts_with(
                "multipart/byteranges; boundary=h3_test_boundary"));
        }
        if (fixture.routes.handlers.sendInterim) {
            RUVIA_CHECK_EQ(response.interimHeads, std::size_t{1});
            RUVIA_CHECK_EQ(response.finalHeads, std::size_t{1});
            RUVIA_CHECK_EQ(response.earlyLink, path == "/stream" ? std::string("</style.css>; rel=preload") : std::string(512, 'h'));
        }
        if (path == "/stream") {
            RUVIA_CHECK_EQ(response.body, fixture.routes.handlers.responseBody + fixture.routes.handlers.responseBody + fixture.routes.handlers.responseBody + fixture.routes.handlers.responseBody);
            RUVIA_CHECK_EQ(response.completeTrailer, "yes");
        } else if (path != "/multipart-file") {
            RUVIA_CHECK_EQ(response.body, fixture.routes.handlers.responseBody);
        }
        RUVIA_CHECK(backpressured);
    }
    RUVIA_CHECK(fixture.session.request(streamId) == nullptr);
}

ruvia::Task<void> exerciseResponseStorage(Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    fixture.routes.handlers.responseBody.assign(32768, 's');
    co_await exerciseAsyncResponse(fixture, worker, 0, "/stream", false, ruvia_ctx);
    const auto warmed = fixture.upstream.liveAllocations();
    co_await exerciseAsyncResponse(fixture, worker, 4, "/stream", false, ruvia_ctx);
    RUVIA_CHECK_EQ(fixture.upstream.liveAllocations(), warmed);
    co_await exerciseAsyncResponse(fixture, worker, 8, "/stream", true, ruvia_ctx);
    fixture.routes.handlers.streamThrowAfterWrite = true;
    co_await exerciseAsyncResponse(fixture, worker, 12, "/stream", false, ruvia_ctx);
}

}  // namespace

RUVIA_TEST(http3BufferedDispatchBackpressuresDataAndFinWithoutAcknowledgingUnpublishedBytes) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    Fixture fixture(workerHandle, upstream, 1, 2, 1);
    runWorkerTask(attachment, exerciseBackpressure(fixture, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchTreatsAbsentAndOmittedPeerFieldLimitsAsUnlimited) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(workerHandle, upstream);
        runWorkerTask(attachment, exerciseUnlimitedPeerFieldSectionSize(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3_nonbuffered_peer_refusal_retires_stream_without_connection_failure) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource memory;
    runWorkerTask(attachment, exercise_nonbuffered_peer_refusal(worker, memory, ruvia_ctx));
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(http3BufferedDispatchUsesDecodedPeerFieldSectionSizeIncludingStatus) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource referenceMemory;
    ruvia::test::CountingMemoryResource exactLimitMemory;
    ruvia::test::CountingMemoryResource belowLimitMemory;
    runWorkerTask(attachment, exerciseDecodedPeerFieldSectionLimit(workerHandle,
                                  referenceMemory, exactLimitMemory, belowLimitMemory, ruvia_ctx));
    RUVIA_CHECK_EQ(referenceMemory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(referenceMemory.allocationCount(), referenceMemory.deallocationCount());
    RUVIA_CHECK_EQ(exactLimitMemory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(exactLimitMemory.allocationCount(), exactLimitMemory.deallocationCount());
    RUVIA_CHECK_EQ(belowLimitMemory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(belowLimitMemory.allocationCount(), belowLimitMemory.deallocationCount());
}

RUVIA_TEST(http3BufferedDispatchRechecksLimitBeforeHandoffAndPreservesCommittedHeaderPrefix) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource beforeHandoffMemory;
    ruvia::test::CountingMemoryResource committedMemory;
    runWorkerTask(attachment, exerciseLatePeerFieldSectionLimit(workerHandle,
                                  beforeHandoffMemory, committedMemory, ruvia_ctx));
    RUVIA_CHECK_EQ(beforeHandoffMemory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(beforeHandoffMemory.allocationCount(),
        beforeHandoffMemory.deallocationCount());
    RUVIA_CHECK_EQ(committedMemory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(committedMemory.allocationCount(), committedMemory.deallocationCount());
}

RUVIA_TEST(http3ResponseStreamPublishesBoundedDataTrailersAndReclaimsOperations) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        fixture.executor = attachment.loop().executor();
        runWorkerTask(attachment, exerciseResponseStorage(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

ruvia::Task<void> exercise_trailer_peer_refusal(Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    fixture.routes.handlers.response_trailer.assign(2048, 't');
    feedPeerSettings(fixture, 512);
    feedRequest(fixture, 0, "GET", "/stream");
    TunnelCallbacksState callbacks(worker, fixture.scanner);
    auto dispatch = fixture.makeDispatch(0, fixture.services, callbacks.callbacks());
    ruvia::TaskScope tasks(worker, {.resource = fixture.worker.resource()});
    ruvia::WorkerSignal finished(worker);
    Dispatch::RunStatus status{Dispatch::RunStatus::kFailed};
    bool joined = false;
    tasks.spawn(runOwner(dispatch, status, joined, finished));
    PublishedWire wire;
    const MessageId id{kEpoch, kGeneration, 0};
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!joined && std::chrono::steady_clock::now() < deadline) {
        (void)dispatch.publishStep();
        drain_buffer(fixture.outbound, id, wire);
        co_await ruvia::sleepFor(worker, 1ms);
    }
    if (!joined) {
        dispatch.cancel();
    }
    co_await tasks.join();
    RUVIA_CHECK(joined);
    RUVIA_CHECK(status == Dispatch::RunStatus::peer_field_section_limit);
    RUVIA_CHECK(!wire.bytes.empty());
    RUVIA_CHECK(!wire.finalWireBytes);
    RUVIA_CHECK(dispatch.publishStep().status == Dispatch::PublishStatus::kPeerLimitRejected);
    RUVIA_CHECK(fixture.session.request(0) == nullptr);
    RUVIA_CHECK_EQ(fixture.session.activeStreamCount(), std::size_t{0});
    RUVIA_CHECK(!fixture.session.terminated());
}

RUVIA_TEST(http3_oversized_trailers_reject_only_the_partial_response_stream) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource memory;
    {
        Fixture fixture(worker, memory);
        runWorkerTask(attachment, exercise_trailer_peer_refusal(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(http3MultipartFileResponsePublishesExactBodyLengthAndFinUnderBackpressure) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("ruvia-h3-multipart-" + std::to_string(
                                                   std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream output(path, std::ios::binary);
        output << std::string(65539, '2');
    }
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    ruvia::BlockingPool pool({.threadCount = 1, .queueCapacity = 8});
    {
        Fixture fixture(worker, upstream);
        fixture.executor = attachment.loop().executor();
        fixture.options.blockingPool = &pool;
        fixture.routes.handlers.filePath = path;
        fixture.routes.handlers.responseBody.assign(65539, '2');
        auto exercise = [&]() -> ruvia::Task<void> {
            co_await exerciseAsyncResponse(fixture, worker, 0, "/multipart-file", false, ruvia_ctx);
            co_await exerciseAsyncResponse(fixture, worker, 4, "/multipart-file", true, ruvia_ctx);
        };
        runWorkerTask(attachment, exercise());
    }
    pool.stop();
    pool.join();
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ResponseFileReadsBoundedBlocksOffWorkerAndPublishesFin) {
    const auto path = std::filesystem::temp_directory_path() / ("ruvia-h3-file-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string content(65539, 'f');
    {
        std::ofstream output(path, std::ios::binary);
        output << content;
    }
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    ruvia::BlockingPool pool({.threadCount = 1, .queueCapacity = 8});
    {
        Fixture fixture(worker, upstream);
        fixture.executor = attachment.loop().executor();
        fixture.options.blockingPool = &pool;
        fixture.routes.handlers.filePath = path;
        fixture.routes.handlers.responseBody = content;
        auto exercise = [&]() -> ruvia::Task<void> {
            co_await exerciseAsyncResponse(fixture, worker, 0, "/file", false, ruvia_ctx);
            feedPeerSettings(fixture, 0);
            feedRequest(fixture, 4, "GET", "/file");
            auto refused = fixture.makeDispatch(4, fixture.services);
            RUVIA_CHECK(co_await refused.runHandler() == Dispatch::RunStatus::peer_field_section_limit);
            RUVIA_CHECK_EQ(refused.publishedWireBytes(), std::uint64_t{0});
            RUVIA_CHECK(fixture.session.request(4) == nullptr);
            RUVIA_CHECK_EQ(fixture.session.activeStreamCount(), std::size_t{0});
            RUVIA_CHECK(!fixture.session.terminated());
        };
        runWorkerTask(attachment, exercise());
    }
    pool.stop();
    pool.join();
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

ruvia::Task<void> exerciseStreamingUpload(Fixture& fixture, const ruvia::WorkerHandle& worker,
    bool cancel, ruvia::testing::TestContext& ruvia_ctx) {
    const std::array initialFields{ruvia::Http3FieldSectionFieldView{"x-checksum", "initial"}};
    const auto head = requestWire(fixture.worker, "POST", "/upload", {}, initialFields);
    RUVIA_CHECK(fixture.session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(fixture.session.streamingRequest(0));
    RUVIA_CHECK(fixture.session.streamState(0) == Engine::StreamState::kReady);
    auto dispatch = fixture.makeDispatch(0, fixture.services);
    {
        auto cold = dispatch.runHandler();
    }
    ruvia::WorkerSignal received(worker);
    fixture.routes.handlers.started = &received;
    ruvia::WorkerSignal finished(worker);
    ruvia::TaskScope tasks(worker, {.resource = fixture.worker.resource()});
    Dispatch::RunStatus status{Dispatch::RunStatus::kFailed};
    bool joined = false;
    tasks.spawn(runOwner(dispatch, status, joined, finished));
    const std::string body(65536, 'u');
    const auto data = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), body);
    const auto retained = fixture.upstream.liveAllocations();
    for (unsigned n = 0; n < 20 && !joined; ++n) {
        RUVIA_CHECK(fixture.session.canAcceptInput(0, body.size()));
        RUVIA_CHECK(fixture.session.feed(0, data).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(!fixture.session.canAcceptInput(0, 1));
        dispatch.notifyTunnelInput();
        co_await received.wait();
        co_await ruvia::sleepFor(worker, 1ms);
        RUVIA_CHECK(fixture.session.canAcceptInput(0, body.size()));
    }
    if (cancel) {
        dispatch.cancel();
        (void)fixture.session.cancelRequest(0);
    } else {
        const std::array fields{ruvia::Http3FieldSectionFieldView{"x-checksum", "final"}};
        const auto section = ruvia::encodeHttp3FieldSection(fields, fixture.worker.resource());
        RUVIA_CHECK((section.index() == 0));
        if ((section.index() == 0)) {
            const auto trailers = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders), std::string_view(std::get<0>(section).data(), std::get<0>(section).size()));
            RUVIA_CHECK(fixture.session.feed(0, trailers).scope == ruvia::Http3ConnectionErrorScope::kNone);
        }
        RUVIA_CHECK(fixture.session.feed(0, {}, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);
        dispatch.notifyTunnelInput();
    }
    co_await finished.wait();
    co_await tasks.join();
    if (cancel) {
        RUVIA_CHECK(status == Dispatch::RunStatus::kCancelled);
    } else {
        RUVIA_CHECK(status == Dispatch::RunStatus::kResponseReady);
        RUVIA_CHECK_EQ(fixture.routes.handlers.uploadBytes, body.size() * 20);
        RUVIA_CHECK(fixture.routes.handlers.uploadChunks >= 80);
        RUVIA_CHECK(fixture.routes.handlers.uploadTrailerObserved);
        PublishedWire wire;
        publishAndDrain(dispatch, fixture, 0, wire, ruvia_ctx);
    }
    RUVIA_CHECK_EQ(fixture.bodyBudget.used(), std::size_t{0});
    RUVIA_CHECK(fixture.upstream.liveAllocations() <= retained + 16);
}

RUVIA_TEST(http3StreamingUploadDispatchesBeforeFinBoundsBacklogAndCancelsPendingRead) {
    for (bool cancel : {false, true}) {
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
        const auto worker = attachment.loop().handle();
        ruvia::test::CountingMemoryResource upstream;
        {
            Fixture fixture(worker, upstream);
            fixture.executor = attachment.loop().executor();
            runWorkerTask(attachment, exerciseStreamingUpload(fixture, worker, cancel, ruvia_ctx));
        }
        RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
    }
}

RUVIA_TEST(http3InterimResponsePrecedesBufferedAndStreamFinalAndOwnsFields) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        fixture.routes.handlers.sendInterim = true;
        fixture.routes.handlers.responseBody.assign(32768, 's');
        auto exercise = [&]() -> ruvia::Task<void> {
            co_await exerciseAsyncResponse(fixture, worker, 0, "/large", false, ruvia_ctx);
            co_await exerciseAsyncResponse(fixture, worker, 4, "/stream", false, ruvia_ctx);
            RUVIA_CHECK(fixture.routes.handlers.interimAfterFinalRejected);
        };
        runWorkerTask(attachment, exercise());
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
}
