#include "ruvia/web/detail/router/RouteTable.h"

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/web/detail/http/StreamingInternal.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/Validation.h"
#include "ruvia/web/detail/http/HttpErrorResponse.h"

namespace ruvia {
namespace {

void setAllowHeader(HttpResponse& response, std::uint32_t methodMask) {
    detail::setResponseAllowHeader(response, methodMask);
}

HttpResponse makeAllowNoContentResponse(RequestMemory& memory, std::uint32_t methodMask) {
    HttpResponse response(memory.resource());
    response.status(204);
    setAllowHeader(response, methodMask);
    return response;
}

Context makeRouteContext(
    RequestMemory& memory,
    const HttpRequest& request,
    const detail::ResolvedRoute& resolved,
    detail::ContextServices services) noexcept {
    const auto& route = resolved.route();
    const auto routeRateLimitScope = reinterpret_cast<std::uintptr_t>(&route);
    const auto values = resolved.match().values();
    if (values.empty()) {
        return detail::ContextAccess::make(
            memory,
            request,
            route.path(),
            route.method(),
            route.middlewareCount(),
            routeRateLimitScope,
            services);
    }

    return detail::ContextAccess::make(
        memory,
        request,
        route.path(),
        route.paramNames().data(),
        values.data(),
        values.size(),
        route.method(),
        route.middlewareCount(),
        routeRateLimitScope,
        services);
}

detail::ContextServices withRouteHandlers(
    detail::ContextServices services,
    HttpErrorHandler errorHandler,
    HttpNotFoundHandler notFoundHandler) noexcept {
    return services
        .withErrorHandler(errorHandler)
        .withNotFoundHandler(notFoundHandler);
}

struct OwnedHttpErrorInfo;
void assignExceptionError(OwnedHttpErrorInfo& errorInfo, std::exception_ptr exception);

struct OwnedHttpErrorInfo final {
    HttpErrorInfo info{};
    std::pmr::string statusText;
    std::pmr::string code;
    std::pmr::string message;
    std::pmr::string detailsJson;

    explicit OwnedHttpErrorInfo(HttpErrorInfo source, std::pmr::memory_resource* resource)
        : statusText(resource),
          code(resource),
          message(resource),
          detailsJson(resource) {
        assign(source);
    }

    OwnedHttpErrorInfo(std::pmr::memory_resource* resource, std::exception_ptr exception)
        : OwnedHttpErrorInfo(
              HttpErrorInfo(500, {}, "unhandled exception"),
              resource) {
        assignExceptionError(*this, exception);
    }

    void assign(HttpErrorInfo source) {
        statusText.assign(source.statusText().data(), source.statusText().size());
        code.assign(source.code().data(), source.code().size());
        message.assign(source.message().data(), source.message().size());
        detailsJson.assign(source.detailsJson().data(), source.detailsJson().size());

        info = HttpErrorInfo(source.status(), code, message, statusText, detailsJson);
    }
};

void assignExceptionError(OwnedHttpErrorInfo& errorInfo, std::exception_ptr exception) {
    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
    } catch (const ValidationError& error) {
        errorInfo.assign(error.info());
    } catch (const HttpError& error) {
        errorInfo.assign(error.info());
    } catch (const HttpProtocolError& error) {
        errorInfo.assign(HttpErrorInfo(error.status(), {}, error.what()));
    } catch (const std::invalid_argument& error) {
        // invalid_argument is the framework's own request-validation signal (bad
        // cookie/json/form); its message describes the request, so it is safe to
        // surface to the client as a 400.
        errorInfo.assign(HttpErrorInfo(400, {}, error.what()));
    } catch (const std::exception&) {
        // An unexpected exception (e.g. a database/library error) may carry
        // internal detail -- table names, query fragments, file paths. Do NOT echo
        // what() to the client: normalizeError renders a generic "Internal Server
        // Error" body. The exception_ptr is still set on the Context, so an onError
        // handler can log or inspect the full detail server-side.
        errorInfo.assign(HttpErrorInfo(500, {}, {}));
    } catch (...) {
        errorInfo.assign(HttpErrorInfo(500, {}, {}));
    }
}

}  // namespace

Task<HttpResponse> detail::RouteTable::dispatch(
    const HttpRequest& request,
    RequestMemory& memory,
    ContextServices services) const {
    const auto resolution = resolve(request);
    co_return co_await dispatch(request, resolution, memory, services);
}

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchResponseStream(
    const HttpRequest& request,
    const ResolvedRoute& resolved,
    RequestMemory& memory,
    ResponseStreamWriter& responseStream,
    ContextServices services) const {
    if (resolved.route().endpoint().responseStream() == nullptr) {
        throw std::logic_error("route is not a response stream route");
    }
    return dispatchStreamRoute(
        request, resolved, memory, services.withResponseStream(responseStream));
}

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchWebSocket(
    const HttpRequest& request,
    const ResolvedRoute& resolved,
    RequestMemory& memory,
    WebSocket& webSocket,
    ContextServices services) const {
    if (resolved.route().endpoint().webSocket() == nullptr) {
        throw std::logic_error("route is not a websocket route");
    }
    return dispatchStreamRoute(
        request, resolved, memory, services.withWebSocket(webSocket));
}

namespace {
// Web-side thunk producing the streaming response head from the bound Context. It is
// handed to the http streaming layer (ResponseStreamWriter::bindContext) so the h1/h2
// sinks can build the head at commit without naming ContextAccess (web).
[[nodiscard]] HttpResponse streamingHeadThunk(Context& context) {
    return detail::ContextAccess::streamingHead(context);
}
}  // namespace

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchStreamRoute(
    const HttpRequest& request,
    const ResolvedRoute& resolved,
    RequestMemory& memory,
    ContextServices services) const {
    const auto& route = resolved.route();
    auto disposition = StreamMiddlewareDisposition::kBufferedResponse;
    auto context = makeRouteContext(
        memory,
        request,
        resolved,
        withRouteHandlers(services, errorHandler_, notFoundHandler_));
    const auto* responseStreamOutput = services.responseOutput().responseStream();
    if (responseStreamOutput != nullptr) {
        detail::StreamingAccess::bindContext(
            responseStreamOutput->writer(), context, &streamingHeadThunk);
    }

    std::exception_ptr exception;
    try {
        if (!route.hasMiddleware()) {
            const auto& endpoint = route.endpoint();
            const auto* responseStream = endpoint.responseStream();
            const auto* webSocket = endpoint.webSocket();
            if (responseStream == nullptr && webSocket == nullptr) {
                throw std::logic_error("route is not a stream-handler route");
            }
            const auto& handler = responseStream != nullptr
                ? responseStream->handler()
                : webSocket->handler();
            co_await handler(context);
            disposition = StreamMiddlewareDisposition::kStreamHandled;
        } else {
            co_await invokeStreamMiddlewareAt(route, 0, context, disposition);
            if (!detail::ContextAccess::hasResponse(context)) {
                if (auto contextException = context.error()) {
                    std::rethrow_exception(contextException);
                }
                const bool streamCommitted =
                    responseStreamOutput != nullptr &&
                    detail::StreamingAccess::committed(
                        responseStreamOutput->writer());
                if (disposition !=
                        StreamMiddlewareDisposition::kStreamHandled &&
                    !streamCommitted) {
                    throw std::logic_error("context is not finalized; stream middleware must set a response, write the stream, or await next()");
                }
            }
        }
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        if (services.responseOutput().webSocket() != nullptr ||
            (responseStreamOutput != nullptr &&
             detail::StreamingAccess::committed(responseStreamOutput->writer()))) {
            std::rethrow_exception(exception);
        }
        auto response = co_await handleException(context, exception);
        co_return StreamDispatchResult::makeBuffered(std::move(response));
    }

    // The middleware chain converts a handler exception into a buffered error
    // response and records it via context.error() (storeMiddlewareExceptionResponse
    // -> handleException -> setError), so a mid-request failure does not surface as
    // a local exception above. When the stream is already committed (or this is a
    // WebSocket route), that buffered response can no longer be sent, and finalizing
    // the stream with a clean terminator would frame a truncated body as complete.
    // Rethrow so the driver aborts (connection close / RST_STREAM), exactly as the
    // no-middleware path does through the committed check above.
    if (services.responseOutput().webSocket() != nullptr ||
        (responseStreamOutput != nullptr &&
         detail::StreamingAccess::committed(responseStreamOutput->writer()))) {
        if (auto contextException = context.error()) {
            std::rethrow_exception(contextException);
        }
    }

    const bool streamCommitted =
        responseStreamOutput != nullptr &&
        detail::StreamingAccess::committed(
            responseStreamOutput->writer());
    if (disposition == StreamMiddlewareDisposition::kStreamHandled ||
        streamCommitted) {
        // The bound Context is local to this coroutine. Finish a response stream
        // before it is destroyed so an empty/bodyless handler cannot leave the
        // sink with a dangling Context* that a higher layer later dereferences.
        // Reaching this common point gives middleware post-processing its full
        // chance to adjust an uncommitted head; an already committed writer no
        // longer needs the Context.
        if (responseStreamOutput != nullptr) {
            co_await responseStreamOutput->writer().end();
        }
        co_return StreamDispatchResult::makeHandled();
    }
    co_return StreamDispatchResult::makeBuffered(
        detail::ContextAccess::takeResponse(context));
}

Task<HttpResponse> detail::RouteTable::dispatch(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    ContextServices services) const {
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        if (request.knownMethod() == HttpKnownMethod::kUnknown) {
            co_return co_await handleError(
                request,
                memory,
                HttpErrorInfo(501, {}, "method not implemented"),
                services);
        }

        if (request.knownMethod() == HttpKnownMethod::kOptions && request.path() == "*") {
            co_return makeAllowNoContentResponse(memory, allowedMethodsForServer());
        }

        if (const auto* methodNotAllowed = resolution.methodNotAllowed()) {
            if (request.knownMethod() == HttpKnownMethod::kOptions) {
                co_return makeAllowNoContentResponse(
                    memory, methodNotAllowed->allowedMethods());
            }

            const auto error = HttpErrorInfo(405, {}, "method not allowed");
            auto response = co_await handleError(request, memory, error, services);
            setAllowHeader(response, methodNotAllowed->allowedMethods());
            co_return response;
        }

        co_return co_await handleNotFound(request, memory, services);
    }

    auto context = makeRouteContext(
        memory,
        request,
        *resolved,
        withRouteHandlers(services, errorHandler_, notFoundHandler_));
    std::exception_ptr exception;
    try {
        const auto& route = resolved->route();
        if (route.endpoint().buffered() == nullptr) {
            throw std::logic_error(
                "streaming route requires its dedicated dispatch path");
        }
        co_return co_await invokeRoute(route, context);
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await handleException(context, exception);
}

Task<HttpResponse> detail::RouteTable::dispatchBuffered(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    ContextServices services) const {
    std::exception_ptr exception;
    try {
        co_return co_await dispatch(request, resolution, memory, services);
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await handleException(request, memory, exception, services);
}

Task<HttpResponse> detail::RouteTable::handleError(
    const HttpRequest& request,
    RequestMemory& memory,
    HttpErrorInfo error,
    ContextServices services) const {
    if (errorHandler_ == nullptr) {
        co_return makeDefaultErrorResponse(memory.resource(), error);
    }

    auto context = detail::ContextAccess::make(
        memory,
        request,
        withRouteHandlers(services, errorHandler_, notFoundHandler_));
    co_return co_await handleError(context, error);
}

Task<HttpResponse> detail::RouteTable::handleException(
    const HttpRequest& request,
    RequestMemory& memory,
    std::exception_ptr exception,
    ContextServices services) const {
    if (errorHandler_ == nullptr) {
        OwnedHttpErrorInfo errorInfo(memory.resource(), exception);
        co_return makeDefaultErrorResponse(memory.resource(), errorInfo.info);
    }

    auto context = detail::ContextAccess::make(
        memory,
        request,
        withRouteHandlers(services, errorHandler_, notFoundHandler_));
    co_return co_await handleException(context, exception);
}

Task<HttpResponse> detail::RouteTable::handleError(
    Context& context,
    HttpErrorInfo error) const {
    return invokeErrorHandler(context, error, errorHandler_);
}

Task<HttpResponse> detail::RouteTable::handleNotFound(
    const HttpRequest& request,
    RequestMemory& memory,
    ContextServices services) const {
    if (notFoundHandler_ == nullptr) {
        co_return makeDefaultErrorResponse(
            memory.resource(),
            HttpErrorInfo(404, {}, "route not found"));
    }

    auto context = detail::ContextAccess::make(
        memory,
        request,
        withRouteHandlers(services, errorHandler_, notFoundHandler_));

    std::exception_ptr exception;
    try {
        co_return co_await notFoundHandler_(context);
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await handleException(context, exception);
}

Task<HttpResponse> detail::RouteTable::handleException(
    Context& context,
    std::exception_ptr exception) const {
    detail::ContextAccess::setError(context, exception);
    OwnedHttpErrorInfo errorInfo(context.resource(), exception);

    co_return co_await handleError(context, errorInfo.info);
}

}  // namespace ruvia
