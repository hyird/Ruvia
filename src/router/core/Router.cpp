#include "../RouterInternal.h"

#include "RouterUtils.h"

namespace ruvia {

using namespace detail;

std::pmr::string joinControllerPaths(std::string_view prefix, std::string_view path) {
    auto* resource = startupResource();
    if (prefix.empty() || prefix == "/") {
        if (path.empty()) {
            return std::pmr::string{"/", resource};
        }
        return path.front() == '/' ? std::pmr::string(path, resource) : std::pmr::string("/", resource).append(path);
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

RouteHandler makeRouteHandler(ControllerRouteHandler handler) noexcept {
    return RouteHandler{handler.target, handler.invoke};
}

RouteStreamHandler makeRouteStreamHandler(ControllerRouteStreamHandler handler) noexcept {
    return RouteStreamHandler{handler.target, handler.invoke};
}

RouteMiddleware makeRouteMiddleware(ControllerMiddlewareDescriptor middleware) noexcept {
    return RouteMiddleware{
        nullptr,
        middleware.invoke,
        middleware.create,
        middleware.destroy};
}

std::pmr::vector<RouteMiddleware> makeRouteMiddlewares(std::pmr::vector<ControllerMiddlewareDescriptor> descriptors) {
    std::pmr::vector<RouteMiddleware> middlewares;
    middlewares.reserve(descriptors.size());
    for (const auto& descriptor : descriptors) {
        middlewares.push_back(makeRouteMiddleware(descriptor));
    }
    return middlewares;
}

std::pmr::vector<RouteMiddleware> makeRouteMiddlewares(std::span<const ControllerMiddlewareDescriptor> descriptors) {
    std::pmr::vector<RouteMiddleware> middlewares;
    middlewares.reserve(descriptors.size());
    for (const auto& descriptor : descriptors) {
        middlewares.push_back(makeRouteMiddleware(descriptor));
    }
    return middlewares;
}

std::pmr::vector<ControllerMiddlewareDescriptor> mergeControllerMiddlewares(
    const std::pmr::vector<ControllerMiddlewareDescriptor>& base,
    std::pmr::vector<ControllerMiddlewareDescriptor> extra) {
    std::pmr::vector<ControllerMiddlewareDescriptor> merged;
    merged.reserve(base.size() + extra.size());
    merged.insert(merged.end(), base.begin(), base.end());
    merged.insert(merged.end(), extra.begin(), extra.end());
    return merged;
}

Router::Router() : impl_(std::make_unique<detail::RouterImpl>(*this)) {}

Router::~Router() = default;

Router& detail::RouterImpl::setErrorHandler(HttpErrorHandler handler) noexcept {
    errorHandler_ = handler;
    if (routeTable_) {
        routeTable_->setErrorHandler(handler);
    }
    return owner;
}

void detail::RouteTable::setErrorHandler(HttpErrorHandler handler) noexcept {
    errorHandler_ = handler;
}

detail::RouterImpl::MiddlewareLifetime::MiddlewareLifetime(
    void* targetValue,
    RouteMiddleware::Destroy destroyValue) noexcept
    : target(targetValue), destroy(destroyValue) {}

detail::RouterImpl::MiddlewareLifetime::MiddlewareLifetime(MiddlewareLifetime&& other) noexcept
    : target(std::exchange(other.target, nullptr)),
      destroy(std::exchange(other.destroy, nullptr)) {}

detail::RouterImpl::MiddlewareLifetime& detail::RouterImpl::MiddlewareLifetime::operator=(
    MiddlewareLifetime&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    reset();
    target = std::exchange(other.target, nullptr);
    destroy = std::exchange(other.destroy, nullptr);
    return *this;
}

detail::RouterImpl::MiddlewareLifetime::~MiddlewareLifetime() {
    reset();
}

void detail::RouterImpl::MiddlewareLifetime::reset() noexcept {
    if (target != nullptr && destroy != nullptr) {
        destroy(target);
    }
    target = nullptr;
    destroy = nullptr;
}

void detail::RouterImpl::registerRoute(
    HttpMethod method,
    std::pmr::string path,
    RouteHandler handler,
    RequestBodyMode bodyMode,
    std::pmr::vector<RouteMiddleware> middlewares) {
    if (finalized_) {
        throw std::logic_error("cannot register route after router finalize");
    }
    if (!handler) {
        throw std::invalid_argument("route handler must not be empty");
    }
    validateRouteTarget(method, path);

    const auto dynamic = isDynamicPath(path);
    pendingRoutes_.push_back(detail::RouterImpl::PendingRoute{
        .method = method,
        .path = std::move(path),
        .handler = std::move(handler),
        .streamHandler = {},
        .bodyMode = bodyMode,
        .responseMode = ResponseBodyMode::kBuffered,
        .dynamic = dynamic,
        .middlewares = materializeMiddlewares(std::move(middlewares)),
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
    if (finalized_) {
        throw std::logic_error("cannot register route after router finalize");
    }
    if (!handler) {
        throw std::invalid_argument("route stream handler must not be empty");
    }
    if (responseMode == ResponseBodyMode::kBuffered) {
        throw std::invalid_argument("response stream route requires a streaming response mode");
    }
    validateRouteTarget(method, path);

    const auto dynamic = isDynamicPath(path);
    pendingRoutes_.push_back(detail::RouterImpl::PendingRoute{
        .method = method,
        .path = std::move(path),
        .handler = {},
        .streamHandler = std::move(handler),
        .bodyMode = RequestBodyMode::kBuffered,
        .responseMode = responseMode,
        .dynamic = dynamic,
        .middlewares = materializeMiddlewares(std::move(middlewares)),
        .webSocketSubprotocols = std::pmr::string(webSocketOptions.subprotocols, startupResource()),
        .webSocketHeartbeat = webSocketOptions.heartbeat});
}

void detail::RouterImpl::prependMiddlewares(std::span<const ControllerMiddlewareDescriptor> middlewares) {
    if (finalized_) {
        throw std::logic_error("cannot register middleware after router finalize");
    }
    if (middlewares.empty() || pendingRoutes_.empty()) {
        return;
    }

    for (auto& route : pendingRoutes_) {
        auto globalFrames = materializeMiddlewares(makeRouteMiddlewares(middlewares));
        std::pmr::vector<RouteMiddleware> merged;
        merged.reserve(globalFrames.size() + route.middlewares.size());
        merged.insert(merged.end(), globalFrames.begin(), globalFrames.end());
        merged.insert(merged.end(), route.middlewares.begin(), route.middlewares.end());
        route.middlewares = std::move(merged);
    }
}

detail::ControllerRouteBuilder::ControllerRouteBuilder(
    Router& router,
    std::string_view prefix,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares)
    : impl_(std::make_unique<Impl>(
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
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares) const {
    auto merged = mergeControllerMiddlewares(impl_->middlewares, std::move(middlewares));
    RouterImpl::from(*impl_->router).registerRoute(
        method,
        joinControllerPaths(impl_->prefix, path),
        makeRouteHandler(handler),
        bodyMode,
        makeRouteMiddlewares(std::move(merged)));
}

void detail::ControllerRouteBuilder::registerStreamRoute(
    HttpMethod method,
    std::pmr::string path,
    ControllerRouteStreamHandler handler,
    ResponseBodyMode responseMode,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
    WebSocketRouteOptions webSocketOptions) const {
    auto merged = mergeControllerMiddlewares(impl_->middlewares, std::move(middlewares));
    RouterImpl::from(*impl_->router).registerStreamRoute(
        method,
        joinControllerPaths(impl_->prefix, path),
        makeRouteStreamHandler(handler),
        responseMode,
        makeRouteMiddlewares(std::move(merged)),
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

detail::RouteMiddleware detail::RouterImpl::materializeMiddleware(RouteMiddleware middleware) {
    if (middleware.target != nullptr) {
        middleware.create = nullptr;
        middleware.destroy = nullptr;
        return middleware;
    }
    if (middleware.create == nullptr || middleware.destroy == nullptr || middleware.invoke == nullptr) {
        throw std::invalid_argument("middleware must be constructible");
    }

    middleware.target = middleware.create();
    middlewareLifetimes_.emplace_back(middleware.target, middleware.destroy);
    middleware.create = nullptr;
    middleware.destroy = nullptr;
    return middleware;
}

std::pmr::vector<detail::RouteMiddleware> detail::RouterImpl::materializeMiddlewares(
    std::pmr::vector<RouteMiddleware> middlewares) {
    std::pmr::vector<RouteMiddleware> result;
    result.reserve(middlewares.size());
    for (auto& middleware : middlewares) {
        result.push_back(materializeMiddleware(std::move(middleware)));
    }
    return result;
}

bool detail::RouterImpl::isDynamicPath(std::string_view path) noexcept {
    return RouteTable::isDynamicPath(path);
}

void detail::RouterImpl::validateRouteTarget(HttpMethod method, std::string_view path) const {
    if (!RouteTable::isRoutableMethod(method)) {
        throw std::invalid_argument("route method must be routable");
    }

    for (const auto& route : pendingRoutes_) {
        if (route.method == method && route.path == path) {
            throw std::invalid_argument("duplicate route registration");
        }
    }
}

bool detail::RouterImpl::splitSegment(
    std::string_view path,
    std::string_view& segment,
    std::string_view& rest) noexcept {
    return RouteTable::splitSegment(path, segment, rest);
}

bool detail::RouterImpl::sameDynamicShape(std::string_view left, std::string_view right) noexcept {
    return RouteTable::sameDynamicShape(left, right);
}

void detail::RouterImpl::finalize() {
    if (finalized_) {
        return;
    }

    validateNoDynamicRouteConflict(pendingRoutes_);
    routeTable_ = std::make_unique<RouteTable>(buildRouteTable());
    routeTable_->setErrorHandler(errorHandler_);
    finalized_ = true;
}

const detail::RouteTable& detail::RouterImpl::routeTable() const {
    if (!routeTable_) {
        throw std::logic_error("router has not been finalized");
    }
    return *routeTable_;
}

detail::RouteTable detail::RouterImpl::buildRouteTable() const {
    RouteTable table;
    table.routes_.reserve(pendingRoutes_.size() * 2);
    std::size_t middlewareCount = 0;
    for (const auto& route : pendingRoutes_) {
        middlewareCount += route.middlewares.size();
    }
    table.middlewareFrames_.reserve(middlewareCount);

    for (const auto& pending : pendingRoutes_) {
        RouteEntry route{
            .method = pending.method,
            .path = pending.path,
            .handler = pending.handler,
            .streamHandler = pending.streamHandler,
            .bodyMode = pending.bodyMode,
            .responseMode = pending.responseMode,
            .dynamic = pending.dynamic,
            .paramNames = {},
            .paramCount = 0,
            .middlewareOffset = 0,
            .middlewareCount = 0,
            .webSocketSubprotocols = pending.webSocketSubprotocols,
            .webSocketHeartbeat = pending.webSocketHeartbeat};
        route.middlewareOffset = table.middlewareFrames_.size();
        route.middlewareCount = pending.middlewares.size();
        table.middlewareFrames_.insert(
            table.middlewareFrames_.end(),
            pending.middlewares.begin(),
            pending.middlewares.end());
        table.routes_.push_back(std::move(route));
    }

    const auto originalRouteCount = table.routes_.size();
    for (std::size_t i = 0; i < originalRouteCount; ++i) {
        const auto& source = table.routes_[i];
        if (source.method != HttpMethod::kGet || source.responseMode != ResponseBodyMode::kBuffered) {
            continue;
        }
        bool conflictsWithExistingHead = false;
        for (std::size_t j = 0; j < originalRouteCount; ++j) {
            const auto& other = table.routes_[j];
            if (other.method != HttpMethod::kHead) {
                continue;
            }
            if (source.dynamic && other.dynamic) {
                if (RouteTable::sameDynamicShape(source.path, other.path)) {
                    conflictsWithExistingHead = true;
                    break;
                }
            } else if (!source.dynamic && !other.dynamic) {
                if (other.path == source.path) {
                    conflictsWithExistingHead = true;
                    break;
                }
            }
        }
        if (conflictsWithExistingHead) {
            continue;
        }
        RouteEntry shadow{
            .method = HttpMethod::kHead,
            .path = source.path,
            .handler = source.handler,
            .streamHandler = source.streamHandler,
            .bodyMode = source.bodyMode,
            .responseMode = source.responseMode,
            .dynamic = source.dynamic,
            .paramNames = source.paramNames,
            .paramCount = source.paramCount,
            .middlewareOffset = source.middlewareOffset,
            .middlewareCount = source.middlewareCount,
            .webSocketSubprotocols = source.webSocketSubprotocols,
            .webSocketHeartbeat = source.webSocketHeartbeat};
        table.routes_.push_back(std::move(shadow));
    }

    table.buildAllowedMethodMask();
    table.buildPerfectHash();
    if (table.exactSlots_.empty()) {
        table.buildRadix();
    }
    table.buildDynamicRoutes();
    return table;
}

detail::RouteResolution detail::RouterImpl::resolve(const HttpRequest& request) const noexcept {
    if (!routeTable_) {
        return RouteResolution{};
    }
    return routeTable_->resolve(request);
}

Task<HttpResponse> detail::RouterImpl::dispatch(
    const HttpRequest& request,
    RequestMemory& memory,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader) const {
    return routeTable().dispatch(request, memory, db, redis, bodyReader, bodyLoader);
}

Task<detail::StreamDispatchResult> detail::RouterImpl::dispatchResponseStream(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    ResponseStreamWriter& responseStream,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader) const {
    co_return co_await routeTable().dispatchResponseStream(request, resolution, memory, responseStream, db, redis, bodyReader, bodyLoader);
}

Task<HttpResponse> detail::RouterImpl::dispatch(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader) const {
    return routeTable().dispatch(request, resolution, memory, db, redis, bodyReader, bodyLoader);
}

Task<HttpResponse> detail::RouterImpl::handleError(
    const HttpRequest& request,
    RequestMemory& memory,
    HttpErrorInfo error,
    bool closeConnection,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader) const {
    return routeTable().handleError(request, memory, error, closeConnection, db, redis, bodyReader, bodyLoader);
}

Task<HttpResponse> detail::RouterImpl::handleException(
    const HttpRequest& request,
    RequestMemory& memory,
    std::exception_ptr exception,
    bool closeConnection,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader) const {
    return routeTable().handleException(request, memory, exception, closeConnection, db, redis, bodyReader, bodyLoader);
}

RequestBodyMode detail::RouterImpl::bodyModeFor(const HttpRequest& request) const noexcept {
    if (!routeTable_) {
        return RequestBodyMode::kBuffered;
    }
    return routeTable_->bodyModeFor(request);
}

}  // namespace ruvia
