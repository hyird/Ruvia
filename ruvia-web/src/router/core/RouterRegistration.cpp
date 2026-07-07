#include "../RouterInternal.h"

#include "ruvia/http/detail/RegistrationResource.h"

namespace ruvia {

using namespace detail;

namespace {

std::pmr::string joinControllerPaths(std::string_view prefix, std::string_view path) {
    auto* resource = registrationResource();
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

template <typename BaseRange, typename ExtraRange>
[[nodiscard]] std::pmr::vector<ControllerMiddlewareDescriptor> mergeMiddlewareDescriptors(
    const BaseRange& base,
    const ExtraRange& extra) {
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares(registrationResource());
    middlewares.reserve(base.size() + extra.size());
    middlewares.insert(middlewares.end(), base.begin(), base.end());
    middlewares.insert(middlewares.end(), extra.begin(), extra.end());
    return middlewares;
}

[[nodiscard]] std::pmr::vector<ControllerMiddlewareDescriptor> normalizeControllerMiddlewares(
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares) {
    if (middlewares.get_allocator().resource() == registrationResource()) {
        return middlewares;
    }

    std::pmr::vector<ControllerMiddlewareDescriptor> normalized(registrationResource());
    normalized.insert(normalized.end(), middlewares.begin(), middlewares.end());
    return normalized;
}

}  // namespace

detail::RouterImpl::PendingRoute::PendingRoute(std::pmr::memory_resource* resource, Init init)
    : method_(init.method),
      path_(resource),
      handler_(std::move(init.handler)),
      streamHandler_(std::move(init.streamHandler)),
      bodyMode_(init.bodyMode),
      responseMode_(init.responseMode),
      dynamic_(init.dynamic),
      middlewares_(resource),
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
    destroyPmrObject(impl, registrationResource());
}

void detail::RouterImpl::registerRoute(
    HttpMethod method,
    std::pmr::string path,
    RouteHandler handler,
    RequestBodyMode bodyMode,
    std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares,
    std::span<const ControllerMiddlewareDescriptor> routeMiddlewares,
    ResponseBodyMode responseMode) {
    if (!handler.valid()) {
        throw std::invalid_argument("route handler must not be empty");
    }
    if (responseMode == ResponseBodyMode::kWebSocket) {
        throw std::invalid_argument("buffered route cannot use the websocket response mode");
    }

    appendPendingRoute(PendingRoute(resource_, PendingRoute::Init{
        .method = method,
        .path = std::move(path),
        .handler = std::move(handler),
        .streamHandler = {},
        .bodyMode = bodyMode,
        .responseMode = responseMode,
        .dynamic = false,
        .middlewares = materializeMiddlewares(controllerMiddlewares, routeMiddlewares),
        .webSocketSubprotocols = {},
        .webSocketHeartbeat = {}}));
}

void detail::RouterImpl::registerStreamRoute(
    HttpMethod method,
    std::pmr::string path,
    RouteStreamHandler handler,
    ResponseBodyMode responseMode,
    std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares,
    std::span<const ControllerMiddlewareDescriptor> routeMiddlewares,
    WebSocketRouteOptions webSocketOptions) {
    if (!handler.valid()) {
        throw std::invalid_argument("route stream handler must not be empty");
    }
    if (responseMode == ResponseBodyMode::kBuffered) {
        throw std::invalid_argument("response stream route requires a streaming response mode");
    }

    appendPendingRoute(PendingRoute(resource_, PendingRoute::Init{
        .method = method,
        .path = std::move(path),
        .handler = {},
        .streamHandler = std::move(handler),
        .bodyMode = RequestBodyMode::kBuffered,
        .responseMode = responseMode,
        .dynamic = false,
        .middlewares = materializeMiddlewares(controllerMiddlewares, routeMiddlewares),
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
              registrationResource(),
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
    std::span<const ControllerMiddlewareDescriptor> middlewares,
    ResponseBodyMode responseMode) const {
    RouterImpl::from(impl_->router()).registerRoute(
        method,
        joinControllerPaths(impl_->prefix(), path),
        std::move(handler),
        bodyMode,
        impl_->middlewares(),
        middlewares,
        responseMode);
}

void detail::ControllerRouteBuilder::registerStreamRoute(
    HttpMethod method,
    std::string_view path,
    ControllerRouteStreamHandler handler,
    ResponseBodyMode responseMode,
    std::span<const ControllerMiddlewareDescriptor> middlewares,
    WebSocketRouteOptions webSocketOptions) const {
    RouterImpl::from(impl_->router()).registerStreamRoute(
        method,
        joinControllerPaths(impl_->prefix(), path),
        std::move(handler),
        responseMode,
        impl_->middlewares(),
        middlewares,
        webSocketOptions);
}

detail::ControllerRouteBuilder detail::ControllerRouteBuilder::createScope(
    std::string_view prefix,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares) const {
    auto merged = mergeMiddlewareDescriptors(impl_->middlewares(), middlewares);
    return ControllerRouteBuilder(
        impl_->router(),
        joinControllerPaths(impl_->prefix(), prefix),
        std::move(merged),
        OwnedPrefixTag{});
}

}  // namespace ruvia
