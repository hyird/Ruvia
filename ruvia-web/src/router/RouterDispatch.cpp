#include "ruvia/web/detail/router/RouteTable.h"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/router/RouteDispatchServices.h"

// Choosing what answers a request: the matched route, a 405 with Allow, the
// document-root fallback, or nothing -- and running a buffered handler once one
// is chosen.

namespace ruvia {

namespace {

void setAllowHeader(HttpResponse& response, std::uint32_t methodMask,
    std::span<const std::string_view> extensionMethods = {}) {
    detail::setResponseAllowHeader(response, methodMask, extensionMethods);
}

HttpResponse makeAllowNoContentResponse(RequestMemory& memory, std::uint32_t methodMask,
    std::span<const std::string_view> extensionMethods = {}) {
    HttpResponse response({.resource = memory.resource()});
    response.status(ruvia::http_status::kNoContent);
    setAllowHeader(response, methodMask, extensionMethods);
    return response;
}

[[nodiscard]] std::optional<HttpResponse> selectDocumentRootFallback(
    const detail::DocumentRootBinding& documentRoot, const HttpRequest& request,
    RequestMemory& memory, const detail::ContextServices& services,
    detail::StaticFileSelectionMode staticFileMode) {
    const auto* const root = documentRoot.root();
    if (root == nullptr || (request.knownMethod() != HttpKnownMethod::kGet &&
                               request.knownMethod() != HttpKnownMethod::kHead)) {
        return std::nullopt;
    }

    auto relative = request.path();
    if (!relative.empty() && relative.front() == '/') {
        relative.remove_prefix(1);
    }

    auto context = detail::ContextAccess::make(memory, request, services);
    try {
        if (staticFileMode == detail::StaticFileSelectionMode::kPrecompressed) {
            return detail::ContextAccess::staticFileWithPrecompressedVariants(
                context, *root, {.relativePath = relative});
        }
        return context.staticFile(*root, {.relativePath = relative});
    } catch (const HttpError& error) {
        // A document-root miss is allowed to fall through to the router's
        // normal not-found path. A real response error (for example 406 when
        // every Accept-Encoding choice is forbidden, or 412 from a file
        // precondition) must retain its status instead of being rewritten as
        // 404 by the fallback probe.
        const auto status = error.info().status();
        if (status == ruvia::http_status::kForbidden || status == ruvia::http_status::kNotFound) {
            return std::nullopt;
        }
        throw;
    }
}

}  // namespace

Task<HttpResponse> detail::RouteTable::dispatch(
    const HttpRequest& request, RequestMemory& memory, ContextServices services) const {
    const auto resolution = resolve(request);
    co_return co_await dispatch(request, resolution, memory, services);
}

Task<std::optional<HttpResponse>> detail::RouteTable::dispatchResponseStream(
    const HttpRequest& request, const ResolvedRoute& resolved, RequestMemory& memory,
    ResponseStreamWriter& responseStream, ContextServices services) const {
    if (resolved.route().endpoint().responseStream() == nullptr) {
        throw std::logic_error("route is not a response stream route");
    }
    return dispatchStreamRoute(request, resolved, memory,
        resolved.route().endpoint().responseStream()->handler(),
        services.withResponseStream(responseStream));
}

Task<std::optional<HttpResponse>> detail::RouteTable::dispatchWebSocket(const HttpRequest& request,
    const ResolvedRoute& resolved, RequestMemory& memory, const RouteStreamHandler& handler,
    ContextServices services) const {
    if (resolved.route().endpoint().webSocket() == nullptr) {
        throw std::logic_error("route is not a websocket route");
    }
    return dispatchStreamRoute(request, resolved, memory, handler, services);
}

Task<HttpResponse> detail::RouteTable::dispatch(const HttpRequest& request,
    const RouteResolution& resolution, RequestMemory& memory, ContextServices services) const {
    co_return co_await dispatchRequest(request, resolution, memory, services,
        DocumentRootBinding::none(), DispatchFailure::kPropagate,
        StaticFileSelectionMode::kIdentityOnly);
}

Task<HttpResponse> detail::RouteTable::dispatchRequest(const HttpRequest& request,
    const RouteResolution& resolution, RequestMemory& memory, ContextServices services,
    DocumentRootBinding documentRoot, DispatchFailure failure,
    StaticFileSelectionMode staticFileMode) const {
    std::exception_ptr dispatchException;
    try {
        const auto* resolved = resolution.resolved();
        if (resolved == nullptr) {
            if (auto documentResponse = selectDocumentRootFallback(
                    documentRoot, request, memory, services, staticFileMode)) {
                co_return std::move(*documentResponse);
            }
            // One handleError co_await serves both rejection kinds: each
            // co_await expression reserves its own slots for the call's
            // temporaries in the frame, so distinct inline sites would each
            // add a resident HttpResponse-sized block.
            std::optional<HttpErrorInfo> error;
            std::uint32_t allowedMethods = 0;
            // Room for the extension tokens a 405 must name in Allow. Fixed and
            // small: a resource carrying more distinct extension methods than
            // this would be pathological, and truncating beats allocating on
            // the error path.
            std::string_view extensionMethodBuffer[8];
            std::span<const std::string_view> extensionMethods;
            if (request.knownMethod() == HttpKnownMethod::kUnknown &&
                !recognizesMethodToken(request.method())) {
                // RFC 9110 15.5.6/15.6.2: 405 says the method is known here but
                // unsupported by this resource; a token no route registered is
                // not known here at all, so it stays 501 whatever the path holds.
                error = HttpErrorInfo({.status = ruvia::http_status::kNotImplemented,
                    .message = "method not implemented"});
            } else if (request.knownMethod() == HttpKnownMethod::kUnknown) {
                if (const auto* methodNotAllowed = resolution.methodNotAllowed()) {
                    error = HttpErrorInfo({.status = ruvia::http_status::kMethodNotAllowed,
                        .message = "method not allowed"});
                    allowedMethods = methodNotAllowed->allowedMethods();
                    extensionMethods = extensionMethodsFor(request.path(), extensionMethodBuffer);
                }
                // Otherwise the method is known here but this path has nothing:
                // an ordinary 404, which the fallback below produces.
            } else if (request.knownMethod() == HttpKnownMethod::kOptions &&
                       request.path() == "*") {
                co_return makeAllowNoContentResponse(
                    memory, allowedMethodsForServer(), extensionMethodsForServer());
            } else if (const auto* methodNotAllowed = resolution.methodNotAllowed()) {
                extensionMethods = extensionMethodsFor(request.path(), extensionMethodBuffer);
                if (request.knownMethod() == HttpKnownMethod::kOptions) {
                    co_return makeAllowNoContentResponse(
                        memory, methodNotAllowed->allowedMethods(), extensionMethods);
                }
                error = HttpErrorInfo({.status = ruvia::http_status::kMethodNotAllowed,
                    .message = "method not allowed"});
                allowedMethods = methodNotAllowed->allowedMethods();
            }

            if (error) {
                auto response = co_await handleError(request, memory, *error, services);
                if (allowedMethods != 0 || !extensionMethods.empty()) {
                    setAllowHeader(response, allowedMethods, extensionMethods);
                }
                co_return std::move(response);
            }

            co_return co_await handleNotFound(request, memory, services);
        }

        auto context = makeRouteContext(memory, request, *resolved,
            withRouteHandlers(services, *this, errorHandlerFor(request.path()),
                notFoundHandlerFor(request.path())));
        std::exception_ptr exception;
        try {
            const auto& route = resolved->route();
            if (route.endpoint().buffered() == nullptr) {
                throw std::logic_error("streaming route requires its dedicated dispatch path");
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

Task<HttpResponse> detail::RouteTable::dispatchBufferedResponse(const HttpRequest& request,
    const RouteResolution& resolution, RequestMemory& memory, DocumentRootBinding documentRoot,
    ContextServices services, StaticFileSelectionMode staticFileMode) const {
    // Plain forwarding, not a coroutine: document-root selection and the
    // failure policy live in the existing dispatch frame, so the unified
    // application entry adds no request-path allocation.
    return dispatchRequest(request, resolution, memory, services, std::move(documentRoot),
        DispatchFailure::kRespond, staticFileMode);
}

}  // namespace ruvia
