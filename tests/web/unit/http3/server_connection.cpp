#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3ClientResponse.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3QpackConnection.h"
#include "ruvia/http/Http3Settings.h"
#include "ruvia/http/HttpPriority.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/CallbackRef.h"
#include "ruvia/web/detail/http3/Http3NetworkRuntime.h"
#include "ruvia/web/detail/http3/Http3ServerConnection.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

#include "memory_resource_fixture.h"
#include "routing_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace ruvia::detail {

struct Http3NetworkRuntimeTestAccess final {
    using Network = Http3NetworkRuntime;
    using Stream = Network::Stream;
    using Connection = Network::Connection;
    using TunnelResult = Network::TunnelEstablishedResult;

    static Connection makeConnection(std::pmr::memory_resource* resource,
        Http3ServerConnectionChannel::Identity identity) {
        Connection connection(resource);
        connection.identity = identity;
        connection.streams.reserve(8);
        return connection;
    }

    static Stream& addRequestStream(Connection& connection, std::uint64_t id) {
        connection.streams.emplace_back(connection.streams.get_allocator().resource());
        auto& stream = connection.streams.back();
        stream.id = id;
        stream.requestStream = true;
        stream.receivePhase = Stream::ReceivePhase::kBody;
        return stream;
    }

    static TunnelResult acceptTunnelEstablished(Connection& connection,
        const Http3StreamControl& control, std::uint64_t acceptedWireBytes) noexcept {
        return Network::acceptTunnelEstablished(connection, control, acceptedWireBytes);
    }

    static bool confirmTunnelEstablished(Connection& connection,
        std::uint64_t streamId, std::uint64_t acceptedWireBytes) noexcept {
        return Network::confirmTunnelEstablished(connection, streamId, acceptedWireBytes);
    }

    static void notePeerFin(Connection& connection, std::uint64_t streamId) noexcept {
        Network::notePeerFin(connection, streamId);
    }

    static void noteInputReset(Connection& connection, std::uint64_t streamId) noexcept {
        Network::noteInputReset(connection, streamId);
    }

    static void completeInputTerminal(Connection& connection,
        std::uint64_t streamId) noexcept {
        Network::completeInputTerminal(connection, streamId);
    }

    static void installFrameTracker(Stream& stream, std::pmr::memory_resource* resource) {
        stream.frameTracker = makePmrObject<Http3StreamFrames>(resource,
            Http3StreamKind::kRequest, resource);
    }
};

struct Http3ServerConnectionResetIntentTestAccess final {
    using Owner = Http3ServerConnection;

    static bool enqueueLocal(Owner& owner, std::uint64_t streamId) noexcept {
        return enqueue(owner, streamId, Http3ConnectionErrorCode::kRequestCancelled,
            Owner::ResetIntentOrigin::kLocalCancellation);
    }

    static bool enqueueProtocol(Owner& owner, std::uint64_t streamId,
        Http3ConnectionErrorCode errorCode) noexcept {
        return enqueue(owner, streamId, errorCode, Owner::ResetIntentOrigin::kStreamProtocolError);
    }

private:
    static bool enqueue(Owner& owner, std::uint64_t streamId,
        Http3ConnectionErrorCode errorCode, Owner::ResetIntentOrigin origin) noexcept {
        bool created = false;
        auto* slot = owner.findOrCreateRequestSlot(streamId, created);
        if (slot == nullptr || !owner.enqueueResetIntent(*slot, errorCode, origin)) {
            return false;
        }
        owner.notifyActivation();
        return true;
    }
};

}  // namespace ruvia::detail

namespace {

using Connection = ruvia::detail::Http3ServerConnection;
using Control = ruvia::detail::Http3StreamControl;
using Mailbox = ruvia::detail::Http3StreamMailbox;
using MessageId = ruvia::detail::Http3StreamMessageId;
using namespace std::chrono_literals;

struct TestActivationSignal final {
    explicit TestActivationSignal(const ruvia::WorkerHandle& worker)
        : signal(worker) {}

    [[nodiscard]] Connection::ActivationRef activationRef() noexcept {
        return {.context = this,
            .activate = [](void* context, std::uint64_t, std::uint64_t, std::uint64_t,
                            const Connection::WorkerActivation&) noexcept {
                static_cast<TestActivationSignal*>(context)->signal.notify();
            },
            .slotGeneration = 1};
    }

    operator Connection::ActivationRef() noexcept {
        return activationRef();
    }

    void notify() noexcept {
        signal.notify();
    }

    [[nodiscard]] ruvia::Task<void> wait() {
        return signal.wait();
    }

    ruvia::WorkerSignal signal;
};

constexpr Connection::WorkLanes kAllWorkLanes{
    .data = true, .control = true, .local = true};

constexpr std::uint64_t kEpoch = 47;
constexpr std::uint64_t kGeneration = 71;

struct HandlerState final {
    ruvia::WorkerSignal* slowStarted{};
    ruvia::WorkerSignal* heldStarted{};
    ruvia::WorkerSignal* heldRelease{};
    bool heldHandlerFinished{};
    bool slowObservedStop{};
    bool slowStartedObserved{};
    std::string bodySeen;
    std::string largeResponseHeader;
    ruvia::WorkerSignal* webSocketStarted{};
    bool webSocketStartedObserved{};
    bool webSocketHandlerFinished{};
    std::size_t handlerCalls{};
    std::atomic<std::size_t>* runtimeHandlerCalls{};
    int pushMode{};
    bool pushCompleted{};
    bool pushAccepted{};
    std::string pushedCookie;
    std::string pushedHeader;
    ruvia::HttpPriority pushedPriority{};
};

ruvia::Task<void> webSocketHandler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<HandlerState*>(raw);
    state.webSocketStartedObserved = true;
    state.webSocketStarted->notify();
    co_await context.webSocket().close();
    state.webSocketHandlerFinished = true;
}

ruvia::Task<ruvia::HttpResponse> requestHandler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<HandlerState*>(raw);
    ++state.handlerCalls;
    if (state.runtimeHandlerCalls != nullptr) {
        state.runtimeHandlerCalls->fetch_add(1, std::memory_order_relaxed);
    }
    const auto path = context.req().path();
    if (path == "/push") {
        std::string target = state.pushMode == 9 ? "/slow" : "/first";
        std::string authority = state.pushMode == 6 ? "other.test" : "example.test";
        std::array headers{ruvia::HttpHeaderView("cookie", "session=pushed"), ruvia::HttpHeaderView("x-pushed", "owned-header")};
        auto operation = context.push({.method = state.pushMode == 8 ? "HEAD" : "GET", .authority = authority, .path = target, .headers = headers});
        target.assign("changed");
        authority.assign("changed");
        if (state.pushMode != 5) {
            state.pushAccepted = co_await std::move(operation);
        }
        state.pushCompleted = true;
        co_return context.text("parent");
    }
    if (path == "/first") {
        state.pushedCookie = context.req().cookie("session").value_or("");
        state.pushedHeader = context.req().header("x-pushed").value_or("");
    }

    if (path == "/advertise") {
        std::array<std::string, 40> storage;
        std::array<std::string_view, 40> origins;
        for (std::size_t index = 0; index != storage.size(); ++index) {
            storage[index] = "https://" + std::string(60, 'a') + "." + std::string(60, 'b') + std::to_string(index) + ".example.test";
            origins[index] = storage[index];
        }
        co_await context.advertiseOrigins(origins);
    }
    if (path == "/throw") {
        throw std::runtime_error("buffered HTTP/3 route failure");
    }
    if (path == "/file") {
        ruvia::HttpResponse response({.resource = context.arena()});
        response.fileBody("virtual-response.bin", 5, 0, 5, ruvia::HttpResponseFileIdentity::checked({}));
        co_return response;
    }
    if (path == "/held") {
        state.heldStarted->notify();
        co_await state.heldRelease->wait();
        state.heldHandlerFinished = true;
        co_return context.text("released");
    }
    if (path == "/slow" || path == "/slow-body") {
        if (path == "/slow-body") {
            const auto body = co_await context.req().text();
            state.bodySeen.assign(body);
        }
        state.slowStartedObserved = true;
        state.slowStarted->notify();
        const auto result = co_await ruvia::sleepFor(
            context.worker(), 5s, context.stopToken());
        state.slowObservedStop = result == ruvia::TimerSleepResult::kStopRequested;
        state.pushedPriority = context.req().priority();
        co_return context.text("must-not-be-published");
    }
    if (context.req().method() == "POST") {
        const auto body = co_await context.req().text();
        state.bodySeen.assign(body);
        co_return context.text(std::string_view(body));
    }
    if (!state.largeResponseHeader.empty()) {
        context.header("x-large-response", state.largeResponseHeader);
    }
    co_return context.text(path);
}

struct Routes final {
    HandlerState handlers;
    ruvia::detail::Router router;
    ruvia::detail::RouterImpl& implementation{ruvia::detail::RouterImpl::from(router)};

    Routes() {
        add(ruvia::HttpKnownMethod::kGet, "/first");
        add(ruvia::HttpKnownMethod::kGet, "/push");
        add(ruvia::HttpKnownMethod::kGet, "/throw");
        add(ruvia::HttpKnownMethod::kGet, "/slow");
        add(ruvia::HttpKnownMethod::kGet, "/held");
        add(ruvia::HttpKnownMethod::kGet, "/file");
        add(ruvia::HttpKnownMethod::kPost, "/body");
        add(ruvia::HttpKnownMethod::kPost, "/slow-body");
        add(ruvia::HttpKnownMethod::kGet, "/advertise");
        implementation.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
            routing_test::path("/socket"),
            ruvia::detail::RouteStreamHandler(&handlers, &webSocketHandler),
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
        implementation.finalize();
    }

    void add(ruvia::HttpKnownMethod method, std::string_view path) {
        implementation.registerRoute(method, routing_test::path(path),
            ruvia::detail::RouteHandler(&handlers, &requestHandler),
            ruvia::detail::RequestBodyMode::kBuffered,
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
    }
};

struct Fixture final {
    Routes routes;
    ruvia::WorkerMemory worker;
    ruvia::StopSource workerStopSource;
    ruvia::StopToken workerStop;
    ruvia::detail::ContextServices services;
    ruvia::detail::HttpServerOptions options;
    ruvia::ConnectionScanner scanner;
    asio::any_io_executor executor;

    Fixture(const ruvia::WorkerHandle& workerHandle,
        std::pmr::memory_resource& upstream)
        : worker(upstream),
          workerStop(workerStopSource.token()),
          services(workerHandle, workerStop),
          scanner(workerHandle, {.scanInterval = 2ms}),
          executor(asio::system_executor{}) {}
};

std::string frame(std::uint64_t type, std::string_view payload) {
    std::array<char, ruvia::kHttp3FrameHeaderMaxBytes> header{};
    const auto size = ruvia::encodeHttp3FrameHeader(header, type, payload.size());
    if (!size) {
        throw std::runtime_error("HTTP/3 test frame encoding failed");
    }
    std::string wire(header.data(), *size);
    wire.append(payload);
    return wire;
}

std::string requestWire(ruvia::WorkerMemory& worker, std::string_view method,
    std::string_view path, std::string_view body = {},
    std::span<const ruvia::Http3FieldSectionFieldView> fields = {}) {
    const auto encoded = ruvia::encodeHttp3ClientRequestHead({.method = method,
                                                                 .scheme = "https",
                                                                 .authority = "example.test",
                                                                 .path = path,
                                                                 .fields = fields,
                                                                 .bodyLength = body.empty()
                                                                                   ? std::nullopt
                                                                                   : std::optional<std::uint64_t>(body.size())},
        {}, worker.resource());
    if (!encoded) {
        throw std::runtime_error("HTTP/3 test request-head encoding failed");
    }
    std::string wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(encoded->fieldSection.data(), encoded->fieldSection.size()));
    if (!body.empty()) {
        wire += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), body);
    }
    return wire;
}

bool accepted(Mailbox::SendResult result) noexcept {
    return result == Mailbox::SendResult::kSent ||
           result == Mailbox::SendResult::kSentNotifyPeer;
}

bool accepted(Mailbox::ControlResult result) noexcept {
    return result == Mailbox::ControlResult::kSent ||
           result == Mailbox::ControlResult::kSentNotifyPeer;
}

Connection::EventResult routeRequest(Connection& connection, Mailbox& inbound,
    ruvia::WorkerMemory& worker, MessageId id, std::string_view method,
    std::string_view path, std::string_view body = {},
    std::span<const ruvia::Http3FieldSectionFieldView> fields = {}) {
    const auto wire = requestWire(worker, method, path, body, fields);
    if (wire.size() > Mailbox::kMaxBlockBytes) {
        throw std::runtime_error("HTTP/3 test request exceeded one mailbox block");
    }
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!accepted(inbound.trySend(id, bytes)) ||
        !accepted(inbound.trySendControl(
            {Control::Kind::kStreamFin, id, static_cast<std::uint64_t>(wire.size())}))) {
        throw std::runtime_error("HTTP/3 test inbound mailbox is full");
    }

    Control fin;
    if (!inbound.tryReceiveControl(fin)) {
        throw std::runtime_error("HTTP/3 test FIN control is missing");
    }
    const auto deferred = connection.acceptControl(fin);
    if (deferred.input.status != Connection::Input::Status::kDeferredFin) {
        throw std::runtime_error("HTTP/3 test FIN was not deferred behind its data");
    }

    Mailbox::BorrowedBlock block;
    if (!inbound.tryReceive(block) || block.id().streamId != id.streamId) {
        throw std::runtime_error("HTTP/3 test data block is missing");
    }
    auto result = connection.acceptData(block);
    block.release();
    (void)inbound.drainReturns();
    (void)inbound.finishDrain();
    return result;
}

Connection::EventResult acceptWireBytes(Connection& connection, Mailbox& inbound,
    MessageId id, std::span<const char> wire);

Connection::EventResult acceptTunnelHead(Connection& connection, Mailbox& inbound,
    ruvia::WorkerMemory& worker, MessageId id) {
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/socket"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", "13"}};
    const auto encoded = ruvia::encodeHttp3FieldSection(fields, worker.resource());
    if (!encoded) {
        throw std::runtime_error("HTTP/3 WebSocket field section encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(encoded->data(), encoded->size()));
    const auto sent = inbound.trySend(id,
        std::as_bytes(std::span(wire.data(), wire.size())));
    if (!accepted(sent)) {
        throw std::runtime_error("HTTP/3 WebSocket request mailbox is full");
    }
    Mailbox::BorrowedBlock block;
    if (!inbound.tryReceive(block)) {
        throw std::runtime_error("HTTP/3 WebSocket request block is missing");
    }
    auto result = connection.acceptData(block);
    block.release();
    (void)inbound.drainReturns();
    (void)inbound.finishDrain();
    return result;
}

Connection::EventResult acceptWireBytes(Connection& connection, Mailbox& inbound,
    MessageId id, std::span<const char> wire) {
    Connection::EventResult result;
    for (std::size_t offset = 0; offset < wire.size();) {
        const auto size = std::min(Mailbox::kMaxBlockBytes, wire.size() - offset);
        const auto chunk = wire.subspan(offset, size);
        if (!accepted(inbound.trySend(id, std::as_bytes(chunk)))) {
            throw std::runtime_error("HTTP/3 raw-wire fixture mailbox is full");
        }
        Mailbox::BorrowedBlock block;
        if (!inbound.tryReceive(block)) {
            throw std::runtime_error("HTTP/3 raw-wire fixture block is missing");
        }
        result = connection.acceptData(block);
        block.release();
        (void)inbound.drainReturns();
        (void)inbound.finishDrain();
        offset += size;
        if (result.status != Connection::EventStatus::kAccepted) {
            return result;
        }
    }
    return result;
}

struct PublishedWire final {
    std::string bytes;
    std::optional<std::uint64_t> finalWireBytes;
};

std::size_t wireIndex(std::uint64_t streamId) {
    if ((streamId & 3U) != 0 || streamId / 4 >= 6) {
        throw std::runtime_error("unexpected HTTP/3 test stream ID");
    }
    return static_cast<std::size_t>(streamId / 4);
}

void drainDataOnly(Mailbox& outbound, std::array<PublishedWire, 6>& wires) {
    Mailbox::BorrowedBlock block;
    while (outbound.tryReceive(block)) {
        auto& wire = wires[wireIndex(block.id().streamId)];
        const auto bytes = block.bytes();
        wire.bytes.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        block.release();
    }
    (void)outbound.drainReturns();
}

void drainAll(Mailbox& outbound, std::array<PublishedWire, 6>& wires) {
    bool again = false;
    do {
        Control control;
        while (outbound.tryReceiveControl(control)) {
            if (control.kind == Control::Kind::kStreamFin) {
                auto& wire = wires[wireIndex(control.id.streamId)];
                wire.finalWireBytes = control.value;
            }
        }
        drainDataOnly(outbound, wires);
        again = outbound.finishDrain();
    } while (again);
    (void)outbound.drainReturns();
}

struct DecodedResponse final {
    std::size_t informationalHeads{};
    std::size_t finalHeads{};
    std::size_t messageEnds{};
    std::uint16_t status{};
    std::string body;
};

void captureResponse(void* raw, const ruvia::Http3ClientResponseEvent& event) {
    auto& response = *static_cast<DecodedResponse*>(raw);
    if (event.kind == ruvia::Http3ClientResponseEventKind::kInformationalHead) {
        ++response.informationalHeads;
    } else if (event.kind == ruvia::Http3ClientResponseEventKind::kFinalHead) {
        ++response.finalHeads;
        response.status = event.head->status;
    } else if (event.kind == ruvia::Http3ClientResponseEventKind::kBody) {
        response.body.append(event.body.data(), event.body.size());
    } else if (event.kind == ruvia::Http3ClientResponseEventKind::kMessageEnd) {
        ++response.messageEnds;
    }
}

DecodedResponse decodeResponse(const PublishedWire& wire, ruvia::HttpKnownMethod method,
    std::uint64_t streamId, std::pmr::memory_resource* resource) {
    if (!wire.finalWireBytes || *wire.finalWireBytes != wire.bytes.size()) {
        throw std::runtime_error("HTTP/3 response FIN length does not match its wire bytes");
    }
    DecodedResponse response;
    ruvia::Http3ClientResponse decoder(streamId, method, resource);
    const auto result = decoder.feed(
        std::span<const char>(wire.bytes.data(), wire.bytes.size()), true, false,
        &captureResponse, &response);
    if (result.status != ruvia::Http3ClientResponseStatus::kMessageEnd) {
        throw std::runtime_error("HTTP/3 response decoder rejected published bytes");
    }
    return response;
}

ruvia::Task<void> stopAfter(ruvia::EventLoopAttachment& attachment,
    ruvia::Task<void> operation) {
    try {
        co_await std::move(operation);
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

void runWorkerTask(ruvia::EventLoopAttachment& attachment, ruvia::Task<void> operation) {
    auto root = attachment.loop().start(stopAfter(attachment, std::move(operation)));
    attachment.run();
    root.get();
}

ruvia::Task<void> waitForReady(Connection& connection, std::size_t count,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stopToken) {
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        if (connection.readyRequestCount() == count) {
            co_return;
        }
        const auto sleep = co_await ruvia::sleepFor(worker, 1ms, stopToken);
        if (sleep != ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    throw std::runtime_error("HTTP/3 buffered requests did not become ready in time");
}

ruvia::Task<bool> waitForSlowStart(Fixture& fixture,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stopToken) {
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        if (fixture.routes.handlers.slowStartedObserved) {
            co_return true;
        }
        if (co_await ruvia::sleepFor(worker, 1ms, stopToken) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    co_return fixture.routes.handlers.slowStartedObserved;
}

void requireWatchdogSuccess(ruvia::testing::TestContext& ruvia_ctx, bool succeeded);

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

ruvia::Task<bool> waitForTaskCount(Connection& connection, std::size_t count,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stopToken) {
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        if (connection.activeTaskCount() == count) {
            co_return true;
        }
        if (co_await ruvia::sleepFor(worker, 1ms, stopToken) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    co_return connection.activeTaskCount() == count;
}

void requireWatchdogSuccess(ruvia::testing::TestContext& ruvia_ctx, bool succeeded) {
    RUVIA_CHECK(succeeded);
    if (!succeeded) {
        std::terminate();
    }
}

void simulateGlobalStopTakeover(Connection& connection) {
    const auto closeIntent = connection.peekTransportIntent();
    if (!closeIntent ||
        closeIntent->token.kind != Connection::TransportIntentKind::kConnectionClose ||
        !connection.takeOverTransportRetirement({.epoch = closeIntent->token.id.epoch,
            .connectionGeneration = closeIntent->token.id.connectionGeneration}) ||
        !connection.ackTransportIntent(closeIntent->token)) {
        throw std::runtime_error("test channel lifecycle owner failed to take over HTTP/3 retirement");
    }
    while (const auto intent = connection.peekTransportIntent()) {
        if (!connection.ackTransportIntent(intent->token)) {
            throw std::runtime_error("test channel lifecycle owner failed to settle HTTP/3 intent");
        }
    }
}

ruvia::Task<void> publishGroup(Connection& connection, Mailbox& outbound,
    std::array<PublishedWire, 6>& wires, std::span<const std::uint64_t> streamIds,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stopToken,
    bool exerciseBackpressure, ruvia::testing::TestContext& ruvia_ctx) {
    std::size_t finished = 0;
    bool sawDataBackpressure = false;
    bool sawControlBackpressure = false;
    bool sawNotifyPeer = false;

    if (exerciseBackpressure) {
        co_await waitForReady(connection, streamIds.size(), worker, stopToken);
        const auto first = connection.publishOne(kAllWorkLanes);
        RUVIA_CHECK(first.status == Connection::PublishStatus::kAttempted);
        RUVIA_CHECK_EQ(first.streamId, streamIds[0]);
        RUVIA_CHECK(first.publication.status ==
                    Connection::Dispatch::PublishStatus::kBytesPublished);
        RUVIA_CHECK(first.publication.notifyPeer);
        RUVIA_CHECK(first.publication.blockReason ==
                    Connection::Dispatch::PublishBlockReason::kNone);
        sawNotifyPeer = first.publication.notifyPeer;

        // Leave the first DATA block queued. The next sibling must still receive
        // its turn and report the shared DATA lane as blocked.
        const auto second = connection.publishOne(kAllWorkLanes);
        RUVIA_CHECK(second.status == Connection::PublishStatus::kAttempted);
        RUVIA_CHECK_EQ(second.streamId, streamIds[1]);
        RUVIA_CHECK(second.publication.status ==
                    Connection::Dispatch::PublishStatus::kBackpressured);
        RUVIA_CHECK(second.publication.blockReason ==
                    Connection::Dispatch::PublishBlockReason::kData);
        RUVIA_CHECK(!second.publication.notifyPeer);
        sawDataBackpressure = true;
        drainAll(outbound, wires);
        RUVIA_CHECK_EQ(connection.reactivateBlocked({.data = true}), std::size_t{1});
    }

    std::size_t attempts = 0;
    while (finished < streamIds.size() && ++attempts < 20000) {
        const auto attempt = connection.publishOne(kAllWorkLanes);
        if (attempt.status == Connection::PublishStatus::kNoReadyRequest) {
            drainAll(outbound, wires);
            const auto reactivated = connection.reactivateBlocked(
                {.data = true, .control = true});
            if (reactivated == 0) {
                throw std::runtime_error("HTTP/3 buffered publisher stalled without blocked work");
            }
            continue;
        }
        RUVIA_CHECK(attempt.status == Connection::PublishStatus::kAttempted);
        if (attempt.status != Connection::PublishStatus::kAttempted) {
            break;
        }
        sawNotifyPeer = sawNotifyPeer || attempt.publication.notifyPeer;
        const auto result = attempt.publication;
        if (result.status == Connection::Dispatch::PublishStatus::kFinPublished) {
            ++finished;
        } else if (result.status == Connection::Dispatch::PublishStatus::kBackpressured) {
            RUVIA_CHECK(!result.notifyPeer);
            if (result.blockReason == Connection::Dispatch::PublishBlockReason::kData) {
                sawDataBackpressure = true;
                drainDataOnly(outbound, wires);
                (void)connection.reactivateBlocked({.data = true});
            } else if (result.blockReason == Connection::Dispatch::PublishBlockReason::kControl) {
                sawControlBackpressure = true;
                drainAll(outbound, wires);
                (void)connection.reactivateBlocked(
                    {.data = true, .control = true});
            } else {
                RUVIA_CHECK(false);
            }
        } else if (result.status == Connection::Dispatch::PublishStatus::kBytesPublished) {
            drainDataOnly(outbound, wires);
            (void)connection.reactivateBlocked({.data = true});
        } else {
            RUVIA_CHECK(result.status == Connection::Dispatch::PublishStatus::kComplete);
            if (result.status != Connection::Dispatch::PublishStatus::kComplete) {
                break;
            }
            ++finished;
        }
    }
    drainAll(outbound, wires);
    RUVIA_CHECK(attempts < 20000);
    RUVIA_CHECK_EQ(finished, streamIds.size());
    RUVIA_CHECK(sawNotifyPeer);
    if (exerciseBackpressure) {
        RUVIA_CHECK(sawDataBackpressure);
        RUVIA_CHECK(sawControlBackpressure);
    }
}

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
    Mailbox inbound(8, 8, 4, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 70, .maxTrackedStreams = 8});

    const MessageId fillerId{kEpoch, kGeneration + 70, 12};
    RUVIA_CHECK(accepted(outbound.trySendControl(
        {Control::Kind::kWritable, fillerId, 0})));
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
    Mailbox inbound(8, 8, 4, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});
    const MessageId id{kEpoch, generation, 0};
    if (controlLane) {
        RUVIA_CHECK(accepted(outbound.trySendControl(
            {Control::Kind::kWritable, {kEpoch, generation, 12}, 0})));
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
    RUVIA_CHECK_EQ(expired.streamId, id.streamId);
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
    RUVIA_CHECK_EQ(reset->token.id.connectionGeneration, id.connectionGeneration);
    RUVIA_CHECK_EQ(reset->token.id.streamId, id.streamId);
    RUVIA_CHECK(reset->streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kRequestCancelled);

    const auto finalSize = requestWire(fixture.worker, method, path).size();
    const auto lateFin = connection.acceptControl(
        {Control::Kind::kStreamFin, id, static_cast<std::uint64_t>(finalSize)});
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
    Mailbox inbound(4, 4, 2, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
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
    RUVIA_CHECK_EQ(reset->token.id.streamId, std::uint64_t{0});
    RUVIA_CHECK(reset->streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kRequestCancelled);
    const auto lateFin = connection.acceptControl({Control::Kind::kStreamFin, id,
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
    Mailbox inbound(8, 8, 8, fixture.worker.resource());
    Mailbox outbound(8, 8, 8, fixture.worker.resource());
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
    RUVIA_CHECK(connection.requestInfo(id.streamId).status == Connection::RequestStatus::kPublished);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{1});

    if (requestStopAfterFin) {
        RUVIA_CHECK(connection.requestStop());
    } else if (peerReset) {
        const auto reset = connection.acceptControl({Control::Kind::kStreamReset, id,
            static_cast<std::uint64_t>(tunnelData.size() + requestHead.size())});
        RUVIA_CHECK(reset.status == Connection::EventStatus::kStreamCancelled);
    } else if (!deadline) {
        const auto peerFin = connection.acceptControl({Control::Kind::kStreamFin, id,
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
        RUVIA_CHECK_EQ(retirement->token.id.streamId, id.streamId);
    } else if (peerReset) {
        RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    }
    if (deadline) {
        const auto lateFin = connection.acceptControl({Control::Kind::kStreamFin, id,
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
            retirement->token.id.streamId != id.streamId) {
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

ruvia::Task<void> exerciseSuccessfulConnection(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    constexpr std::array<std::uint64_t, 4> firstIds{0, 4, 8, 12};
    constexpr std::array<std::uint64_t, 2> secondIds{16, 20};
    constexpr std::array expectField{ruvia::Http3FieldSectionFieldView{"expect", "custom-expectation"}};

    TestActivationSignal scheduler(worker);
    Mailbox inbound(8, 8, 8, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = kGeneration, .maxTrackedStreams = 32});

    const auto stale = connection.acceptControl(
        {Control::Kind::kStreamFin, {kEpoch + 1, kGeneration, 12}, 0});
    RUVIA_CHECK(stale.status == Connection::EventStatus::kInputRejected);
    RUVIA_CHECK(stale.input.status == Connection::Input::Status::kForeignEpoch);

    const auto first = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration, firstIds[0]}, "GET", "/first");
    RUVIA_CHECK(first.status == Connection::EventStatus::kDispatched);
    const auto taskCountBeforeDuplicate = connection.activeTaskCount();
    const auto firstWireSize = requestWire(fixture.worker, "GET", "/first").size();
    const auto duplicateFin = connection.acceptControl(
        {Control::Kind::kStreamFin, {kEpoch, kGeneration, firstIds[0]}, firstWireSize});
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
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
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
    RUVIA_CHECK(headers.publication.notifyPeer);
    drainDataOnly(outbound, wires);
    const auto fin = connection.publishOne({.control = true});
    RUVIA_CHECK(fin.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK(fin.publication.status ==
                Connection::Dispatch::PublishStatus::kFinPublished);
    drainAll(outbound, wires);
    const auto response = decodeResponse(wires[0], ruvia::HttpKnownMethod::kGet,
        id.streamId, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, std::uint16_t{417});
    RUVIA_CHECK_EQ(response.messageEnds, std::size_t{1});
    RUVIA_CHECK_EQ(connection.requestInfo(id.streamId).status,
        Connection::RequestStatus::kPublished);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{1});
    const auto lateBody = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), "late");
    const auto discarded = acceptWireBytes(connection, inbound, id,
        std::span<const char>(lateBody.data(), lateBody.size()));
    RUVIA_CHECK(discarded.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.readyRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});

    const auto inputFin = connection.acceptControl({Control::Kind::kStreamFin, id,
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

ruvia::Task<void> exercisePeerLimitZeroRejection(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(fixture.services.worker());
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    constexpr std::uint64_t generation = kGeneration + 94;
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 4});
    std::array<char, 64> settingsPayload{};
    ruvia::Http3Settings settings;
    settings.maxFieldSectionSize = 0;
    const auto settingsSize = ruvia::encodeHttp3Settings(settingsPayload, settings);
    if (!settingsSize) {
        throw std::runtime_error("HTTP/3 peer setting encoding failed");
    }
    std::string controlWire(1, '\0');
    controlWire += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kSettings),
        std::string_view(settingsPayload.data(), *settingsSize));
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
        id.streamId, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, std::uint16_t{417});
    RUVIA_CHECK(!connection.peekTransportIntent().has_value());
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});
    const auto inputFin = connection.acceptControl({Control::Kind::kStreamFin, id,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(inputFin.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{0});
    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    (void)ruvia_ctx;
}

ruvia::Task<void> exerciseRejectionBackpressure(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(fixture.services.worker());
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    constexpr std::uint64_t generation = kGeneration + 92;
    const MessageId id{kEpoch, generation, 0};
    const MessageId filler{kEpoch, generation, 12};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 4});
    constexpr std::array expectField{ruvia::Http3FieldSectionFieldView{"expect", "custom-expectation"}};
    const auto wire = requestWire(fixture.worker, "GET", "/first", {}, expectField);
    constexpr std::array<std::byte, 1> fillerBytes{std::byte{'x'}};
    RUVIA_CHECK(accepted(outbound.trySend(filler, fillerBytes)));
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
    RUVIA_CHECK(accepted(outbound.trySendControl({Control::Kind::kWritable, filler, 0})));
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
        id.streamId, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, std::uint16_t{417});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});
    const auto inputFin = connection.acceptControl({Control::Kind::kStreamFin, id,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(inputFin.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
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
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
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
        id.streamId, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, expectedStatus);
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{1});
    const auto inputFin = connection.acceptControl({Control::Kind::kStreamFin, id,
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
        {.maxBufferedBodyBytes = 4, .maxLiveStreams = 4, .maxBufferedBytesInFlight = 32},
        Connection::Session::Rejection::kBodyTooLarge, 413, ruvia_ctx);
    co_await exerciseRejectedBodyStatus(fixture, kGeneration + 96,
        {.maxBufferedBodyBytes = 1024, .maxLiveStreams = 4, .maxBufferedBytesInFlight = 2},
        Connection::Session::Rejection::kInFlightBodyCapacity, 503, ruvia_ctx);
}

ruvia::Task<void> exerciseUnsupportedConnect(Fixture& fixture,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(fixture.services.worker());
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    constexpr std::uint64_t generation = kGeneration + 97;
    const MessageId id{kEpoch, generation, 0};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 4});
    auto encoded = ruvia::encodeHttp3ClientRequestHead(
        {.method = "CONNECT", .authority = "localhost:443"}, {}, fixture.worker.resource());
    if (!encoded) {
        throw std::runtime_error("HTTP/3 CONNECT request-head encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(encoded->fieldSection.data(), encoded->fieldSection.size()));
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
        id.streamId, fixture.worker.resource());
    RUVIA_CHECK_EQ(response.status, std::uint16_t{501});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{1});
    const auto inputFin = connection.acceptControl({Control::Kind::kStreamFin, id,
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
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
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
    Mailbox::BorrowedBlock output;
    Control outputControl;
    RUVIA_CHECK(!outbound.tryReceive(output));
    RUVIA_CHECK(!outbound.tryReceiveControl(outputControl));
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
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
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
    const auto reset = connection.acceptControl({.kind = Control::Kind::kStreamReset,
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
    RUVIA_CHECK(deferredRequestHead.has_value());
    if (!deferredRequestHead) {
        co_return;
    }
    const auto headWire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(deferredRequestHead->fieldSection.data(), deferredRequestHead->fieldSection.size()));
    const auto bodyWire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), "late");
    const auto pendingWire = headWire + bodyWire;
    const MessageId deferredId{kEpoch, generation, 12};
    const auto deferredHead = acceptWireBytes(connection, inbound, deferredId,
        std::span<const char>(headWire.data(), headWire.size()));
    RUVIA_CHECK(deferredHead.status == Connection::EventStatus::kRejected);
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{1});
    RUVIA_CHECK(ruvia::detail::Http3ServerConnectionResetIntentTestAccess::enqueueLocal(
        connection, deferredId.streamId));
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    RUVIA_CHECK(accepted(inbound.trySend(deferredId, std::as_bytes(
                                                         std::span<const char>(bodyWire.data(), bodyWire.size())))));
    RUVIA_CHECK(accepted(inbound.trySendControl({.kind = Control::Kind::kStreamReset,
        .id = deferredId,
        .value = pendingWire.size(),
        .streamResetErrorCode = ruvia::Http3ConnectionErrorCode::kRequestRejected})));
    Control deferredReset;
    RUVIA_CHECK(inbound.tryReceiveControl(deferredReset));
    const auto deferred = connection.acceptControl(deferredReset);
    RUVIA_CHECK(deferred.status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK(deferred.input.status == Connection::Input::Status::kDeferredReset);
    RUVIA_CHECK_EQ(connection.activeRejectionCount(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    Mailbox::BorrowedBlock finalData;
    RUVIA_CHECK(inbound.tryReceive(finalData));
    const auto cancelledByData = connection.acceptData(finalData);
    finalData.release();
    (void)inbound.drainReturns();
    (void)inbound.finishDrain();
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
    const auto lateReset = connection.acceptControl({.kind = Control::Kind::kStreamReset,
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
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
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
    const auto fin = connection.acceptControl({Control::Kind::kStreamFin, firstId,
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
    Mailbox inbound(4, 4, 4, fixture.worker.resource());
    Mailbox outbound(2, 2, 2, fixture.worker.resource());
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
    Mailbox inbound(4, 4, 4, fixture.worker.resource());
    Mailbox outbound(2, 2, 2, fixture.worker.resource());
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
    if (!accepted(inbound.trySendControl({.kind = Control::Kind::kStreamReset,
            .id = id,
            .value = wire.size()}))) {
        throw std::runtime_error("HTTP/3 test RESET mailbox is full");
    }
    Control reset;
    if (!inbound.tryReceiveControl(reset)) {
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
        {.kind = Control::Kind::kStreamReset, .id = id, .value = wire.size()});
    RUVIA_CHECK(localRetirement.status == Connection::EventStatus::kAdmissionClosed);
    RUVIA_CHECK(localRetirement.input.status == Connection::Input::Status::kStopped);
    RUVIA_CHECK(localRetirement.input.status != Connection::Input::Status::kReset);
    RUVIA_CHECK(localRetirement.connectionCloseRequired);
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK(fixture.routes.handlers.slowObservedStop);
    RUVIA_CHECK_EQ(connection.activeRequestCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    Mailbox::BorrowedBlock unexpected;
    Control unexpectedControl;
    RUVIA_CHECK(!outbound.tryReceive(unexpected));
    RUVIA_CHECK(!outbound.tryReceiveControl(unexpectedControl));
}

ruvia::Task<void> exercisePartialPublishStop(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    Mailbox inbound(4, 4, 4, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
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

ruvia::Task<void> exerciseUnknownUniFinOrder(Fixture& fixture,
    const ruvia::WorkerHandle& worker, std::uint64_t generation, bool finFirst,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    Mailbox inbound(4, 4, 4, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});

    constexpr std::uint64_t unknownStreamId = 2;
    constexpr std::array<char, 1> unknownStreamType{static_cast<char>(0x21)};
    const MessageId unknownId{kEpoch, generation, unknownStreamId};
    const auto unknownBytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(unknownStreamType.data()), unknownStreamType.size());
    if (!accepted(inbound.trySend(unknownId, unknownBytes)) ||
        !accepted(inbound.trySendControl(
            {Control::Kind::kStreamFin, unknownId, unknownStreamType.size()}))) {
        throw std::runtime_error("HTTP/3 unknown-unidirectional fixture mailbox is full");
    }

    const auto acceptUnknownData = [&]() {
        Mailbox::BorrowedBlock block;
        if (!inbound.tryReceive(block)) {
            throw std::runtime_error("HTTP/3 unknown-unidirectional data is missing");
        }
        auto result = connection.acceptData(block);
        block.release();
        (void)inbound.drainReturns();
        (void)inbound.finishDrain();
        return result;
    };
    const auto acceptUnknownFin = [&]() {
        Control fin;
        if (!inbound.tryReceiveControl(fin)) {
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
    Mailbox inbound(2, 2, 2, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});

    const MessageId id{kEpoch, generation, streamId};
    const std::array<char, 1> streamTypeBytes{streamType};
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(streamTypeBytes.data()), streamTypeBytes.size());
    if (!accepted(inbound.trySend(id, bytes))) {
        throw std::runtime_error("HTTP/3 critical-unidirectional fixture mailbox is full");
    }
    Mailbox::BorrowedBlock block;
    if (!inbound.tryReceive(block)) {
        throw std::runtime_error("HTTP/3 critical-unidirectional data is missing");
    }
    const auto fed = connection.acceptData(block);
    block.release();
    (void)inbound.drainReturns();
    (void)inbound.finishDrain();
    RUVIA_CHECK(fed.status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK(fed.input.status == Connection::Input::Status::kFed);

    const auto closed = connection.acceptControl(
        {Control::Kind::kStreamFin, id, streamTypeBytes.size()});
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
        {Control::Kind::kStreamFin, id, streamTypeBytes.size()});
    RUVIA_CHECK(persistentClose.status == Connection::EventStatus::kAdmissionClosed);
    RUVIA_CHECK(persistentClose.connectionCloseRequired);
    RUVIA_CHECK(persistentClose.input.status == Connection::Input::Status::kStopped);
    RUVIA_CHECK(connection.peekTransportIntent()->token == closeIntent->token);

    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.activeSessionStreamCount(), std::size_t{0});
}

ruvia::Task<void> exerciseSharedBodyBudget(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    ruvia::detail::Http3ServerBodyBudget budget(8);
    TestActivationSignal firstScheduler(worker);
    TestActivationSignal secondScheduler(worker);
    ruvia::WorkerSignal slowStarted(worker);
    fixture.routes.handlers.slowStarted = &slowStarted;
    Mailbox firstInbound(4, 4, 4, fixture.worker.resource());
    Mailbox firstOutbound(2, 2, 2, fixture.worker.resource());
    Mailbox secondInbound(4, 4, 4, fixture.worker.resource());
    Mailbox secondOutbound(2, 2, 2, fixture.worker.resource());
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
        {Control::Kind::kStreamReset, {kEpoch, kGeneration + 10, 0}, 0});
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

ruvia::Task<void> exerciseResetIntentMergePolicy(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
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
    RUVIA_CHECK_EQ(protocol->token.id.connectionGeneration,
        local->token.id.connectionGeneration);
    RUVIA_CHECK_EQ(protocol->token.id.streamId, local->token.id.streamId);
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
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    constexpr auto generation = kGeneration + 90;
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});
    const MessageId id{kEpoch, generation, 0};
    const MessageId queuedControlId{kEpoch, generation, 12};
    RUVIA_CHECK(accepted(outbound.trySendControl(
        {Control::Kind::kWritable, queuedControlId, 0})));

    const std::array<ruvia::Http3FieldSectionFieldView, 4> invalidFields{{{":method", "GET"}, {":scheme", "https"}, {"x-before-path", "bad"}, {":path", "/"}}};
    const auto section = ruvia::encodeHttp3FieldSection(invalidFields, fixture.worker.resource());
    if (!section) {
        throw std::runtime_error("HTTP/3 malformed-field fixture encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(section->data(), section->size()));
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
    RUVIA_CHECK_EQ(intent->token.id.streamId, std::uint64_t{0});
    RUVIA_CHECK(intent->streamResetErrorCode == ruvia::Http3ConnectionErrorCode::kMessageError);

    // A late duplicate cannot change the persistent intent or its token.
    const auto duplicate = acceptWireBytes(connection, inbound, id, wire);
    RUVIA_CHECK(duplicate.input.status == Connection::Input::Status::kClosedStream);
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    RUVIA_CHECK(connection.peekTransportIntent()->token == intent->token);

    const Control resetControl{.kind = Control::Kind::kStreamReset,
        .id = intent->token.id,
        .streamResetErrorCode = intent->streamResetErrorCode};
    RUVIA_CHECK(outbound.trySendControl(resetControl) == Mailbox::ControlResult::kFull);
    RUVIA_CHECK(connection.peekTransportIntent()->token == intent->token);
    Control queuedControl;
    RUVIA_CHECK(outbound.tryReceiveControl(queuedControl));
    RUVIA_CHECK(queuedControl.kind == Control::Kind::kWritable);
    RUVIA_CHECK(!outbound.finishDrain());
    const auto sent = outbound.trySendControl(resetControl);
    RUVIA_CHECK(sent == Mailbox::ControlResult::kSentNotifyPeer);
    RUVIA_CHECK_EQ(resetControl.value, std::uint64_t{0});
    RUVIA_CHECK(connection.ackTransportIntent(intent->token));
    RUVIA_CHECK(!connection.ackTransportIntent(intent->token));
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    RUVIA_CHECK(!connection.transportRetired());
    Control receivedReset;
    RUVIA_CHECK(outbound.tryReceiveControl(receivedReset));
    RUVIA_CHECK(receivedReset.kind == Control::Kind::kStreamReset);
    RUVIA_CHECK_EQ(receivedReset.value, std::uint64_t{0});
    RUVIA_CHECK(receivedReset.streamResetErrorCode == ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK(!outbound.finishDrain());
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
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
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

ruvia::Task<void> exerciseHeaderLimitConnectionClose(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    constexpr auto generation = kGeneration + 92;
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .maxTrackedStreams = 8});
    const std::string largeValue(70 * 1024, 'h');
    const std::array<ruvia::Http3FieldSectionFieldView, 4> fields{{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"x-large", largeValue}}};
    const auto section = ruvia::encodeHttp3FieldSection(fields, fixture.worker.resource());
    if (!section) {
        throw std::runtime_error("HTTP/3 oversized-field fixture encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(section->data(), section->size()));
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
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox capacityOutbound(1, 1, 1, fixture.worker.resource());
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
    Mailbox finalSizeOutbound(1, 1, 1, fixture.worker.resource());
    constexpr auto finalSizeGeneration = kGeneration + 94;
    Connection finalSize(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, finalSizeOutbound, finalSizeScheduler,
        {.epoch = kEpoch, .connectionGeneration = finalSizeGeneration, .maxTrackedStreams = 8});
    const auto fed = acceptWireBytes(finalSize, inbound,
        {kEpoch, finalSizeGeneration, 0}, partialData);
    RUVIA_CHECK(fed.input.status == Connection::Input::Status::kFed);
    const auto badFin = finalSize.acceptControl({Control::Kind::kStreamFin,
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

ruvia::Task<Connection::TransportIntentToken> createPeerLimitIntent(
    Connection& connection, Mailbox& inbound, ruvia::WorkerMemory& worker,
    std::uint64_t epoch, std::uint64_t generation, const ruvia::WorkerHandle& workerHandle,
    const ruvia::StopToken& stopToken, ruvia::testing::TestContext& ruvia_ctx) {
    std::array<char, 64> settingsPayload{};
    ruvia::Http3Settings settings;
    settings.maxFieldSectionSize = 0;
    const auto settingsSize = ruvia::encodeHttp3Settings(settingsPayload, settings);
    if (!settingsSize) {
        throw std::runtime_error("HTTP/3 peer-settings fixture encoding failed");
    }
    std::string controlWire(1, '\0');
    controlWire += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kSettings),
        std::string_view(settingsPayload.data(), *settingsSize));
    const MessageId controlId{epoch, generation, 2};
    const auto controlBytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(controlWire.data()), controlWire.size());
    if (!accepted(inbound.trySend(controlId, controlBytes))) {
        throw std::runtime_error("HTTP/3 peer-settings fixture mailbox is full");
    }
    Mailbox::BorrowedBlock controlBlock;
    if (!inbound.tryReceive(controlBlock)) {
        throw std::runtime_error("HTTP/3 peer-settings data is missing");
    }
    const auto settingsResult = connection.acceptData(controlBlock);
    controlBlock.release();
    (void)inbound.drainReturns();
    (void)inbound.finishDrain();
    RUVIA_CHECK(settingsResult.status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK(settingsResult.input.status == Connection::Input::Status::kFed);

    const auto request = routeRequest(connection, inbound, worker,
        {epoch, generation, 0}, "GET", "/first");
    RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    co_await waitForReady(connection, 1, workerHandle, stopToken);
    const auto attempt = connection.publishOne(kAllWorkLanes);
    RUVIA_CHECK(attempt.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK(attempt.publication.status ==
                Connection::Dispatch::PublishStatus::kPeerLimitRejected);
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

ruvia::Task<void> exercisePeerLimitRejection(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::test::CountingMemoryResource& upstream,
    ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    Mailbox inbound(4, 4, 4, fixture.worker.resource());
    Mailbox outbound(2, 2, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 40, .maxTrackedStreams = 8});

    const MessageId queuedControlId{kEpoch, kGeneration + 40, 12};
    RUVIA_CHECK(accepted(outbound.trySendControl(
        {Control::Kind::kWritable, queuedControlId, 0})));

    std::array<char, 64> settingsPayload{};
    ruvia::Http3Settings settings;
    settings.maxFieldSectionSize = 0;
    const auto settingsSize = ruvia::encodeHttp3Settings(settingsPayload, settings);
    if (!settingsSize) {
        throw std::runtime_error("HTTP/3 peer-settings fixture encoding failed");
    }
    std::string controlWire(1, '\0');
    controlWire += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kSettings),
        std::string_view(settingsPayload.data(), *settingsSize));
    const MessageId controlId{kEpoch, kGeneration + 40, 2};
    const auto controlBytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(controlWire.data()), controlWire.size());
    if (!accepted(inbound.trySend(controlId, controlBytes))) {
        throw std::runtime_error("HTTP/3 peer-settings fixture mailbox is full");
    }
    Mailbox::BorrowedBlock controlBlock;
    if (!inbound.tryReceive(controlBlock)) {
        throw std::runtime_error("HTTP/3 peer-settings data is missing");
    }
    const auto settingsResult = connection.acceptData(controlBlock);
    controlBlock.release();
    (void)inbound.drainReturns();
    (void)inbound.finishDrain();
    RUVIA_CHECK(settingsResult.status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK(settingsResult.input.status == Connection::Input::Status::kFed);

    const auto request = routeRequest(connection, inbound, fixture.worker,
        {kEpoch, kGeneration + 40, 0}, "GET", "/first");
    RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    co_await waitForReady(connection, 1, worker, fixture.workerStop);
    RUVIA_CHECK_EQ(fixture.routes.handlers.handlerCalls, std::size_t{1});
    const auto allocationCountBeforeIntent = upstream.allocationCount();
    const auto attempt = connection.publishOne(kAllWorkLanes);
    RUVIA_CHECK(attempt.status == Connection::PublishStatus::kAttempted);
    RUVIA_CHECK(attempt.publication.status ==
                Connection::Dispatch::PublishStatus::kPeerLimitRejected);
    RUVIA_CHECK(attempt.publication.blockReason ==
                Connection::Dispatch::PublishBlockReason::kNone);
    RUVIA_CHECK(!attempt.publication.notifyPeer);
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
    RUVIA_CHECK_EQ(resetIntent->token.id.streamId, std::uint64_t{0});
    RUVIA_CHECK_EQ(resetIntent->token.id.epoch, kEpoch);
    RUVIA_CHECK_EQ(resetIntent->token.id.connectionGeneration, kGeneration + 40);
    RUVIA_CHECK(resetIntent->streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kRequestCancelled);

    const Control resetControl{.kind = Control::Kind::kStreamReset,
        .id = resetIntent->token.id,
        .streamResetErrorCode = resetIntent->streamResetErrorCode};
    RUVIA_CHECK(outbound.trySendControl(resetControl) == Mailbox::ControlResult::kFull);
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{1});
    RUVIA_CHECK(connection.peekTransportIntent()->token == resetIntent->token);

    Control queuedControl;
    RUVIA_CHECK(outbound.tryReceiveControl(queuedControl));
    RUVIA_CHECK(queuedControl.kind == Control::Kind::kWritable);
    RUVIA_CHECK(!outbound.tryReceiveControl(queuedControl));
    RUVIA_CHECK(!outbound.finishDrain());
    (void)outbound.drainReturns();

    const auto resetSend = outbound.trySendControl(resetControl);
    RUVIA_CHECK(accepted(resetSend));
    RUVIA_CHECK(resetSend == Mailbox::ControlResult::kSentNotifyPeer);
    if (resetSend == Mailbox::ControlResult::kSentNotifyPeer) {
        scheduler.notify();
    }
    RUVIA_CHECK(resetControl.value == 0);
    const auto allocationCountBeforeAck = upstream.allocationCount();
    RUVIA_CHECK(connection.ackTransportIntent(resetIntent->token));
    RUVIA_CHECK(!connection.ackTransportIntent(resetIntent->token));
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});
    RUVIA_CHECK(!connection.peekTransportIntent().has_value());
    RUVIA_CHECK_EQ(upstream.allocationCount(), allocationCountBeforeAck);

    Control sentReset;
    RUVIA_CHECK(outbound.tryReceiveControl(sentReset));
    RUVIA_CHECK(sentReset.kind == Control::Kind::kStreamReset);
    RUVIA_CHECK(sentReset.value == 0);
    RUVIA_CHECK(!outbound.tryReceiveControl(sentReset));
    RUVIA_CHECK(!outbound.finishDrain());

    RUVIA_CHECK(connection.requestStop());
    co_await connection.join();
    simulateGlobalStopTakeover(connection);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
}

ruvia::Task<void> exerciseStaleIntentTokens(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    constexpr std::uint64_t oldGeneration = kGeneration + 50;
    TestActivationSignal oldScheduler(worker);
    Mailbox oldInbound(4, 4, 4, fixture.worker.resource());
    Mailbox oldOutbound(2, 2, 2, fixture.worker.resource());
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
        Mailbox inbound(4, 4, 4, fixture.worker.resource());
        Mailbox outbound(2, 2, 2, fixture.worker.resource());
        Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.services, fixture.options, outbound, scheduler,
            {.epoch = epoch, .connectionGeneration = generation, .maxTrackedStreams = 8});
        const auto currentToken = co_await createPeerLimitIntent(connection, inbound,
            fixture.worker, epoch, generation, worker, fixture.workerStop, ruvia_ctx);

        RUVIA_CHECK_EQ(currentToken.id.streamId, oldToken.id.streamId);
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
    Mailbox inbound(4, 4, 4, fixture.worker.resource());
    Mailbox outbound(2, 2, 1, fixture.worker.resource());
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = kGeneration + 60, .maxTrackedStreams = 8});
    const MessageId queuedControlId{kEpoch, kGeneration + 60, 12};
    RUVIA_CHECK(accepted(outbound.trySendControl(
        {Control::Kind::kWritable, queuedControlId, 0})));

    const auto resetToken = co_await createPeerLimitIntent(connection, inbound,
        fixture.worker, kEpoch, kGeneration + 60, worker, fixture.workerStop, ruvia_ctx);
    RUVIA_CHECK(outbound.trySendControl(
                    {Control::Kind::kStreamReset, resetToken.id, 0}) == Mailbox::ControlResult::kFull);
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
        Mailbox outbound(1, 1, 1, fixture.worker.resource());
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
        RUVIA_CHECK_EQ(reset->token.id.streamId, std::uint64_t{0});
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
    using Scheduler = ruvia::detail::Http3WorkerMailboxScheduler;
    using TestAccess = ruvia::detail::Http3ServerConnectionResetIntentTestAccess;
    Scheduler scheduler(worker, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    const auto registration = scheduler.reserve(kEpoch + 180, kGeneration + 180);
    RUVIA_CHECK(registration.has_value());
    if (!registration) {
        throw std::runtime_error("scheduler reset-ACK fixture slot unavailable");
    }
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, registration->activation,
        {.epoch = kEpoch + 180,
            .connectionGeneration = kGeneration + 180,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(scheduler.attach(registration->token, connection));
    RUVIA_CHECK(TestAccess::enqueueLocal(connection, 0));
    const auto offered = scheduler.step();
    RUVIA_CHECK(offered.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(offered.intent.token.kind == Connection::TransportIntentKind::kStreamReset);
    RUVIA_CHECK(TestAccess::enqueueProtocol(connection, 0,
        ruvia::Http3ConnectionErrorCode::kMessageError));
    const auto current = connection.peekTransportIntent();
    RUVIA_CHECK(current.has_value());
    RUVIA_CHECK(current->token.sequence != offered.intent.token.sequence);
    RUVIA_CHECK(scheduler.step().kind == Scheduler::StepKind::kIdle);
    RUVIA_CHECK(scheduler.acknowledgeIntent(registration->token, offered.intent.token));
    const auto replacement = scheduler.step();
    RUVIA_CHECK(replacement.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(replacement.intent.token == current->token);
    RUVIA_CHECK(scheduler.acknowledgeIntent(registration->token, replacement.intent.token));
    RUVIA_CHECK_EQ(connection.pendingTransportIntentCount(), std::size_t{0});

    RUVIA_CHECK(connection.requestStop());
    const auto close = scheduler.step();
    RUVIA_CHECK(close.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(close.intent.token.kind == Connection::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(scheduler.beginRetirement(registration->token));
    co_await connection.join();
    RUVIA_CHECK(connection.confirmTransportRetired(
        {.epoch = registration->token.epoch,
            .connectionGeneration = registration->token.connectionGeneration}));
    RUVIA_CHECK(scheduler.acknowledgeIntent(registration->token, close.intent.token));
    RUVIA_CHECK(scheduler.retire(registration->token));
    RUVIA_CHECK(outbound.stop());
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

RUVIA_TEST(http3NetworkTunnelBodyTimeoutWaitsForAcceptedHandshakeBarrier) {
    using Access = ruvia::detail::Http3NetworkRuntimeTestAccess;
    using Identity = ruvia::detail::Http3ServerConnectionChannel::Identity;
    using Control = ruvia::detail::Http3StreamControl;
    std::pmr::monotonic_buffer_resource resource;
    const Identity identity{
        .epoch = 7, .connectionGeneration = 11, .slot = 0, .slotGeneration = 3};
    auto makeControl = [](std::uint64_t streamId) {
        return Control{
            .kind = Control::Kind::kTunnelEstablished,
            .id = {.epoch = 7, .connectionGeneration = 11, .streamId = streamId},
            .value = 100};
    };

    auto finFirst = Access::makeConnection(&resource, identity);
    auto& finStream = Access::addRequestStream(finFirst, 4);
    auto& finSibling = Access::addRequestStream(finFirst, 8);
    Access::notePeerFin(finFirst, 4);
    Access::completeInputTerminal(finFirst, 4);
    RUVIA_CHECK(finStream.inputTerminal);
    RUVIA_CHECK(!finStream.bodyTimeoutApplies());
    RUVIA_CHECK(finSibling.bodyTimeoutApplies());
    RUVIA_CHECK(Access::acceptTunnelEstablished(finFirst, makeControl(4), 99) ==
                Access::TunnelResult::kAccepted);
    RUVIA_CHECK_EQ(finFirst.pendingTunnelHandshakes, std::size_t{1});
    RUVIA_CHECK(!finStream.bodyTimeoutApplies());
    RUVIA_CHECK(finSibling.bodyTimeoutApplies());
    RUVIA_CHECK(!Access::confirmTunnelEstablished(finFirst, 4, 99));
    RUVIA_CHECK(Access::confirmTunnelEstablished(finFirst, 4, 100));
    RUVIA_CHECK_EQ(finFirst.pendingTunnelHandshakes, std::size_t{0});
    RUVIA_CHECK(Access::acceptTunnelEstablished(finFirst, makeControl(4), 100) ==
                Access::TunnelResult::kProtocolFailure);
    auto wrongIdentity = makeControl(8);
    ++wrongIdentity.id.epoch;
    RUVIA_CHECK(Access::acceptTunnelEstablished(finFirst, wrongIdentity, 100) ==
                Access::TunnelResult::kProtocolFailure);
    RUVIA_CHECK(finSibling.bodyTimeoutApplies());

    auto markerFirst = Access::makeConnection(&resource, identity);
    auto& markerStream = Access::addRequestStream(markerFirst, 12);
    auto& markerSibling = Access::addRequestStream(markerFirst, 16);
    RUVIA_CHECK(Access::acceptTunnelEstablished(markerFirst, makeControl(12), 99) ==
                Access::TunnelResult::kAccepted);
    RUVIA_CHECK_EQ(markerFirst.pendingTunnelHandshakes, std::size_t{1});
    Access::notePeerFin(markerFirst, 12);
    Access::completeInputTerminal(markerFirst, 12);
    RUVIA_CHECK(markerStream.inputTerminal);
    RUVIA_CHECK(!markerStream.bodyTimeoutApplies());
    RUVIA_CHECK(markerSibling.bodyTimeoutApplies());
    RUVIA_CHECK(Access::confirmTunnelEstablished(markerFirst, 12, 100));
    RUVIA_CHECK_EQ(markerFirst.pendingTunnelHandshakes, std::size_t{0});

    ruvia::test::CountingMemoryResource resetResource;
    {
        auto resetConnection = Access::makeConnection(&resetResource, identity);
        auto& resetStream = Access::addRequestStream(resetConnection, 20);
        auto& resetSibling = Access::addRequestStream(resetConnection, 24);
        Access::installFrameTracker(resetStream, &resetResource);
        RUVIA_CHECK(Access::acceptTunnelEstablished(resetConnection, makeControl(20), 99) ==
                    Access::TunnelResult::kAccepted);
        RUVIA_CHECK_EQ(resetConnection.pendingTunnelHandshakes, std::size_t{1});
        const auto deallocationsBeforeReset = resetResource.deallocationCount();
        const auto liveAllocationsBeforeReset = resetResource.liveAllocations();
        Access::noteInputReset(resetConnection, 20);
        Access::completeInputTerminal(resetConnection, 20);
        RUVIA_CHECK(resetStream.inputTerminal);
        RUVIA_CHECK(resetStream.inputReset);
        RUVIA_CHECK(!resetStream.frameTracker);
        // The tracker can own more than one PMR allocation on MSVC; verify
        // actual release rather than assuming its object is the only block.
        RUVIA_CHECK(resetResource.deallocationCount() > deallocationsBeforeReset);
        RUVIA_CHECK(resetResource.liveAllocations() < liveAllocationsBeforeReset);
        RUVIA_CHECK(!resetStream.bodyTimeoutApplies());
        RUVIA_CHECK_EQ(resetConnection.pendingTunnelHandshakes, std::size_t{0});
        RUVIA_CHECK(Access::acceptTunnelEstablished(
                        resetConnection, makeControl(20), 100) ==
                    Access::TunnelResult::kIgnoredTerminal);
        RUVIA_CHECK_EQ(resetConnection.pendingTunnelHandshakes, std::size_t{0});
        RUVIA_CHECK(!resetConnection.closeStarted);
        RUVIA_CHECK(Access::acceptTunnelEstablished(
                        resetConnection, makeControl(20), 100) ==
                    Access::TunnelResult::kIgnoredTerminal);
        RUVIA_CHECK_EQ(resetConnection.pendingTunnelHandshakes, std::size_t{0});
        RUVIA_CHECK(!resetConnection.closeStarted);
        auto staleIdentity = makeControl(20);
        ++staleIdentity.id.connectionGeneration;
        RUVIA_CHECK(Access::acceptTunnelEstablished(
                        resetConnection, staleIdentity, 100) ==
                    Access::TunnelResult::kProtocolFailure);
        RUVIA_CHECK(resetSibling.bodyTimeoutApplies());
        RUVIA_CHECK_EQ(resetConnection.identity.connectionGeneration,
            identity.connectionGeneration);
    }
    RUVIA_CHECK_EQ(resetResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resetResource.allocationCount(), resetResource.deallocationCount());
}

RUVIA_TEST(http3_server_connection_starts_join_while_handler_is_active) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exercise_join_while_active(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionWaitsForTunnelPeerFinAfterLocalOutputFin) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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

RUVIA_TEST(http3ServerConnectionRoutesAndFairlyPublishesBufferedRequests) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseEarlyRejectionBeforeRequestFin(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionPublishesMinimalRejectionWithPeerFieldLimitZero) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exercisePeerLimitZeroRejection(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionParksRejectionOnExactMailboxLane) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseRejectionBackpressure(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionRetiresRejectedResetAndUnfinishedStop) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseRejectionIndexCapacity(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionKeepsDataAndControlBackpressureInSeparateLanes) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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

RUVIA_TEST(http3ServerConnectionMergesPendingResetCodesWithFreshTokens) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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

RUVIA_TEST(http3ServerConnectionKeepsHeaderLimitAsConnectionError) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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

RUVIA_TEST(http3ServerConnectionResetAfterFinCancelsAndJoinsHandler) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exercisePartialPublishStop(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionIgnoresUnknownClientUniStreamsInEitherFinOrder) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseCriticalUniFins(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionSharesBodyBudgetUntilStoppedLeaseJoins) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseSharedBodyBudget(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerConnectionForwardsPeerLimitPublishRejection) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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

RUVIA_TEST(http3ServerConnectionRejectsStaleIntentTokensAndPrioritizesClose) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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

namespace {
ruvia::Task<void> exerciseDynamicQpackPublication(Fixture& fixture, const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    constexpr auto generation = kGeneration + 103;
    const MessageId requestId{kEpoch, generation, 0};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker, fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .session = {.connection = {.qpackMaxTableCapacity = 256, .qpackBlockedStreams = 2, .enableConnectProtocol = true}}, .maxTrackedStreams = 8});
    const auto prefixes = ruvia::Http3LocalCriticalStreams::create({.qpackMaxTableCapacity = 256, .qpackBlockedStreams = 2});
    RUVIA_CHECK(prefixes.has_value());
    const auto settings = prefixes->controlPrefix();
    RUVIA_CHECK(acceptWireBytes(connection, inbound, {kEpoch, generation, 2}, settings).status == Connection::EventStatus::kAccepted);
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 256, .maxBlockedStreams = 2}, fixture.worker.resource());
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{":method", "POST"}, ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"}, ruvia::Http3FieldSectionFieldView{":path", "/body"},
        ruvia::Http3FieldSectionFieldView{"content-length", "7"}, ruvia::Http3FieldSectionFieldView{"x-custom", "value"}};
    const auto section = encoder.encode(0, fields);
    RUVIA_CHECK(section.has_value());
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders), std::string_view(section->data(), section->size())) +
                      frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), "payload");
    const auto head = acceptWireBytes(connection, inbound, requestId, std::span(wire.data(), wire.size()));
    RUVIA_CHECK(head.input.status == Connection::Input::Status::kDeferredQpack);
    const auto fin = connection.acceptControl({Control::Kind::kStreamFin, requestId, wire.size()});
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
        Mailbox::BorrowedBlock block;
        if (outbound.tryReceive(block)) {
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
            (void)outbound.drainReturns();
        }
        Control control;
        while (outbound.tryReceiveControl(control)) {
            if (control.kind == Control::Kind::kStreamFin) {
                responseFin = true;
                RUVIA_CHECK_EQ(control.value, responseWire.size());
            }
        }
        (void)outbound.finishDrain();
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
}  // namespace

namespace {
ruvia::Task<void> exerciseOriginPublication(Fixture& fixture, const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
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
        Mailbox::BorrowedBlock block;
        if (outbound.tryReceive(block)) {
            if (const auto* critical = block.critical()) {
                RUVIA_CHECK(critical->epoch == kEpoch && critical->connectionGeneration == generation);
                RUVIA_CHECK(critical->kind == ruvia::http3_critical_stream_output::stream_kind::control);
                controlWire.append(reinterpret_cast<const char*>(block.bytes().data()), block.bytes().size());
            } else {
                responseWire.append(reinterpret_cast<const char*>(block.bytes().data()), block.bytes().size());
            }
            const auto blocked = connection.publishOne(kAllWorkLanes);
            sawBlocked |= blocked.publication.status == Connection::Dispatch::PublishStatus::kBackpressured;
            block.release();
            (void)outbound.drainReturns();
        }
        Control control;
        while (outbound.tryReceiveControl(control)) {
            if (control.kind == Control::Kind::kStreamFin) {
                responseFin = true;
                RUVIA_CHECK_EQ(control.value, responseWire.size());
            }
        }
        (void)outbound.finishDrain();
        (void)connection.reactivateBlocked({.data = true, .control = true});
    }
    RUVIA_CHECK(sawBlocked && responseFin && controlWire.size() > Mailbox::kMaxBlockBytes);
    ruvia::Http3Connection peer(ruvia::Http3PeerRole::kClient, fixture.worker.resource(), {.receiveOriginAdvertisements = true});
    const auto prefixes = ruvia::Http3LocalCriticalStreams::create({});
    const auto prefix = prefixes->controlPrefix();
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

RUVIA_TEST(http3_server_advertises_origins_on_control_stream_under_mailbox_backpressure) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exerciseDynamicQpackPublication(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
}

namespace {
ruvia::Task<void> exerciseContinueBeforeBody(Fixture& fixture, const ruvia::WorkerHandle& worker, unsigned mode, ruvia::testing::TestContext& ruvia_ctx) {
    TestActivationSignal scheduler(worker);
    Mailbox inbound(2, 2, 2, fixture.worker.resource());
    Mailbox outbound(1, 1, 1, fixture.worker.resource());
    const auto generation = kGeneration + 120 + mode;
    const MessageId id{kEpoch, generation, 0};
    Connection connection(fixture.routes.implementation.routeTable(), fixture.worker, fixture.services, fixture.options, outbound, scheduler,
        {.epoch = kEpoch, .connectionGeneration = generation, .session = {.maxBufferedBodyBytes = mode == 2 ? 3u : 16u}, .maxTrackedStreams = 8});
    const std::array fields{ruvia::Http3FieldSectionFieldView{"expect", "100-continue"}};
    const auto head = ruvia::encodeHttp3ClientRequestHead({.method = "POST", .scheme = "https", .authority = "example.test", .path = "/body", .fields = fields, .bodyLength = mode == 2 ? std::nullopt : std::optional<std::uint64_t>{mode == 3 ? 0u : 7u}}, {}, fixture.worker.resource());
    RUVIA_CHECK(head.has_value());
    const auto wire = frame(1, {head->fieldSection.data(), head->fieldSection.size()});
    const auto received = acceptWireBytes(connection, inbound, id, std::span(wire.data(), wire.size()));
    RUVIA_CHECK(received.status == Connection::EventStatus::kAccepted);
    RUVIA_CHECK_EQ(connection.activeTaskCount(), std::size_t{0});
    std::array<PublishedWire, 6> wires{};
    if (mode == 1) {
        RUVIA_CHECK(connection.workState().runnable.data);
        RUVIA_CHECK(connection.acceptControl({Control::Kind::kStreamReset, id, wire.size()}).status == Connection::EventStatus::kAccepted);
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
        const auto finished = connection.acceptControl({Control::Kind::kStreamFin, id, wire.size() + body.size()});
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
        auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
        Mailbox inbound(2, 2, 2, fixture.worker.resource());
        Mailbox outbound(1, 1, 1, fixture.worker.resource());
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
                        RUVIA_CHECK_EQ(intent->token.id.streamId, std::uint64_t{0});
                        RUVIA_CHECK_EQ(intent->token.id.pushId, std::optional<std::uint64_t>{0});
                        if (mode == 3) {
                            RUVIA_CHECK(connection.acceptControl({.kind = Control::Kind::kStreamReset,
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
                        forged.id.pushId = 1;
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
                    RUVIA_CHECK(encoded.has_value());
                    RUVIA_CHECK(acceptWireBytes(connection, inbound, {kEpoch, kGeneration, 2}, std::span(priority).first(*encoded)).status == Connection::EventStatus::kAccepted);
                    constexpr std::array<char, 3> cancel{3, 1, 0};
                    RUVIA_CHECK(acceptWireBytes(connection, inbound, {kEpoch, kGeneration, 2}, cancel).status == Connection::EventStatus::kAccepted);
                    cancelledActivePush = true;
                } else if (!cancelledActivePush && mode == 10 && connection.requestInfo(31).status == Connection::RequestStatus::kRunning) {
                    RUVIA_CHECK(connection.acceptControl({.kind = Control::Kind::kStreamReset,
                                                             .id = {kEpoch, kGeneration, 31, 0}})
                                    .status == Connection::EventStatus::kStreamCancelled);
                    cancelledActivePush = true;
                }
                for (std::size_t turn = 0; turn != 8; ++turn) {
                    (void)connection.publishOne(kAllWorkLanes);
                    Control control;
                    while (outbound.tryReceiveControl(control)) {
                        if (control.kind == Control::Kind::kStreamFin) {
                            wires[control.id.streamId].finalWireBytes = control.value;
                        }
                    }
                    Mailbox::BorrowedBlock block;
                    while (outbound.tryReceive(block)) {
                        if (!block.critical()) {
                            RUVIA_CHECK_EQ(block.id().pushId, block.id().streamId == 31 ? std::optional<std::uint64_t>{0} : std::nullopt);
                            auto bytes = block.bytes();
                            wires[block.id().streamId].bytes.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                        }
                        block.release();
                    }
                    (void)outbound.drainReturns();
                    (void)outbound.finishDrain();
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment, exercisePushPublication(fixture, worker, upstream, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
