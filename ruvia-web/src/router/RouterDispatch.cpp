#include "ruvia/web/detail/router/RouteTable.h"

#include <cstdint>
#include <optional>
#include <utility>

#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/web/detail/router/RouteDispatchServices.h"

// Choosing what answers a request: the matched route, a 405 with Allow, the
// document-root fallback, or nothing -- and running a buffered handler once one
// is chosen.

namespace ruvia {

namespace {

void setAllowHeader(HttpResponse& response, std::uint32_t methodMask) {
    detail::setResponseAllowHeader(response, methodMask);
}

HttpResponse makeAllowNoContentResponse(RequestMemory& memory, std::uint32_t methodMask) {
    HttpResponse response(memory.resource());
    response.status(ruvia::http_status::kNoContent);
    setAllowHeader(response, methodMask);
    return response;
}


[[nodiscard]] std::optional<HttpResponse> selectDocumentRootFallback(
    const StaticRoot* root,
    const HttpRequest& request,
    RequestMemory& memory) {
    if (root == nullptr ||
        (request.knownMethod() != HttpKnownMethod::kGet &&
         request.knownMethod() != HttpKnownMethod::kHead)) {
        return std::nullopt;
    }

    auto relative = request.path();
    if (!relative.empty() && relative.front() == '/') {
        relative.remove_prefix(1);
    }

    auto context = detail::ContextAccess::make(memory, request);
    try {
        return context.staticFile(*root, relative);
    } catch (const HttpError&) {
        return std::nullopt;
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

Task<std::optional<HttpResponse>> detail::RouteTable::dispatchResponseStream(
    const HttpRequest& request,
    const ResolvedRoute& resolved,
    RequestMemory& memory,
    ResponseStreamWriter& responseStream,
    ContextServices services) const {
    if (resolved.route().endpoint().responseStream() == nullptr) {
        throw std::logic_error("route is not a response stream route");
    }
    return dispatchStreamRoute(
        request,
        resolved,
        memory,
        resolved.route().endpoint().responseStream()->handler(),
        services.withResponseStream(responseStream));
}

Task<std::optional<HttpResponse>> detail::RouteTable::dispatchWebSocket(
    const HttpRequest& request,
    const ResolvedRoute& resolved,
    RequestMemory& memory,
    const RouteStreamHandler& handler,
    ContextServices services) const {
    if (resolved.route().endpoint().webSocket() == nullptr) {
        throw std::logic_error("route is not a websocket route");
    }
    return dispatchStreamRoute(request, resolved, memory, handler, services);
}

Task<HttpResponse> detail::RouteTable::dispatch(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    ContextServices services) const {
    return dispatchRequest(
        request,
        resolution,
        memory,
        services,
        nullptr,
        DispatchFailure::kPropagate);
}

Task<HttpResponse> detail::RouteTable::dispatchRequest(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    ContextServices services,
    const StaticRoot* documentRoot,
    DispatchFailure failure) const {
    std::exception_ptr dispatchException;
    try {
        const auto* resolved = resolution.resolved();
        if (resolved == nullptr) {
            if (auto documentResponse = selectDocumentRootFallback(
                    documentRoot, request, memory)) {
                co_return std::move(*documentResponse);
            }
            // One handleError co_await serves both rejection kinds: each
            // co_await expression reserves its own slots for the call's
            // temporaries in the frame, so distinct inline sites would each
            // add a resident HttpResponse-sized block.
            std::optional<HttpErrorInfo> error;
            std::uint32_t allowedMethods = 0;
            if (request.knownMethod() == HttpKnownMethod::kUnknown) {
                error = HttpErrorInfo(ruvia::http_status::kNotImplemented, {}, "method not implemented");
            } else if (request.knownMethod() == HttpKnownMethod::kOptions && request.path() == "*") {
                co_return makeAllowNoContentResponse(memory, allowedMethodsForServer());
            } else if (const auto* methodNotAllowed = resolution.methodNotAllowed()) {
                if (request.knownMethod() == HttpKnownMethod::kOptions) {
                    co_return makeAllowNoContentResponse(
                        memory, methodNotAllowed->allowedMethods());
                }
                error = HttpErrorInfo(ruvia::http_status::kMethodNotAllowed, {}, "method not allowed");
                allowedMethods = methodNotAllowed->allowedMethods();
            }

            if (error) {
                auto response = co_await handleError(request, memory, *error, services);
                if (allowedMethods != 0) {
                    setAllowHeader(response, allowedMethods);
                }
                co_return response;
            }

            co_return co_await handleNotFound(request, memory, services);
        }

        auto context = makeRouteContext(
            memory,
            request,
            *resolved,
            withRouteHandlers(
                services,
                *this,
                errorHandlerFor(request.path()),
                notFoundHandlerFor(request.path())));
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
    } catch (...) {
        if (failure == DispatchFailure::kPropagate) {
            throw;
        }
        dispatchException = std::current_exception();
    }
    co_return co_await handleException(request, memory, dispatchException, services);
}

Task<HttpResponse> detail::RouteTable::dispatchBufferedResponse(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    const StaticRoot* documentRoot,
    ContextServices services) const {
    // Plain forwarding, not a coroutine: document-root selection and the
    // failure policy live in the existing dispatch frame, so the unified
    // application entry adds no request-path allocation.
    return dispatchRequest(
        request,
        resolution,
        memory,
        services,
        documentRoot,
        DispatchFailure::kRespond);
}

}  // namespace ruvia
