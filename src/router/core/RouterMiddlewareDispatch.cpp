#include "../RouteTable.h"

#include <utility>

namespace ruvia {

Task<HttpResponse> detail::RouteTable::invokeRoute(const RouteEntry& route, Context& context) const {
    // Hot path: a route with no middleware goes straight to the handler. This
    // is a plain (non-coroutine) forward, so skipping invokeMiddlewareAt saves
    // one heap-allocated coroutine frame and one HttpResponse move per request
    // for the common zero-middleware route. invokeMiddlewareAt at index 0 with
    // middlewareCount == 0 is exactly `co_await route.handler()(context)`, so the
    // behavior (including exception propagation into dispatch's try/catch) is
    // identical.
    if (!route.hasMiddleware()) {
        return route.handler()(context);
    }
    return invokeMiddlewareAt(route, 0, context);
}

Task<HttpResponse> detail::RouteTable::invokeMiddlewareAt(
    const RouteEntry& route,
    std::size_t index,
    Context& context) const {
    if (index >= route.middlewareCount()) {
        co_return co_await route.handler()(context);
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset() + index];
    MiddlewareContinuation continuation{this, &route, index + 1};
    const auto next = NextAccess::make(&continuation, &RouteTable::invokeMiddlewareContinuation);
    auto response = middleware(context, next);
    co_return co_await std::move(response);
}

Task<HttpResponse> detail::RouteTable::invokeMiddlewareContinuation(void* target, Context& context) {
    const auto* continuation = static_cast<const MiddlewareContinuation*>(target);
    return continuation->table->invokeMiddlewareAt(*continuation->route, continuation->index, context);
}

Task<HttpResponse> detail::RouteTable::invokeStreamMiddlewareAt(
    const RouteEntry& route,
    std::size_t index,
    Context& context,
    RouteStreamDispatchOutcome& outcome) const {
    if (index >= route.middlewareCount()) {
        co_await route.streamHandler()(context);
        outcome = RouteStreamDispatchOutcome::kStreamHandled;
        co_return HttpResponse(context.resource());
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset() + index];
    StreamMiddlewareContinuation continuation{this, &route, index + 1, &outcome};
    const auto next = NextAccess::make(&continuation, &RouteTable::invokeStreamMiddlewareContinuation);
    auto response = middleware(context, next);
    co_return co_await std::move(response);
}

Task<HttpResponse> detail::RouteTable::invokeStreamMiddlewareContinuation(void* target, Context& context) {
    const auto* continuation = static_cast<const StreamMiddlewareContinuation*>(target);
    return continuation->table->invokeStreamMiddlewareAt(
        *continuation->route,
        continuation->index,
        context,
        *continuation->outcome);
}

}  // namespace ruvia
