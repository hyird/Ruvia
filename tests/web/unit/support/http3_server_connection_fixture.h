#pragma once

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

#include "http3/Http3ServerConnection.h"
#include "http3/http3_connection_driver.h"
#include "http3/http3_connection_state.h"
#include "integration/WorkerCapabilities.h"
#include "memory_resource_fixture.h"
#include "router/Router.h"
#include "router/RouterImpl.h"
#include "routing_fixture.h"
#include "server/HttpServerOptions.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace ruvia::detail {

struct http3_connection_driver_test_access final {
    using driver = http3_connection_driver;
    using stream = driver::stream_state;
    using connection = driver;
    using tunnel_result = driver::tunnel_established_result;

    static connection make_connection(std::pmr::memory_resource* resource,
        http3_connection_identity identity) {
        connection value(resource);
        value.identity_ = identity;
        value.streams_.reserve(8);
        return value;
    }

    static stream& add_request_stream(connection& value, std::uint64_t id) {
        value.streams_.emplace_back(value.streams_.get_allocator().resource());
        auto& result = value.streams_.back();
        result.id = id;
        result.request_stream = true;
        result.input_phase = stream::receive_phase::body;
        return result;
    }

    static tunnel_result accept_tunnel_established(connection& value,
        const http3_stream_control& control, std::uint64_t accepted_wire_bytes) noexcept {
        return value.accept_tunnel_established(control, accepted_wire_bytes);
    }

    static bool confirm_tunnel_established(connection& value,
        std::uint64_t stream_id, std::uint64_t accepted_wire_bytes) noexcept {
        return value.confirm_tunnel_established(stream_id, accepted_wire_bytes);
    }

    static void note_peer_fin(connection& value, std::uint64_t stream_id) noexcept {
        value.note_peer_fin(stream_id);
    }

    static void note_input_reset(connection& value, std::uint64_t stream_id) noexcept {
        value.note_input_reset(stream_id);
    }

    static void complete_input_terminal(connection& value, std::uint64_t stream_id) noexcept {
        value.complete_input_terminal(stream_id);
    }

    static void install_frame_tracker(stream& value, std::pmr::memory_resource* resource) {
        value.frame_tracker = makePmrObject<Http3StreamFrames>(resource,
            Http3StreamKind::kRequest, resource);
    }

    static std::size_t pending_handshakes(const connection& value) noexcept {
        return value.pending_tunnel_handshakes_;
    }

    static bool closing(const connection& value) noexcept {
        return value.close_started_;
    }

    static http3_connection_identity identity(const connection& value) noexcept {
        return value.identity_;
    }

    static Http3DatagramReceiveStatus plan_received_datagram(connection& value,
        const Http3DatagramView& datagram) noexcept {
        value.config_.local_settings.h3Datagram = true;
        return value.plan_datagram_receive(datagram);
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
        auto* slot = owner.requests_.find_or_create(streamId, created);
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
using Control = ruvia::detail::http3_stream_control;
using buffer = ruvia::detail::http3_stream_buffer;
using MessageId = ruvia::detail::http3_stream_id;
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

inline ruvia::Task<void> webSocketHandler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<HandlerState*>(raw);
    state.webSocketStartedObserved = true;
    state.webSocketStarted->notify();
    co_await context.webSocket().close();
    state.webSocketHandlerFinished = true;
}

inline ruvia::Task<ruvia::HttpResponse> requestHandler(void* raw, ruvia::Context& context) {
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

inline std::string frame(std::uint64_t type, std::string_view payload) {
    std::array<char, ruvia::kHttp3FrameHeaderMaxBytes> header{};
    const auto size = ruvia::encodeHttp3FrameHeader(header, type, payload.size());
    if (!size) {
        throw std::runtime_error("HTTP/3 test frame encoding failed");
    }
    std::string wire(header.data(), *size);
    wire.append(payload);
    return wire;
}

inline std::string requestWire(ruvia::WorkerMemory& worker, std::string_view method,
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

inline bool accepted(buffer::send_result result) noexcept {
    return result == buffer::send_result::sent;
}

inline bool accepted(buffer::control_result result) noexcept {
    return result == buffer::control_result::sent;
}

inline Connection::EventResult routeRequest(Connection& connection, buffer& inbound,
    ruvia::WorkerMemory& worker, MessageId id, std::string_view method,
    std::string_view path, std::string_view body = {},
    std::span<const ruvia::Http3FieldSectionFieldView> fields = {}) {
    const auto wire = requestWire(worker, method, path, body, fields);
    if (wire.size() > buffer::max_block_bytes) {
        throw std::runtime_error("HTTP/3 test request exceeded one buffer block");
    }
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!accepted(inbound.try_send(id, bytes)) ||
        !accepted(inbound.try_send_control(
            {Control::kind::stream_fin, id, static_cast<std::uint64_t>(wire.size())}))) {
        throw std::runtime_error("HTTP/3 test inbound buffer is full");
    }

    Control fin;
    if (!inbound.try_receive_control(fin)) {
        throw std::runtime_error("HTTP/3 test FIN control is missing");
    }
    const auto deferred = connection.acceptControl(fin);
    if (deferred.input.status != Connection::Input::Status::kDeferredFin) {
        throw std::runtime_error("HTTP/3 test FIN was not deferred behind its data");
    }

    buffer::borrowed_block block;
    if (!inbound.try_receive(block) || block.id().stream_id != id.stream_id) {
        throw std::runtime_error("HTTP/3 test data block is missing");
    }
    auto result = connection.acceptData(block);
    block.release();
    return result;
}

inline Connection::EventResult acceptWireBytes(Connection& connection, buffer& inbound,
    MessageId id, std::span<const char> wire);

inline std::string web_socket_request_wire(ruvia::WorkerMemory& worker, std::string_view version) {
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/socket"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", version}};
    const auto encoded = ruvia::encodeHttp3FieldSection(fields, worker.resource());
    if (!encoded) {
        throw std::runtime_error("HTTP/3 WebSocket field section encoding failed");
    }
    return frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(encoded->data(), encoded->size()));
}

inline Connection::EventResult acceptWireBytes(Connection& connection, buffer& inbound,
    MessageId id, std::span<const char> wire) {
    Connection::EventResult result;
    for (std::size_t offset = 0; offset < wire.size();) {
        const auto size = std::min(buffer::max_block_bytes, wire.size() - offset);
        const auto chunk = wire.subspan(offset, size);
        if (!accepted(inbound.try_send(id, std::as_bytes(chunk)))) {
            throw std::runtime_error("HTTP/3 raw-wire fixture buffer is full");
        }
        buffer::borrowed_block block;
        if (!inbound.try_receive(block)) {
            throw std::runtime_error("HTTP/3 raw-wire fixture block is missing");
        }
        result = connection.acceptData(block);
        block.release();
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

inline std::size_t wireIndex(std::uint64_t streamId) {
    if ((streamId & 3U) != 0 || streamId / 4 >= 6) {
        throw std::runtime_error("unexpected HTTP/3 test stream ID");
    }
    return static_cast<std::size_t>(streamId / 4);
}

inline void drainDataOnly(buffer& outbound, std::array<PublishedWire, 6>& wires) {
    buffer::borrowed_block block;
    while (outbound.try_receive(block)) {
        auto& wire = wires[wireIndex(block.id().stream_id)];
        const auto bytes = block.bytes();
        wire.bytes.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        block.release();
    }
}

inline void drainAll(buffer& outbound, std::array<PublishedWire, 6>& wires) {
    bool again = false;
    do {
        Control control;
        while (outbound.try_receive_control(control)) {
            if (control.kind == Control::kind::stream_fin) {
                auto& wire = wires[wireIndex(control.id.stream_id)];
                wire.finalWireBytes = control.value;
            }
        }
        drainDataOnly(outbound, wires);
        again = outbound.has_pending();
    } while (again);
}

struct DecodedResponse final {
    std::size_t informationalHeads{};
    std::size_t finalHeads{};
    std::size_t messageEnds{};
    std::uint16_t status{};
    std::string body;
};

inline void captureResponse(void* raw, const ruvia::Http3ClientResponseEvent& event) {
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

inline DecodedResponse decodeResponse(const PublishedWire& wire, ruvia::HttpKnownMethod method,
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

inline ruvia::Task<void> stopAfter(ruvia::EventLoopAttachment& attachment,
    ruvia::Task<void> operation) {
    try {
        co_await std::move(operation);
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

inline void runWorkerTask(ruvia::EventLoopAttachment& attachment, ruvia::Task<void> operation) {
    auto root = attachment.loop().start(stopAfter(attachment, std::move(operation)));
    attachment.run();
    root.get();
}

inline ruvia::Task<void> waitForReady(Connection& connection, std::size_t count,
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

inline ruvia::Task<bool> waitForSlowStart(Fixture& fixture,
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

inline void requireWatchdogSuccess(ruvia::testing::TestContext& ruvia_ctx, bool succeeded);

inline ruvia::Task<bool> waitForTaskCount(Connection& connection, std::size_t count,
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

inline void requireWatchdogSuccess(ruvia::testing::TestContext& ruvia_ctx, bool succeeded) {
    RUVIA_CHECK(succeeded);
    if (!succeeded) {
        std::terminate();
    }
}

inline void simulateGlobalStopTakeover(Connection& connection) {
    const auto closeIntent = connection.peekTransportIntent();
    if (!closeIntent ||
        closeIntent->token.kind != Connection::TransportIntentKind::kConnectionClose ||
        !connection.takeOverTransportRetirement({.epoch = closeIntent->token.id.epoch,
            .connectionGeneration = closeIntent->token.id.connection_generation}) ||
        !connection.ackTransportIntent(closeIntent->token)) {
        throw std::runtime_error("test local lifecycle owner failed to take over HTTP/3 retirement");
    }
    while (const auto intent = connection.peekTransportIntent()) {
        if (!connection.ackTransportIntent(intent->token)) {
            throw std::runtime_error("test local lifecycle owner failed to settle HTTP/3 intent");
        }
    }
}

inline ruvia::Task<void> publishGroup(Connection& connection, buffer& outbound,
    std::array<PublishedWire, 6>& wires, std::span<const std::uint64_t> streamIds,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stopToken,
    bool exerciseBackpressure, ruvia::testing::TestContext& ruvia_ctx) {
    std::size_t finished = 0;
    bool sawDataBackpressure = false;
    bool sawControlBackpressure = false;

    if (exerciseBackpressure) {
        co_await waitForReady(connection, streamIds.size(), worker, stopToken);
        const auto first = connection.publishOne(kAllWorkLanes);
        RUVIA_CHECK(first.status == Connection::PublishStatus::kAttempted);
        RUVIA_CHECK_EQ(first.streamId, streamIds[0]);
        RUVIA_CHECK(first.publication.status ==
                    Connection::Dispatch::PublishStatus::kBytesPublished);
        RUVIA_CHECK(first.publication.blockReason ==
                    Connection::Dispatch::PublishBlockReason::kNone);

        // Leave the first DATA block queued. The next sibling must still receive
        // its turn and report the shared DATA lane as blocked.
        const auto second = connection.publishOne(kAllWorkLanes);
        RUVIA_CHECK(second.status == Connection::PublishStatus::kAttempted);
        RUVIA_CHECK_EQ(second.streamId, streamIds[1]);
        RUVIA_CHECK(second.publication.status ==
                    Connection::Dispatch::PublishStatus::kBackpressured);
        RUVIA_CHECK(second.publication.blockReason ==
                    Connection::Dispatch::PublishBlockReason::kData);
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
        const auto result = attempt.publication;
        if (result.status == Connection::Dispatch::PublishStatus::kFinPublished) {
            ++finished;
        } else if (result.status == Connection::Dispatch::PublishStatus::kBackpressured) {
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
    if (exerciseBackpressure) {
        RUVIA_CHECK(sawDataBackpressure);
        RUVIA_CHECK(sawControlBackpressure);
    }
}

}  // namespace
