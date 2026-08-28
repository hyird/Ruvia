#pragma once

#include "test_harness.h"

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/web/detail/http/StreamingAccess.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamState.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/Timer.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/RateLimit.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/websocket/WebSocketAccess.h"

RUVIA_REQUEST_MODEL(ScopedValidationRequest, RUVIA_OPTIONAL_FIELD(value, ruvia::String));

namespace routing_test {

using ruvia::HttpKnownMethod;
using ruvia::detail::ControllerMiddlewareDescriptor;
using ruvia::detail::RequestBodyMode;
using ruvia::detail::RouteHandler;
using ruvia::detail::RouteMatch;

template <typename T>
concept ExposesRvalueRouteListIterator = requires(T&& list) { std::move(list).begin(); } || requires(T&& list) { std::move(list).end(); };

template <typename String>
concept AcceptsTemporaryRoutePath = requires(String&& path) { ruvia::detail::RuviaPathList(std::forward<String>(path)); };

static_assert(!ExposesRvalueRouteListIterator<ruvia::detail::RuviaMethodList>);
static_assert(!ExposesRvalueRouteListIterator<ruvia::detail::RuviaPathList>);
static_assert(!AcceptsTemporaryRoutePath<std::string>);
static_assert(!AcceptsTemporaryRoutePath<const std::string>);
static_assert(!AcceptsTemporaryRoutePath<std::pmr::string>);

class FirstIntValidator final : public ruvia::Middleware<FirstIntValidator> {
public:
    using RuviaValidationBody = int;

    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next&) {
        co_return;
    }
};

class SecondIntValidator final : public ruvia::Middleware<SecondIntValidator> {
public:
    using RuviaValidationBody = int;

    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next&) {
        co_return;
    }
};

class ScopedValidationValidator final : public ruvia::Middleware<ScopedValidationValidator> {
public:
    RUVIA_VALIDATE_JSON(ScopedValidationRequest, RUVIA_RULE(value, RUVIA_REQUIRED("value is required")))
};

class ValidationScopeProbe final : public ruvia::Middleware<ValidationScopeProbe> {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        co_await next();
        try {
            (void)context.req().validated<ScopedValidationRequest>();
        } catch (const std::logic_error&) {
            releasedAfterNext = true;
        }
    }

    static inline bool releasedAfterNext{false};
};

inline bool scopedValidationHandlerRead{false};
inline bool scopedValidationRawRead{false};
inline bool scopedValidationHandlerThrows{false};

inline ruvia::Task<ruvia::HttpResponse> scopedValidationHandler(void*, ruvia::Context& context) {
    const auto& model = context.req().validated<ScopedValidationRequest>();
    const auto json = context.req().validatedJson<ScopedValidationRequest>();
    scopedValidationHandlerRead = model.get<"value">().has_value() && model.get<"value">()->view() == "ok";
    scopedValidationRawRead = &json.value() == &model && json.raw() == R"({"value":"ok"})";
    if (scopedValidationHandlerThrows) {
        throw std::runtime_error("validated handler failure");
    }
    co_return context.text("validated");
}

// Never invoked — resolve() only needs a registered route with a valid handler.
inline ruvia::Task<ruvia::HttpResponse> dummyHandler(void*, ruvia::Context&) {
    co_return ruvia::HttpResponse({.resource = std::pmr::get_default_resource()});
}

inline ruvia::Task<void> dummyStreamHandler(void*, ruvia::Context&) {
    co_return;
}

inline std::pmr::string path(std::string_view value) {
    return std::pmr::string(value, std::pmr::get_default_resource());
}

inline void addRoute(ruvia::detail::RouterImpl& impl, HttpKnownMethod method, std::string_view route) {
    impl.registerRoute(method, path(route), RouteHandler(nullptr, &dummyHandler), RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{});
}

inline void addRoute(ruvia::detail::RouterImpl& impl, std::string_view route) {
    addRoute(impl, HttpKnownMethod::kGet, route);
}

// Registers the given routes and reports whether finalize() rejects them as a
// dynamic route-shape conflict.
inline bool finalizeConflicts(std::initializer_list<std::string_view> routes) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    for (const auto route : routes) {
        addRoute(impl, route);
    }
    try {
        impl.finalize();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

struct Router final {
    ruvia::detail::Router router;
    ruvia::detail::RouterImpl& impl = ruvia::detail::RouterImpl::from(router);

    void finalize() {
        impl.finalize();
    }

    bool matches(std::string_view p) {
        const auto resolution = impl.routeTable().resolve(HttpKnownMethod::kGet, p);
        return resolution.resolved() != nullptr;
    }

    std::string_view routePathOf(std::string_view p) {
        return routePathOf(HttpKnownMethod::kGet, p);
    }

    std::string_view routePathOf(HttpKnownMethod method, std::string_view p) {
        const auto res = impl.routeTable().resolve(method, p);
        const auto* resolved = res.resolved();
        return resolved != nullptr ? resolved->route().path() : "<none>";
    }

    // Returns the single captured param value, or "<none>" if unmatched / no param.
    std::string_view paramOf(std::string_view p) {
        const auto res = impl.routeTable().resolve(HttpKnownMethod::kGet, p);
        const auto* resolved = res.resolved();
        if (resolved == nullptr || resolved->match().size() != 1) {
            return "<none>";
        }
        return resolved->match().values()[0];
    }
};

}  // namespace routing_test

namespace routing_test {

// Records the order middlewares and the handler run: positive on entry (before
// next()), negative on unwind (after next()), 0 for the handler.
inline std::vector<int> g_chainOrder;

class ChainMwA final : public ruvia::Middleware<ChainMwA> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        g_chainOrder.push_back(1);
        co_await next();
        g_chainOrder.push_back(-1);
    }
};

class ChainMwB final : public ruvia::Middleware<ChainMwB> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        g_chainOrder.push_back(2);
        co_await next();
        g_chainOrder.push_back(-2);
    }
};

class ChainMwOverrideAfterNext final : public ruvia::Middleware<ChainMwOverrideAfterNext> {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        co_await next();
        context.respond(context.body("override"));
    }
};

// Short-circuits: sets a response and does NOT call next().
class ChainMwStop final : public ruvia::Middleware<ChainMwStop> {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next&) {
        g_chainOrder.push_back(9);
        context.respond(context.body("stopped"));
        co_return;
    }
};

// Misuse: respond() ends the middleware chain, so a later next() must not run
// downstream handlers and silently replace the response.
class ChainMwRespondThenNext final : public ruvia::Middleware<ChainMwRespondThenNext> {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        context.respond(context.body("early"));
        co_await next();
    }
};

// Misuse: calls next() twice. The second invocation must be rejected rather than
// re-entering the downstream chain (which would run the handler -- and its side
// effects -- a second time).
class ChainMwDoubleNext final : public ruvia::Middleware<ChainMwDoubleNext> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        co_await next();
        co_await next();
    }
};

inline bool g_webSocketUnavailableAfterNext = false;

class ChainMwProbeWebSocketAfterNext final : public ruvia::Middleware<ChainMwProbeWebSocketAfterNext> {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        co_await next();
        try {
            (void)context.webSocket();
        } catch (const std::logic_error&) {
            g_webSocketUnavailableAfterNext = true;
        }
    }
};

class ChainMwThrowsAfterNext final : public ruvia::Middleware<ChainMwThrowsAfterNext> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        co_await next();
        throw std::runtime_error("middleware post failed");
    }
};

// Throws before calling next(): the chain is short-circuited and the exception
// must be mapped to an error response (never escaping the dispatch).
class ChainMwThrows final : public ruvia::Middleware<ChainMwThrows> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next&) {
        throw ruvia::HttpError({.status = ruvia::http_status::kUnauthorized, .code = "mw_rejected", .message = "middleware rejected the request"});
        co_return;  // unreachable
    }
};

inline ruvia::Task<ruvia::HttpResponse> chainHandler(void*, ruvia::Context& context) {
    g_chainOrder.push_back(0);
    co_return context.body("ok");
}

inline std::string dispatchChain(std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares = {}) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpKnownMethod::kGet, path("/chain"), RouteHandler(nullptr, &chainHandler), RequestBodyMode::kBuffered, controllerMiddlewares, routeMiddlewares);
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setPath(request, "/chain");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})), asio::use_future);
    ctx.run();
    auto response = future.get();
    const auto body = ruvia::detail::responseBody(response).bytes();
    return std::string(body.data(), body.size());
}

// A capture sink whose committed flag flips true on the first write, mirroring the
// real streaming sink (which commits the head before the first body chunk).
struct StreamCaptureSink final {
    std::pmr::string scratch{std::pmr::get_default_resource()};
    bool committedFlag = false;
    bool endedFlag = false;
    bool contextReleased = false;
    std::vector<std::string> writes;
};

inline ruvia::Task<void> scWrite(void* target, std::string_view chunk) {
    auto* sink = static_cast<StreamCaptureSink*>(target);
    sink->committedFlag = true;
    sink->writes.emplace_back(chunk);
    co_return;
}
inline ruvia::Task<void> scEnd(void* target, std::span<const ruvia::HttpHeaderView>) {
    auto* sink = static_cast<StreamCaptureSink*>(target);
    sink->committedFlag = true;
    sink->endedFlag = true;
    co_return;
}
inline ruvia::Task<ruvia::TimerSleepResult> scSleep(void*, std::chrono::milliseconds, const ruvia::StopToken&) {
    co_return ruvia::TimerSleepResult::kElapsed;
}
inline void scBind(void*, ruvia::Context*, ruvia::HttpResponse (*)(ruvia::Context&)) noexcept {}
inline void scReleaseContext(void* target) noexcept {
    static_cast<StreamCaptureSink*>(target)->contextReleased = true;
}
inline bool scCommitted(void* target) noexcept {
    return static_cast<StreamCaptureSink*>(target)->committedFlag;
}
inline bool scAborted(void*) noexcept {
    return false;
}

inline ruvia::ResponseStreamWriter scMakeWriter(StreamCaptureSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(&sink, &scWrite, &scEnd, &scSleep, &scBind, &scReleaseContext, &scCommitted, &scAborted);
}

struct EmptyStreamDispatchObservation final {
    bool handled{false};
    bool buffered{false};
    bool threw{false};
    bool ended{false};
    bool committed{false};
    bool contextReleased{false};
    std::string bufferedBody;
};

inline EmptyStreamDispatchObservation dispatchEmptyStreamWith(const ControllerMiddlewareDescriptor& middleware) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerResponseStreamRoute(HttpKnownMethod::kGet, path("/empty-stream"), ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler), std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>(&middleware, 1));
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setPath(request, "/empty-stream");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    const auto resolution = table.resolve(HttpKnownMethod::kGet, "/empty-stream");
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::logic_error("empty stream test route did not resolve");
    }

    StreamCaptureSink sink;
    auto writer = scMakeWriter(sink);
    EmptyStreamDispatchObservation observation;
    asio::io_context context(1);
    asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            try {
                auto result = co_await ruvia::detail::taskAsAwaitable(table.dispatchResponseStream(request, *resolved, memory, writer, {}));
                observation.handled = !result.has_value();
                if (result.has_value()) {
                    observation.buffered = true;
                    auto response = std::move(*result);
                    const auto body = ruvia::detail::responseBody(response).bytes();
                    observation.bufferedBody.assign(body.data(), body.size());
                }
            } catch (...) {
                observation.threw = true;
            }
        },
        asio::detached);
    context.run();
    observation.ended = sink.endedFlag;
    observation.committed = sink.committedFlag;
    observation.contextReleased = sink.contextReleased;
    return observation;
}

struct WebSocketDispatchObservation final {
    bool terminalInvoked{false};
    bool capabilityAvailableInTerminal{false};
    bool buffered{false};
    std::string bufferedBody;
};

struct WebSocketTerminalTarget final {
    WebSocketDispatchObservation* observation;
    ruvia::WebSocket* webSocket;
};

inline ruvia::Task<void> webSocketTerminal(void* target, ruvia::Context& context) {
    auto& terminal = *static_cast<WebSocketTerminalTarget*>(target);
    terminal.observation->terminalInvoked = true;
    ruvia::detail::ContextWebSocketBinding binding(context, *terminal.webSocket);
    terminal.observation->capabilityAvailableInTerminal = &context.webSocket() == terminal.webSocket;
    g_chainOrder.push_back(0);
    co_return;
}

inline WebSocketDispatchObservation dispatchWebSocketWith(const ControllerMiddlewareDescriptor& middleware) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerWebSocketRoute(HttpKnownMethod::kGet, path("/ws-middleware"), ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler), std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>(&middleware, 1), {});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setPath(request, "/ws-middleware");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    const auto resolution = table.resolve(HttpKnownMethod::kGet, "/ws-middleware");
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::logic_error("websocket middleware test route did not resolve");
    }

    WebSocketDispatchObservation observation;
    auto webSocket = ruvia::detail::WebSocketAccess::make(nullptr, nullptr, nullptr, nullptr);
    WebSocketTerminalTarget terminalTarget{&observation, &webSocket};
    const auto terminal = ruvia::detail::RouteStreamHandler(&terminalTarget, &webSocketTerminal);
    asio::io_context context(1);
    auto future = asio::co_spawn(context, ruvia::detail::taskAsAwaitable(table.dispatchWebSocket(request, *resolved, memory, terminal, {})), asio::use_future);
    context.run();
    auto response = future.get();
    if (response.has_value()) {
        observation.buffered = true;
        const auto body = ruvia::detail::responseBody(*response).bytes();
        observation.bufferedBody.assign(body.data(), body.size());
    }
    return observation;
}

// Writes a chunk (committing the stream) then throws mid-body.
inline ruvia::Task<void> streamCommitThenThrow(void*, ruvia::Context& context) {
    co_await context.stream().write("partial");
    throw std::runtime_error("mid-stream handler failure");
}

}  // namespace routing_test

namespace routing_test {

// Simulates the real sinks' body-suppressed commit for an explicit HEAD stream
// route: the head commits, then the first body write raises the head-only
// completion signal instead of accepting the chunk.
inline bool g_headOnlyHandlerResumedPastFirstWrite = false;

inline ruvia::Task<void> headOnlyWrite(void* target, std::string_view) {
    static_cast<StreamCaptureSink*>(target)->committedFlag = true;
    throw ruvia::detail::ResponseStreamHeadOnlyComplete();
    co_return;  // unreachable
}

inline ruvia::ResponseStreamWriter makeHeadOnlyWriter(StreamCaptureSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(&sink, &headOnlyWrite, &scEnd, &scSleep, &scBind, &scReleaseContext, &scCommitted, &scAborted);
}

inline ruvia::Task<void> headOnlyProbeStreamHandler(void*, ruvia::Context& context) {
    g_chainOrder.push_back(0);
    co_await context.stream().write("event-1");
    g_headOnlyHandlerResumedPastFirstWrite = true;
    co_await context.stream().write("event-2");
}

struct HeadOnlyDispatchObservation final {
    bool handled{false};
    bool buffered{false};
    bool threw{false};
    bool ended{false};
};

inline HeadOnlyDispatchObservation dispatchHeadOnlyStream(std::span<const ControllerMiddlewareDescriptor> middlewares) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerResponseStreamRoute(HttpKnownMethod::kHead, path("/head-only-stream"), ruvia::detail::RouteStreamHandler(nullptr, &headOnlyProbeStreamHandler), std::span<const ControllerMiddlewareDescriptor>{}, middlewares);
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "HEAD");
    ruvia::detail::HttpRequestAccess::setPath(request, "/head-only-stream");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    const auto resolution = table.resolve(HttpKnownMethod::kHead, "/head-only-stream");
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::logic_error("head-only stream test route did not resolve for HEAD");
    }

    StreamCaptureSink sink;
    auto writer = makeHeadOnlyWriter(sink);
    HeadOnlyDispatchObservation observation;
    asio::io_context context(1);
    asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            try {
                auto result = co_await ruvia::detail::taskAsAwaitable(table.dispatchResponseStream(request, *resolved, memory, writer, {}));
                observation.handled = !result.has_value();
                observation.buffered = result.has_value();
            } catch (...) {
                observation.threw = true;
            }
        },
        asio::detached);
    context.run();
    observation.ended = sink.endedFlag;
    return observation;
}

}  // namespace routing_test

namespace routing_test {

inline ruvia::Task<ruvia::HttpResponse> throwsHttpErrorHandler(void*, ruvia::Context&) {
    throw ruvia::HttpError({.status = ruvia::http_status::kForbidden, .code = "forbidden", .message = "nope"});
    co_return ruvia::HttpResponse({.resource = std::pmr::get_default_resource()});  // unreachable
}

inline ruvia::Task<ruvia::HttpResponse> throwsGenericHandler(void*, ruvia::Context&) {
    throw std::runtime_error("boom");
    co_return ruvia::HttpResponse({.resource = std::pmr::get_default_resource()});  // unreachable
}

inline ruvia::Task<ruvia::HttpResponse> throwsInvalidArgumentHandler(void*, ruvia::Context&) {
    throw std::invalid_argument("application bug");
    co_return ruvia::HttpResponse({.resource = std::pmr::get_default_resource()});  // unreachable
}

inline ruvia::Task<ruvia::HttpResponse> throwsProtocolErrorHandler(void*, ruvia::Context&) {
    throw ruvia::HttpProtocolError(ruvia::http_status::kContentTooLarge, "request body is too large");
    co_return ruvia::HttpResponse({.resource = std::pmr::get_default_resource()});  // unreachable
}

inline ruvia::Task<ruvia::HttpResponse> okHandler(void*, ruvia::Context& context) {
    co_return context.body("ok");
}

inline ruvia::Task<ruvia::HttpResponse> readsRequestBodyHandler(void*, ruvia::Context& context) {
    (void)co_await context.req().text();
    co_return context.body("ok");
}

// The dispatched response's storage lives in the per-request arena, which is
// destroyed when the helper returns -- so values must be copied out here, while
// the arena is still alive, rather than returning the HttpResponse itself.
struct DispatchResult final {
    std::uint16_t status{0};
    std::string body;
    std::string allow;
    std::string connection;
    std::string acceptEncoding;
};

inline DispatchResult extractDispatchResult(const ruvia::HttpResponse& response) {
    DispatchResult result;
    result.status = response.status().value();
    const auto body = ruvia::detail::responseBody(response).bytes();
    result.body.assign(body.data(), body.size());
    const auto allow = response.header("Allow").value_or(std::string_view{});
    result.allow.assign(allow.data(), allow.size());
    const auto connection = response.header("Connection").value_or(std::string_view{});
    result.connection.assign(connection.data(), connection.size());
    const auto acceptEncoding = response.header("Accept-Encoding").value_or(std::string_view{});
    result.acceptEncoding.assign(acceptEncoding.data(), acceptEncoding.size());
    return result;
}

// Registers GET /x with `handler`, dispatches the exact wire method token and
// path, then returns the rendered result.
inline DispatchResult dispatchOneToken(RouteHandler handler, std::string_view method, std::string_view p, std::string_view contentEncoding = {}, std::string_view body = {}) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpKnownMethod::kGet, path("/x"), handler, RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, method);
    ruvia::detail::HttpRequestAccess::setPath(request, p);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    if (!contentEncoding.empty()) {
        const auto slot = ruvia::detail::HttpRequestAccess::knownHeaderSlot(ruvia::detail::RequestKnownHeader::kContentEncoding);
        (void)ruvia::detail::HttpRequestAccess::addHeader(request, ruvia::HttpHeaderView{"Content-Encoding", contentEncoding}, slot);
    }
    ruvia::detail::HttpRequestAccess::setBody(request, body);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})), asio::use_future);
    ctx.run();
    return extractDispatchResult(future.get());  // arena still alive here
}

inline DispatchResult dispatchOne(RouteHandler handler, HttpKnownMethod method, std::string_view p) {
    return dispatchOneToken(handler, ruvia::knownHttpMethodToken(method), p);
}

}  // namespace routing_test

namespace routing_test {

using ruvia::HttpErrorHandler;
using ruvia::HttpErrorInfo;
using ruvia::HttpNotFoundHandler;

inline ruvia::Task<ruvia::HttpResponse> customNotFound(ruvia::Context& context) {
    context.status(ruvia::http_status::kNotFound);
    co_return context.body("custom-not-found");
}

inline ruvia::Task<ruvia::HttpResponse> customError(ruvia::Context& context, HttpErrorInfo info) {
    context.status(info.status());
    co_return context.body("custom-error");
}

inline DispatchResult dispatchWithHandlersToken(RouteHandler handler, const HttpErrorHandler& errorH, const HttpNotFoundHandler& notFoundH, std::string_view method, std::string_view p, std::string_view contentEncoding = {}, std::string_view body = {}) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    if (errorH != nullptr) {
        impl.setErrorHandler(ruvia::detail::CallbackAccess::ref(errorH));
    }
    if (notFoundH != nullptr) {
        impl.setNotFoundHandler(ruvia::detail::CallbackAccess::ref(notFoundH));
    }
    impl.registerRoute(HttpKnownMethod::kGet, path("/x"), handler, RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, method);
    ruvia::detail::HttpRequestAccess::setPath(request, p);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    if (!contentEncoding.empty()) {
        const auto slot = ruvia::detail::HttpRequestAccess::knownHeaderSlot(ruvia::detail::RequestKnownHeader::kContentEncoding);
        (void)ruvia::detail::HttpRequestAccess::addHeader(request, ruvia::HttpHeaderView{"Content-Encoding", contentEncoding}, slot);
    }
    ruvia::detail::HttpRequestAccess::setBody(request, body);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})), asio::use_future);
    ctx.run();
    return extractDispatchResult(future.get());  // arena still alive here
}

inline DispatchResult dispatchWithHandlers(RouteHandler handler, const HttpErrorHandler& errorH, const HttpNotFoundHandler& notFoundH, HttpKnownMethod method, std::string_view p) {
    return dispatchWithHandlersToken(handler, errorH, notFoundH, ruvia::knownHttpMethodToken(method), p);
}

}  // namespace routing_test

namespace routing_test {

inline ruvia::Task<ruvia::HttpResponse> apiScopedNotFound(ruvia::Context& context) {
    context.status(ruvia::http_status::kNotFound);
    co_return context.body("api-scope-404");
}

inline ruvia::Task<ruvia::HttpResponse> v2ScopedNotFound(ruvia::Context& context) {
    context.status(ruvia::http_status::kNotFound);
    co_return context.body("v2-scope-404");
}

inline ruvia::Task<ruvia::HttpResponse> apiScopedError(ruvia::Context& context, HttpErrorInfo info) {
    context.status(info.status());
    co_return context.body("api-scope-error");
}

inline DispatchResult dispatchOn(const ruvia::detail::RouteTable& table, std::string_view method, std::string_view p) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, method);
    ruvia::detail::HttpRequestAccess::setPath(request, p);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})), asio::use_future);
    ctx.run();
    return extractDispatchResult(future.get());  // arena still alive here
}

}  // namespace routing_test

namespace routing_test {

inline ruvia::Task<ruvia::HttpResponse> urlForEchoHandler(void*, ruvia::Context& context) {
    co_return context.body(context.urlFor("/users/:id", {"7"}));
}

inline ruvia::Task<ruvia::HttpResponse> jsonModelEchoHandler(void*, ruvia::Context& context) {
    const auto body = co_await context.req().json<ScopedValidationRequest>();
    co_return context.body(body.get<"value">().has_value() ? body.get<"value">()->view() : "missing");
}

inline ruvia::Task<ruvia::HttpResponse> formModelEchoHandler(void*, ruvia::Context& context) {
    const auto body = co_await context.req().form<ScopedValidationRequest>();
    co_return context.body(body.get<"value">().has_value() ? body.get<"value">()->view() : "missing");
}

inline ruvia::Task<ruvia::HttpResponse> jsonIfEchoHandler(void*, ruvia::Context& context) {
    const auto body = co_await context.req().jsonIf<ScopedValidationRequest>();
    co_return context.body(body.has_value() && body->get<"value">().has_value() ? body->get<"value">()->view() : "no-json");
}

inline ruvia::Task<ruvia::HttpResponse> formIfEchoHandler(void*, ruvia::Context& context) {
    const auto body = co_await context.req().formIf<ScopedValidationRequest>();
    co_return context.body(body.has_value() && body->get<"value">().has_value() ? body->get<"value">()->view() : "no-form");
}

inline ruvia::Task<ruvia::HttpResponse> jsonValueIfEchoHandler(void*, ruvia::Context& context) {
    const auto body = co_await context.req().jsonValueIf();
    co_return context.body(std::string_view(body.has_value() ? "json" : "no-json"));
}

// Dispatches one GET /x with an optional Content-Type header and body.
inline DispatchResult dispatchBodyRequest(RouteHandler handler, std::string_view contentType, std::string_view body) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpKnownMethod::kGet, path("/x"), handler, RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setPath(request, "/x");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    if (!contentType.empty()) {
        const auto slot = ruvia::detail::HttpRequestAccess::knownHeaderSlot(ruvia::detail::RequestKnownHeader::kContentType);
        (void)ruvia::detail::HttpRequestAccess::addHeader(request, ruvia::HttpHeaderView{"Content-Type", contentType}, slot);
    }
    ruvia::detail::HttpRequestAccess::setBody(request, body);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})), asio::use_future);
    ctx.run();
    return extractDispatchResult(future.get());
}

}  // namespace routing_test

using namespace routing_test;  // NOLINT(google-build-using-namespace)
