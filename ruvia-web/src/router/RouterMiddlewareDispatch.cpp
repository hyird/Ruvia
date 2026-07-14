#include "ruvia/web/detail/router/RouteTable.h"

#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/http/HttpErrorResponse.h"

#include <exception>
#include <memory>
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
        detail::makeDefaultErrorResponse(
            context.resource(),
            HttpErrorInfo(500, "next_called_multiple_times", "next() called multiple times")));
}

detail::NextState::Control* makeNextControl(Context& context) {
    auto* control = static_cast<detail::NextState::Control*>(
        context.resource()->allocate(
            sizeof(detail::NextState::Control),
            alignof(detail::NextState::Control)));
    std::construct_at(control);
    return control;
}

class NextControlScope final {
public:
    explicit NextControlScope(detail::NextState::Control& control) noexcept
        : control_(&control) {}

    NextControlScope(const NextControlScope&) = delete;
    NextControlScope& operator=(const NextControlScope&) = delete;

    ~NextControlScope() {
        control_->active = false;
    }

private:
    detail::NextState::Control* control_;
};

NextControlScope makeNextControlScope(detail::NextState::Control& control) noexcept {
    return NextControlScope(control);
}

}  // namespace

Task<HttpResponse> detail::RouteTable::invokeRoute(const RouteEntry& route, Context& context) const {
    const auto* endpoint = route.endpoint().buffered();
    if (endpoint == nullptr) {
        throw std::logic_error("route is not a buffered-response route");
    }
    // Hot path: a route with no middleware goes straight to the handler. The
    // Context response slot is only needed when middleware can observe the
    // downstream response through Context::response().
    if (!route.hasMiddleware()) {
        return endpoint->handler()(context);
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
        co_return co_await handleException(context, exception);
    }
    throw std::logic_error("context is not finalized; middleware must set a response or await next()");
}

Task<void> detail::RouteTable::invokeMiddlewareAt(
    const RouteEntry& route,
    std::size_t index,
    Context& context) const {
    if (index >= route.middlewareCount()) {
        const auto* endpoint = route.endpoint().buffered();
        if (endpoint == nullptr) {
            throw std::logic_error("route is not a buffered-response route");
        }
        auto response = co_await endpoint->handler()(context);
        detail::ContextAccess::setResponse(context, std::move(response));
        co_return;
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset() + index];
    auto& control = *makeNextControl(context);
    auto controlScope = makeNextControlScope(control);
    auto& next = NextAccess::makeIn(
        context.resource(),
        detail::NextState{
            .table = this,
            .route = &route,
            .context = &context,
            .control = &control,
            .index = index + 1},
        &RouteTable::invokeMiddlewareContinuation);
    auto task = middleware(context, next);
    co_await std::move(task);
    co_return;
}

Task<void> detail::RouteTable::invokeMiddlewareContinuation(NextState state) {
    auto* context = state.context;
    if (state.repeated) {
        storeRepeatedNextError(*context);
        co_return;
    }
    const auto* table = state.table;
    const auto* route = state.route;
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
    StreamMiddlewareChainState& chain,
    const RouteStreamHandler& handler) const {
    if (index >= route.middlewareCount()) {
        co_await handler(context);
        chain.markHandlerInvoked();
        co_return;
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset() + index];
    auto& control = *makeNextControl(context);
    auto controlScope = makeNextControlScope(control);
    auto& next = NextAccess::makeIn(
        context.resource(),
        detail::NextState{
            .table = this,
            .route = &route,
            .context = &context,
            .streamChain = &chain,
            .streamHandler = &handler,
            .control = &control,
            .index = index + 1},
        &RouteTable::invokeStreamMiddlewareContinuation);
    auto task = middleware(context, next);
    co_await std::move(task);
    co_return;
}

Task<void> detail::RouteTable::invokeStreamMiddlewareContinuation(NextState state) {
    auto* context = state.context;
    if (state.repeated) {
        storeRepeatedNextError(*context);
        co_return;
    }
    const auto* table = state.table;
    const auto* route = state.route;
    auto* chain = state.streamChain;
    std::exception_ptr exception;
    try {
        co_await table->invokeStreamMiddlewareAt(
            *route,
            state.index,
            *context,
            *chain,
            *state.streamHandler);
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
    auto response = co_await handleException(context, exception);
    detail::ContextAccess::setResponse(context, std::move(response));
}

}  // namespace ruvia
