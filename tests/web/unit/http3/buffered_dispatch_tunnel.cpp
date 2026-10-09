#include <variant>

#include "http3_buffered_dispatch_fixture.h"

namespace {

std::string clientTextFrame(std::string_view payload) {
    if (payload.size() > 125) {
        throw std::runtime_error("HTTP/3 WebSocket fixture payload is too large");
    }
    constexpr std::array<char, 4> mask{char{0x11}, char{0x22}, char{0x33}, char{0x44}};
    std::string wire;
    wire.push_back(static_cast<char>(0x81));
    wire.push_back(static_cast<char>(0x80U | payload.size()));
    wire.append(mask.data(), mask.size());
    for (std::size_t index = 0; index < payload.size(); ++index) {
        wire.push_back(static_cast<char>(payload[index] ^ mask[index % mask.size()]));
    }
    return wire;
}

void feedTunnelData(Fixture& fixture, Dispatch& dispatch, std::uint64_t streamId,
    std::string_view payload) {
    const auto input = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData),
        clientTextFrame(payload));
    const auto result = fixture.session.feed(streamId, input);
    if (result.scope != ruvia::Http3ConnectionErrorScope::kNone) {
        throw std::runtime_error("HTTP/3 WebSocket DATA was rejected");
    }
    dispatch.notifyTunnelInput();
}

void feedRawTunnelData(Engine& session, std::uint64_t streamId, std::string_view payload) {
    const auto input = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), payload);
    const auto result = session.feed(streamId, input);
    if (result.scope != ruvia::Http3ConnectionErrorScope::kNone) {
        throw std::runtime_error("HTTP/3 WebSocket tunnel payload was rejected");
    }
}

void feedRawTunnelData(Fixture& fixture, std::uint64_t streamId, std::string_view payload) {
    feedRawTunnelData(fixture.session, streamId, payload);
}

void feedTunnelFin(Fixture& fixture, Dispatch& dispatch, std::uint64_t streamId) {
    const auto result = fixture.session.feed(streamId, {}, true);
    if (result.scope != ruvia::Http3ConnectionErrorScope::kNone) {
        throw std::runtime_error("HTTP/3 WebSocket FIN was rejected");
    }
    dispatch.notifyTunnelInput();
}

struct WebSocketStatusCapture final {
    std::string status;
};

bool captureWebSocketStatus(void* raw, ruvia::Http3FieldSectionFieldView field) {
    if (field.name == ":status") {
        static_cast<WebSocketStatusCapture*>(raw)->status.assign(field.value);
    }
    return true;
}

void checkWebSocketPublishedWire(const PublishedWire& wire, Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    RUVIA_CHECK(wire.finalWireBytes.has_value());
    RUVIA_CHECK_EQ(wire.finalWireBytes.value_or(0), wire.bytes.size());
    const auto headers = ruvia::decodeHttp3Frame(
        std::span<const char>(wire.bytes.data(), wire.bytes.size()));
    RUVIA_CHECK((headers.index() == 0));
    if ((headers.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(headers).type, static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders));
    WebSocketStatusCapture status;
    const auto decoded = ruvia::decodeHttp3FieldSection(std::get<0>(headers).payload,
        &captureWebSocketStatus, &status, {}, fixture.worker.resource());
    RUVIA_CHECK((decoded.index() == 0));
    RUVIA_CHECK(status.status == "200");

    std::string webSocketBytes;
    std::size_t offset = std::get<0>(headers).encodedBytes;
    while (offset < wire.bytes.size()) {
        const auto data = ruvia::decodeHttp3Frame(
            std::span<const char>(wire.bytes.data() + offset, wire.bytes.size() - offset));
        RUVIA_CHECK((data.index() == 0));
        if ((data.index() != 0)) {
            return;
        }
        RUVIA_CHECK_EQ(std::get<0>(data).type, static_cast<std::uint64_t>(ruvia::Http3FrameType::kData));
        webSocketBytes.append(std::get<0>(data).payload.data(), std::get<0>(data).payload.size());
        offset += std::get<0>(data).encodedBytes;
    }
    RUVIA_CHECK_EQ(offset, wire.bytes.size());
    std::size_t webSocketOffset = 0;
    for (const auto expected : {std::string_view("first"), std::string_view("second")}) {
        RUVIA_CHECK(webSocketBytes.size() - webSocketOffset >= 2);
        if (webSocketBytes.size() - webSocketOffset < 2) {
            return;
        }
        const auto* frameBytes = webSocketBytes.data() + webSocketOffset;
        RUVIA_CHECK(static_cast<unsigned char>(frameBytes[0]) == 0x81U);
        RUVIA_CHECK(static_cast<unsigned char>(frameBytes[1]) == expected.size());
        RUVIA_CHECK(webSocketBytes.size() - webSocketOffset >= expected.size() + 2);
        if (webSocketBytes.size() - webSocketOffset < expected.size() + 2) {
            return;
        }
        RUVIA_CHECK(std::string_view(frameBytes + 2, expected.size()) == expected);
        webSocketOffset += expected.size() + 2;
    }
}

ruvia::Task<bool> driveWebSocketHandshake(Fixture& fixture, Dispatch& dispatch,
    const ruvia::WorkerHandle& worker, MessageId id, PublishedWire& wire,
    ruvia::testing::TestContext& ruvia_ctx) {
    ruvia::Http3ClientResponse peer(id.stream_id, ruvia::HttpKnownMethod::kConnect,
        fixture.worker.resource());
    DecodedResponse response;
    std::optional<Control> establishment;
    std::size_t decoded_bytes{};
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        (void)dispatch.publishStep();
        Control control;
        while (fixture.outbound.try_receive_control(control)) {
            if (control.kind == Control::kind::tunnel_established) {
                establishment = control;
                RUVIA_CHECK(control.id.epoch == id.epoch);
                RUVIA_CHECK(control.id.connection_generation == id.connection_generation);
                RUVIA_CHECK(control.id.stream_id == id.stream_id);
            }
        }
        (void)drainDataOnly(fixture.outbound, id, wire);
        (void)peer.feed(std::span<const char>(wire.bytes).subspan(decoded_bytes),
            false, false, &onResponse, &response);
        decoded_bytes = wire.bytes.size();
        co_await ruvia::sleepFor(worker, 1ms);
    } while ((!establishment || response.finalHeads == 0) &&
             std::chrono::steady_clock::now() < deadline);
    RUVIA_CHECK_EQ(response.finalHeads, std::size_t{1});
    RUVIA_CHECK_EQ(response.status, std::uint16_t{200});
    RUVIA_CHECK(establishment &&
                establishment->value == static_cast<std::uint64_t>(wire.bytes.size()));
    co_return establishment&& response.finalHeads == 1 && response.status == 200;
}

ruvia::Task<void> exerciseWebSocketDataFinBackpressure(Fixture& fixture,
    const ruvia::WorkerHandle& workerHandle, std::uint64_t streamId,
    ruvia::testing::TestContext& ruvia_ctx) {
    auto& handlerState = fixture.routes.handlers;
    handlerState.webSocketMessages = {};
    handlerState.webSocketEchoes = 0;
    handlerState.webSocketSawFin = false;
    handlerState.webSocketRetainedDataStable = true;
    ruvia::WorkerSignal handlerStarted(workerHandle);
    ruvia::WorkerSignal messageReceived(workerHandle);
    ruvia::WorkerSignal messageEchoed(workerHandle);
    handlerState.webSocketStarted = &handlerStarted;
    handlerState.webSocketMessageReceived = &messageReceived;
    handlerState.webSocketMessageEchoed = &messageEchoed;

    TunnelCallbacksState callbacks(workerHandle, fixture.scanner);
    feedWebSocketRequest(fixture, streamId);
    auto dispatch = fixture.makeDispatch(streamId, fixture.services, callbacks.callbacks());
    ruvia::WorkerSignal finished(workerHandle);
    Dispatch::RunStatus runStatus{Dispatch::RunStatus::kFailed};
    bool joined = false;
    ruvia::TaskScope tasks(workerHandle, {.resource = fixture.worker.resource()});
    tasks.spawn(runOwner(dispatch, runStatus, joined, finished));

    PublishedWire wire;
    const MessageId id{kEpoch, kGeneration, streamId};
    if (!(co_await driveWebSocketHandshake(fixture, dispatch, workerHandle, id, wire, ruvia_ctx))) {
        dispatch.cancel();
        co_await tasks.join();
        co_return;
    }
    co_await handlerStarted.wait();
    RUVIA_CHECK(callbacks.scannerAttached);

    const MessageId fillerId{kEpoch, kGeneration, streamId + 1000};
    constexpr std::array<std::byte, 1> fillerBytes{std::byte{0x7f}};
    RUVIA_CHECK(sendAccepted(fixture.outbound.try_send(fillerId, fillerBytes)));
    feedTunnelData(fixture, dispatch, streamId, "first");
    co_await messageReceived.wait();
    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);
    const auto blocked = dispatch.publishStep();
    RUVIA_CHECK(blocked.status == Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blocked.blockReason == BlockReason::kData);
    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), wire.bytes.size());

    buffer::borrowed_block filler;
    RUVIA_CHECK(fixture.outbound.try_receive(filler));
    RUVIA_CHECK(filler.id().stream_id == fillerId.stream_id);
    RUVIA_CHECK(filler.bytes().size() == fillerBytes.size());
    filler.release();
    const auto firstEcho = dispatch.publishStep();
    RUVIA_CHECK(firstEcho.status == Dispatch::PublishStatus::kBytesPublished);
    drain_buffer(fixture.outbound, id, wire);
    co_await messageEchoed.wait();

    feedTunnelData(fixture, dispatch, streamId, "second");
    co_await messageReceived.wait();
    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);
    const auto secondEcho = dispatch.publishStep();
    RUVIA_CHECK(secondEcho.status == Dispatch::PublishStatus::kBytesPublished);
    drain_buffer(fixture.outbound, id, wire);
    co_await messageEchoed.wait();

    feedTunnelFin(fixture, dispatch, streamId);
    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kControl);
    const auto fin = dispatch.publishStep();
    RUVIA_CHECK(fin.status == Dispatch::PublishStatus::kFinPublished);
    drain_buffer(fixture.outbound, id, wire);
    co_await finished.wait();
    co_await tasks.join();
    if (callbacks.scannerAttached) {
        fixture.scanner.unregisterEntry(fixture.scannerEntry);
    }

    RUVIA_CHECK(joined);
    RUVIA_CHECK(runStatus == Dispatch::RunStatus::kTunnelComplete);
    RUVIA_CHECK(dispatch.complete());
    RUVIA_CHECK_EQ(handlerState.webSocketEchoes, std::size_t{2});
    RUVIA_CHECK(handlerState.webSocketMessages[0] == "first");
    RUVIA_CHECK(handlerState.webSocketMessages[1] == "second");
    RUVIA_CHECK(handlerState.webSocketSawFin);
    RUVIA_CHECK(handlerState.webSocketRetainedDataStable);
    RUVIA_CHECK(!callbacks.aborted);
    RUVIA_CHECK(fixture.session.request(streamId) == nullptr);
    RUVIA_CHECK_EQ(fixture.session.activeStreamCount(), std::size_t{0});
    checkWebSocketPublishedWire(wire, fixture, ruvia_ctx);
    handlerState.webSocketStarted = nullptr;
    handlerState.webSocketMessageReceived = nullptr;
    handlerState.webSocketMessageEchoed = nullptr;
}

ruvia::Task<void> exerciseWebSocketTunnelReusesOperationStorage(Fixture& fixture,
    const ruvia::WorkerHandle& workerHandle, ruvia::testing::TestContext& ruvia_ctx,
    std::size_t& warmedLiveAllocations) {
    co_await exerciseWebSocketDataFinBackpressure(fixture, workerHandle, 0, ruvia_ctx);
    warmedLiveAllocations = fixture.upstream.liveAllocations();
    co_await exerciseWebSocketDataFinBackpressure(fixture, workerHandle, 4, ruvia_ctx);
    RUVIA_CHECK_EQ(fixture.upstream.liveAllocations(), warmedLiveAllocations);
}

enum class WebSocketTermination : std::uint8_t { kReset,
    kCancel,
    kDeadline };

ruvia::Task<void> exerciseWebSocketTermination(Fixture& fixture,
    const ruvia::WorkerHandle& workerHandle, WebSocketTermination termination,
    ruvia::testing::TestContext& ruvia_ctx) {
    if (termination == WebSocketTermination::kDeadline) {
        fixture.options.deadline = ruvia::DeadlineConfig{.handler = 25ms};
    }
    ruvia::WorkerSignal handlerStarted(workerHandle);
    ruvia::WorkerSignal messageReceived(workerHandle);
    ruvia::WorkerSignal messageEchoed(workerHandle);
    auto& handlerState = fixture.routes.handlers;
    handlerState.webSocketStarted = &handlerStarted;
    handlerState.webSocketMessageReceived = &messageReceived;
    handlerState.webSocketMessageEchoed = &messageEchoed;
    TunnelCallbacksState callbacks(workerHandle, fixture.scanner);
    feedWebSocketRequest(fixture, 0);
    auto dispatch = fixture.makeDispatch(0, fixture.services, callbacks.callbacks());
    ruvia::WorkerSignal finished(workerHandle);
    Dispatch::RunStatus runStatus{Dispatch::RunStatus::kFailed};
    bool joined = false;
    ruvia::TaskScope tasks(workerHandle, {.resource = fixture.worker.resource()});
    tasks.spawn(runOwner(dispatch, runStatus, joined, finished));

    PublishedWire handshake;
    if (!(co_await driveWebSocketHandshake(fixture, dispatch, workerHandle,
            {kEpoch, kGeneration, 0}, handshake, ruvia_ctx))) {
        dispatch.cancel();
        co_await tasks.join();
        co_return;
    }
    co_await handlerStarted.wait();

    if (termination == WebSocketTermination::kReset) {
        const auto reset = fixture.session.feed(0, {}, false, true);
        RUVIA_CHECK(reset.status == ruvia::Http3ConnectionStatus::kNeedMoreData);
        RUVIA_CHECK(reset.scope == ruvia::Http3ConnectionErrorScope::kNone);
        dispatch.notifyTunnelInput();
    } else if (termination == WebSocketTermination::kCancel) {
        dispatch.cancel();
        (void)fixture.session.cancelRequest(0);
    } else {
        RUVIA_CHECK(co_await ruvia::sleepFor(workerHandle, 100ms, fixture.workerStop) ==
                    ruvia::TimerSleepResult::kElapsed);
        (void)fixture.session.cancelRequest(0);
    }
    co_await finished.wait();
    co_await tasks.join();
    if (callbacks.scannerAttached) {
        fixture.scanner.unregisterEntry(fixture.scannerEntry);
    }

    RUVIA_CHECK(joined);
    RUVIA_CHECK(runStatus == Dispatch::RunStatus::kCancelled);
    RUVIA_CHECK(!dispatch.handlerActive());
    RUVIA_CHECK(callbacks.aborted != (termination == WebSocketTermination::kCancel));
    RUVIA_CHECK(fixture.session.request(0) == nullptr);
    RUVIA_CHECK_EQ(fixture.session.activeStreamCount(), std::size_t{0});
    RUVIA_CHECK(dispatch.cancellationReason() ==
                (termination == WebSocketTermination::kDeadline
                        ? Dispatch::CancellationReason::kDeadline
                        : Dispatch::CancellationReason::kExplicit));
    handlerState.webSocketStarted = nullptr;
    handlerState.webSocketMessageReceived = nullptr;
    handlerState.webSocketMessageEchoed = nullptr;
}

enum class PeerFinWaitOutcome : std::uint8_t { kTimeout,
    kPeerFin,
    kCancel };

ruvia::Task<void> exercisePeerTransportFinWait(Fixture& fixture,
    const ruvia::WorkerHandle& workerHandle, PeerFinWaitOutcome outcome, bool throwHandler,
    ruvia::testing::TestContext& ruvia_ctx) {
    fixture.scanner.start();
    auto& state = fixture.routes.handlers;
    state.webSocketThrowOnStart = throwHandler;
    ruvia::WorkerSignal handlerStarted(workerHandle);
    state.webSocketStarted = &handlerStarted;
    TunnelCallbacksState callbacks(workerHandle, fixture.scanner);
    constexpr std::uint64_t streamId = 0;
    feedWebSocketRequest(fixture, streamId);
    auto dispatch = fixture.makeDispatch(streamId, fixture.services, callbacks.callbacks());
    ruvia::WorkerSignal finished(workerHandle);
    Dispatch::RunStatus runStatus{Dispatch::RunStatus::kFailed};
    bool joined = false;
    ruvia::TaskScope tasks(workerHandle, {.resource = fixture.worker.resource()});
    tasks.spawn(runOwner(dispatch, runStatus, joined, finished));

    PublishedWire handshake;
    if (!(co_await driveWebSocketHandshake(fixture, dispatch, workerHandle,
            {kEpoch, kGeneration, streamId}, handshake, ruvia_ctx))) {
        dispatch.cancel();
        co_await tasks.join();
        co_return;
    }
    co_await handlerStarted.wait();

    // No peer WebSocket Close means no local QUIC FIN deadline yet, even while
    // the scanner continues to inspect the attached request entry.
    RUVIA_CHECK(co_await ruvia::sleepFor(workerHandle, 120ms, fixture.workerStop) ==
                ruvia::TimerSleepResult::kElapsed);
    RUVIA_CHECK(!callbacks.aborted);
    constexpr std::array<char, 6> peerClose{
        static_cast<char>(0x88), static_cast<char>(0x80), char{0x11}, char{0x22}, char{0x33}, char{0x44}};
    feedRawTunnelData(fixture, streamId, std::string_view(peerClose.data(), peerClose.size()));
    dispatch.notifyTunnelInput();

    PublishedWire closeFrameWire;
    bool received_fin = false;
    const auto publication_deadline = std::chrono::steady_clock::now() + 3s;
    while (!received_fin && std::chrono::steady_clock::now() < publication_deadline) {
        (void)dispatch.publishStep();
        Control control;
        while (fixture.outbound.try_receive_control(control)) {
            if (control.kind == Control::kind::stream_fin) {
                RUVIA_CHECK_EQ(control.value, static_cast<std::uint64_t>(
                                                  handshake.bytes.size() + closeFrameWire.bytes.size()));
                received_fin = true;
            }
        }
        (void)drainDataOnly(fixture.outbound, {kEpoch, kGeneration, streamId}, closeFrameWire);
        if (!received_fin) {
            co_await ruvia::sleepFor(workerHandle, 1ms);
        }
    }
    RUVIA_CHECK(received_fin);
    if (!received_fin) {
        dispatch.cancel();
        (void)fixture.session.cancelRequest(streamId);
        co_await tasks.join();
        fixture.scanner.unregisterEntry(fixture.scannerEntry);
        co_return;
    }

    if (outcome == PeerFinWaitOutcome::kPeerFin) {
        feedTunnelFin(fixture, dispatch, streamId);
    } else if (outcome == PeerFinWaitOutcome::kCancel) {
        fixture.scanner.unregisterEntry(fixture.scannerEntry);
        dispatch.cancel();
        (void)fixture.session.cancelRequest(streamId);
    } else {
        // Activity is deliberately refreshed repeatedly. It must not move the
        // absolute FIN deadline or turn it into an inactivity timeout.
        for (unsigned i = 0; i < 8 && !joined; ++i) {
            fixture.scannerEntry.touch();
            RUVIA_CHECK(co_await ruvia::sleepFor(workerHandle, 5ms, fixture.workerStop) ==
                        ruvia::TimerSleepResult::kElapsed);
        }
    }
    co_await finished.wait();
    co_await tasks.join();
    RUVIA_CHECK(joined);
    if (outcome == PeerFinWaitOutcome::kTimeout) {
        RUVIA_CHECK(callbacks.aborted);
        RUVIA_CHECK(runStatus == Dispatch::RunStatus::kCancelled);
        if (throwHandler) {
            bool sawInternalErrorClose = false;
            std::size_t offset = 0;
            while (offset < closeFrameWire.bytes.size()) {
                const auto data = ruvia::decodeHttp3Frame(std::span<const char>(
                    closeFrameWire.bytes.data() + offset, closeFrameWire.bytes.size() - offset));
                RUVIA_CHECK((data.index() == 0));
                if ((data.index() != 0)) {
                    break;
                }
                if (std::get<0>(data).type == static_cast<std::uint64_t>(ruvia::Http3FrameType::kData) &&
                    std::get<0>(data).payload.size() >= 4 &&
                    static_cast<unsigned char>(std::get<0>(data).payload[0]) == 0x88U &&
                    static_cast<unsigned char>(std::get<0>(data).payload[2]) == 0x03U &&
                    static_cast<unsigned char>(std::get<0>(data).payload[3]) == 0xf3U) {
                    sawInternalErrorClose = true;
                }
                offset += std::get<0>(data).encodedBytes;
            }
            RUVIA_CHECK(sawInternalErrorClose);
        }
    } else {
        RUVIA_CHECK(!callbacks.aborted);
        RUVIA_CHECK(runStatus == Dispatch::RunStatus::kTunnelComplete ||
                    runStatus == Dispatch::RunStatus::kCancelled);
    }
    if (outcome != PeerFinWaitOutcome::kCancel) {
        fixture.scanner.unregisterEntry(fixture.scannerEntry);
    }
    fixture.scanner.stop();
    state.webSocketThrowOnStart = false;
    state.webSocketStarted = nullptr;
}

ruvia::Task<void> exerciseConnectTunnel(Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx, bool udp = false) {
    for (unsigned round = 0; round != 3; ++round) {
        const std::uint64_t id = round * 4;
        std::pmr::vector<ruvia::Http3FieldSectionFieldView> fields(fixture.worker.resource());
        fields.push_back({":method", "CONNECT"});
        fields.push_back({":authority", "backend.test:443"});
        if (udp || round != 0) {
            fields.push_back({":protocol", udp ? "connect-udp" : "test-tunnel"});
            fields.push_back({":scheme", "https"});
            fields.push_back({":path", udp ? "/udp/target.test" : "/tunnel/target.test"});
            if (udp) {
                fields.push_back({"capsule-protocol", "?1"});
            }
        }
        auto encoded = ruvia::encodeHttp3FieldSection(fields, fixture.worker.resource());
        if ((encoded.index() != 0)) {
            throw std::runtime_error("CONNECT fixture field encoding failed");
        }
        const auto request = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders), std::string_view(std::get<0>(encoded).data(), std::get<0>(encoded).size()));
        RUVIA_CHECK(fixture.session.feed(id, request).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(fixture.session.streamState(id) == Engine::StreamState::kReady);
        TunnelCallbacksState callbacks(worker, fixture.scanner);
        auto dispatch = fixture.makeDispatch(id, fixture.services, callbacks.callbacks());
        fixture.routes.handlers.tunnelReadAfterFinish = round == 2;
        fixture.routes.handlers.tunnelReceived.clear();
        ruvia::TaskScope tasks(worker, {.resource = fixture.worker.resource()});
        ruvia::WorkerSignal finished(worker);
        bool joined = false;
        auto status = Dispatch::RunStatus::kFailed;
        tasks.spawn(runOwner(dispatch, status, joined, finished));
        PublishedWire wire;
        const MessageId messageId{kEpoch, kGeneration, id};
        bool sent = false;
        const std::string payload(62007, 'c');
        std::string tunnelPayload = payload;
        if (udp) {
            std::array<char, 16> header;
            const auto capsule_header_size = ruvia::encodeHttpCapsuleHeader(header, 0, payload.size() + 1);
            tunnelPayload.assign(header.data(), std::get<0>(capsule_header_size));
            tunnelPayload.push_back('\0');
            tunnelPayload.append(payload);
            tunnelPayload.append("\0\1\0", 3);
        }
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!joined && std::chrono::steady_clock::now() < deadline) {
            (void)dispatch.publishStep();
            drain_buffer(fixture.outbound, messageId, wire);
            if (!sent && !wire.bytes.empty() && (round != 2 || wire.finalWireBytes)) {
                const auto input = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), tunnelPayload);
                const auto fed = fixture.session.feed(id, input, true);
                RUVIA_CHECK(fed.scope == ruvia::Http3ConnectionErrorScope::kNone);
                dispatch.notifyTunnelInput();
                sent = true;
            }
            co_await ruvia::sleepFor(worker, 1ms);
        }
        if (!joined) {
            dispatch.cancel();
        }
        co_await tasks.join();
        if (callbacks.scannerAttached) {
            fixture.scanner.unregisterEntry(fixture.scannerEntry);
        }
        RUVIA_CHECK(joined && sent);
        RUVIA_CHECK(status == Dispatch::RunStatus::kTunnelComplete);
        RUVIA_CHECK(!callbacks.aborted);
        RUVIA_CHECK(fixture.routes.handlers.tunnelReceived == payload);
        RUVIA_CHECK(fixture.routes.handlers.tunnelStable);
        RUVIA_CHECK(wire.finalWireBytes && *wire.finalWireBytes == wire.bytes.size());
        struct Result {
            std::string body;
            std::uint16_t status{};
            bool ended{};
            bool framingField{};
            bool capsule{};
        } result;
        ruvia::Http3ClientResponse response(id, ruvia::HttpKnownMethod::kConnect, fixture.worker.resource());
        const auto callback = [](void* raw, const ruvia::Http3ClientResponseEvent& event) {
            auto& observed = *static_cast<Result*>(raw);
            if (event.kind == ruvia::Http3ClientResponseEventKind::kFinalHead) {
                observed.status = event.head->status;
                for (const auto& field : event.head->headers) {
                    observed.capsule = observed.capsule || (field.name == "capsule-protocol" && field.value == "?1");
                    observed.framingField = observed.framingField || field.name == "content-length" || field.name == "transfer-encoding";
                }
            }
            if (event.kind == ruvia::Http3ClientResponseEventKind::kTunnelData) {
                observed.body.append(event.body.data(), event.body.size());
            }
            if (event.kind == ruvia::Http3ClientResponseEventKind::kMessageEnd) {
                observed.ended = true;
            }
        };
        const auto decoded = response.feed(std::span<const char>(wire.bytes.data(), wire.bytes.size()), true, false, callback, &result);
        RUVIA_CHECK(decoded.scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(result.status == 200 && result.ended && !result.framingField);
        RUVIA_CHECK(!udp || result.capsule);
        RUVIA_CHECK(result.body == (round == 2 ? std::string{} : tunnelPayload));
        RUVIA_CHECK(fixture.session.activeStreamCount() == 0);
    }
}

}  // namespace

RUVIA_TEST(http3BufferedDispatchBoundsTunnelInputAcrossWorkerAndReleasesEveryReservation) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(workerHandle, upstream, 1, 1, 1, 10, 16);
        feedWebSocketRequest(fixture, 0);
        feedWebSocketRequest(fixture, 4);
        feedRawTunnelData(fixture, 0, "first");
        feedRawTunnelData(fixture, 4, "other");
        RUVIA_CHECK_EQ(fixture.bodyBudget.used(), std::size_t{10});

        Engine siblingSession(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.bodyBudget, {.maxTunnelBufferedBytes = 16});
        feedWebSocketRequest(siblingSession, fixture.worker, 0);
        feedRawTunnelData(siblingSession, 0, "x");
        RUVIA_CHECK(siblingSession.tunnelInputOverflowed(0));
        RUVIA_CHECK_EQ(fixture.bodyBudget.used(), std::size_t{10});
        RUVIA_CHECK(siblingSession.cancelRequest(0));
        RUVIA_CHECK_EQ(fixture.bodyBudget.used(), std::size_t{10});

        std::array<char, 8> retained{};
        const auto other = fixture.session.readTunnelData(4, retained);
        RUVIA_CHECK_EQ(other.bytes, std::size_t{5});
        RUVIA_CHECK(std::string_view(retained.data(), other.bytes) == "other");
        RUVIA_CHECK_EQ(fixture.bodyBudget.used(), std::size_t{5});
        RUVIA_CHECK(fixture.session.cancelRequest(0));
        RUVIA_CHECK_EQ(fixture.bodyBudget.used(), std::size_t{0});
        RUVIA_CHECK(std::string_view(retained.data(), other.bytes) == "other");
        RUVIA_CHECK(fixture.session.cancelRequest(4));
        RUVIA_CHECK_EQ(fixture.bodyBudget.used(), std::size_t{0});

        feedWebSocketRequest(fixture, 12);
        feedRawTunnelData(fixture, 12, "seventeen-bytes!!");
        RUVIA_CHECK(fixture.session.tunnelInputOverflowed(12));
        RUVIA_CHECK_EQ(fixture.bodyBudget.used(), std::size_t{0});
        RUVIA_CHECK(fixture.session.cancelRequest(12));
    }
    {
        // Exceed every implementation's worker pool size class so the upstream
        // rejection cannot be satisfied from a cached block on MSVC.
        constexpr std::size_t tunnelLimit = 512 * 1024;
        Fixture fixture(workerHandle, upstream, 1, 1, 1, tunnelLimit, tunnelLimit);
        const std::string allocationFailurePayload(256 * 1024, 'a');
        feedWebSocketRequest(fixture, 16);
        fixture.allocations.reject = true;
        feedRawTunnelData(fixture, 16, allocationFailurePayload);
        fixture.allocations.reject = false;
        RUVIA_CHECK(fixture.session.tunnelInputOverflowed(16));
        RUVIA_CHECK_EQ(fixture.bodyBudget.used(), std::size_t{0});
        RUVIA_CHECK(fixture.session.cancelRequest(16));

        feedWebSocketRequest(fixture, 20);
        feedRawTunnelData(fixture, 20, "reset");
        RUVIA_CHECK_EQ(fixture.bodyBudget.used(), std::size_t{5});
        const auto reset = fixture.session.feed(20, {}, false, true);
        RUVIA_CHECK(reset.scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK_EQ(fixture.bodyBudget.used(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
    (void)workerHandle;
}

RUVIA_TEST(http3BufferedDispatchWebSocketTunnelPublishesDataFinAndBoundsPmrStorage) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    std::size_t warmedLiveAllocations{};
    {
        Fixture fixture(workerHandle, upstream);
        fixture.executor = attachment.loop().executor();
        runWorkerTask(attachment, exerciseWebSocketTunnelReusesOperationStorage(
                                      fixture, workerHandle, ruvia_ctx, warmedLiveAllocations));
    }
    RUVIA_CHECK(warmedLiveAllocations > 0);
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

void runWebSocketTerminationCase(WebSocketTermination termination,
    ruvia::testing::TestContext& ruvia_ctx) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(workerHandle, upstream);
        fixture.executor = attachment.loop().executor();
        runWorkerTask(attachment,
            exerciseWebSocketTermination(fixture, workerHandle, termination, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3BufferedDispatchWebSocketTunnelResetCancelAndDeadlineWakeReaders) {
    runWebSocketTerminationCase(WebSocketTermination::kReset, ruvia_ctx);
    runWebSocketTerminationCase(WebSocketTermination::kCancel, ruvia_ctx);
    runWebSocketTerminationCase(WebSocketTermination::kDeadline, ruvia_ctx);
}

void runPeerTransportFinWaitCase(PeerFinWaitOutcome outcome, bool throwHandler,
    ruvia::testing::TestContext& ruvia_ctx) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(workerHandle, upstream, 1, 1, 1, 64 * 1024 * 1024,
            64 * 1024, 80ms, 1ms);
        fixture.executor = attachment.loop().executor();
        runWorkerTask(attachment,
            exercisePeerTransportFinWait(
                fixture, workerHandle, outcome, throwHandler, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3BufferedDispatchPeerTransportFinDeadlineIsPublicationBoundAndCleansUp) {
    runPeerTransportFinWaitCase(PeerFinWaitOutcome::kTimeout, false, ruvia_ctx);
    runPeerTransportFinWaitCase(PeerFinWaitOutcome::kTimeout, true, ruvia_ctx);
    runPeerTransportFinWaitCase(PeerFinWaitOutcome::kPeerFin, false, ruvia_ctx);
    runPeerTransportFinWaitCase(PeerFinWaitOutcome::kCancel, false, ruvia_ctx);
}

RUVIA_TEST(http3BufferedDispatchConnectStreamsOwnBytesAndReadAfterPublishedSendFin) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto& worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource resource;
    {
        Fixture fixture(worker, resource);
        fixture.executor = attachment.loop().executor();
        runWorkerTask(attachment, exerciseConnectTunnel(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(http3_connect_success_admits_native_datagrams_before_peer_response_visibility) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto& worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource resource;
    {
        Fixture fixture(worker, resource, 1, 1, 1, 64 * 1024 * 1024, 64 * 1024, 5s, 1ms, 1200);
        fixture.executor = attachment.loop().executor();
        feedPeerSettings(fixture, std::nullopt, true);
        auto exercise = [&]() -> ruvia::Task<void> {
            constexpr std::uint64_t stream_id = 0;
            const std::array fields{
                ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
                ruvia::Http3FieldSectionFieldView{":protocol", "connect-udp"},
                ruvia::Http3FieldSectionFieldView{":scheme", "https"},
                ruvia::Http3FieldSectionFieldView{":authority", "backend.test:443"},
                ruvia::Http3FieldSectionFieldView{":path", "/udp/target.test"},
                ruvia::Http3FieldSectionFieldView{"capsule-protocol", "?1"}};
            const auto encoded = ruvia::encodeHttp3FieldSection(fields, fixture.worker.resource());
            if ((encoded.index() != 0)) {
                throw std::runtime_error("CONNECT publication fixture could not encode request");
            }
            const auto request = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
                std::string_view(std::get<0>(encoded).data(), std::get<0>(encoded).size()));
            RUVIA_CHECK(fixture.session.feed(stream_id, request).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(fixture.session.streamState(stream_id) == Engine::StreamState::kReady);
            const auto negotiated = fixture.session.datagramConfig(stream_id);
            RUVIA_CHECK(negotiated.localH3Datagram && negotiated.peerH3Datagram && negotiated.quicDatagram);
            const MessageId message_id{kEpoch, kGeneration, stream_id};
            const Control occupying_control{Control::kind::stream_fin,
                {kEpoch, kGeneration, 4}, 0};
            RUVIA_CHECK(controlAccepted(fixture.outbound.try_send_control(occupying_control)));
            TunnelCallbacksState callbacks(worker, fixture.scanner);
            auto dispatch = fixture.makeDispatch(stream_id, fixture.services, callbacks.callbacks());
            ruvia::TaskScope tasks(worker, {.resource = fixture.worker.resource()});
            ruvia::WorkerSignal finished(worker);
            bool joined = false;
            auto status = Dispatch::RunStatus::kFailed;
            tasks.spawn(runOwner(dispatch, status, joined, finished));
            PublishedWire wire;
            DecodedResponse observed;
            ruvia::Http3ClientResponse peer(stream_id, ruvia::HttpKnownMethod::kConnect, fixture.worker.resource());
            std::array<char, 16> native{};
            const auto prefix = ruvia::encodeHttp3DatagramPrefix(native, stream_id);
            RUVIA_CHECK((prefix.index() == 0));
            native[std::get<0>(prefix)] = '\0';  // CONNECT-UDP Context ID zero.
            native[std::get<0>(prefix) + 1] = 'c';
            const auto packet = std::span<const char>(native.data(), std::get<0>(prefix) + 2);
            std::optional<Control> marker;
            const auto plan = [&]() {
                return ruvia::testing::plan_connect_datagram_for_peer(
                    {kEpoch, kGeneration}, marker, wire.bytes.size(), packet);
            };
            co_await callbacks.outputReady.wait();
            (void)dispatch.publishStep();
            (void)drainDataOnly(fixture.outbound, message_id, wire);
            (void)peer.feed(wire.bytes, false, false, &onResponse, &observed);
            // Control admission is held: a peer must not already see success.
            // If it does, its legal first native datagram must already be admissible.
            if (observed.finalHeads != 0) {
                RUVIA_CHECK_EQ(observed.status, std::uint16_t{200});
                RUVIA_CHECK_EQ(static_cast<unsigned>(plan()),
                    static_cast<unsigned>(ruvia::Http3DatagramReceiveStatus::kDeliver));
            }
            RUVIA_CHECK_EQ(observed.finalHeads, std::size_t{0});
            Control control;
            RUVIA_CHECK(fixture.outbound.try_receive_control(control));
            std::size_t decoded_bytes = wire.bytes.size();
            for (unsigned turn = 0; turn != 8 && observed.finalHeads == 0; ++turn) {
                (void)dispatch.publishStep();
                while (fixture.outbound.try_receive_control(control)) {
                    if (control.kind == Control::kind::tunnel_established) {
                        marker = control;
                    }
                }
                (void)drainDataOnly(fixture.outbound, message_id, wire);
                (void)peer.feed(std::span<const char>(wire.bytes).subspan(decoded_bytes),
                    false, false, &onResponse, &observed);
                decoded_bytes = wire.bytes.size();
                co_await ruvia::sleepFor(worker, 1ms);
            }
            RUVIA_CHECK_EQ(observed.finalHeads, std::size_t{1});
            RUVIA_CHECK_EQ(observed.status, std::uint16_t{200});
            RUVIA_CHECK_EQ(static_cast<unsigned>(plan()),
                static_cast<unsigned>(ruvia::Http3DatagramReceiveStatus::kDeliver));
            RUVIA_CHECK_EQ(fixture.routes.handlers.udp_tunnel_starts, std::size_t{1});
            dispatch.cancel();
            (void)fixture.session.cancelRequest(stream_id);
            co_await tasks.join();
            if (callbacks.scannerAttached) {
                fixture.scanner.unregisterEntry(fixture.scannerEntry);
            }
            RUVIA_CHECK(joined && status == Dispatch::RunStatus::kCancelled);
            RUVIA_CHECK(!dispatch.handlerActive());
        };
        runWorkerTask(attachment, exercise());
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(http3_connect_cancellation_before_head_visibility_retires_admitted_tunnel_barrier) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto& worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource resource;
    {
        Fixture fixture(worker, resource, 1, 1, 1, 64 * 1024 * 1024, 64 * 1024, 5s, 1ms, 1200);
        fixture.executor = attachment.loop().executor();
        feedPeerSettings(fixture, std::nullopt, true);
        auto exercise = [&]() -> ruvia::Task<void> {
            const std::array fields{
                ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
                ruvia::Http3FieldSectionFieldView{":protocol", "connect-udp"},
                ruvia::Http3FieldSectionFieldView{":scheme", "https"},
                ruvia::Http3FieldSectionFieldView{":authority", "backend.test:443"},
                ruvia::Http3FieldSectionFieldView{":path", "/udp/target.test"},
                ruvia::Http3FieldSectionFieldView{"capsule-protocol", "?1"}};
            const auto encoded = ruvia::encodeHttp3FieldSection(fields, fixture.worker.resource());
            if ((encoded.index() != 0)) {
                throw std::runtime_error("CONNECT cancellation fixture could not encode request");
            }
            const auto request = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
                std::string_view(std::get<0>(encoded).data(), std::get<0>(encoded).size()));
            RUVIA_CHECK(fixture.session.feed(0, request).scope == ruvia::Http3ConnectionErrorScope::kNone);
            const auto negotiated = fixture.session.datagramConfig(0);
            RUVIA_CHECK(negotiated.localH3Datagram && negotiated.peerH3Datagram && negotiated.quicDatagram);
            const std::array<std::byte, 1> filler{std::byte{0x7f}};
            RUVIA_CHECK(sendAccepted(fixture.outbound.try_send(
                {kEpoch, kGeneration, 4}, filler)));
            TunnelCallbacksState callbacks(worker, fixture.scanner);
            auto dispatch = fixture.makeDispatch(0, fixture.services, callbacks.callbacks());
            ruvia::TaskScope tasks(worker, {.resource = fixture.worker.resource()});
            ruvia::WorkerSignal finished(worker);
            bool joined = false;
            auto status = Dispatch::RunStatus::kFailed;
            tasks.spawn(runOwner(dispatch, status, joined, finished));
            std::optional<Control> marker;
            for (unsigned turn = 0; turn != 8 && !marker; ++turn) {
                (void)dispatch.publishStep();
                Control control;
                while (fixture.outbound.try_receive_control(control)) {
                    if (control.kind == Control::kind::tunnel_established) {
                        marker = control;
                    }
                }
                co_await ruvia::sleepFor(worker, 1ms);
            }
            RUVIA_CHECK(marker && marker->value != 0);
            buffer::borrowed_block held_credit;
            RUVIA_CHECK(fixture.outbound.try_receive(held_credit));
            RUVIA_CHECK_EQ(held_credit.id().stream_id, std::uint64_t{4});
            (void)dispatch.publishStep();  // HEAD still cannot borrow the held credit.
            co_await ruvia::sleepFor(worker, 1ms);
            DecodedResponse observed;
            ruvia::Http3ClientResponse peer(0, ruvia::HttpKnownMethod::kConnect, fixture.worker.resource());
            (void)peer.feed({}, false, false, &onResponse, &observed);
            RUVIA_CHECK_EQ(observed.finalHeads, std::size_t{0});
            RUVIA_CHECK_EQ(fixture.routes.handlers.udp_tunnel_starts, std::size_t{0});
            constexpr std::array<char, 3> native{'\0', '\0', 'c'};
            RUVIA_CHECK_EQ(static_cast<unsigned>(ruvia::testing::plan_connect_datagram_for_peer(
                               {kEpoch, kGeneration}, marker, 0, native)),
                static_cast<unsigned>(ruvia::Http3DatagramReceiveStatus::kStreamError));
            dispatch.cancel();
            (void)fixture.session.cancelRequest(0);
            co_await tasks.join();
            if (callbacks.scannerAttached) {
                fixture.scanner.unregisterEntry(fixture.scannerEntry);
            }
            RUVIA_CHECK(joined && status == Dispatch::RunStatus::kCancelled);
            RUVIA_CHECK(!dispatch.handlerActive());
            RUVIA_CHECK(fixture.session.request(0) == nullptr);
            RUVIA_CHECK_EQ(held_credit.bytes().size(), std::size_t{1});
            RUVIA_CHECK(held_credit.bytes()[0] == std::byte{0x7f});
            held_credit.release();
            RUVIA_CHECK(fixture.outbound.quiescent());
        };
        runWorkerTask(attachment, exercise());
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(http3BufferedDispatchConnectDrainTimeoutRetiresOnlyItsStream) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto& worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource resource;
    {
        Fixture fixture(worker, resource, 1, 1, 1, 64 * 1024 * 1024, 64 * 1024, 20ms, 1ms);
        fixture.executor = attachment.loop().executor();
        fixture.routes.handlers.tunnelReturnEarly = true;
        auto exercise = [&]() -> ruvia::Task<void> {
            fixture.scanner.start();
            const std::array fields{
                ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
                ruvia::Http3FieldSectionFieldView{":authority", "backend.test:443"}};
            const auto encoded = ruvia::encodeHttp3FieldSection(fields, fixture.worker.resource());
            if ((encoded.index() != 0)) {
                throw std::runtime_error("CONNECT timeout field encoding failed");
            }
            const auto request = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
                std::string_view(std::get<0>(encoded).data(), std::get<0>(encoded).size()));
            RUVIA_CHECK(fixture.session.feed(0, request).scope == ruvia::Http3ConnectionErrorScope::kNone);
            TunnelCallbacksState callbacks(worker, fixture.scanner);
            auto dispatch = fixture.makeDispatch(0, fixture.services, callbacks.callbacks());
            ruvia::TaskScope tasks(worker, {.resource = fixture.worker.resource()});
            ruvia::WorkerSignal finished(worker);
            bool joined = false;
            auto status = Dispatch::RunStatus::kFailed;
            tasks.spawn(runOwner(dispatch, status, joined, finished));
            PublishedWire wire;
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!joined && std::chrono::steady_clock::now() < deadline) {
                (void)dispatch.publishStep();
                drain_buffer(fixture.outbound, {kEpoch, kGeneration, 0}, wire);
                co_await ruvia::sleepFor(worker, 1ms);
            }
            if (!joined) {
                dispatch.cancel();
            }
            co_await tasks.join();
            if (callbacks.scannerAttached) {
                fixture.scanner.unregisterEntry(fixture.scannerEntry);
            }
            fixture.scanner.stop();
            RUVIA_CHECK(joined && status == Dispatch::RunStatus::kCancelled && callbacks.aborted);
            RUVIA_CHECK(wire.finalWireBytes.has_value());
            RUVIA_CHECK(fixture.session.request(0) == nullptr);
            feedRequest(fixture, 4, "GET", "/large");
            RUVIA_CHECK(fixture.session.streamState(4) == Engine::StreamState::kReady);
            RUVIA_CHECK(fixture.session.release(4));
        };
        runWorkerTask(attachment, exercise());
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(http3BufferedDispatchUdpTunnelNegotiatesCapsulesAndPreservesDatagramBoundariesAndHalfClose) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto& worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource resource;
    {
        Fixture fixture(worker, resource);
        fixture.executor = attachment.loop().executor();
        runWorkerTask(attachment, exerciseConnectTunnel(fixture, worker, ruvia_ctx, true));
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}
