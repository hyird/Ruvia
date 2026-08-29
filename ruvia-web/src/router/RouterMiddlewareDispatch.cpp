#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/router/RouteStreamState.h"

#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamState.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/http/error/HttpErrorResponse.h"

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace ruvia {

namespace {

void storeRepeatedNextError(Context& context) {
    detail::ContextAccess::setError(
        context, std::make_exception_ptr(std::logic_error("next() called multiple times")));
    detail::ContextAccess::setResponse(
        context, detail::makeDefaultErrorResponse(context.resource(),
                     HttpErrorInfo({.status = ruvia::http_status::kInternalServerError,
                         .code = "next_called_multiple_times",
                         .message = "next() called multiple times"})));
}

void storeNextAfterResponseError(Context& context) {
    detail::ContextAccess::setError(
        context, std::make_exception_ptr(std::logic_error("next() called after respond()")));
    detail::ContextAccess::setResponse(
        context, detail::makeDefaultErrorResponse(context.resource(),
                     HttpErrorInfo({.status = ruvia::http_status::kInternalServerError,
                         .code = "next_called_after_response",
                         .message = "next() called after respond()"})));
}

[[nodiscard]] bool validateNextInvocation(detail::NextState& state) {
    auto& context = *state.context;
    if (state.invocation != detail::NextState::Invocation::kReady) {
        storeRepeatedNextError(context);
        return false;
    }
    if (detail::ContextAccess::hasResponse(context)) {
        storeNextAfterResponseError(context);
        return false;
    }
    return true;
}

detail::NextState::Control* makeNextControl(Context& context) {
    auto* control = static_cast<detail::NextState::Control*>(context.resource()->allocate(
        sizeof(detail::NextState::Control), alignof(detail::NextState::Control)));
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
        control_->expire();
    }

private:
    detail::NextState::Control* control_;
};

NextControlScope makeNextControlScope(detail::NextState::Control& control) noexcept {
    return NextControlScope(control);
}

}  // namespace

Task<HttpResponse> detail::RouteTable::invokeRoute(
    const RouteEntry& route, Context& context) const {
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
    const RouteEntry& route, Context& context) const {
    co_await invokeMiddlewareAt(route, 0, context);
    if (detail::ContextAccess::hasResponse(context)) {
        co_return detail::ContextAccess::takeResponse(context);
    }
    if (auto exception = context.exception()) {
        co_return co_await handleException(context, exception);
    }
    throw std::logic_error(
        "context is not finalized; middleware must set a response or await next()");
}

Task<void> detail::RouteTable::invokeMiddlewareAt(
    const RouteEntry& route, std::size_t index, Context& context) const {
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
    auto& next = NextAccess::makeIn(context.resource(),
        detail::NextState{.table = this,
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
    if (!validateNextInvocation(state)) {
        co_return;
    }
    auto* context = state.context;
    const auto* table = state.table;
    const auto* route = state.route;
    std::exception_ptr exception;
    try {
        co_await table->invokeMiddlewareAt(*route, state.index, *context);
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        co_await table->storeMiddlewareExceptionResponse(*context, exception);
    }
}

// The unmatched-request chain. It mirrors the route chain exactly except for
// its terminal: there is no route endpoint, so the 404/405/501 response comes
// from the caller-supplied thunk instead.
Task<HttpResponse> detail::RouteTable::runUnmatchedChain(
    Context& context, const UnmatchedTerminal& terminal) const {
    if (unmatchedMiddlewareCount_ == 0) {
        return terminal(context);
    }
    return [](const RouteTable* table, Context* unmatchedContext,
               const UnmatchedTerminal* unmatchedTerminal) -> Task<HttpResponse> {
        co_await table->invokeUnmatchedMiddlewareAt(0, *unmatchedContext, *unmatchedTerminal);
        if (detail::ContextAccess::hasResponse(*unmatchedContext)) {
            co_return detail::ContextAccess::takeResponse(*unmatchedContext);
        }
        if (auto exception = unmatchedContext->exception()) {
            co_return co_await table->handleException(*unmatchedContext, exception);
        }
        throw std::logic_error(
            "context is not finalized; middleware must set a response or await next()");
    }(this, &context, &terminal);
}

Task<void> detail::RouteTable::invokeUnmatchedMiddlewareAt(
    std::size_t index, Context& context, const UnmatchedTerminal& terminal) const {
    if (index >= unmatchedMiddlewareCount_) {
        auto response = co_await terminal(context);
        detail::ContextAccess::setResponse(context, std::move(response));
        co_return;
    }

    const auto& middleware = middlewareFrames_[unmatchedMiddlewareOffset_ + index];
    auto& control = *makeNextControl(context);
    auto controlScope = makeNextControlScope(control);
    auto& next = NextAccess::makeIn(context.resource(),
        detail::NextState{.table = this,
            .context = &context,
            .unmatchedTerminal = &terminal,
            .control = &control,
            .index = index + 1},
        &RouteTable::invokeUnmatchedMiddlewareContinuation);
    auto task = middleware(context, next);
    co_await std::move(task);
    co_return;
}

Task<void> detail::RouteTable::invokeUnmatchedMiddlewareContinuation(NextState state) {
    if (!validateNextInvocation(state)) {
        co_return;
    }
    auto* context = state.context;
    const auto* table = state.table;
    const auto* terminal =
        static_cast<const RouteTable::UnmatchedTerminal*>(state.unmatchedTerminal);
    std::exception_ptr exception;
    try {
        co_await table->invokeUnmatchedMiddlewareAt(state.index, *context, *terminal);
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        co_await table->storeMiddlewareExceptionResponse(*context, exception);
    }
}

Task<void> detail::RouteTable::invokeStreamMiddlewareAt(const RouteEntry& route, std::size_t index,
    Context& context, StreamMiddlewareChainState& chain, const RouteStreamHandler& handler) const {
    if (index >= route.middlewareCount()) {
        chain.markHandlerInvoked();
        co_await handler(context);
        co_return;
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset() + index];
    auto& control = *makeNextControl(context);
    auto controlScope = makeNextControlScope(control);
    auto& next = NextAccess::makeIn(context.resource(),
        detail::NextState{.table = this,
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
    if (!validateNextInvocation(state)) {
        co_return;
    }
    auto* context = state.context;
    const auto* table = state.table;
    const auto* route = state.route;
    auto* chain = state.streamChain;
    std::exception_ptr exception;
    try {
        co_await table->invokeStreamMiddlewareAt(
            *route, state.index, *context, *chain, *state.streamHandler);
    } catch (const ResponseStreamHeadOnlyComplete&) {
        // Not a failure: the committed head already completed a
        // body-suppressed message (HEAD on a streaming route). Let the signal
        // unwind through every middleware frame so dispatchStreamRoute can
        // finish the stream as a head-only success instead of rendering a
        // buffered error response that can no longer be sent.
        throw;
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        co_await table->storeMiddlewareExceptionResponse(*context, exception);
    }
}

Task<void> detail::RouteTable::storeMiddlewareExceptionResponse(
    Context& context, std::exception_ptr exception) const {
    auto response = co_await handleException(context, exception);
    detail::ContextAccess::setResponse(context, std::move(response));
}

}  // namespace ruvia
