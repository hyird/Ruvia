#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

#include <asio/system_executor.hpp>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3ClientResponse.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3Settings.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/detail/CallbackRef.h"
#include "ruvia/web/detail/http3/Http3BufferedRequestDispatch.h"
#include "ruvia/web/detail/http3/Http3ServerStreamInput.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"

#include "memory_resource_fixture.h"
#include "routing_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using Dispatch = ruvia::detail::Http3BufferedRequestDispatch;
using Engine = ruvia::detail::Http3SansIoSessionEngine;
using Input = ruvia::detail::Http3ServerStreamInput;
using Mailbox = ruvia::detail::Http3StreamMailbox;
using MessageId = ruvia::detail::Http3StreamMessageId;
using Control = ruvia::detail::Http3StreamControl;
using BlockReason = Dispatch::PublishBlockReason;
using PublicationDemand = Dispatch::PublicationDemand;

constexpr std::uint64_t kEpoch = 37;
constexpr std::uint64_t kGeneration = 53;
using namespace std::chrono_literals;

struct HandlerState final {
    std::string responseBody{"buffered-h3-ok"};
    std::size_t handlerCalls{};
    ruvia::WorkerSignal* started{};
    bool requestReadCorrectly{};
    bool handlerResumed{};
    bool errorHandlerCalled{};
    std::string errorCode;
    bool allocateErrorHeaders{};
    bool throwFromErrorHandler{};
    std::string largeResponseHeader;
    ruvia::WorkerSignal* webSocketStarted{};
    ruvia::WorkerSignal* webSocketMessageReceived{};
    ruvia::WorkerSignal* webSocketMessageEchoed{};
    std::array<std::string, 2> webSocketMessages{};
    std::size_t webSocketEchoes{};
    bool webSocketSawFin{};
    bool webSocketThrowOnStart{};
    bool webSocketRetainedDataStable{true};
};

ruvia::Task<ruvia::HttpResponse> bufferedHandler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<HandlerState*>(raw);
    ++state.handlerCalls;
    const auto path = context.req().path();
    if (path == "/throw") {
        throw std::runtime_error("buffered handler failure");
    }
    if (path == "/suspend") {
        state.started->notify();
        const auto sleep = co_await ruvia::sleepFor(
            context.worker(), 10s, context.stopToken());
        state.handlerResumed = true;
        if (sleep == ruvia::TimerSleepResult::kStopRequested) {
            throw std::runtime_error("buffered handler stopped");
        }
        co_return context.text("unexpectedly resumed");
    }
    if (path == "/file") {
        ruvia::HttpResponse response({.resource = context.arena()});
        response.fileBody("virtual-response.bin", 5, 0, 5, {}, true);
        co_return response;
    }
    if (path == "/empty-file") {
        ruvia::HttpResponse response({.resource = context.arena()});
        response.fileBody("virtual-empty-response.bin", 0, 0, 0, {}, true);
        co_return response;
    }
    if (path == "/items") {
        const auto body = co_await context.req().text();
        state.requestReadCorrectly = context.req().method() == "POST" &&
                                     context.req().path() == "/items" &&
                                     body == "payload" &&
                                     context.req().header("x-input") == "present" &&
                                     context.req().cookie("session") == "one" &&
                                     context.req().cookie("other") == "two";
    }
    context.header("x-dispatch", "buffered");
    if (!state.largeResponseHeader.empty()) {
        context.header("x-large-response", state.largeResponseHeader);
    }
    co_return context.text(std::string_view(state.responseBody));
}

ruvia::Task<void> bufferedWebSocketHandler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<HandlerState*>(raw);
    const std::string expectedRetained(512, 'r');
    const std::pmr::string retained(expectedRetained, context.pool());
    state.webSocketStarted->notify();
    if (state.webSocketThrowOnStart) {
        throw std::runtime_error("websocket handler failure");
    }
    auto& webSocket = context.webSocket();
    for (std::size_t index = 0; index < state.webSocketMessages.size(); ++index) {
        auto message = co_await webSocket.read();
        if (!message) {
            break;
        }
        std::pmr::string temporary(message->payload(), context.pool());
        state.webSocketMessages[index].assign(temporary);
        state.webSocketMessageReceived->notify();
        co_await webSocket.text(std::move(temporary));
        ++state.webSocketEchoes;
        state.webSocketRetainedDataStable = state.webSocketRetainedDataStable &&
                                            std::string_view(retained.data(), retained.size()) ==
                                                expectedRetained;
        state.webSocketMessageEchoed->notify();
    }
    state.webSocketSawFin = !(co_await webSocket.read()).has_value();
    state.webSocketRetainedDataStable = state.webSocketRetainedDataStable &&
                                        std::string_view(retained.data(), retained.size()) ==
                                            expectedRetained;
}

struct Routes final {
    ruvia::detail::Router router;
    ruvia::detail::RouterImpl& implementation{ruvia::detail::RouterImpl::from(router)};
    HandlerState handlers;
    ruvia::HttpErrorHandler errorHandler;

    explicit Routes(std::chrono::milliseconds peerTransportFinTimeout = 5s)
        : errorHandler([this](ruvia::Context& context, ruvia::HttpErrorInfo error)
                           -> ruvia::Task<ruvia::HttpResponse> {
              handlers.errorHandlerCalled = true;
              handlers.errorCode.assign(error.code());
              if (handlers.throwFromErrorHandler) {
                  throw std::runtime_error("error handler failed");
              }
              context.status(error.status());
              context.header("x-error-handler", "used");
              if (handlers.allocateErrorHeaders) {
                  for (unsigned i = 0; i < 32; ++i) {
                      context.header("x-owned-error-" + std::to_string(i), std::string_view(handlers.responseBody));
                  }
              }
              co_return context.text("handled-error");
          }) {
        implementation.setErrorHandler(ruvia::detail::CallbackAccess::ref(errorHandler));
        add(ruvia::HttpKnownMethod::kPost, "/items");
        add(ruvia::HttpKnownMethod::kGet, "/throw");
        add(ruvia::HttpKnownMethod::kGet, "/suspend");
        add(ruvia::HttpKnownMethod::kGet, "/file");
        add(ruvia::HttpKnownMethod::kGet, "/empty-file");
        add(ruvia::HttpKnownMethod::kGet, "/large");
        ruvia::WebSocketRouteConfig webSocketConfig;
        webSocketConfig.lifecycle.peerTransportFinTimeout = peerTransportFinTimeout;
        implementation.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
            routing_test::path("/socket"),
            ruvia::detail::RouteStreamHandler(&handlers, &bufferedWebSocketHandler),
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::move(webSocketConfig));
        implementation.finalize();
    }

    void add(ruvia::HttpKnownMethod method, std::string_view path) {
        implementation.registerRoute(method, routing_test::path(path),
            ruvia::detail::RouteHandler(&handlers, &bufferedHandler),
            ruvia::detail::RequestBodyMode::kBuffered,
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
    }
};

class AllocationSwitch final : public std::pmr::memory_resource {
public:
    explicit AllocationSwitch(std::pmr::memory_resource& upstream)
        : upstream_(upstream) {}
    bool reject{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject) {
            throw std::bad_alloc();
        }
        return upstream_.allocate(bytes, alignment);
    }
    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        upstream_.deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::pmr::memory_resource& upstream_;
};

struct Fixture final {
    Routes routes;
    ruvia::test::CountingMemoryResource& upstream;
    AllocationSwitch allocations;
    ruvia::WorkerMemory worker;
    ruvia::StopSource workerStopSource;
    ruvia::StopToken workerStop;
    ruvia::detail::ContextServices services;
    ruvia::detail::HttpServerOptions options;
    ruvia::detail::Http3ServerBodyBudget bodyBudget;
    Engine session;
    Mailbox inbound;
    Input input;
    Mailbox outbound;
    ruvia::ConnectionScanner::Entry scannerEntry;
    ruvia::ConnectionScanner scanner;
    asio::any_io_executor executor{asio::system_executor{}};

    Fixture(const ruvia::WorkerHandle& workerHandle,
        ruvia::test::CountingMemoryResource& upstream,
        std::uint32_t outboundBlocks = 1, std::uint32_t outboundDataSlots = 1,
        std::uint32_t outboundControlSlots = 1,
        std::size_t workerBodyBudget = 64 * 1024 * 1024,
        std::size_t tunnelBytesPerStream = 64 * 1024,
        std::chrono::milliseconds peerTransportFinTimeout = 5s,
        std::chrono::milliseconds scannerInterval = 1s)
        : routes(peerTransportFinTimeout),
          upstream(upstream),
          allocations(upstream),
          worker(allocations),
          workerStop(workerStopSource.token()),
          services(workerHandle, workerStop),
          bodyBudget(workerBodyBudget),
          session(routes.implementation.routeTable(), worker, bodyBudget,
              {.maxTunnelBufferedBytes = tunnelBytesPerStream}),
          inbound(8, 8, 4, worker.resource()),
          input(session, worker, kEpoch, kGeneration, 32),
          outbound(outboundBlocks, outboundDataSlots, outboundControlSlots, worker.resource()),
          scanner(workerHandle, {.scanInterval = scannerInterval}) {}

    [[nodiscard]] Dispatch makeDispatch(std::uint64_t streamId,
        ruvia::detail::ContextServices dispatchServices,
        ruvia::detail::Http3TunnelCallbacks callbacks = {}) {
        return Dispatch(session, routes.implementation.routeTable(), worker,
            std::move(dispatchServices), options, outbound,
            {kEpoch, kGeneration, streamId}, scannerEntry, executor, callbacks);
    }
};

struct TunnelCallbacksState final {
    TunnelCallbacksState(const ruvia::WorkerHandle& worker,
        ruvia::ConnectionScanner& scanner)
        : scanner(scanner),
          outputReady(worker) {}

    [[nodiscard]] ruvia::detail::Http3TunnelCallbacks callbacks() noexcept {
        return {.context = this,
            .attachScanner = [](void* raw, std::uint64_t,
                                 ruvia::ConnectionScanner::Entry& entry) noexcept {
                auto& state = *static_cast<TunnelCallbacksState*>(raw);
                if (!state.scannerAttached) {
                    state.scanner.registerEntry(entry);
                    state.scannerAttached = true;
                }
                return true; },
            .outputReady = [](void* raw, std::uint64_t) noexcept { static_cast<TunnelCallbacksState*>(raw)->outputReady.notify(); },
            .abort = [](void* raw, std::uint64_t) noexcept {
                auto& state = *static_cast<TunnelCallbacksState*>(raw);
                state.aborted = true;
                state.outputReady.notify(); }};
    }

    ruvia::ConnectionScanner& scanner;
    ruvia::WorkerSignal outputReady;
    bool scannerAttached{};
    bool aborted{};
};

std::string frame(std::uint64_t type, std::string_view payload) {
    std::array<char, ruvia::kHttp3FrameHeaderMaxBytes> header{};
    const auto size = ruvia::encodeHttp3FrameHeader(header, type, payload.size());
    if (!size) {
        throw std::runtime_error("HTTP/3 fixture frame encoding failed");
    }
    std::string result(header.data(), *size);
    result.append(payload);
    return result;
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
        throw std::runtime_error("HTTP/3 fixture request-head encoding failed");
    }
    std::string result = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(encoded->fieldSection.data(), encoded->fieldSection.size()));
    if (!body.empty()) {
        result += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), body);
    }
    return result;
}

void feedPeerSettings(Fixture& fixture, std::optional<std::uint64_t> maxFieldSectionSize) {
    std::array<char, 64> payload{};
    ruvia::Http3Settings settings;
    settings.maxFieldSectionSize = maxFieldSectionSize;
    const auto encoded = ruvia::encodeHttp3Settings(payload, settings);
    if (!encoded) {
        throw std::runtime_error("HTTP/3 fixture SETTINGS encoding failed");
    }
    std::string controlWire(1, '\0');  // Peer unidirectional control stream type.
    controlWire += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kSettings),
        std::string_view(payload.data(), *encoded));
    const auto result = fixture.session.feed(2, controlWire);
    if (result.scope != ruvia::Http3ConnectionErrorScope::kNone ||
        result.status != ruvia::Http3ConnectionStatus::kNeedMoreData) {
        throw std::runtime_error("HTTP/3 fixture peer SETTINGS was rejected");
    }
}

bool sendAccepted(Mailbox::SendResult result) noexcept {
    return result == Mailbox::SendResult::kSent ||
           result == Mailbox::SendResult::kSentNotifyPeer;
}

bool controlAccepted(Mailbox::ControlResult result) noexcept {
    return result == Mailbox::ControlResult::kSent ||
           result == Mailbox::ControlResult::kSentNotifyPeer;
}

void feedWebSocketRequest(Engine& session, ruvia::WorkerMemory& worker,
    std::uint64_t streamId, std::string_view version = "13") {
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/socket"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", version}};
    const auto fieldsToEncode = version.empty()
                                    ? std::span<const ruvia::Http3FieldSectionFieldView>(fields.data(), fields.size() - 1)
                                    : std::span<const ruvia::Http3FieldSectionFieldView>(fields);
    const auto fieldSection = ruvia::encodeHttp3FieldSection(fieldsToEncode, worker.resource());
    if (!fieldSection) {
        throw std::runtime_error("HTTP/3 WebSocket fixture field section encoding failed");
    }
    const std::string wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(fieldSection->data(), fieldSection->size()));
    const auto result = session.feed(streamId, wire);
    if (result.scope != ruvia::Http3ConnectionErrorScope::kNone ||
        session.streamState(streamId) != Engine::StreamState::kReady) {
        throw std::runtime_error("HTTP/3 WebSocket fixture request was rejected");
    }
}

void feedWebSocketRequest(Fixture& fixture, std::uint64_t streamId) {
    feedWebSocketRequest(fixture.session, fixture.worker, streamId);
}

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

void feedRequest(Fixture& fixture, std::uint64_t streamId, std::string_view method,
    std::string_view path, std::string_view body = {},
    std::span<const ruvia::Http3FieldSectionFieldView> fields = {}) {
    const MessageId id{kEpoch, kGeneration, streamId};
    const auto wire = requestWire(fixture.worker, method, path, body, fields);
    if (wire.size() > Mailbox::kMaxBlockBytes) {
        throw std::runtime_error("HTTP/3 fixture request exceeds one mailbox block");
    }
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!sendAccepted(fixture.inbound.trySend(id, bytes)) ||
        !controlAccepted(fixture.inbound.trySendControl(
            {Control::Kind::kStreamFin, id, wire.size()}))) {
        throw std::runtime_error("HTTP/3 fixture inbound mailbox is full");
    }

    Control fin;
    if (!fixture.inbound.tryReceiveControl(fin) ||
        fixture.input.acceptControl(fin).status != Input::Status::kDeferredFin) {
        throw std::runtime_error("HTTP/3 fixture FIN was not deferred");
    }
    Mailbox::BorrowedBlock block;
    if (!fixture.inbound.tryReceive(block) || block.id().streamId != streamId) {
        throw std::runtime_error("HTTP/3 fixture DATA block is unavailable");
    }
    const auto fed = fixture.input.acceptData(block);
    block.release();
    (void)fixture.inbound.drainReturns();
    (void)fixture.inbound.finishDrain();
    if (fed.status != Input::Status::kFinished ||
        fixture.session.streamState(streamId) != Engine::StreamState::kReady) {
        throw std::runtime_error("HTTP/3 fixture request did not become ready");
    }
}

struct PublishedWire final {
    std::string bytes;
    std::optional<std::uint64_t> finalWireBytes;
    std::size_t dataBlocks{};
    bool identityMatched{true};
};

std::size_t drainDataOnly(Mailbox& mailbox, const MessageId& expected, PublishedWire& output) {
    std::size_t received = 0;
    Mailbox::BorrowedBlock block;
    while (mailbox.tryReceive(block)) {
        if (block.id().epoch != expected.epoch ||
            block.id().connectionGeneration != expected.connectionGeneration ||
            block.id().streamId != expected.streamId) {
            output.identityMatched = false;
        }
        const auto bytes = block.bytes();
        output.bytes.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ++output.dataBlocks;
        ++received;
        block.release();
    }
    (void)mailbox.drainReturns();
    return received;
}

void drainMailbox(Mailbox& mailbox, const MessageId& expected, PublishedWire& output) {
    bool again = false;
    do {
        Control control;
        while (mailbox.tryReceiveControl(control)) {
            if (control.kind == Control::Kind::kStreamFin &&
                control.id.epoch == expected.epoch &&
                control.id.connectionGeneration == expected.connectionGeneration &&
                control.id.streamId == expected.streamId) {
                output.finalWireBytes = control.value;
            }
        }
        (void)drainDataOnly(mailbox, expected, output);
        again = mailbox.finishDrain();
    } while (again);
    (void)mailbox.drainReturns();
}

struct DecodedResponse final {
    std::size_t finalHeads{};
    std::size_t bodyEvents{};
    std::size_t messageEnds{};
    std::uint16_t status{};
    std::size_t decodedFieldSectionSize{};
    std::size_t largeResponseHeaderBytes{};
    std::optional<std::uint64_t> contentLength;
    std::string dispatchHeader;
    std::string errorHeader;
    std::string websocketVersionHeader;
    std::string connectionHeader;
    std::string upgradeHeader;
    std::string websocketAcceptHeader;
    std::string body;
};

void onResponse(void* raw, const ruvia::Http3ClientResponseEvent& event) {
    auto& response = *static_cast<DecodedResponse*>(raw);
    switch (event.kind) {
        case ruvia::Http3ClientResponseEventKind::kFinalHead:
            ++response.finalHeads;
            response.status = event.head->status;
            response.contentLength = event.head->contentLength;
            response.decodedFieldSectionSize = 7 + std::to_string(event.head->status).size() + 32;
            for (const auto& header : event.head->headers) {
                response.decodedFieldSectionSize += header.name.size() + header.value.size() + 32;
                if (header.name == "x-dispatch") {
                    response.dispatchHeader = header.value;
                } else if (header.name == "x-error-handler") {
                    response.errorHeader = header.value;
                } else if (header.name == "x-large-response") {
                    response.largeResponseHeaderBytes = header.value.size();
                } else if (header.name == "sec-websocket-version") {
                    response.websocketVersionHeader = header.value;
                } else if (header.name == "connection") {
                    response.connectionHeader = header.value;
                } else if (header.name == "upgrade") {
                    response.upgradeHeader = header.value;
                } else if (header.name == "sec-websocket-accept") {
                    response.websocketAcceptHeader = header.value;
                }
            }
            break;
        case ruvia::Http3ClientResponseEventKind::kBody:
            ++response.bodyEvents;
            response.body.append(event.body.data(), event.body.size());
            break;
        case ruvia::Http3ClientResponseEventKind::kMessageEnd:
            ++response.messageEnds;
            break;
        case ruvia::Http3ClientResponseEventKind::kInformationalHead:
        case ruvia::Http3ClientResponseEventKind::kTunnelData:
        case ruvia::Http3ClientResponseEventKind::kTrailerField:
        case ruvia::Http3ClientResponseEventKind::kReset:
            break;
    }
}

ruvia::Http3ClientResponseResult decodePublished(const PublishedWire& wire,
    ruvia::HttpKnownMethod requestMethod, std::uint64_t streamId,
    std::pmr::memory_resource* resource, DecodedResponse& output) {
    if (!wire.finalWireBytes || *wire.finalWireBytes != wire.bytes.size()) {
        return {ruvia::Http3ClientResponseStatus::kStreamError,
            ruvia::Http3ConnectionErrorScope::kStream,
            ruvia::Http3ConnectionErrorCode::kMessageError};
    }
    ruvia::Http3ClientResponse decoder(streamId, requestMethod, resource);
    return decoder.feed(std::span<const char>(wire.bytes.data(), wire.bytes.size()), true, false,
        &onResponse, &output);
}

void publishAndDrain(Dispatch& dispatch, Fixture& fixture, std::uint64_t streamId,
    PublishedWire& output, ruvia::testing::TestContext& ruvia_ctx,
    bool assertDataBackpressure = false) {
    const MessageId id{kEpoch, kGeneration, streamId};
    std::size_t attempts = 0;
    bool sawDataBackpressure = false;
    while (!dispatch.complete() && ++attempts < 10000) {
        const auto result = dispatch.publishStep();
        if (result.status == Dispatch::PublishStatus::kBytesPublished) {
            RUVIA_CHECK(result.blockReason == BlockReason::kNone);
            if (assertDataBackpressure && !sawDataBackpressure && !dispatch.complete()) {
                const auto before = dispatch.publishedWireBytes();
                const auto blocked = dispatch.publishStep();
                if (blocked.status == Dispatch::PublishStatus::kBackpressured) {
                    sawDataBackpressure = true;
                    RUVIA_CHECK(blocked.blockReason == BlockReason::kData);
                    RUVIA_CHECK(!blocked.notifyPeer);
                    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), before);
                }
            }
            drainMailbox(fixture.outbound, id, output);
        } else if (result.status == Dispatch::PublishStatus::kBackpressured) {
            RUVIA_CHECK(result.blockReason == BlockReason::kData);
            drainMailbox(fixture.outbound, id, output);
        } else if (result.status == Dispatch::PublishStatus::kFinPublished) {
            RUVIA_CHECK(result.blockReason == BlockReason::kNone);
            drainMailbox(fixture.outbound, id, output);
        } else if (result.status == Dispatch::PublishStatus::kComplete) {
            RUVIA_CHECK(result.blockReason == BlockReason::kNone);
        } else {
            throw std::runtime_error("HTTP/3 response publication failed");
        }
    }
    if (assertDataBackpressure) {
        RUVIA_CHECK(sawDataBackpressure);
    }
    if (attempts >= 10000 || !dispatch.complete()) {
        throw std::runtime_error("HTTP/3 response publication did not complete");
    }
    drainMailbox(fixture.outbound, id, output);
}

ruvia::Task<void> runOwner(Dispatch& dispatch, Dispatch::RunStatus& result, bool& joined,
    ruvia::WorkerSignal& finished) {
    result = co_await dispatch.runHandler();
    joined = true;
    finished.notify();
}

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
    RUVIA_CHECK(co_await file.runHandler() == Dispatch::RunStatus::kFilePayloadUnsupported);
    RUVIA_CHECK(file.failure() == nullptr);
    RUVIA_CHECK(fixture.session.request(4) == nullptr);
    Mailbox::BorrowedBlock unexpected;
    RUVIA_CHECK(!fixture.outbound.tryReceive(unexpected));

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
    Mailbox::BorrowedBlock block;
    RUVIA_CHECK(!fixture.outbound.tryReceive(block));
    Control control;
    RUVIA_CHECK(!fixture.outbound.tryReceiveControl(control));

    feedRequest(fixture, 4, "GET", "/large");
    auto partial = fixture.makeDispatch(4, fixture.services);
    RUVIA_CHECK(co_await partial.runHandler() == Dispatch::RunStatus::kResponseReady);
    const auto published = partial.publishStep();
    RUVIA_CHECK(published.status == Dispatch::PublishStatus::kBytesPublished && published.notifyPeer);
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
    drainMailbox(fixture.outbound, {kEpoch, kGeneration, 4}, partialWire);
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
    Mailbox::BorrowedBlock block;
    Control control;
    RUVIA_CHECK(!fixture.outbound.tryReceive(block));
    RUVIA_CHECK(!fixture.outbound.tryReceiveControl(control));
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

ruvia::Task<void> exerciseMailboxCloseDuringData(
    Fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    feedRequest(fixture, 4, "GET", "/large");
    auto dispatch = fixture.makeDispatch(4, fixture.services);
    RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);

    PublishedWire wire;
    const MessageId id{kEpoch, kGeneration, 4};
    const auto headers = dispatch.publishStep();
    RUVIA_CHECK(headers.status == Dispatch::PublishStatus::kBytesPublished);
    drainMailbox(fixture.outbound, id, wire);
    const auto dataHeader = dispatch.publishStep();
    RUVIA_CHECK(dataHeader.status == Dispatch::PublishStatus::kBytesPublished);
    drainMailbox(fixture.outbound, id, wire);

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
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kLocalMailboxStopped);
    const auto failure = dispatch.publishStep();
    RUVIA_CHECK(failure.status == Dispatch::PublishStatus::kFailed);
    RUVIA_CHECK(failure.blockReason == BlockReason::kNone);
    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), beforeFailure);
    RUVIA_CHECK(!dispatch.complete());
    drainMailbox(fixture.outbound, id, wire);
    RUVIA_CHECK(!wire.finalWireBytes.has_value());
    Control control;
    RUVIA_CHECK(!fixture.outbound.tryReceiveControl(control));
    RUVIA_CHECK(fixture.session.request(4) == nullptr);
}

ruvia::Task<void> exerciseMailboxCloseDuringFin(
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
        drainMailbox(fixture.outbound, id, wire);
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

    const auto controlFiller = fixture.outbound.trySendControl(
        {Control::Kind::kWritable, id, 0});
    RUVIA_CHECK(controlAccepted(controlFiller));
    const auto blockedFin = dispatch.publishStep();
    RUVIA_CHECK(blockedFin.status == Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blockedFin.blockReason == BlockReason::kControl);
    RUVIA_CHECK(!blockedFin.notifyPeer);

    const auto beforeFailure = dispatch.publishedWireBytes();
    RUVIA_CHECK(fixture.outbound.stop());
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kLocalMailboxStopped);
    const auto failure = dispatch.publishStep();
    RUVIA_CHECK(failure.status == Dispatch::PublishStatus::kFailed);
    RUVIA_CHECK(failure.blockReason == BlockReason::kNone);
    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), beforeFailure);
    RUVIA_CHECK(!dispatch.complete());
    drainMailbox(fixture.outbound, id, wire);
    RUVIA_CHECK(!wire.finalWireBytes.has_value());
    Control control;
    RUVIA_CHECK(!fixture.outbound.tryReceiveControl(control));
    RUVIA_CHECK(fixture.session.request(4) == nullptr);
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
    Mailbox::BorrowedBlock block;
    Control control;
    RUVIA_CHECK(!fixture.outbound.tryReceive(block) && !fixture.outbound.tryReceiveControl(control));
    RUVIA_CHECK(!fixture.session.terminated());
}

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
    RUVIA_CHECK(first.status == Dispatch::PublishStatus::kBytesPublished && first.notifyPeer);
    RUVIA_CHECK(first.blockReason == BlockReason::kNone);
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);
    const auto firstSize = dispatch.publishedWireBytes();
    const auto blockedData = dispatch.publishStep();
    RUVIA_CHECK(blockedData.status == Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blockedData.blockReason == BlockReason::kData);
    RUVIA_CHECK(!blockedData.notifyPeer);
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);
    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), firstSize);
    RUVIA_CHECK(drainDataOnly(fixture.outbound, id, wire) == 1);

    // Keep the independent control lane full until the response cursor reaches FIN.
    const auto filler = fixture.outbound.trySendControl(
        {Control::Kind::kWritable, id, 0});
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
            RUVIA_CHECK(result.bytesPublished <= Mailbox::kMaxBlockBytes);
            RUVIA_CHECK(drainDataOnly(fixture.outbound, id, wire) == 1);
        } else if (result.status == Dispatch::PublishStatus::kBackpressured) {
            const auto received = drainDataOnly(fixture.outbound, id, wire);
            if (received == 0) {
                blockedFin = true;
                RUVIA_CHECK(demand == PublicationDemand::kControl);
                RUVIA_CHECK(result.blockReason == BlockReason::kControl);
                RUVIA_CHECK(!result.notifyPeer);
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
    RUVIA_CHECK(fixture.outbound.tryReceiveControl(queuedFiller));
    RUVIA_CHECK(queuedFiller.kind == Control::Kind::kWritable);
    (void)fixture.outbound.finishDrain();

    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kControl);
    const auto fin = dispatch.publishStep();
    RUVIA_CHECK(fin.status == Dispatch::PublishStatus::kFinPublished);
    RUVIA_CHECK(fin.notifyPeer);
    RUVIA_CHECK(fin.blockReason == BlockReason::kNone);
    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), beforeFin);
    drainMailbox(fixture.outbound, id, wire);
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
    const auto controlFiller = fixture.outbound.trySendControl(
        {Control::Kind::kWritable, {kEpoch, kGeneration, 4}, 0});
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
    RUVIA_CHECK(fixture.outbound.tryReceiveControl(preservedFiller));
    RUVIA_CHECK(preservedFiller.kind == Control::Kind::kWritable);
    (void)fixture.outbound.finishDrain();
    PublishedWire cancelledWire;
    drainMailbox(fixture.outbound, {kEpoch, kGeneration, 4}, cancelledWire);
    RUVIA_CHECK(!cancelledWire.finalWireBytes.has_value());
    RUVIA_CHECK(fixture.session.request(4) == nullptr);

    feedRequest(fixture, 8, "GET", "/large");
    auto stopAfterBlock = fixture.makeDispatch(8, fixture.services);
    RUVIA_CHECK(co_await stopAfterBlock.runHandler() == Dispatch::RunStatus::kResponseReady);
    RUVIA_CHECK(stopAfterBlock.publicationDemand() == PublicationDemand::kData);
    const auto queued = stopAfterBlock.publishStep();
    RUVIA_CHECK(queued.status == Dispatch::PublishStatus::kBytesPublished);
    RUVIA_CHECK(queued.notifyPeer);
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
                PublicationDemand::kLocalMailboxStopped);
    RUVIA_CHECK_EQ(fixture.upstream.allocationCount(), allocationsAtStop);
    RUVIA_CHECK_EQ(fixture.upstream.deallocationCount(), returnsAtStop);
    RUVIA_CHECK_EQ(fixture.upstream.liveAllocations(), liveAtStop);
    const auto stopped = stopAfterBlock.publishStep();
    RUVIA_CHECK(stopped.status == Dispatch::PublishStatus::kFailed);
    RUVIA_CHECK(stopped.blockReason == BlockReason::kNone);
    RUVIA_CHECK_EQ(stopAfterBlock.publishedWireBytes(), beforeStop);
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
    RUVIA_CHECK(headers.has_value());
    if (headers) {
        RUVIA_CHECK_EQ(headers->type,
            static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders));
        encodedFieldSectionSize = headers->payload.size();
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
        RUVIA_CHECK(co_await dispatch.runHandler() == Dispatch::RunStatus::kResponseReady);
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
        Mailbox::BorrowedBlock block;
        Control control;
        RUVIA_CHECK(!belowLimit.outbound.tryReceive(block));
        RUVIA_CHECK(!belowLimit.outbound.tryReceiveControl(control));
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
        RUVIA_CHECK(sendAccepted(beforeHandoff.outbound.trySend(fillerId, fillerBytes)));
        RUVIA_CHECK(controlAccepted(beforeHandoff.outbound.trySendControl(
            {Control::Kind::kWritable, fillerId, 0})));
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
        Mailbox::BorrowedBlock preservedBlock;
        RUVIA_CHECK(beforeHandoff.outbound.tryReceive(preservedBlock));
        RUVIA_CHECK(preservedBlock.id().streamId == fillerId.streamId);
        RUVIA_CHECK(preservedBlock.bytes().size() == fillerBytes.size());
        RUVIA_CHECK(preservedBlock.bytes().front() == fillerBytes.front());
        preservedBlock.release();
        Control preservedControl;
        RUVIA_CHECK(beforeHandoff.outbound.tryReceiveControl(preservedControl));
        RUVIA_CHECK(preservedControl.kind == Control::Kind::kWritable);
        RUVIA_CHECK(preservedControl.id.streamId == fillerId.streamId);
        (void)beforeHandoff.outbound.drainReturns();
        (void)beforeHandoff.outbound.finishDrain();

        feedRequest(beforeHandoff, 4, "GET", "/large");
        auto cancelled = beforeHandoff.makeDispatch(4, beforeHandoff.services);
        RUVIA_CHECK(co_await cancelled.runHandler() == Dispatch::RunStatus::kResponseReady);
        cancelled.cancel();
        RUVIA_CHECK(cancelled.publicationDemand() == PublicationDemand::kLocalCancelled);
        RUVIA_CHECK(cancelled.publishStep().status == Dispatch::PublishStatus::kCancelled);
        RUVIA_CHECK(beforeHandoff.session.request(4) == nullptr);
        RUVIA_CHECK_EQ(beforeHandoff.session.activeStreamCount(), std::size_t{0});

        Mailbox::BorrowedBlock block;
        Control control;
        RUVIA_CHECK(!beforeHandoff.outbound.tryReceive(block));
        RUVIA_CHECK(!beforeHandoff.outbound.tryReceiveControl(control));
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
        RUVIA_CHECK_EQ(prefix.bytesPublished, Mailbox::kMaxBlockBytes);
        RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), Mailbox::kMaxBlockBytes);
        drainMailbox(committed.outbound, id, wire);
        RUVIA_CHECK_EQ(wire.bytes.size(), Mailbox::kMaxBlockBytes);
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
        RUVIA_CHECK(co_await nextRequest.runHandler() == Dispatch::RunStatus::kResponseReady);
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
        RUVIA_CHECK(co_await shuttingDown.runHandler() == Dispatch::RunStatus::kResponseReady);
        RUVIA_CHECK(committed.outbound.stop());
        RUVIA_CHECK(shuttingDown.publishStep().status == Dispatch::PublishStatus::kFailed);
        RUVIA_CHECK(committed.session.request(8) == nullptr);
        RUVIA_CHECK_EQ(committed.session.activeStreamCount(), std::size_t{0});

        Mailbox::BorrowedBlock block;
        Control control;
        RUVIA_CHECK(!committed.outbound.tryReceive(block));
        RUVIA_CHECK(!committed.outbound.tryReceiveControl(control));
    }
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
    RUVIA_CHECK(headers.has_value());
    if (!headers) {
        return;
    }
    RUVIA_CHECK_EQ(headers->type, static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders));
    WebSocketStatusCapture status;
    const auto decoded = ruvia::decodeHttp3FieldSection(headers->payload,
        &captureWebSocketStatus, &status, {}, fixture.worker.resource());
    RUVIA_CHECK(decoded.has_value());
    RUVIA_CHECK(status.status == "200");

    std::string webSocketBytes;
    std::size_t offset = headers->encodedBytes;
    while (offset < wire.bytes.size()) {
        const auto data = ruvia::decodeHttp3Frame(
            std::span<const char>(wire.bytes.data() + offset, wire.bytes.size() - offset));
        RUVIA_CHECK(data.has_value());
        if (!data) {
            return;
        }
        RUVIA_CHECK_EQ(data->type, static_cast<std::uint64_t>(ruvia::Http3FrameType::kData));
        webSocketBytes.append(data->payload.data(), data->payload.size());
        offset += data->encodedBytes;
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
    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);
    const auto handshake = dispatch.publishStep();
    RUVIA_CHECK(handshake.status == Dispatch::PublishStatus::kBytesPublished);
    drainMailbox(fixture.outbound, id, wire);
    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kControl);
    const auto established = dispatch.publishStep();
    RUVIA_CHECK(established.status == Dispatch::PublishStatus::kControlPublished);
    Control establishedControl;
    RUVIA_CHECK(fixture.outbound.tryReceiveControl(establishedControl));
    RUVIA_CHECK(establishedControl.kind == Control::Kind::kTunnelEstablished);
    RUVIA_CHECK(establishedControl.id.epoch == id.epoch);
    RUVIA_CHECK(establishedControl.id.connectionGeneration == id.connectionGeneration);
    RUVIA_CHECK(establishedControl.id.streamId == id.streamId);
    RUVIA_CHECK_EQ(establishedControl.value, static_cast<std::uint64_t>(wire.bytes.size()));
    (void)fixture.outbound.finishDrain();
    co_await handlerStarted.wait();
    RUVIA_CHECK(callbacks.scannerAttached);

    const MessageId fillerId{kEpoch, kGeneration, streamId + 1000};
    constexpr std::array<std::byte, 1> fillerBytes{std::byte{0x7f}};
    RUVIA_CHECK(sendAccepted(fixture.outbound.trySend(fillerId, fillerBytes)));
    feedTunnelData(fixture, dispatch, streamId, "first");
    co_await messageReceived.wait();
    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);
    const auto blocked = dispatch.publishStep();
    RUVIA_CHECK(blocked.status == Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(blocked.blockReason == BlockReason::kData);
    RUVIA_CHECK(!blocked.notifyPeer);
    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), wire.bytes.size());

    Mailbox::BorrowedBlock filler;
    RUVIA_CHECK(fixture.outbound.tryReceive(filler));
    RUVIA_CHECK(filler.id().streamId == fillerId.streamId);
    RUVIA_CHECK(filler.bytes().size() == fillerBytes.size());
    filler.release();
    (void)fixture.outbound.drainReturns();
    (void)fixture.outbound.finishDrain();
    const auto firstEcho = dispatch.publishStep();
    RUVIA_CHECK(firstEcho.status == Dispatch::PublishStatus::kBytesPublished);
    drainMailbox(fixture.outbound, id, wire);
    co_await messageEchoed.wait();

    feedTunnelData(fixture, dispatch, streamId, "second");
    co_await messageReceived.wait();
    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kData);
    const auto secondEcho = dispatch.publishStep();
    RUVIA_CHECK(secondEcho.status == Dispatch::PublishStatus::kBytesPublished);
    drainMailbox(fixture.outbound, id, wire);
    co_await messageEchoed.wait();

    feedTunnelFin(fixture, dispatch, streamId);
    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kControl);
    const auto fin = dispatch.publishStep();
    RUVIA_CHECK(fin.status == Dispatch::PublishStatus::kFinPublished);
    drainMailbox(fixture.outbound, id, wire);
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

    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publishStep().status == Dispatch::PublishStatus::kBytesPublished);
    PublishedWire handshake;
    drainMailbox(fixture.outbound, {kEpoch, kGeneration, 0}, handshake);
    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publicationDemand() == PublicationDemand::kControl);
    RUVIA_CHECK(dispatch.publishStep().status == Dispatch::PublishStatus::kControlPublished);
    drainMailbox(fixture.outbound, {kEpoch, kGeneration, 0}, handshake);
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

    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publishStep().status == Dispatch::PublishStatus::kBytesPublished);
    PublishedWire handshake;
    drainMailbox(fixture.outbound, {kEpoch, kGeneration, streamId}, handshake);
    co_await callbacks.outputReady.wait();
    RUVIA_CHECK(dispatch.publishStep().status == Dispatch::PublishStatus::kControlPublished);
    Control established;
    RUVIA_CHECK(fixture.outbound.tryReceiveControl(established));
    RUVIA_CHECK(established.kind == Control::Kind::kTunnelEstablished);
    (void)fixture.outbound.finishDrain();
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
    for (;;) {
        co_await callbacks.outputReady.wait();
        const auto demand = dispatch.publicationDemand();
        if (demand == PublicationDemand::kData) {
            const auto result = dispatch.publishStep();
            RUVIA_CHECK(result.status == Dispatch::PublishStatus::kBytesPublished);
            drainMailbox(fixture.outbound, {kEpoch, kGeneration, streamId}, closeFrameWire);
            continue;
        }
        RUVIA_CHECK(demand == PublicationDemand::kControl);
        const auto result = dispatch.publishStep();
        if (result.status == Dispatch::PublishStatus::kControlPublished) {
            Control control;
            RUVIA_CHECK(fixture.outbound.tryReceiveControl(control));
            (void)fixture.outbound.finishDrain();
            continue;
        }
        RUVIA_CHECK(result.status == Dispatch::PublishStatus::kFinPublished);
        Control fin;
        RUVIA_CHECK(fixture.outbound.tryReceiveControl(fin));
        RUVIA_CHECK(fin.kind == Control::Kind::kStreamFin);
        (void)fixture.outbound.finishDrain();
        break;
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
                RUVIA_CHECK(data.has_value());
                if (!data) {
                    break;
                }
                if (data->type == static_cast<std::uint64_t>(ruvia::Http3FrameType::kData) &&
                    data->payload.size() >= 4 &&
                    static_cast<unsigned char>(data->payload[0]) == 0x88U &&
                    static_cast<unsigned char>(data->payload[2]) == 0x03U &&
                    static_cast<unsigned char>(data->payload[3]) == 0xf3U) {
                    sawInternalErrorClose = true;
                }
                offset += data->encodedBytes;
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

}  // namespace

RUVIA_TEST(http3BufferedDispatchRoutesBodyAndPublishesBoundedWireResponse) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    Fixture fixture(workerHandle, upstream);
    runWorkerTask(attachment, exerciseRouteAndPublish(fixture, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchSupportsHeadFileMetadataAndRejectsFilePayload) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    Fixture fixture(workerHandle, upstream);
    runWorkerTask(attachment, exerciseHeadAndFile(fixture, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchColdTasksDoNotLeaseAndRouterErrorsUseErrorHandler) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    Fixture fixture(workerHandle, upstream);
    runWorkerTask(attachment, exerciseColdAndError(fixture, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchCancellationStopsAndJoinsHandlerBeforeDiscardingErrorResponse) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    const auto explicitFin = fixture.input.acceptControl({Control::Kind::kStreamFin,
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
    const auto expiredFin = fixture.input.acceptControl({Control::Kind::kStreamFin,
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

ruvia::Task<void> exerciseMailboxCloseStages(
    const ruvia::WorkerHandle& workerHandle, ruvia::testing::TestContext& ruvia_ctx) {
    ruvia::test::CountingMemoryResource dataUpstream;
    {
        Fixture fixture(workerHandle, dataUpstream);
        co_await warmDispatch(fixture, ruvia_ctx);
        const auto warmedPoolAllocations = dataUpstream.liveAllocations();
        co_await exerciseMailboxCloseDuringData(fixture, ruvia_ctx);
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
        co_await exerciseMailboxCloseDuringFin(fixture, ruvia_ctx);
        RUVIA_CHECK_EQ(finUpstream.liveAllocations(), warmedPoolAllocations);
    }
    RUVIA_CHECK_EQ(finUpstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(finUpstream.allocationCount(), finUpstream.deallocationCount());
}

RUVIA_TEST(http3BufferedDispatchCombinesWorkerStopAndHandlerDeadlineThroughRouter) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto workerHandle = attachment.loop().handle();
    runWorkerTask(attachment, exerciseWorkerStopAndDeadline(workerHandle, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchRegistersPublicationDeadlineInlineAndLatchesCancellation) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto workerHandle = attachment.loop().handle();
    runWorkerTask(attachment,
        exercisePublicationDeadlineRegistration(workerHandle, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchFailsWithoutFinWhenOutboundClosesDuringDataOrFin) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto workerHandle = attachment.loop().handle();
    runWorkerTask(attachment, exerciseMailboxCloseStages(workerHandle, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchBoundsTunnelInputAcrossWorkerAndReleasesEveryReservation) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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

RUVIA_TEST(http3BufferedDispatchBackpressuresDataAndFinWithoutAcknowledgingUnpublishedBytes) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    Fixture fixture(workerHandle, upstream, 1, 2, 1);
    runWorkerTask(attachment, exerciseBackpressure(fixture, ruvia_ctx));
}

RUVIA_TEST(http3BufferedDispatchErrorHandlerFallbackAndAllocationFailureReleaseStorage) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(workerHandle, upstream);
        runWorkerTask(attachment, exerciseRepeatedRequestMemory(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3BufferedDispatchTreatsAbsentAndOmittedPeerFieldLimitsAsUnlimited) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(workerHandle, upstream);
        runWorkerTask(attachment, exerciseUnlimitedPeerFieldSectionSize(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3BufferedDispatchUsesDecodedPeerFieldSectionSizeIncludingStatus) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
        if (!requestEnded) {
            const auto fin = fixture.session.feed(streamId, {}, true);
            RUVIA_CHECK(fin.scope == ruvia::Http3ConnectionErrorScope::kNone);
        }
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
        fixture.routes.handlers.errorHandlerCalled = false;
    }
}

RUVIA_TEST(http3BufferedDispatchWebSocketHandshakeFailureAppliesRequiredHeaders) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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

RUVIA_TEST(http3BufferedDispatchWebSocketTunnelPublishesDataFinAndBoundsPmrStorage) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
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
