#include "ruvia/web/detail/router/RouterImpl.h"

#include "ruvia/web/detail/util/RegistrationResource.h"

namespace ruvia {

using namespace detail;

detail::RouterImpl::PendingRoute::PendingRoute(std::pmr::memory_resource* resource, Init init)
    : method_(init.method),
      methodToken_(init.methodToken, resource),
      path_(resource),
      endpoint_(std::move(init.endpoint)),
      dynamic_(init.dynamic),
      maxRequestBodyBytes_(init.maxRequestBodyBytes),
      deadlineMs_(init.deadlineMs),
      middlewares_(resource) {
    auto* const routeResource = path_.get_allocator().resource();
    if (init.path.get_allocator().resource() == routeResource) {
        path_ = std::move(init.path);
    } else {
        path_.assign(init.path.data(), init.path.size());
    }

    if (init.middlewares.get_allocator().resource() == routeResource) {
        middlewares_ = std::move(init.middlewares);
    } else {
        middlewares_.insert(middlewares_.end(), init.middlewares.begin(), init.middlewares.end());
    }
}

void detail::RouterImpl::registerRoute(HttpKnownMethod method, std::pmr::string path, RouteHandler handler, RequestBodyMode bodyMode, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares) {
    registerEndpoint(method, std::move(path), RouteEndpoint::buffered(handler, bodyMode), controllerMiddlewares, routeMiddlewares);
}

void detail::RouterImpl::registerResponseStreamRoute(HttpKnownMethod method, std::pmr::string path, RouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares) {
    registerEndpoint(method, std::move(path), RouteEndpoint::responseStream(handler, ResponseStreamKind::kGeneric), controllerMiddlewares, routeMiddlewares);
}

void detail::RouterImpl::registerSseRoute(HttpKnownMethod method, std::pmr::string path, RouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares) {
    registerEndpoint(method, std::move(path), RouteEndpoint::responseStream(handler, ResponseStreamKind::kSse), controllerMiddlewares, routeMiddlewares);
}

void detail::RouterImpl::registerWebSocketRoute(HttpKnownMethod method, std::pmr::string path, RouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares, WebSocketRouteConfig webSocketConfig) {
    registerEndpoint(method, std::move(path), RouteEndpoint::webSocket(resource_, handler, std::move(webSocketConfig)), controllerMiddlewares, routeMiddlewares);
}

namespace {

// The tightest ceiling any of the route's middlewares declared. Several may:
// a controller-wide one and a route-specific one, and the stricter must win
// rather than the last registered.
[[nodiscard]] std::size_t declaredRequestBodyLimit(std::span<const detail::ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const detail::ControllerMiddlewareDescriptor> routeMiddlewares) noexcept {
    std::size_t limit = 0;
    const auto consider = [&limit](std::span<const detail::ControllerMiddlewareDescriptor> descriptors) noexcept {
        for (const auto& descriptor : descriptors) {
            const auto declared = descriptor.requestBodyLimit();
            if (declared != 0 && (limit == 0 || declared < limit)) {
                limit = declared;
            }
        }
    };
    consider(controllerMiddlewares);
    consider(routeMiddlewares);
    return limit;
}

// The strictest deadline any of the route's middlewares declared, by the same
// rule the body limit uses: a narrower scope may only tighten.
[[nodiscard]] std::int64_t declaredDeadlineMs(std::span<const detail::ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const detail::ControllerMiddlewareDescriptor> routeMiddlewares) noexcept {
    std::int64_t deadline = 0;
    const auto consider = [&deadline](std::span<const detail::ControllerMiddlewareDescriptor> descriptors) noexcept {
        for (const auto& descriptor : descriptors) {
            const auto declared = descriptor.deadlineMs();
            if (declared != 0 && (deadline == 0 || declared < deadline)) {
                deadline = declared;
            }
        }
    };
    consider(controllerMiddlewares);
    consider(routeMiddlewares);
    return deadline;
}

}  // namespace

void detail::RouterImpl::registerEndpoint(HttpKnownMethod method, std::pmr::string path, RouteEndpoint endpoint, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares) {
    registerEndpointWithToken(method, {}, std::move(path), std::move(endpoint), controllerMiddlewares, routeMiddlewares);
}

void detail::RouterImpl::registerEndpointWithToken(HttpKnownMethod method, std::string_view methodToken, std::pmr::string path, RouteEndpoint endpoint, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares) {
    appendPendingRoute(PendingRoute(resource_, PendingRoute::Init{.method = method, .methodToken = std::pmr::string(methodToken, resource_), .path = std::move(path), .endpoint = std::move(endpoint), .dynamic = false, .maxRequestBodyBytes = declaredRequestBodyLimit(controllerMiddlewares, routeMiddlewares), .deadlineMs = declaredDeadlineMs(controllerMiddlewares, routeMiddlewares), .middlewares = materializeMiddlewares(controllerMiddlewares, routeMiddlewares)}));
}

void detail::RouterImpl::registerExtensionMethodRoute(std::string_view methodToken, std::pmr::string path, RouteHandler handler, RequestBodyMode bodyMode, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares) {
    if (!isValidHttpMethodToken(methodToken)) {
        throw std::invalid_argument("extension route method must be a valid HTTP method token");
    }
    // A method the framework classifies keeps the enum as its identity, so the
    // enum-indexed lookup stays the single authority for it. Accepting both
    // spellings would mean two routes for one method that only one of the two
    // lookups can ever find.
    if (classifyHttpMethod(methodToken) != HttpKnownMethod::kUnknown) {
        throw std::invalid_argument("extension route method is a known method; register it with its typed route macro");
    }
    registerEndpointWithToken(HttpKnownMethod::kUnknown, methodToken, std::move(path), RouteEndpoint::buffered(handler, bodyMode), controllerMiddlewares, routeMiddlewares);
}

void detail::RouterImpl::appendPendingRoute(PendingRoute route) {
    if (routeTable_) {
        throw std::logic_error("cannot register route after router finalize");
    }
    validateRouteTarget(route.method(), route.methodToken(), route.path());
    route.setDynamic(RouteTable::isDynamicPath(route.path()));
    pendingRoutes_.push_back(std::move(route));
}

}  // namespace ruvia
