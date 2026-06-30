#include "../RouteTable.h"

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

#include "../../http/ContextInternal.h"
#include "../../http/HttpResponseHeaderState.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/Validation.h"

namespace ruvia {
namespace {

void setAllowHeader(HttpResponse& response, std::uint32_t methodMask) {
    detail::setResponseAllowHeader(response, methodMask);
}

HttpResponse makeAllowNoContentResponse(RequestMemory& memory, std::uint32_t methodMask) {
    HttpResponse response(memory.resource());
    response.setStatus(204, {});
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
        return detail::ContextAccess::make(memory, request, routeRateLimitScope, services);
    }

    const auto values = match->values();
    return detail::ContextAccess::make(
        memory,
        request,
        route.paramNames().data(),
        values.data(),
        values.size(),
        routeRateLimitScope,
        services);
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
              HttpErrorInfo{.statusCode = 500, .message = "unhandled exception"},
              resource) {
        assignExceptionError(*this, exception);
    }

    void assign(HttpErrorInfo source) {
        statusText.assign(source.statusText.data(), source.statusText.size());
        code.assign(source.code.data(), source.code.size());
        message.assign(source.message.data(), source.message.size());
        detailsJson.assign(source.detailsJson.data(), source.detailsJson.size());

        info = HttpErrorInfo{
            .statusCode = source.statusCode,
            .statusText = statusText,
            .code = code,
            .message = message,
            .detailsJson = detailsJson};
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
    } catch (const std::invalid_argument& error) {
        errorInfo.assign(HttpErrorInfo{.statusCode = 400, .message = error.what()});
    } catch (const std::exception& error) {
        errorInfo.assign(HttpErrorInfo{.statusCode = 500, .message = error.what()});
    } catch (...) {
        errorInfo.assign(HttpErrorInfo{.statusCode = 500, .message = "unhandled exception"});
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
        services);
    if (auto* responseStream = services.responseStream(); responseStream != nullptr) {
        responseStream->bindContext(context);
    }
    // A kDynamic route runs the ordinary buffered handler chain with the stream
    // writer bound: if the handler streams, the sink commits and that is the
    // response; otherwise the returned HttpResponse is sent buffered. outcome
    // stays kBufferedResponse so the caller lets committed() decide.
    if (route.isDynamicResponse()) {
        auto response = co_await invokeRoute(route, context);
        co_return StreamDispatchResult(
            std::move(response),
            RouteStreamDispatchOutcome::kBufferedResponse);
    }
    if (!route.hasMiddleware()) {
        co_await route.streamHandler()(context);
        co_return StreamDispatchResult(
            HttpResponse(context.resource()),
            RouteStreamDispatchOutcome::kStreamHandled);
    }
    co_await invokeStreamMiddlewareAt(route, 0, context, outcome);
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
        if (request.method() == HttpMethod::kOptions && request.path() == "*") {
            co_return makeAllowNoContentResponse(memory, allowedMethodsForServer());
        }

        if (resolution.methodNotAllowed()) {
            if (request.method() == HttpMethod::kOptions) {
                co_return makeAllowNoContentResponse(memory, resolution.allowedMethods());
            }

            const auto error = HttpErrorInfo{.statusCode = 405, .message = "method not allowed"};
            auto response = co_await handleError(request, memory, error, false, services);
            setAllowHeader(response, resolution.allowedMethods());
            co_return response;
        }

        const auto error = HttpErrorInfo{.statusCode = 404, .message = "route not found"};
        co_return co_await handleError(request, memory, error, false, services);
    }

    auto context = makeRouteContext(memory, request, resolution, services);
    std::exception_ptr exception;
    try {
        const auto& route = resolution.route();
        co_return co_await invokeRoute(route, context);
    } catch (...) {
        exception = std::current_exception();
        detail::ContextAccess::setError(context, exception);
    }
    co_return co_await handleException(context, exception, true);
}

Task<HttpResponse> detail::RouteTable::dispatchBuffered(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    bool closeConnectionOnError,
    ContextServices services) const {
    std::exception_ptr exception;
    try {
        co_return co_await dispatch(request, resolution, memory, services);
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await handleException(request, memory, exception, closeConnectionOnError, services);
}

Task<HttpResponse> detail::RouteTable::handleError(
    const HttpRequest& request,
    RequestMemory& memory,
    HttpErrorInfo error,
    bool closeConnection,
    ContextServices services) const {
    if (errorHandler_ == nullptr) {
        co_return makeErrorResponse(memory.resource(), error, closeConnection);
    }

    auto context = detail::ContextAccess::make(
        memory,
        request,
        services);
    co_return co_await handleError(context, error, closeConnection);
}

Task<HttpResponse> detail::RouteTable::handleException(
    const HttpRequest& request,
    RequestMemory& memory,
    std::exception_ptr exception,
    bool closeConnection,
    ContextServices services) const {
    if (errorHandler_ == nullptr) {
        OwnedHttpErrorInfo errorInfo(memory.resource(), exception);
        co_return makeErrorResponse(memory.resource(), errorInfo.info, closeConnection);
    }

    auto context = detail::ContextAccess::make(
        memory,
        request,
        services);
    co_return co_await handleException(context, exception, closeConnection);
}

Task<HttpResponse> detail::RouteTable::handleError(
    Context& context,
    HttpErrorInfo error,
    bool closeConnection) const {
    return makeErrorResponse(context, error, closeConnection, errorHandler_);
}

Task<HttpResponse> detail::RouteTable::handleException(
    Context& context,
    std::exception_ptr exception,
    bool closeConnection) const {
    OwnedHttpErrorInfo errorInfo(context.resource(), exception);

    co_return co_await handleError(context, errorInfo.info, closeConnection);
}

}  // namespace ruvia
