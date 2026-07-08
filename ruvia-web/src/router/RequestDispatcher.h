#pragma once

#include <exception>
#include <string_view>

#include "ruvia/app/Task.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/Streaming.h"
#include "ruvia/http/WebSocket.h"
#include "ruvia/memory/MemoryPool.h"
#include "http/ContextServices.h"
#include "router/RouteResolution.h"
#include "router/RouteStreamResult.h"

namespace ruvia::detail {

// Context-agnostic dispatch contract. Implemented in ruvia-web by RouteTable
// (which builds a Context and runs controllers + middleware) and, in future, by
// the ruvia-edge product (reverse proxy, no Context). Server sessions (h1/h2/ws)
// hold a const RequestDispatcher& instead of naming RouteTable, so they can live
// in ruvia-http and be reused by ruvia-web and ruvia-edge -- siblings that cannot
// link each other. Every parameter/return type here is http-layer, so the sessions
// no longer need any ruvia-web header for dispatch.
class RequestDispatcher {
public:
    virtual ~RequestDispatcher() = default;

    [[nodiscard]] virtual RouteResolution resolve(
        const HttpRequest& request, RouteMatch& match) const noexcept = 0;
    [[nodiscard]] virtual RouteResolution resolve(
        HttpMethod method, std::string_view path, RouteMatch& match) const noexcept = 0;

    virtual Task<HttpResponse> dispatch(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ContextServices services) const = 0;
    virtual Task<HttpResponse> dispatchBuffered(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        bool closeConnectionOnError,
        ContextServices services) const = 0;
    virtual Task<HttpResponse> handleError(
        const HttpRequest& request,
        RequestMemory& memory,
        HttpErrorInfo error,
        bool closeConnection,
        ContextServices services) const = 0;
    virtual Task<HttpResponse> handleException(
        const HttpRequest& request,
        RequestMemory& memory,
        std::exception_ptr exception,
        bool closeConnection,
        ContextServices services) const = 0;
    virtual Task<StreamDispatchResult> dispatchResponseStream(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ResponseStreamWriter& responseStream,
        ContextServices services) const = 0;
    virtual Task<StreamDispatchResult> dispatchWebSocket(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        WebSocket& webSocket,
        ContextServices services) const = 0;
};

}  // namespace ruvia::detail
