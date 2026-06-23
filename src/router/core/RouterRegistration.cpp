#include "../RouterInternal.h"

#include "RouterUtils.h"

namespace ruvia {

using namespace detail;

namespace {

std::pmr::string joinControllerPaths(std::string_view prefix, std::string_view path) {
    auto* resource = startupResource();
    if (prefix.empty() || prefix == "/") {
        if (path.empty()) {
            return std::pmr::string{"/", resource};
        }
        if (path.front() == '/') {
            return std::pmr::string(path, resource);
        }

        std::pmr::string output(resource);
        output.reserve(path.size() + 1);
        output.push_back('/');
        output.append(path);
        return output;
    }

    std::pmr::string output(resource);
    output.reserve(prefix.size() + path.size() + 1);
    output.append(prefix.front() == '/' ? prefix : std::string_view{});
    if (prefix.front() != '/') {
        output.push_back('/');
        output.append(prefix);
    }
    if (output.size() > 1 && output.back() == '/') {
        output.pop_back();
    }
    if (path.empty() || path == "/") {
        return output;
    }
    if (path.front() == '/') {
        path.remove_prefix(1);
    }
    output.push_back('/');
    output.append(path);
    return output;
}

[[nodiscard]] RouteHandler makeRouteHandler(ControllerRouteHandler handler) noexcept {
    return RouteHandler{handler.target, handler.invoke};
}

[[nodiscard]] RouteStreamHandler makeRouteStreamHandler(ControllerRouteStreamHandler handler) noexcept {
    return RouteStreamHandler{handler.target, handler.invoke};
}

[[nodiscard]] RouteMiddleware makeRouteMiddleware(ControllerMiddlewareDescriptor middleware) noexcept {
    return RouteMiddleware{
        nullptr,
        middleware.invoke,
        middleware.create,
        middleware.destroy};
}

template <typename DescriptorRange>
void appendRouteMiddlewares(
    std::pmr::vector<RouteMiddleware>& middlewares,
    const DescriptorRange& descriptors) {
    for (const auto& descriptor : descriptors) {
        middlewares.push_back(makeRouteMiddleware(descriptor));
    }
}

template <typename BaseRange, typename ExtraRange>
[[nodiscard]] std::pmr::vector<RouteMiddleware> makeRouteMiddlewares(
    const BaseRange& base,
    const ExtraRange& extra) {
    std::pmr::vector<RouteMiddleware> middlewares(startupResource());
    middlewares.reserve(base.size() + extra.size());
    appendRouteMiddlewares(middlewares, base);
    appendRouteMiddlewares(middlewares, extra);
    return middlewares;
}

[[nodiscard]] std::pmr::vector<ControllerMiddlewareDescriptor> mergeControllerMiddlewares(
    const std::pmr::vector<ControllerMiddlewareDescriptor>& base,
    std::pmr::vector<ControllerMiddlewareDescriptor> extra) {
    std::pmr::vector<ControllerMiddlewareDescriptor> merged(startupResource());
    merged.reserve(base.size() + extra.size());
    merged.insert(merged.end(), base.begin(), base.end());
    merged.insert(merged.end(), extra.begin(), extra.end());
    return merged;
}

}  // namespace

void detail::ControllerRouteBuilder::ImplDeleter::operator()(Impl* impl) const noexcept {
    destroyPmrObject(impl, startupResource());
}

void detail::RouterImpl::registerRoute(
    HttpMethod method,
    std::pmr::string path,
    RouteHandler handler,
    RequestBodyMode bodyMode,
    std::pmr::vector<RouteMiddleware> middlewares,
    ResponseBodyMode responseMode) {
    if (!handler) {
        throw std::invalid_argument("route handler must not be empty");
    }
    if (responseMode == ResponseBodyMode::kWebSocket) {
        throw std::invalid_argument("buffered route cannot use the websocket response mode");
    }

    appendPendingRoute(PendingRoute{
        .method = method,
        .path = std::move(path),
        .handler = std::move(handler),
        .streamHandler = {},
        .bodyMode = bodyMode,
        .responseMode = responseMode,
        .dynamic = false,
        .middlewares = std::move(middlewares),
        .webSocketSubprotocols = std::pmr::string(startupResource()),
        .webSocketHeartbeat = {}});
}

void detail::RouterImpl::registerStreamRoute(
    HttpMethod method,
    std::pmr::string path,
    RouteStreamHandler handler,
    ResponseBodyMode responseMode,
    std::pmr::vector<RouteMiddleware> middlewares,
    WebSocketRouteOptions webSocketOptions) {
    if (!handler) {
        throw std::invalid_argument("route stream handler must not be empty");
    }
    if (responseMode == ResponseBodyMode::kBuffered) {
        throw std::invalid_argument("response stream route requires a streaming response mode");
    }

    appendPendingRoute(PendingRoute{
        .method = method,
        .path = std::move(path),
        .handler = {},
        .streamHandler = std::move(handler),
        .bodyMode = RequestBodyMode::kBuffered,
        .responseMode = responseMode,
        .dynamic = false,
        .middlewares = std::move(middlewares),
        .webSocketSubprotocols = std::pmr::string(webSocketOptions.subprotocols, startupResource()),
        .webSocketHeartbeat = webSocketOptions.heartbeat});
}

void detail::RouterImpl::appendPendingRoute(PendingRoute route) {
    if (finalized_) {
        throw std::logic_error("cannot register route after router finalize");
    }
    validateRouteTarget(route.method, route.path);
    route.dynamic = RouteTable::isDynamicPath(route.path);
    materializeMiddlewares(route.middlewares);
    pendingRoutes_.push_back(std::move(route));
}

void detail::RouterImpl::prependMiddlewares(std::span<const ControllerMiddlewareDescriptor> middlewares) {
    if (finalized_) {
        throw std::logic_error("cannot register middleware after router finalize");
    }
    if (middlewares.empty() || pendingRoutes_.empty()) {
        return;
    }

    for (auto& route : pendingRoutes_) {
        std::pmr::vector<RouteMiddleware> merged(startupResource());
        merged.reserve(middlewares.size() + route.middlewares.size());
        appendRouteMiddlewares(merged, middlewares);
        materializeMiddlewares(merged);
        merged.insert(merged.end(), route.middlewares.begin(), route.middlewares.end());
        route.middlewares = std::move(merged);
    }
}

detail::ControllerRouteBuilder::ControllerRouteBuilder(
    Router& router,
    std::string_view prefix,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares)
    : impl_(
          constructPmrObject<Impl>(
              startupResource(),
              router,
              joinControllerPaths({}, prefix),
              std::move(middlewares))) {}

detail::ControllerRouteBuilder::ControllerRouteBuilder(ControllerRouteBuilder&&) noexcept = default;

detail::ControllerRouteBuilder& detail::ControllerRouteBuilder::operator=(ControllerRouteBuilder&&) noexcept = default;

detail::ControllerRouteBuilder::~ControllerRouteBuilder() = default;

void detail::ControllerRouteBuilder::registerRoute(
    HttpMethod method,
    std::pmr::string path,
    ControllerRouteHandler handler,
    RequestBodyMode bodyMode,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
    ResponseBodyMode responseMode) const {
    RouterImpl::from(*impl_->router).registerRoute(
        method,
        joinControllerPaths(impl_->prefix, path),
        makeRouteHandler(handler),
        bodyMode,
        makeRouteMiddlewares(impl_->middlewares, middlewares),
        responseMode);
}

void detail::ControllerRouteBuilder::registerStreamRoute(
    HttpMethod method,
    std::pmr::string path,
    ControllerRouteStreamHandler handler,
    ResponseBodyMode responseMode,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
    WebSocketRouteOptions webSocketOptions) const {
    RouterImpl::from(*impl_->router).registerStreamRoute(
        method,
        joinControllerPaths(impl_->prefix, path),
        makeRouteStreamHandler(handler),
        responseMode,
        makeRouteMiddlewares(impl_->middlewares, middlewares),
        webSocketOptions);
}

detail::ControllerRouteBuilder detail::ControllerRouteBuilder::createScope(
    std::string_view prefix,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares) const {
    auto merged = mergeControllerMiddlewares(impl_->middlewares, std::move(middlewares));
    return ControllerRouteBuilder(
        *impl_->router,
        joinControllerPaths(impl_->prefix, prefix),
        std::move(merged));
}

}  // namespace ruvia
