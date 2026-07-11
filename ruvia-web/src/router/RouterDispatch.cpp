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
    const detail::RouteResolution& resolution,
    detail::ContextServices services) noexcept {
    const auto& route = resolution.route();
    const auto routeRateLimitScope = reinterpret_cast<std::uintptr_t>(&route);
    const auto* match = resolution.match();
    if (match == nullptr) {
        return detail::ContextAccess::make(
            memory,
            request,
            route.path(),
            route.method(),
            route.middlewareCount(),
            routeRateLimitScope,
            services);
    }

    const auto values = match->values();
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
    RouteMatch match;
    const auto resolution = resolve(request, match);
    co_return co_await dispatch(request, resolution, memory, services);
}

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchResponseStream(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    ResponseStreamWriter& responseStream,
    ContextServices services) const {
    if (!resolution.found()) {
        throw std::logic_error("route is not a response stream route");
    }
    const auto& route = resolution.route();
    if (!route.usesResponseStream()) {
        throw std::logic_error("route is not a response stream route");
    }
    return dispatchStreamRoute(request, resolution, memory, services.withResponseStream(responseStream));
}

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchWebSocket(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    WebSocket& webSocket,
    ContextServices services) const {
    if (!resolution.found()) {
        throw std::logic_error("route is not a websocket route");
    }
    const auto& route = resolution.route();
    if (!route.isWebSocketResponse()) {
        throw std::logic_error("route is not a websocket route");
    }
    return dispatchStreamRoute(request, resolution, memory, services.withWebSocket(webSocket));
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
    const RouteResolution& resolution,
    RequestMemory& memory,
    ContextServices services) const {
    const auto& route = resolution.route();
    auto outcome = RouteStreamDispatchOutcome::kBufferedResponse;
    auto context = makeRouteContext(
        memory,
        request,
        resolution,
        withRouteHandlers(services, errorHandler_, notFoundHandler_));
    auto* responseStream = services.responseStream();
    if (responseStream != nullptr) {
        detail::StreamingAccess::bindContext(*responseStream, context, &streamingHeadThunk);
    }

    std::exception_ptr exception;
    try {
        if (!route.hasMiddleware()) {
            co_await route.streamHandler()(context);
            co_return StreamDispatchResult(
                HttpResponse(context.resource()),
                RouteStreamDispatchOutcome::kStreamHandled);
        }
        co_await invokeStreamMiddlewareAt(route, 0, context, outcome);
        if (!detail::ContextAccess::hasResponse(context)) {
            if (auto contextException = context.error()) {
                std::rethrow_exception(contextException);
            }
            if (outcome != RouteStreamDispatchOutcome::kStreamHandled) {
                throw std::logic_error("context is not finalized; stream middleware must set a response, write the stream, or await next()");
            }
        }
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        if (services.webSocket() != nullptr ||
            (responseStream != nullptr && detail::StreamingAccess::committed(*responseStream))) {
            std::rethrow_exception(exception);
        }
        auto response = co_await handleException(context, exception);
        co_return StreamDispatchResult(
            std::move(response),
            RouteStreamDispatchOutcome::kBufferedResponse);
    }

    // The middleware chain converts a handler exception into a buffered error
    // response and records it via context.error() (storeMiddlewareExceptionResponse
    // -> handleException -> setError), so a mid-request failure does not surface as
    // a local exception above. When the stream is already committed (or this is a
    // WebSocket route), that buffered response can no longer be sent, and finalizing
    // the stream with a clean terminator would frame a truncated body as complete.
    // Rethrow so the driver aborts (connection close / RST_STREAM), exactly as the
    // no-middleware path does through the committed check above.
    if (services.webSocket() != nullptr ||
        (responseStream != nullptr && detail::StreamingAccess::committed(*responseStream))) {
        if (auto contextException = context.error()) {
            std::rethrow_exception(contextException);
        }
    }

    auto response = detail::ContextAccess::hasResponse(context)
        ? detail::ContextAccess::takeResponse(context)
        : HttpResponse(context.resource());
    co_return StreamDispatchResult(
        std::move(response),
        outcome);
}

Task<HttpResponse> detail::RouteTable::dispatch(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    ContextServices services) const {
    if (!resolution.found()) {
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

        if (resolution.methodNotAllowed()) {
            if (request.knownMethod() == HttpKnownMethod::kOptions) {
                co_return makeAllowNoContentResponse(memory, resolution.allowedMethods());
            }

            const auto error = HttpErrorInfo(405, {}, "method not allowed");
            auto response = co_await handleError(request, memory, error, services);
            setAllowHeader(response, resolution.allowedMethods());
            co_return response;
        }

        co_return co_await handleNotFound(request, memory, services);
    }

    auto context = makeRouteContext(
        memory,
        request,
        resolution,
        withRouteHandlers(services, errorHandler_, notFoundHandler_));
    std::exception_ptr exception;
    try {
        const auto& route = resolution.route();
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
