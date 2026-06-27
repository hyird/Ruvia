#include "../RouterInternal.h"

#include "RouterUtils.h"
#include "ruvia/memory/PmrResource.h"

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
    return RouteHandler(handler.target(), handler.invoke());
}

[[nodiscard]] RouteStreamHandler makeRouteStreamHandler(ControllerRouteStreamHandler handler) noexcept {
    return RouteStreamHandler(handler.target(), handler.invoke());
}

template <typename BaseRange, typename ExtraRange>
[[nodiscard]] std::pmr::vector<ControllerMiddlewareDescriptor> makeRouteMiddlewares(
    const BaseRange& base,
    const ExtraRange& extra) {
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares(startupResource());
    middlewares.reserve(base.size() + extra.size());
    middlewares.insert(middlewares.end(), base.begin(), base.end());
    middlewares.insert(middlewares.end(), extra.begin(), extra.end());
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

[[nodiscard]] std::pmr::vector<ControllerMiddlewareDescriptor> normalizeControllerMiddlewares(
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares) {
    if (middlewares.get_allocator().resource() == startupResource()) {
        return middlewares;
    }

    std::pmr::vector<ControllerMiddlewareDescriptor> normalized(startupResource());
    normalized.insert(normalized.end(), middlewares.begin(), middlewares.end());
    return normalized;
}

}  // namespace

detail::RouterImpl::PendingRoute::PendingRoute(std::pmr::memory_resource* resource, Init init)
    : method_(init.method),
      path_(detail::pmrResourceOrDefault(resource)),
      handler_(std::move(init.handler)),
      streamHandler_(std::move(init.streamHandler)),
      bodyMode_(init.bodyMode),
      responseMode_(init.responseMode),
      dynamic_(init.dynamic),
      middlewares_(detail::pmrResourceOrDefault(resource)),
      webSocketSubprotocols_(
          init.webSocketSubprotocols,
          path_.get_allocator().resource()),
      webSocketHeartbeat_(init.webSocketHeartbeat) {
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

void detail::ControllerRouteBuilder::ImplDeleter::operator()(Impl* impl) const noexcept {
    destroyPmrObject(impl, startupResource());
}

void detail::RouterImpl::registerRoute(
    HttpMethod method,
    std::pmr::string path,
    RouteHandler handler,
    RequestBodyMode bodyMode,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
    ResponseBodyMode responseMode) {
    if (!handler.valid()) {
        throw std::invalid_argument("route handler must not be empty");
    }
    if (responseMode == ResponseBodyMode::kWebSocket) {
        throw std::invalid_argument("buffered route cannot use the websocket response mode");
    }

    appendPendingRoute(PendingRoute(startupResource(), PendingRoute::Init{
        .method = method,
        .path = std::move(path),
        .handler = std::move(handler),
        .streamHandler = {},
        .bodyMode = bodyMode,
        .responseMode = responseMode,
        .dynamic = false,
        .middlewares = materializeMiddlewares(std::move(middlewares)),
        .webSocketSubprotocols = {},
        .webSocketHeartbeat = {}}));
}

void detail::RouterImpl::registerStreamRoute(
    HttpMethod method,
    std::pmr::string path,
    RouteStreamHandler handler,
    ResponseBodyMode responseMode,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
    WebSocketRouteOptions webSocketOptions) {
    if (!handler.valid()) {
        throw std::invalid_argument("route stream handler must not be empty");
    }
    if (responseMode == ResponseBodyMode::kBuffered) {
        throw std::invalid_argument("response stream route requires a streaming response mode");
    }

    appendPendingRoute(PendingRoute(startupResource(), PendingRoute::Init{
        .method = method,
        .path = std::move(path),
        .handler = {},
        .streamHandler = std::move(handler),
        .bodyMode = RequestBodyMode::kBuffered,
        .responseMode = responseMode,
        .dynamic = false,
        .middlewares = materializeMiddlewares(std::move(middlewares)),
        .webSocketSubprotocols = webSocketOptions.subprotocols,
        .webSocketHeartbeat = webSocketOptions.heartbeat}));
}

void detail::RouterImpl::appendPendingRoute(PendingRoute route) {
    if (finalized_) {
        throw std::logic_error("cannot register route after router finalize");
    }
    validateRouteTarget(route.method(), route.path());
    route.setDynamic(RouteTable::isDynamicPath(route.path()));
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
        const auto routeMiddlewares = route.middlewares();
        merged.reserve(middlewares.size() + routeMiddlewares.size());
        auto prepend = materializeMiddlewares(makeRouteMiddlewares(middlewares, std::span<const ControllerMiddlewareDescriptor>{}));
        merged.insert(merged.end(), prepend.begin(), prepend.end());
        merged.insert(merged.end(), routeMiddlewares.begin(), routeMiddlewares.end());
        route.setMiddlewares(std::move(merged));
    }
}

detail::ControllerRouteBuilder::ControllerRouteBuilder(
    Router& router,
    std::string_view prefix,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares)
    : ControllerRouteBuilder(
          router,
          joinControllerPaths({}, prefix),
          std::move(middlewares),
          OwnedPrefixTag{}) {}

detail::ControllerRouteBuilder::ControllerRouteBuilder(
    Router& router,
    std::pmr::string prefix,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
    OwnedPrefixTag)
    : impl_(
          constructPmrObject<Impl>(
              startupResource(),
              router,
              std::move(prefix),
              normalizeControllerMiddlewares(std::move(middlewares)))) {}

detail::ControllerRouteBuilder::ControllerRouteBuilder(ControllerRouteBuilder&&) noexcept = default;

detail::ControllerRouteBuilder& detail::ControllerRouteBuilder::operator=(ControllerRouteBuilder&&) noexcept = default;

detail::ControllerRouteBuilder::~ControllerRouteBuilder() = default;

void detail::ControllerRouteBuilder::registerRoute(
    HttpMethod method,
    std::string_view path,
    ControllerRouteHandler handler,
    RequestBodyMode bodyMode,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
    ResponseBodyMode responseMode) const {
    RouterImpl::from(impl_->router()).registerRoute(
        method,
        joinControllerPaths(impl_->prefix(), path),
        makeRouteHandler(handler),
        bodyMode,
        makeRouteMiddlewares(impl_->middlewares(), middlewares),
        responseMode);
}

void detail::ControllerRouteBuilder::registerStreamRoute(
    HttpMethod method,
    std::string_view path,
    ControllerRouteStreamHandler handler,
    ResponseBodyMode responseMode,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
    WebSocketRouteOptions webSocketOptions) const {
    RouterImpl::from(impl_->router()).registerStreamRoute(
        method,
        joinControllerPaths(impl_->prefix(), path),
        makeRouteStreamHandler(handler),
        responseMode,
        makeRouteMiddlewares(impl_->middlewares(), middlewares),
        webSocketOptions);
}

detail::ControllerRouteBuilder detail::ControllerRouteBuilder::createScope(
    std::string_view prefix,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares) const {
    auto merged = mergeControllerMiddlewares(impl_->middlewares(), std::move(middlewares));
    return ControllerRouteBuilder(
        impl_->router(),
        joinControllerPaths(impl_->prefix(), prefix),
        std::move(merged),
        OwnedPrefixTag{});
}

}  // namespace ruvia
