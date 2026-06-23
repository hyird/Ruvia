#include "../RouteTable.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include "RouterUtils.h"
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

detail::ContextServices makeContextServices(
    detail::RouteServices services,
    ResponseStreamWriter* responseStream = nullptr,
    WebSocket* webSocket = nullptr) noexcept {
    return detail::ContextServices{
        .db = services.db,
        .redis = services.redis,
        .bodyReader = services.bodyReader,
        .bodyLoader = services.bodyLoader,
        .responseStream = responseStream,
        .webSocket = webSocket,
        .httpClients = services.httpClients};
}

Context makeRouteContext(
    RequestMemory& memory,
    const HttpRequest& request,
    const detail::RouteResolution& resolution,
    detail::ContextServices services) noexcept {
    if (!resolution.dynamic) {
        return detail::ContextAccess::make(memory, request, services);
    }

    return detail::ContextAccess::make(
        memory,
        request,
        resolution.match.params,
        resolution.match.paramCount,
        services);
}

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

}  // namespace

Task<HttpResponse> detail::RouteTable::dispatch(
    const HttpRequest& request,
    RequestMemory& memory,
    RouteServices services) const {
    return dispatch(request, resolve(request), memory, services);
}

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchResponseStream(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    ResponseStreamWriter& responseStream,
    RouteServices services) const {
    if (!resolution.found() || resolution.route == nullptr || resolution.route->responseMode == ResponseBodyMode::kBuffered) {
        throw std::logic_error("route is not a response stream route");
    }
    return dispatchStreamRoute(request, resolution, memory, services, &responseStream, nullptr);
}

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchWebSocket(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    WebSocket& webSocket,
    RouteServices services) const {
    if (!resolution.found() || resolution.route == nullptr || resolution.route->responseMode != ResponseBodyMode::kWebSocket) {
        throw std::logic_error("route is not a websocket route");
    }
    return dispatchStreamRoute(request, resolution, memory, services, nullptr, &webSocket);
}

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchStreamRoute(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    RouteServices services,
    ResponseStreamWriter* responseStream,
    WebSocket* webSocket) const {
    const auto* route = resolution.route;
    auto streamHandled = false;
    auto context = makeRouteContext(
        memory,
        request,
        resolution,
        makeContextServices(services, responseStream, webSocket));
    if (responseStream != nullptr) {
        responseStream->bindContext(context);
    }
    // A kDynamic route runs the ordinary buffered handler chain with the stream
    // writer bound: if the handler streams, the sink commits and that is the
    // response; otherwise the returned HttpResponse is sent buffered. streamHandled
    // stays false so the caller lets committed() decide.
    if (route->responseMode == ResponseBodyMode::kDynamic) {
        auto response = co_await invokeRoute(*route, context);
        co_return StreamDispatchResult{std::move(response), false};
    }
    auto response = co_await invokeStreamRoute(*route, context, streamHandled);
    co_return StreamDispatchResult{std::move(response), streamHandled};
}

Task<HttpResponse> detail::RouteTable::dispatch(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    RouteServices services) const {
    if (!resolution.found()) {
        auto context = detail::ContextAccess::make(
            memory,
            request,
            makeContextServices(services));
        if (request.method() == HttpMethod::kOptions && request.path() == "*") {
            co_return makeAllowNoContentResponse(memory, allowedMethodsForServer());
        }

        if (resolution.status == RouteResolveStatus::kMethodNotAllowed) {
            if (request.method() == HttpMethod::kOptions) {
                co_return makeAllowNoContentResponse(memory, resolution.allowedMethods);
            }

            auto response = co_await handleError(
                context,
                HttpErrorInfo{.statusCode = 405, .message = "method not allowed"},
                false);
            setAllowHeader(response, resolution.allowedMethods);
            co_return response;
        }

        co_return co_await handleError(
            context,
            HttpErrorInfo{.statusCode = 404, .message = "route not found"},
            false);
    }

    auto context = makeRouteContext(memory, request, resolution, makeContextServices(services));
    std::exception_ptr exception;
    try {
        co_return co_await invokeRoute(*resolution.route, context);
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await handleException(context, exception, true);
}

Task<HttpResponse> detail::RouteTable::dispatchBuffered(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    bool closeConnectionOnError,
    RouteServices services) const {
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
    RouteServices services) const {
    auto context = detail::ContextAccess::make(
        memory,
        request,
        makeContextServices(services));
    co_return co_await handleError(context, error, closeConnection);
}

Task<HttpResponse> detail::RouteTable::handleException(
    const HttpRequest& request,
    RequestMemory& memory,
    std::exception_ptr exception,
    bool closeConnection,
    RouteServices services) const {
    auto context = detail::ContextAccess::make(
        memory,
        request,
        makeContextServices(services));
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
    OwnedHttpErrorInfo errorInfo(
        HttpErrorInfo{.statusCode = 500, .message = "unhandled exception"},
        context.resource());

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

    co_return co_await handleError(context, errorInfo.info, closeConnection);
}

}  // namespace ruvia
