#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

#include <asio/system_executor.hpp>

#include "ruvia/core/BlockingPool.h"
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
#include "ruvia/http/HttpByteRange.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/HttpUdpTunnel.h"
#include "ruvia/web/detail/CallbackRef.h"

#include "http3/Http3BufferedRequestDispatch.h"
#include "http3/Http3ServerStreamInput.h"
#include "http3/http3_connection_state.h"
#include "memory_resource_fixture.h"
#include "router/Router.h"
#include "router/RouterImpl.h"
#include "routing_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace ruvia::testing {
Http3DatagramReceiveStatus plan_connect_datagram_for_peer(
    detail::http3_connection_identity identity,
    std::optional<detail::http3_stream_control> marker,
    std::uint64_t accepted_wire_bytes, std::span<const char> bytes);
}

namespace {

using Dispatch = ruvia::detail::Http3BufferedRequestDispatch;
using Engine = ruvia::detail::Http3SansIoSessionEngine;
using Input = ruvia::detail::Http3ServerStreamInput;
using buffer = ruvia::detail::http3_stream_buffer;
using MessageId = ruvia::detail::http3_stream_id;
using Control = ruvia::detail::http3_stream_control;
using BlockReason = Dispatch::PublishBlockReason;
using PublicationDemand = Dispatch::PublicationDemand;

constexpr std::uint64_t kEpoch = 37;
constexpr std::uint64_t kGeneration = 53;
using namespace std::chrono_literals;

struct HandlerState final {
    std::string responseBody{"buffered-h3-ok"};
    std::string response_trailer{"yes"};
    std::filesystem::path filePath;
    bool streamThrowAfterWrite{};
    bool sendInterim{};
    bool interimAfterFinalRejected{};
    bool streamRetainedStable{true};
    std::size_t uploadBytes{};
    std::size_t uploadChunks{};
    bool uploadTrailerObserved{};
    std::size_t handlerCalls{};
    std::size_t replay_safe_middleware_calls{};
    ruvia::http3_early_data_info early_data_info{};
    ruvia::WorkerSignal* started{};
    bool requestReadCorrectly{};
    bool handlerResumed{};
    bool errorHandlerCalled{};
    bool error_handler_response_ready{};
    std::string errorCode;
    bool allocateErrorHeaders{};
    bool throwFromErrorHandler{};
    bool response_no_transform{};
    bool error_no_transform{};
    bool first_error_no_transform{};
    std::string error_body{"handled-error"};
    std::size_t error_handler_calls{};
    std::size_t suspend_error_call{};
    ruvia::WorkerSignal* error_started{};
    std::string largeResponseHeader;
    ruvia::WorkerSignal* webSocketStarted{};
    ruvia::WorkerSignal* webSocketMessageReceived{};
    ruvia::WorkerSignal* webSocketMessageEchoed{};
    std::array<std::string, 2> webSocketMessages{};
    std::size_t webSocketEchoes{};
    bool webSocketSawFin{};
    bool webSocketThrowOnStart{};
    std::size_t udp_tunnel_starts{};
    bool webSocketRetainedDataStable{true};
    std::string tunnelReceived;
    bool tunnelReadAfterFinish{};
    bool tunnelReturnEarly{};
    bool tunnelStable{true};
};

inline ruvia::Task<ruvia::HttpResponse> bufferedHandler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<HandlerState*>(raw);
    ++state.handlerCalls;
    state.early_data_info = context.early_data_info();
    if (state.sendInterim) {
        std::string value(512, 'h');
        const std::array headers{ruvia::HttpHeaderView("Link", value)};
        auto operation = context.inform(ruvia::HttpInterimResponseHead(ruvia::http_status::kEarlyHints, headers));
        value.assign("changed");
        co_await std::move(operation);
    }
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
    if (path == "/multipart-file") {
        ruvia::HttpResponse response({.resource = context.arena()});
        auto ranges = ruvia::resolve_http_byte_range_set(
            "bytes=0-19999,30000-", state.responseBody.size());
        auto plan = ruvia::make_http_multipart_byte_range_plan(
            ranges, state.responseBody.size(), "text/plain", "h3_test_boundary", {},
            context.arena());
        response.status(ruvia::http_status::kPartialContent);
        response.header("Content-Type", plan.content_type());
        response.multipart_file_body(state.filePath, state.responseBody.size(),
            ruvia::HttpResponseFileIdentity::unchecked(), std::move(plan));
        co_return response;
    }
    if (path == "/file") {
        ruvia::HttpResponse response({.resource = context.arena()});
        if (state.filePath.empty()) {
            response.fileBody("virtual-response.bin", 5, 0, 5, ruvia::HttpResponseFileIdentity::checked({}));
        } else {
            response.fileBody(state.filePath, state.responseBody.size(), 0, state.responseBody.size(), ruvia::HttpResponseFileIdentity::unchecked());
        }
        co_return response;
    }
    if (path == "/empty-file") {
        ruvia::HttpResponse response({.resource = context.arena()});
        response.fileBody("virtual-empty-response.bin", 0, 0, 0, ruvia::HttpResponseFileIdentity::checked({}));
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
    if (path == "/upload") {
        auto& reader = context.req().bodyReader();
        while (auto bytes = co_await reader.read()) {
            state.uploadBytes += bytes->size();
            ++state.uploadChunks;
            if (state.started != nullptr) {
                state.started->notify();
            }
            if (std::ranges::any_of(*bytes, [](std::byte byte) { return byte != std::byte{'u'}; })) {
                throw std::runtime_error("upload chunk corrupted");
            }
        }
        state.uploadTrailerObserved = context.req().trailer("x-checksum") == "final" && context.req().header("x-checksum") == "initial";
    }
    context.header("x-dispatch", "buffered");
    if (state.response_no_transform) {
        context.header("cache-control", "no-transform");
    }
    if (!state.largeResponseHeader.empty()) {
        context.header("x-large-response", state.largeResponseHeader);
    }
    co_return context.text(std::string_view(state.responseBody));
}

inline ruvia::Task<void> bufferedWebSocketHandler(void* raw, ruvia::Context& context) {
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

inline ruvia::Task<void> bufferedTunnelHandler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<HandlerState*>(raw);
    if (state.tunnelReturnEarly) {
        co_return;
    }
    auto& tunnel = context.tunnel();
    std::optional<std::pmr::string> retained;
    if (state.tunnelReadAfterFinish) {
        co_await tunnel.finish();
    }
    while (auto chunk = co_await tunnel.read()) {
        state.tunnelReceived.append(*chunk);
        if (!retained) {
            retained.emplace(*chunk, context.pool());
        }
        if (!state.tunnelReadAfterFinish) {
            auto write = tunnel.write(std::string_view(*chunk));
            chunk->assign("mutated");
            co_await std::move(write);
        }
        state.tunnelStable = state.tunnelStable && retained->find_first_not_of('c') == std::string_view::npos;
    }
}

inline ruvia::Task<void> bufferedUdpTunnelHandler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<HandlerState*>(raw);
    ruvia::HttpUdpTunnel udp(context.tunnel().capsules());
    ++state.udp_tunnel_starts;
    if (state.tunnelReadAfterFinish) {
        co_await udp.finish();
    }
    std::optional<ruvia::HttpUdpDatagram> retained;
    while (auto packet = co_await udp.read()) {
        const auto bytes = packet->payload();
        state.tunnelReceived.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!state.tunnelReadAfterFinish) {
            co_await udp.send(bytes);
        }
        if (!retained) {
            retained = std::move(packet);
        }
        state.tunnelStable = state.tunnelStable && std::ranges::all_of(retained->payload(), [](std::byte byte) { return byte == std::byte{'c'}; });
    }
    co_await udp.finish();
}

inline ruvia::Task<void> responseStreamHandler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<HandlerState*>(raw);
    ++state.handlerCalls;
    const std::pmr::string retained(2048, 'r', context.pool());
    if (state.sendInterim) {
        const std::array headers{ruvia::HttpHeaderView("Link", std::string_view("</style.css>; rel=preload"))};
        {
            auto cold = context.inform(ruvia::HttpInterimResponseHead(ruvia::http_status::kEarlyHints, headers));
        }
        co_await context.inform(ruvia::HttpInterimResponseHead(ruvia::http_status::kEarlyHints, headers));
    }
    context.header("x-dispatch", "stream");
    for (unsigned i = 0; i < 4; ++i) {
        {
            auto cold = context.stream().write("discarded");
        }
        co_await context.stream().write(std::string_view(state.responseBody));
        if (state.sendInterim) {
            try {
                auto invalid = context.inform(ruvia::HttpInterimResponseHead(ruvia::http_status::kContinue));
            } catch (const std::logic_error&) {
                state.interimAfterFinalRejected = true;
            }
        }
        state.streamRetainedStable = state.streamRetainedStable && retained == std::pmr::string(2048, 'r', context.pool());
        if (state.streamThrowAfterWrite) {
            throw std::runtime_error("stream failed after head");
        }
    }
    const std::array trailers{ruvia::HttpHeaderView("x-complete", state.response_trailer)};
    co_await context.stream().end(trailers);
}

struct ReplaySafeRouteMiddleware final : ruvia::Middleware {
    static constexpr bool ruvia_replay_safe = true;
    HandlerState* state{};

    explicit ReplaySafeRouteMiddleware(HandlerState* state)
        : state(state) {}
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        ++state->replay_safe_middleware_calls;
        co_await next();
    }
};

struct Routes final {
    ruvia::detail::Router router;
    ruvia::detail::RouterImpl& implementation{ruvia::detail::RouterImpl::from(router)};
    HandlerState handlers;
    ruvia::HttpErrorHandler errorHandler;

    explicit Routes(std::chrono::milliseconds peerTransportFinTimeout = 5s)
        : errorHandler([this](ruvia::Context& context, ruvia::HttpErrorInfo error)
                           -> ruvia::Task<ruvia::HttpResponse> {
              handlers.errorHandlerCalled = true;
              ++handlers.error_handler_calls;
              handlers.errorCode.assign(error.code());
              if (handlers.throwFromErrorHandler) {
                  throw std::runtime_error("error handler failed");
              }
              context.status(error.status());
              context.header("x-error-handler", "used");
              if (handlers.error_no_transform ||
                  (handlers.first_error_no_transform && handlers.error_handler_calls == 1)) {
                  context.header("cache-control", "no-transform");
              }
              if (handlers.suspend_error_call == handlers.error_handler_calls) {
                  handlers.error_started->notify();
                  (void)co_await ruvia::sleepFor(context.worker(), 10s, context.stopToken());
              }
              if (handlers.allocateErrorHeaders) {
                  for (unsigned i = 0; i < 32; ++i) {
                      context.header("x-owned-error-" + std::to_string(i), std::string_view(handlers.responseBody));
                  }
              }
              auto response = context.text(std::string_view(handlers.error_body));
              handlers.error_handler_response_ready = true;
              co_return response;
          }) {
        implementation.setErrorHandler(ruvia::detail::CallbackAccess::ref(errorHandler));
        add(ruvia::HttpKnownMethod::kPost, "/items");
        add(ruvia::HttpKnownMethod::kGet, "/throw");
        add(ruvia::HttpKnownMethod::kGet, "/suspend");
        add(ruvia::HttpKnownMethod::kGet, "/file");
        add(ruvia::HttpKnownMethod::kGet, "/multipart-file");
        add(ruvia::HttpKnownMethod::kGet, "/empty-file");
        add(ruvia::HttpKnownMethod::kGet, "/large");
        const auto replaySafeMiddleware =
            ruvia::detail::makeMiddlewareDescriptor<ReplaySafeRouteMiddleware>(&handlers);
        implementation.registerRoute(ruvia::HttpKnownMethod::kGet, routing_test::path("/early-safe"),
            ruvia::detail::RouteHandler(&handlers, &bufferedHandler),
            ruvia::detail::RequestBodyMode::kBuffered, {},
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>(
                &replaySafeMiddleware, 1));
        implementation.registerRoute(ruvia::HttpKnownMethod::kPost, routing_test::path("/upload"),
            ruvia::detail::RouteHandler(&handlers, &bufferedHandler), ruvia::detail::RequestBodyMode::kStream, {}, {});
        implementation.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet, routing_test::path("/stream"),
            ruvia::detail::RouteStreamHandler(&handlers, &responseStreamHandler), {}, {});
        ruvia::WebSocketRouteConfig webSocketConfig;
        webSocketConfig.lifecycle.peerTransportFinTimeout = peerTransportFinTimeout;
        implementation.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
            routing_test::path("/socket"),
            ruvia::detail::RouteStreamHandler(&handlers, &bufferedWebSocketHandler),
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::move(webSocketConfig));
        implementation.registerTunnelRoute({}, std::pmr::string("backend.test:443"),
            ruvia::detail::RouteStreamHandler(&handlers, &bufferedTunnelHandler), {}, {}, {.peerTransportFinTimeout = peerTransportFinTimeout});
        implementation.registerTunnelRoute("test-tunnel", std::pmr::string("/tunnel/:destination"),
            ruvia::detail::RouteStreamHandler(&handlers, &bufferedTunnelHandler), {}, {}, {.peerTransportFinTimeout = peerTransportFinTimeout});
        implementation.registerTunnelRoute("connect-udp", std::pmr::string("/udp/:destination"),
            ruvia::detail::RouteStreamHandler(&handlers, &bufferedUdpTunnelHandler), {}, {}, {.peerTransportFinTimeout = peerTransportFinTimeout});
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
    buffer inbound;
    Input input;
    buffer outbound;
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
        std::chrono::milliseconds scannerInterval = 1ms, std::size_t maxNativePayloadBytes = 0)
        : routes(peerTransportFinTimeout),
          upstream(upstream),
          allocations(upstream),
          worker(allocations),
          workerStop(workerStopSource.token()),
          services(workerHandle, workerStop),
          bodyBudget(workerBodyBudget),
          session(routes.implementation.routeTable(), worker, bodyBudget,
              {.maxTunnelBufferedBytes = tunnelBytesPerStream,
                  .connection = {.enableConnectProtocol = true, .enableDatagrams = maxNativePayloadBytes != 0},
                  .maxQuicDatagramPayloadBytes = maxNativePayloadBytes}),
          inbound(8, 8, 4, worker.resource()),
          input(session, worker, kEpoch, kGeneration, 32),
          outbound(outboundBlocks, outboundDataSlots, outboundControlSlots, worker.resource()),
          scanner(workerHandle, {.scanInterval = scannerInterval}) {}

    [[nodiscard]] Dispatch makeDispatch(std::uint64_t streamId,
        ruvia::detail::ContextServices dispatchServices,
        ruvia::detail::Http3TunnelCallbacks callbacks = {}, bool receivedEarlyData = false) {
        return Dispatch(session, routes.implementation.routeTable(), worker,
            std::move(dispatchServices), options, outbound,
            {kEpoch, kGeneration, streamId, {}, receivedEarlyData},
            scannerEntry, executor, callbacks);
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

inline std::string frame(std::uint64_t type, std::string_view payload) {
    std::array<char, ruvia::kHttp3FrameHeaderMaxBytes> header{};
    const auto size = ruvia::encodeHttp3FrameHeader(header, type, payload.size());
    if ((size.index() != 0)) {
        throw std::runtime_error("HTTP/3 fixture frame encoding failed");
    }
    std::string result(header.data(), std::get<0>(size));
    result.append(payload);
    return result;
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
    if ((encoded.index() != 0)) {
        throw std::runtime_error("HTTP/3 fixture request-head encoding failed");
    }
    std::string result = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(std::get<0>(encoded).fieldSection.data(), std::get<0>(encoded).fieldSection.size()));
    if (!body.empty()) {
        result += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), body);
    }
    return result;
}

inline bool sendAccepted(buffer::send_result result) noexcept {
    return result == buffer::send_result::sent;
}

inline bool controlAccepted(buffer::control_result result) noexcept {
    return result == buffer::control_result::sent;
}

inline void feedWebSocketRequest(Engine& session, ruvia::WorkerMemory& worker,
    std::uint64_t streamId, std::string_view version = "13",
    std::string_view accept_encoding = {}) {
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/socket"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", version},
        ruvia::Http3FieldSectionFieldView{"accept-encoding", accept_encoding}};
    std::pmr::vector<ruvia::Http3FieldSectionFieldView> fieldsToEncode(worker.resource());
    for (const auto& field : fields) {
        if ((field.name == "sec-websocket-version" && version.empty()) ||
            (field.name == "accept-encoding" && accept_encoding.empty())) {
            continue;
        }
        fieldsToEncode.push_back(field);
    }
    const auto fieldSection = ruvia::encodeHttp3FieldSection(fieldsToEncode, worker.resource());
    if ((fieldSection.index() != 0)) {
        throw std::runtime_error("HTTP/3 WebSocket fixture field section encoding failed");
    }
    const std::string wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(std::get<0>(fieldSection).data(), std::get<0>(fieldSection).size()));
    const auto result = session.feed(streamId, wire);
    if (result.scope != ruvia::Http3ConnectionErrorScope::kNone ||
        session.streamState(streamId) != Engine::StreamState::kReady) {
        throw std::runtime_error("HTTP/3 WebSocket fixture request was rejected");
    }
}

inline void feedWebSocketRequest(Fixture& fixture, std::uint64_t streamId) {
    feedWebSocketRequest(fixture.session, fixture.worker, streamId);
}

inline void feedRequest(Fixture& fixture, std::uint64_t streamId, std::string_view method,
    std::string_view path, std::string_view body = {},
    std::span<const ruvia::Http3FieldSectionFieldView> fields = {},
    bool receivedEarlyData = false) {
    const MessageId id{kEpoch, kGeneration, streamId, {}, receivedEarlyData};
    const auto wire = requestWire(fixture.worker, method, path, body, fields);
    if (wire.size() > buffer::max_block_bytes) {
        throw std::runtime_error("HTTP/3 fixture request exceeds one buffer block");
    }
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!sendAccepted(fixture.inbound.try_send(id, bytes)) ||
        !controlAccepted(fixture.inbound.try_send_control(
            {Control::kind::stream_fin, id, wire.size()}))) {
        throw std::runtime_error("HTTP/3 fixture inbound buffer is full");
    }

    Control fin;
    if (!fixture.inbound.try_receive_control(fin) ||
        fixture.input.acceptControl(fin).status != Input::Status::kDeferredFin) {
        throw std::runtime_error("HTTP/3 fixture FIN was not deferred");
    }
    buffer::borrowed_block block;
    if (!fixture.inbound.try_receive(block) || block.id().stream_id != streamId) {
        throw std::runtime_error("HTTP/3 fixture DATA block is unavailable");
    }
    const auto fed = fixture.input.acceptData(block);
    block.release();
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

inline std::size_t drainDataOnly(buffer& buffer, const MessageId& expected, PublishedWire& output) {
    std::size_t received = 0;
    buffer::borrowed_block block;
    while (buffer.try_receive(block)) {
        if (block.id().epoch != expected.epoch ||
            block.id().connection_generation != expected.connection_generation ||
            block.id().stream_id != expected.stream_id) {
            output.identityMatched = false;
        }
        const auto bytes = block.bytes();
        output.bytes.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ++output.dataBlocks;
        ++received;
        block.release();
    }
    return received;
}

inline void drain_buffer(buffer& buffer, const MessageId& expected, PublishedWire& output) {
    bool again = false;
    do {
        Control control;
        while (buffer.try_receive_control(control)) {
            if (control.kind == Control::kind::stream_fin &&
                control.id.epoch == expected.epoch &&
                control.id.connection_generation == expected.connection_generation &&
                control.id.stream_id == expected.stream_id) {
                output.finalWireBytes = control.value;
            }
        }
        (void)drainDataOnly(buffer, expected, output);
        again = buffer.has_pending();
    } while (again);
}

struct DecodedResponse final {
    std::size_t finalHeads{};
    std::size_t interimHeads{};
    std::string earlyLink;
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
    std::string contentType;
    std::string content_encoding;
    std::string body;
    std::string completeTrailer;
};

inline void onResponse(void* raw, const ruvia::Http3ClientResponseEvent& event) {
    auto& response = *static_cast<DecodedResponse*>(raw);
    switch (event.kind) {
        case ruvia::Http3ClientResponseEventKind::kFinalHead:
            ++response.finalHeads;
            response.status = event.head->status;
            response.contentLength = event.head->contentLength;
            response.decodedFieldSectionSize = 7 + std::to_string(event.head->status).size() + 32;
            for (const auto& header : event.head->headers) {
                response.decodedFieldSectionSize += header.name.size() + header.value.size() + 32;
                if (header.name == "content-type") {
                    response.contentType = header.value;
                } else if (header.name == "content-encoding") {
                    response.content_encoding = header.value;
                } else if (header.name == "x-dispatch") {
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
            ++response.interimHeads;
            for (const auto& header : event.head->headers) {
                if (header.name == "link") {
                    response.earlyLink.assign(header.value);
                }
            }
            break;
        case ruvia::Http3ClientResponseEventKind::kTunnelData:
        case ruvia::Http3ClientResponseEventKind::kReset:
        case ruvia::Http3ClientResponseEventKind::kPushPromise:
            break;
        case ruvia::Http3ClientResponseEventKind::kTrailerField:
            if (event.trailer.name == "x-complete") {
                response.completeTrailer.assign(event.trailer.value);
            }
            break;
    }
}

inline ruvia::Http3ClientResponseResult decodePublished(const PublishedWire& wire,
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

inline void publishAndDrain(Dispatch& dispatch, Fixture& fixture, std::uint64_t streamId,
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
                    RUVIA_CHECK_EQ(dispatch.publishedWireBytes(), before);
                }
            }
            drain_buffer(fixture.outbound, id, output);
        } else if (result.status == Dispatch::PublishStatus::kBackpressured) {
            RUVIA_CHECK(result.blockReason == BlockReason::kData);
            drain_buffer(fixture.outbound, id, output);
        } else if (result.status == Dispatch::PublishStatus::kFinPublished) {
            RUVIA_CHECK(result.blockReason == BlockReason::kNone);
            drain_buffer(fixture.outbound, id, output);
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
    drain_buffer(fixture.outbound, id, output);
}

inline ruvia::Task<void> runOwner(Dispatch& dispatch, Dispatch::RunStatus& result, bool& joined,
    ruvia::WorkerSignal& finished) {
    result = co_await dispatch.runHandler();
    joined = true;
    finished.notify();
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

inline void feedPeerSettings(Fixture& fixture, std::optional<std::uint64_t> maxFieldSectionSize,
    bool nativeDatagrams = false) {
    std::array<char, 64> payload{};
    ruvia::Http3Settings settings;
    settings.maxFieldSectionSize = maxFieldSectionSize;
    settings.h3Datagram = nativeDatagrams;
    const auto encoded = ruvia::encodeHttp3Settings(payload, settings);
    if ((encoded.index() != 0)) {
        throw std::runtime_error("HTTP/3 fixture SETTINGS encoding failed");
    }
    std::string controlWire(1, '\0');  // Peer unidirectional control stream type.
    controlWire += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kSettings),
        std::string_view(payload.data(), std::get<0>(encoded)));
    const auto result = fixture.session.feed(2, controlWire);
    if (result.scope != ruvia::Http3ConnectionErrorScope::kNone ||
        result.status != ruvia::Http3ConnectionStatus::kNeedMoreData) {
        throw std::runtime_error("HTTP/3 fixture peer SETTINGS was rejected");
    }
}

}  // namespace
