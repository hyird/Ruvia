#include "../RouterInternal.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include "RouterUtils.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/Validation.h"

namespace ruvia {
namespace {

std::pmr::string makeAllowHeader(std::pmr::memory_resource* resource, std::uint32_t methodMask) {
    std::pmr::string output(resource);
    output.reserve(64);
    bool first = true;
    for (std::size_t i = 0; i < 7; ++i) {
        if ((methodMask & (1U << i)) == 0) {
            continue;
        }
        if (!first) {
            output.append(", ");
        }
        first = false;
        const auto method = methodName(static_cast<HttpMethod>(i));
        output.append(method.data(), method.size());
    }
    return output;
}

struct OwnedHttpErrorInfo final {
    HttpErrorInfo info{};
    std::pmr::string statusText;
    std::pmr::string code;
    std::pmr::string message;
    std::pmr::string detailsJson;

    explicit OwnedHttpErrorInfo(HttpErrorInfo source)
        : statusText(detail::startupResource()),
          code(detail::startupResource()),
          message(detail::startupResource()),
          detailsJson(detail::startupResource()) {
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
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader,
    HttpClientRegistry* httpClients) const {
    return dispatch(request, resolve(request), memory, db, redis, bodyReader, bodyLoader, httpClients);
}

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchResponseStream(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    ResponseStreamWriter& responseStream,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader,
    HttpClientRegistry* httpClients) const {
    if (!resolution.found() || resolution.route == nullptr || resolution.route->responseMode == ResponseBodyMode::kBuffered) {
        throw std::logic_error("route is not a response stream route");
    }
    const auto* route = resolution.route;
    auto streamHandled = false;
    if (!resolution.dynamic) {
        Context context(memory, request, &responseStream, db, redis, bodyReader, bodyLoader, nullptr, httpClients);
        responseStream.bindContext(context);
        auto response = co_await invokeStreamRoute(*route, context, streamHandled);
        co_return StreamDispatchResult{std::move(response), streamHandled};
    }

    Context context(
        memory,
        request,
        resolution.match.params,
        resolution.match.paramCount,
        &responseStream,
        db,
        redis,
        bodyReader,
        bodyLoader,
        nullptr,
        httpClients);
    responseStream.bindContext(context);
    auto response = co_await invokeStreamRoute(*route, context, streamHandled);
    co_return StreamDispatchResult{std::move(response), streamHandled};
}

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchWebSocket(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    WebSocket& webSocket,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader,
    HttpClientRegistry* httpClients) const {
    if (!resolution.found() || resolution.route == nullptr || resolution.route->responseMode != ResponseBodyMode::kWebSocket) {
        throw std::logic_error("route is not a websocket route");
    }
    const auto* route = resolution.route;
    auto streamHandled = false;
    if (!resolution.dynamic) {
        Context context(memory, request, db, redis, bodyReader, bodyLoader, &webSocket, httpClients);
        auto response = co_await invokeStreamRoute(*route, context, streamHandled);
        co_return StreamDispatchResult{std::move(response), streamHandled};
    }

    Context context(
        memory,
        request,
        resolution.match.params,
        resolution.match.paramCount,
        db,
        redis,
        bodyReader,
        bodyLoader,
        &webSocket,
        httpClients);
    auto response = co_await invokeStreamRoute(*route, context, streamHandled);
    co_return StreamDispatchResult{std::move(response), streamHandled};
}

Task<HttpResponse> detail::RouteTable::dispatch(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader,
    HttpClientRegistry* httpClients) const {
    if (resolution.found() && !resolution.dynamic) {
        const auto* route = resolution.route;
        Context context(memory, request, db, redis, bodyReader, bodyLoader, nullptr, httpClients);
        std::exception_ptr exception;
        try {
            co_return co_await invokeRoute(*route, context);
        } catch (...) {
            exception = std::current_exception();
        }
        co_return co_await handleException(context, exception, true);
    }

    if (!resolution.found()) {
        Context context(memory, request, db, redis, bodyReader, bodyLoader, nullptr, httpClients);
        if (request.method() == HttpMethod::kOptions && request.path() == "*") {
            auto allow = makeAllowHeader(memory.resource(), allowedMethodsForServer());
            HttpResponse response(memory.resource());
            response.setStatus(204, "No Content");
            response.setHeader("Allow", allow);
            co_return response;
        }

        if (resolution.status == RouteResolveStatus::kMethodNotAllowed) {
            auto allow = makeAllowHeader(memory.resource(), resolution.allowedMethods);
            if (request.method() == HttpMethod::kOptions) {
                HttpResponse response(memory.resource());
                response.setStatus(204, "No Content");
                response.setHeader("Allow", allow);
                co_return response;
            }

            auto response = co_await handleError(
                context,
                HttpErrorInfo{.statusCode = 405, .message = "method not allowed"},
                false);
            response.setHeader("Allow", allow);
            co_return response;
        }

        co_return co_await handleError(
            context,
            HttpErrorInfo{.statusCode = 404, .message = "route not found"},
            false);
    }

    Context context(
        memory,
        request,
        resolution.match.params,
        resolution.match.paramCount,
        db,
        redis,
        bodyReader,
        bodyLoader,
        nullptr,
        httpClients);
    std::exception_ptr exception;
    try {
        co_return co_await invokeRoute(*resolution.route, context);
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await handleException(context, exception, true);
}

Task<HttpResponse> detail::RouteTable::invokeRoute(const RouteEntry& route, Context& context) const {
    // Hot path: a route with no middleware goes straight to the handler. This
    // is a plain (non-coroutine) forward, so skipping invokeMiddlewareAt saves
    // one heap-allocated coroutine frame and one HttpResponse move per request
    // for the common zero-middleware route. invokeMiddlewareAt at index 0 with
    // middlewareCount == 0 is exactly `co_await route.handler(context)`, so the
    // behavior (including exception propagation into dispatch's try/catch) is
    // identical.
    if (route.middlewareCount == 0) {
        return route.handler(context);
    }
    return invokeMiddlewareAt(route, 0, context);
}

Task<HttpResponse> detail::RouteTable::invokeMiddlewareAt(
    const RouteEntry& route,
    std::size_t index,
    Context& context) const {
    if (index >= route.middlewareCount) {
        co_return co_await route.handler(context);
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset + index];
    MiddlewareContinuation continuation{this, &route, index + 1};
    const Next next(&continuation, &RouteTable::invokeMiddlewareContinuation);
    auto response = middleware(context, next);
    co_return co_await std::move(response);
}

Task<HttpResponse> detail::RouteTable::invokeMiddlewareContinuation(void* target, Context& context) {
    const auto* continuation = static_cast<const MiddlewareContinuation*>(target);
    return continuation->table->invokeMiddlewareAt(*continuation->route, continuation->index, context);
}

Task<HttpResponse> detail::RouteTable::invokeStreamRoute(
    const RouteEntry& route,
    Context& context,
    bool& streamHandled) const {
    return invokeStreamMiddlewareAt(route, 0, context, streamHandled);
}

Task<HttpResponse> detail::RouteTable::invokeStreamMiddlewareAt(
    const RouteEntry& route,
    std::size_t index,
    Context& context,
    bool& streamHandled) const {
    if (index >= route.middlewareCount) {
        co_await route.streamHandler(context);
        streamHandled = true;
        co_return HttpResponse(context.resource());
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset + index];
    StreamMiddlewareContinuation continuation{this, &route, index + 1, &streamHandled};
    const Next next(&continuation, &RouteTable::invokeStreamMiddlewareContinuation);
    auto response = middleware(context, next);
    co_return co_await std::move(response);
}

Task<HttpResponse> detail::RouteTable::invokeStreamMiddlewareContinuation(void* target, Context& context) {
    const auto* continuation = static_cast<const StreamMiddlewareContinuation*>(target);
    return continuation->table->invokeStreamMiddlewareAt(
        *continuation->route,
        continuation->index,
        context,
        *continuation->streamHandled);
}

Task<HttpResponse> detail::RouteTable::handleError(
    const HttpRequest& request,
    RequestMemory& memory,
    HttpErrorInfo error,
    bool closeConnection,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader,
    HttpClientRegistry* httpClients) const {
    Context context(memory, request, db, redis, bodyReader, bodyLoader, nullptr, httpClients);
    co_return co_await handleError(context, error, closeConnection);
}

Task<HttpResponse> detail::RouteTable::handleException(
    const HttpRequest& request,
    RequestMemory& memory,
    std::exception_ptr exception,
    bool closeConnection,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader,
    HttpClientRegistry* httpClients) const {
    Context context(memory, request, db, redis, bodyReader, bodyLoader, nullptr, httpClients);
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
    OwnedHttpErrorInfo errorInfo(HttpErrorInfo{.statusCode = 500, .message = "unhandled exception"});

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
