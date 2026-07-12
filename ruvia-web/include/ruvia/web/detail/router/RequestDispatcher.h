#pragma once

#include <exception>
#include <string_view>

#include "ruvia/core/Task.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/detail/http/ContextServices.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/router/RouteStreamResult.h"

namespace ruvia::detail {

// Web dispatch contract implemented by RouteTable, which builds a Context and runs
// controllers + middleware. Server sessions hold a const RequestDispatcher& instead
// of naming RouteTable so transport code stays decoupled from the route index shape.
class RequestDispatcher {
public:
    virtual ~RequestDispatcher() = default;

    [[nodiscard]] virtual RouteResolution resolve(
        const HttpRequest& request) const noexcept = 0;
    [[nodiscard]] virtual RouteResolution resolve(
        HttpKnownMethod method, std::string_view path) const noexcept = 0;

    virtual Task<HttpResponse> dispatch(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ContextServices services) const = 0;
    virtual Task<HttpResponse> dispatchBuffered(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ContextServices services) const = 0;
    virtual Task<HttpResponse> handleError(
        const HttpRequest& request,
        RequestMemory& memory,
        HttpErrorInfo error,
        ContextServices services) const = 0;
    virtual Task<HttpResponse> handleException(
        const HttpRequest& request,
        RequestMemory& memory,
        std::exception_ptr exception,
        ContextServices services) const = 0;
    virtual Task<StreamDispatchResult> dispatchResponseStream(
        const HttpRequest& request,
        const ResolvedRoute& route,
        RequestMemory& memory,
        ResponseStreamWriter& responseStream,
        ContextServices services) const = 0;
    virtual Task<StreamDispatchResult> dispatchWebSocket(
        const HttpRequest& request,
        const ResolvedRoute& route,
        RequestMemory& memory,
        WebSocket& webSocket,
        ContextServices services) const = 0;
};

}  // namespace ruvia::detail
