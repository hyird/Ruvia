#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/router/RouteStreamState.h"

#include <optional>
#include <utility>

#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/StreamingAccess.h"
#include "ruvia/web/detail/router/RouteDispatchServices.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamState.h"

// Running a route that streams its response: binding the writer to the Context
// for the handler's lifetime, producing the head the h1/h2 sinks commit, and
// unwinding when the handler throws after bytes are already on the wire.

namespace ruvia {

namespace {

class ResponseStreamContextBinding final {
public:
    explicit ResponseStreamContextBinding(ResponseStreamWriter* writer) noexcept
        : writer_(writer) {}

    ~ResponseStreamContextBinding() {
        if (writer_ != nullptr) {
            detail::StreamingAccess::releaseContext(*writer_);
        }
    }

    ResponseStreamContextBinding(const ResponseStreamContextBinding&) = delete;
    ResponseStreamContextBinding& operator=(const ResponseStreamContextBinding&) = delete;

private:
    ResponseStreamWriter* writer_;
};

// Web-side thunk producing the streaming response head from the bound Context. It is
// handed to the http streaming layer (ResponseStreamWriter::bindContext) so the h1/h2
// sinks can build the head at commit without naming ContextAccess (web).
[[nodiscard]] HttpResponse streamingHeadThunk(Context& context) {
    return detail::ContextAccess::streamingHead(context);
}

}  // namespace

Task<std::optional<HttpResponse>> detail::RouteTable::dispatchStreamRoute(const HttpRequest& request, const ResolvedRoute& resolved, RequestMemory& memory, const RouteStreamHandler& handler, ContextServices services) const {
    const auto& route = resolved.route();
    StreamMiddlewareChainState middlewareChain;
    auto context = makeRouteContext(memory, request, resolved, withRouteHandlers(services, *this, errorHandlerFor(request.path()), notFoundHandlerFor(request.path())));
    const auto* responseStreamOutput = services.responseOutput().responseStream();
    const bool webSocketRoute = route.endpoint().webSocket() != nullptr;
    ResponseStreamContextBinding streamContextBinding(responseStreamOutput != nullptr ? &responseStreamOutput->writer() : nullptr);
    if (responseStreamOutput != nullptr) {
        detail::StreamingAccess::bindContext(responseStreamOutput->writer(), context, context.stopToken(), &streamingHeadThunk);
    }

    std::exception_ptr exception;
    try {
        if (!route.hasMiddleware()) {
            middlewareChain.markHandlerInvoked();
            co_await handler(context);
        } else {
            co_await invokeStreamMiddlewareAt(route, 0, context, middlewareChain, handler);
            if (!detail::ContextAccess::hasResponse(context)) {
                if (auto contextException = context.exception()) {
                    std::rethrow_exception(contextException);
                }
                const bool streamCommitted = responseStreamOutput != nullptr && detail::StreamingAccess::committed(responseStreamOutput->writer());
                if (!middlewareChain.handlerInvoked() && !streamCommitted) {
                    throw std::logic_error(
                        "context is not finalized; stream middleware must set a response, write "
                        "the stream, or await next()");
                }
            }
        }
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        // Head-only completion is a control signal from the writer, not a
        // failure: the committed head already ended the message, and the
        // handler was merely stopped at its first body write. Finish the stream
        // as a normal head-only success.
        bool headOnlyComplete = false;
        if (responseStreamOutput != nullptr && detail::StreamingAccess::committed(responseStreamOutput->writer())) {
            try {
                std::rethrow_exception(exception);
            } catch (const detail::ResponseStreamHeadOnlyComplete&) {
                headOnlyComplete = true;
            } catch (...) {
                // Classification only: `exception` still holds this and the
                // committed-failure path below reports it.
            }
        }
        if (headOnlyComplete) {
            co_await responseStreamOutput->writer().end();
            co_return std::nullopt;
        }
        if ((webSocketRoute && middlewareChain.handlerInvoked()) || (responseStreamOutput != nullptr && detail::StreamingAccess::committed(responseStreamOutput->writer()))) {
            std::rethrow_exception(exception);
        }
        auto response = co_await handleException(context, exception);
        co_return std::move(response);
    }

    // The middleware chain converts a handler exception into a buffered error
    // response and records it via context.exception() (storeMiddlewareExceptionResponse
    // -> handleException -> setError), so a mid-request failure does not surface as
    // a local exception above. When the stream is already committed (or this is a
    // WebSocket route), that buffered response can no longer be sent, and finalizing
    // the stream with a clean terminator would frame a truncated body as complete.
    // Rethrow so the driver aborts (connection close / RST_STREAM), exactly as the
    // no-middleware path does through the committed check above.
    if (webSocketRoute || (responseStreamOutput != nullptr && detail::StreamingAccess::committed(responseStreamOutput->writer()))) {
        if (auto contextException = context.exception()) {
            std::rethrow_exception(contextException);
        }
    }

    const bool streamCommitted = responseStreamOutput != nullptr && detail::StreamingAccess::committed(responseStreamOutput->writer());
    const bool handlerInvoked = middlewareChain.handlerInvoked();
    const bool webSocketHandled = webSocketRoute && handlerInvoked;
    // Middleware may replace an uncommitted response stream with a buffered
    // response after `next()`. A WebSocket handler, however, owns its upgraded
    // session as soon as it is invoked and cannot return to HTTP response mode.
    if (detail::ContextAccess::hasResponse(context) && !streamCommitted && !webSocketHandled) {
        co_return detail::ContextAccess::takeResponse(context);
    }
    if (handlerInvoked || streamCommitted) {
        // The bound Context is local to this coroutine. Finish a response stream
        // before it is destroyed so an empty/bodyless handler cannot leave the
        // sink with a dangling Context* that a higher layer later dereferences.
        // Reaching this common point gives middleware post-processing its full
        // chance to adjust an uncommitted head; an already committed writer no
        // longer needs the Context.
        if (responseStreamOutput != nullptr) {
            co_await responseStreamOutput->writer().end();
        }
        co_return std::nullopt;
    }
    throw std::logic_error("stream route completed without a response or handled output");
}

}  // namespace ruvia
