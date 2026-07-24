#include "ruvia/web/detail/router/RouterImpl.h"

#include "ruvia/web/detail/util/RegistrationResource.h"

namespace ruvia {

using namespace detail;

detail::RouterImpl::PendingRoute::PendingRoute(std::pmr::memory_resource* resource, Init init)
    : method_(init.method),
      path_(resource),
      endpoint_(std::move(init.endpoint)),
      dynamic_(init.dynamic),
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

void detail::RouterImpl::registerWebSocketRoute(HttpKnownMethod method, std::pmr::string path, RouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares, WebSocketRouteOptions webSocketOptions) {
    registerEndpoint(method, std::move(path), RouteEndpoint::webSocket(resource_, handler, webSocketOptions), controllerMiddlewares, routeMiddlewares);
}

void detail::RouterImpl::registerEndpoint(HttpKnownMethod method, std::pmr::string path, RouteEndpoint endpoint, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares) {
    appendPendingRoute(PendingRoute(resource_, PendingRoute::Init{.method = method, .path = std::move(path), .endpoint = std::move(endpoint), .dynamic = false, .middlewares = materializeMiddlewares(controllerMiddlewares, routeMiddlewares)}));
}

void detail::RouterImpl::appendPendingRoute(PendingRoute route) {
    if (routeTable_) {
        throw std::logic_error("cannot register route after router finalize");
    }
    validateRouteTarget(route.method(), route.path());
    route.setDynamic(RouteTable::isDynamicPath(route.path()));
    pendingRoutes_.push_back(std::move(route));
}

}  // namespace ruvia
