#include "../RouteTable.h"

#include "../../http/ContextInternal.h"

#include <exception>
#include <utility>

namespace ruvia {

Task<HttpResponse> detail::RouteTable::invokeRoute(const RouteEntry& route, Context& context) const {
    // Hot path: a route with no middleware goes straight to the handler. The
    // Context response slot is only needed when middleware can observe or mutate
    // the downstream response through c.res().
    if (!route.hasMiddleware()) {
        return route.handler()(context);
    }
    return invokeRouteWithMiddleware(route, context);
}

Task<HttpResponse> detail::RouteTable::invokeRouteWithMiddleware(
    const RouteEntry& route,
    Context& context) const {
    co_await invokeMiddlewareAt(route, 0, context);
    co_return detail::ContextAccess::takeResponse(context);
}

Task<void> detail::RouteTable::invokeMiddlewareAt(
    const RouteEntry& route,
    std::size_t index,
    Context& context) const {
    if (index >= route.middlewareCount()) {
        try {
            auto response = co_await route.handler()(context);
            detail::ContextAccess::setResponse(context, std::move(response));
            co_return;
        } catch (...) {
            detail::ContextAccess::setError(context, std::current_exception());
            throw;
        }
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset() + index];
    MiddlewareContinuation continuation{this, &route, index + 1};
    const auto next = NextAccess::make(context, &continuation, &RouteTable::invokeMiddlewareContinuation);
    try {
        auto task = middleware(context, next);
        co_await std::move(task);
        co_return;
    } catch (...) {
        detail::ContextAccess::setError(context, std::current_exception());
        throw;
    }
}

Task<void> detail::RouteTable::invokeMiddlewareContinuation(void* target, Context& context) {
    const auto* continuation = static_cast<const MiddlewareContinuation*>(target);
    return continuation->table->invokeMiddlewareAt(*continuation->route, continuation->index, context);
}

Task<void> detail::RouteTable::invokeStreamMiddlewareAt(
    const RouteEntry& route,
    std::size_t index,
    Context& context,
    RouteStreamDispatchOutcome& outcome) const {
    if (index >= route.middlewareCount()) {
        try {
            co_await route.streamHandler()(context);
            outcome = RouteStreamDispatchOutcome::kStreamHandled;
            co_return;
        } catch (...) {
            detail::ContextAccess::setError(context, std::current_exception());
            throw;
        }
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset() + index];
    StreamMiddlewareContinuation continuation{this, &route, index + 1, &outcome};
    const auto next = NextAccess::make(context, &continuation, &RouteTable::invokeStreamMiddlewareContinuation);
    try {
        auto task = middleware(context, next);
        co_await std::move(task);
        co_return;
    } catch (...) {
        detail::ContextAccess::setError(context, std::current_exception());
        throw;
    }
}

Task<void> detail::RouteTable::invokeStreamMiddlewareContinuation(void* target, Context& context) {
    const auto* continuation = static_cast<const StreamMiddlewareContinuation*>(target);
    return continuation->table->invokeStreamMiddlewareAt(
        *continuation->route,
        continuation->index,
        context,
        *continuation->outcome);
}

}  // namespace ruvia
