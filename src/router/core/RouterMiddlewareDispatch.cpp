#include "../RouteTable.h"

#include "../../http/ContextInternal.h"
#include "ruvia/http/Error.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace ruvia {

namespace {

void storeRepeatedNextError(Context& context) {
    detail::ContextAccess::setError(
        context,
        std::make_exception_ptr(std::logic_error("next() called multiple times")));
    detail::ContextAccess::setResponse(
        context,
        makeErrorResponse(
            context.resource(),
            HttpErrorInfo{
                .statusCode = 500,
                .code = "next_called_multiple_times",
                .message = "next() called multiple times"}));
}

}  // namespace

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
    auto next = NextAccess::make(
        Next::State{.table = this, .route = &route, .context = &context, .index = index + 1},
        &RouteTable::invokeMiddlewareContinuation);
    auto task = middleware(context, std::move(next));
    co_await std::move(task);
    co_return;
}

Task<void> detail::RouteTable::invokeMiddlewareContinuation(Next::State state) {
    auto* context = state.context;
    if (state.repeated) {
        storeRepeatedNextError(*context);
        co_return;
    }
    auto* table = static_cast<const RouteTable*>(state.table);
    auto* route = static_cast<const RouteEntry*>(state.route);
    std::exception_ptr exception;
    try {
        co_await table->invokeMiddlewareAt(
            *route,
            state.index,
            *context);
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        co_await table->storeMiddlewareExceptionResponse(*context, exception);
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
    auto next = NextAccess::make(
        Next::State{
            .table = this,
            .route = &route,
            .context = &context,
            .outcome = &outcome,
            .index = index + 1},
        &RouteTable::invokeStreamMiddlewareContinuation);
    auto task = middleware(context, std::move(next));
    co_await std::move(task);
    co_return;
}

Task<void> detail::RouteTable::invokeStreamMiddlewareContinuation(Next::State state) {
    auto* context = state.context;
    if (state.repeated) {
        storeRepeatedNextError(*context);
        co_return;
    }
    auto* table = static_cast<const RouteTable*>(state.table);
    auto* route = static_cast<const RouteEntry*>(state.route);
    auto* outcome = static_cast<RouteStreamDispatchOutcome*>(state.outcome);
    std::exception_ptr exception;
    try {
        co_await table->invokeStreamMiddlewareAt(
            *route,
            state.index,
            *context,
            *outcome);
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        co_await table->storeMiddlewareExceptionResponse(*context, exception);
    }
}

Task<void> detail::RouteTable::storeMiddlewareExceptionResponse(
    Context& context,
    std::exception_ptr exception) const {
    auto response = co_await handleException(
        context,
        exception,
        true);
    detail::ContextAccess::setResponse(context, std::move(response));
}

}  // namespace ruvia
