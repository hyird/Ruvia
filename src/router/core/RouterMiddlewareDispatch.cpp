#include "../RouteTable.h"

#include "../../http/ContextInternal.h"

#include <exception>
#include <stdexcept>
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
    if (detail::ContextAccess::hasResponse(context)) {
        co_return detail::ContextAccess::takeResponse(context);
    }
    if (auto exception = context.error()) {
        co_return co_await handleException(context, exception, true);
    }
    throw std::logic_error("context is not finalized; middleware must set a response or await next()");
}

Task<void> detail::RouteTable::invokeMiddlewareAt(
    const RouteEntry& route,
    std::size_t index,
    Context& context) const {
    if (index >= route.middlewareCount()) {
        auto response = co_await route.handler()(context);
        detail::ContextAccess::setResponse(context, std::move(response));
        co_return;
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset() + index];
    MiddlewareContinuation continuation{this, &route, &context, index + 1};
    const auto next = NextAccess::make(&continuation, &RouteTable::invokeMiddlewareContinuation);
    auto task = middleware(context, next);
    co_await std::move(task);
    co_return;
}

Task<void> detail::RouteTable::invokeMiddlewareContinuation(void* target) {
    const auto* continuation = static_cast<const MiddlewareContinuation*>(target);
    try {
        co_await continuation->table->invokeMiddlewareAt(
            *continuation->route,
            continuation->index,
            *continuation->context);
    } catch (...) {
        detail::ContextAccess::setError(*continuation->context, std::current_exception());
    }
}

Task<void> detail::RouteTable::invokeStreamMiddlewareAt(
    const RouteEntry& route,
    std::size_t index,
    Context& context,
    RouteStreamDispatchOutcome& outcome) const {
    if (index >= route.middlewareCount()) {
        co_await route.streamHandler()(context);
        outcome = RouteStreamDispatchOutcome::kStreamHandled;
        co_return;
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset() + index];
    StreamMiddlewareContinuation continuation{this, &route, &context, index + 1, &outcome};
    const auto next = NextAccess::make(&continuation, &RouteTable::invokeStreamMiddlewareContinuation);
    auto task = middleware(context, next);
    co_await std::move(task);
    co_return;
}

Task<void> detail::RouteTable::invokeStreamMiddlewareContinuation(void* target) {
    const auto* continuation = static_cast<const StreamMiddlewareContinuation*>(target);
    try {
        co_await continuation->table->invokeStreamMiddlewareAt(
            *continuation->route,
            continuation->index,
            *continuation->context,
            *continuation->outcome);
    } catch (...) {
        detail::ContextAccess::setError(*continuation->context, std::current_exception());
    }
}

}  // namespace ruvia
