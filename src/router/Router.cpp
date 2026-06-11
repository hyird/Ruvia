#include "RouterInternal.h"

#include "ruvia/http/Error.h"
#include "ruvia/http/Validation.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia {

using namespace detail;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] std::pmr::memory_resource* startupResource() noexcept {
    return ProcessMemory::instance().upstreamResource();
}

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
    auto merged = base;
    merged.reserve(base.size() + extra.size());
    merged.insert(merged.end(), extra.begin(), extra.end());
    return merged;
}

std::pmr::string makeAllowHeader(std::pmr::memory_resource* resource, std::uint32_t methodMask) {
    std::pmr::string output(resource);
    output.reserve(64);
    bool first = true;
    for (std::size_t i = 0; i < 7; ++i) {
        if ((methodMask & (1U << i)) == 0) {
            continue;
        }
        if (!first) {
            output.append(", ");
        }
        first = false;
        const auto method = methodName(static_cast<HttpMethod>(i));
        output.append(method.data(), method.size());
    }
    return output;
}

struct OwnedHttpErrorInfo final {
    HttpErrorInfo info{};
    std::pmr::string statusText;
    std::pmr::string code;
    std::pmr::string message;
    std::pmr::string detailsJson;

    explicit OwnedHttpErrorInfo(HttpErrorInfo source)
        : statusText(startupResource()),
          code(startupResource()),
          message(startupResource()),
          detailsJson(startupResource()) {
        assign(source);
    }

    void assign(HttpErrorInfo source) {
        statusText.assign(source.statusText.data(), source.statusText.size());
        code.assign(source.code.data(), source.code.size());
        message.assign(source.message.data(), source.message.size());
        detailsJson.assign(source.detailsJson.data(), source.detailsJson.size());

        info = HttpErrorInfo{
            .statusCode = source.statusCode,
            .statusText = statusText,
            .code = code,
            .message = message,
            .detailsJson = detailsJson};
    }
};

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

Task<HttpResponse> detail::RouteTable::dispatch(
    const HttpRequest& request,
    RequestMemory& memory,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader) const {
    return dispatch(request, resolve(request), memory, db, redis, bodyReader, bodyLoader);
}

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchResponseStream(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    ResponseStreamWriter& responseStream,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader) const {
    if (!resolution.found() || resolution.route == nullptr || resolution.route->responseMode == ResponseBodyMode::kBuffered) {
        throw std::logic_error("route is not a response stream route");
    }
    const auto* route = resolution.route;
    auto streamHandled = false;
    if (!resolution.dynamic) {
        Context context(memory, request, &responseStream, db, redis, bodyReader, bodyLoader);
        responseStream.bindContext(context);
        auto response = co_await invokeStreamRoute(*route, context, streamHandled);
        co_return StreamDispatchResult{std::move(response), streamHandled};
    }

    Context context(
        memory,
        request,
        resolution.match.params,
        resolution.match.paramCount,
        &responseStream,
        db,
        redis,
        bodyReader,
        bodyLoader);
    responseStream.bindContext(context);
    auto response = co_await invokeStreamRoute(*route, context, streamHandled);
    co_return StreamDispatchResult{std::move(response), streamHandled};
}

Task<detail::StreamDispatchResult> detail::RouteTable::dispatchWebSocket(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    WebSocket& webSocket,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader) const {
    if (!resolution.found() || resolution.route == nullptr || resolution.route->responseMode != ResponseBodyMode::kWebSocket) {
        throw std::logic_error("route is not a websocket route");
    }
    const auto* route = resolution.route;
    auto streamHandled = false;
    if (!resolution.dynamic) {
        Context context(memory, request, db, redis, bodyReader, bodyLoader, &webSocket);
        auto response = co_await invokeStreamRoute(*route, context, streamHandled);
        co_return StreamDispatchResult{std::move(response), streamHandled};
    }

    Context context(
        memory,
        request,
        resolution.match.params,
        resolution.match.paramCount,
        db,
        redis,
        bodyReader,
        bodyLoader,
        &webSocket);
    auto response = co_await invokeStreamRoute(*route, context, streamHandled);
    co_return StreamDispatchResult{std::move(response), streamHandled};
}

detail::RouteResolution detail::RouteTable::resolve(const HttpRequest& request) const noexcept {
    const auto path = request.path();
    const auto method = request.method();

    if (const auto* route = findStaticRoute(method, path); route != nullptr) {
        return RouteResolution{
            .status = RouteResolveStatus::kFound,
            .route = route,
            .bodyMode = route->bodyMode};
    }

    auto match = findDynamicRoute(method, path);
    if (match.route != nullptr) {
        return RouteResolution{
            .status = RouteResolveStatus::kFound,
            .route = match.route,
            .match = match,
            .bodyMode = match.route->bodyMode,
            .dynamic = true};
    }

    auto methodMask = allowedMethods(path);
    if (methodMask != 0) {
        methodMask |= 1U << methodIndex(HttpMethod::kOptions);
        return RouteResolution{
            .status = RouteResolveStatus::kMethodNotAllowed,
            .allowedMethods = methodMask};
    }

    return RouteResolution{};
}

Task<HttpResponse> detail::RouteTable::dispatch(
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& memory,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader) const {
    if (resolution.found() && !resolution.dynamic) {
        const auto* route = resolution.route;
        Context context(memory, request, db, redis, bodyReader, bodyLoader);
        std::exception_ptr exception;
        try {
            co_return co_await invokeRoute(*route, context);
        } catch (...) {
            exception = std::current_exception();
        }
        co_return co_await handleException(context, exception, true);
    }

    if (!resolution.found()) {
        Context context(memory, request, db, redis, bodyReader, bodyLoader);
        if (request.method() == HttpMethod::kOptions && request.path() == "*") {
            auto allow = makeAllowHeader(memory.resource(), allowedMethodsForServer());
            HttpResponse response(memory.resource());
            response.setStatus(204, "No Content");
            response.setHeader("Allow", allow);
            co_return response;
        }

        if (resolution.status == RouteResolveStatus::kMethodNotAllowed) {
            auto allow = makeAllowHeader(memory.resource(), resolution.allowedMethods);
            if (request.method() == HttpMethod::kOptions) {
                HttpResponse response(memory.resource());
                response.setStatus(204, "No Content");
                response.setHeader("Allow", allow);
                co_return response;
            }

            auto response = co_await handleError(
                context,
                HttpErrorInfo{.statusCode = 405, .message = "method not allowed"},
                false);
            response.setHeader("Allow", allow);
            co_return response;
        }

        co_return co_await handleError(
            context,
            HttpErrorInfo{.statusCode = 404, .message = "route not found"},
            false);
    }

    Context context(
        memory,
        request,
        resolution.match.params,
        resolution.match.paramCount,
        db,
        redis,
        bodyReader,
        bodyLoader);
    std::exception_ptr exception;
    try {
        co_return co_await invokeRoute(*resolution.route, context);
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await handleException(context, exception, true);
}

Task<HttpResponse> detail::RouteTable::invokeRoute(const RouteEntry& route, Context& context) const {
    return invokeMiddlewareAt(route, 0, context);
}

Task<HttpResponse> detail::RouteTable::invokeMiddlewareAt(
    const RouteEntry& route,
    std::size_t index,
    Context& context) const {
    if (index >= route.middlewareCount) {
        co_return co_await route.handler(context);
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset + index];
    MiddlewareContinuation continuation{this, &route, index + 1};
    const Next next(&continuation, &RouteTable::invokeMiddlewareContinuation);
    auto response = middleware(context, next);
    co_return co_await std::move(response);
}

Task<HttpResponse> detail::RouteTable::invokeMiddlewareContinuation(void* target, Context& context) {
    const auto* continuation = static_cast<const MiddlewareContinuation*>(target);
    return continuation->table->invokeMiddlewareAt(*continuation->route, continuation->index, context);
}

Task<HttpResponse> detail::RouteTable::invokeStreamRoute(
    const RouteEntry& route,
    Context& context,
    bool& streamHandled) const {
    return invokeStreamMiddlewareAt(route, 0, context, streamHandled);
}

Task<HttpResponse> detail::RouteTable::invokeStreamMiddlewareAt(
    const RouteEntry& route,
    std::size_t index,
    Context& context,
    bool& streamHandled) const {
    if (index >= route.middlewareCount) {
        co_await route.streamHandler(context);
        streamHandled = true;
        co_return HttpResponse(context.resource());
    }

    const auto& middleware = middlewareFrames_[route.middlewareOffset + index];
    StreamMiddlewareContinuation continuation{this, &route, index + 1, &streamHandled};
    const Next next(&continuation, &RouteTable::invokeStreamMiddlewareContinuation);
    auto response = middleware(context, next);
    co_return co_await std::move(response);
}

Task<HttpResponse> detail::RouteTable::invokeStreamMiddlewareContinuation(void* target, Context& context) {
    const auto* continuation = static_cast<const StreamMiddlewareContinuation*>(target);
    return continuation->table->invokeStreamMiddlewareAt(
        *continuation->route,
        continuation->index,
        context,
        *continuation->streamHandled);
}

Task<HttpResponse> detail::RouteTable::handleError(
    const HttpRequest& request,
    RequestMemory& memory,
    HttpErrorInfo error,
    bool closeConnection,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader) const {
    Context context(memory, request, db, redis, bodyReader, bodyLoader);
    co_return co_await handleError(context, error, closeConnection);
}

Task<HttpResponse> detail::RouteTable::handleException(
    const HttpRequest& request,
    RequestMemory& memory,
    std::exception_ptr exception,
    bool closeConnection,
    DbRegistry* db,
    RedisRegistry* redis,
    BodyReader* bodyReader,
    RequestBodyLoader* bodyLoader) const {
    Context context(memory, request, db, redis, bodyReader, bodyLoader);
    co_return co_await handleException(context, exception, closeConnection);
}

Task<HttpResponse> detail::RouteTable::handleError(
    Context& context,
    HttpErrorInfo error,
    bool closeConnection) const {
    return makeErrorResponse(context, error, closeConnection, errorHandler_);
}

Task<HttpResponse> detail::RouteTable::handleException(
    Context& context,
    std::exception_ptr exception,
    bool closeConnection) const {
    OwnedHttpErrorInfo errorInfo(HttpErrorInfo{.statusCode = 500, .message = "unhandled exception"});

    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
    } catch (const ValidationError& error) {
        errorInfo.assign(error.info());
    } catch (const HttpError& error) {
        errorInfo.assign(error.info());
    } catch (const std::invalid_argument& error) {
        errorInfo.assign(HttpErrorInfo{.statusCode = 400, .message = error.what()});
    } catch (const std::exception& error) {
        errorInfo.assign(HttpErrorInfo{.statusCode = 500, .message = error.what()});
    } catch (...) {
        errorInfo.assign(HttpErrorInfo{.statusCode = 500, .message = "unhandled exception"});
    }

    co_return co_await handleError(context, errorInfo.info, closeConnection);
}

RequestBodyMode detail::RouteTable::bodyModeFor(const HttpRequest& request) const noexcept {
    return resolve(request).bodyMode;
}

void detail::RouterImpl::validateNoDynamicRouteConflict(std::span<const PendingRoute> routes) {
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const auto& left = routes[i];
        if (!left.dynamic) {
            continue;
        }
        for (std::size_t j = i + 1; j < routes.size(); ++j) {
            const auto& right = routes[j];
            if (!right.dynamic || left.method != right.method) {
                continue;
            }
            if (sameDynamicShape(left.path, right.path)) {
                throw std::invalid_argument("conflicting dynamic route shape");
            }
        }
    }
}

void detail::RouteTable::buildPerfectHash() {
    exactSlots_.clear();
    exactSeed_ = 0;
    exactMask_ = 0;

    std::pmr::vector<const RouteEntry*> exactRoutes;
    exactRoutes.reserve(routes_.size());
    for (const auto& route : routes_) {
        if (!route.dynamic) {
            exactRoutes.push_back(&route);
        }
    }

    if (exactRoutes.empty()) {
        return;
    }

    auto slotCount = nextPowerOfTwo(exactRoutes.size());
    std::pmr::vector<const RouteEntry*> candidate;

    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        const auto mask = slotCount - 1;
        for (std::uint64_t seed = 0; seed < 4096; ++seed) {
            candidate.assign(slotCount, nullptr);
            bool collision = false;

            for (const auto* route : exactRoutes) {
                const auto index = static_cast<std::size_t>(routeHash(route->method, route->path, seed)) & mask;
                if (candidate[index] != nullptr) {
                    collision = true;
                    break;
                }
                candidate[index] = route;
            }

            if (!collision) {
                exactSlots_.resize(slotCount);
                for (std::size_t i = 0; i < slotCount; ++i) {
                    exactSlots_[i].route = candidate[i];
                }
                exactSeed_ = seed;
                exactMask_ = mask;
                return;
            }
        }

        slotCount <<= 1U;
    }
}

void detail::RouteTable::buildRadix() {
    radixRoots_ = {};
    for (const auto& route : routes_) {
        if (route.dynamic) {
            continue;
        }
        insertRadix(radixRoots_[methodIndex(route.method)], route.path, route);
    }
}

void detail::RouteTable::buildDynamicRoutes() {
    hasDynamicRoutes_.fill(false);
    for (auto& root : dynamicRoots_) {
        root = DynamicNode{};
    }

    for (auto& route : routes_) {
        if (route.dynamic) {
            hasDynamicRoutes_[methodIndex(route.method)] = true;
            collectDynamicParamNames(route);
            insertDynamic(dynamicRoots_[methodIndex(route.method)], route);
        }
    }
}

void detail::RouteTable::buildAllowedMethodMask() noexcept {
    allowedMethodMask_ = 0;
    for (const auto& route : routes_) {
        if (isRoutableMethod(route.method)) {
            allowedMethodMask_ |= 1U << methodIndex(route.method);
        }
    }
    allowedMethodMask_ |= 1U << methodIndex(HttpMethod::kOptions);
}

std::size_t detail::RouteTable::methodIndex(HttpMethod method) noexcept {
    return static_cast<std::size_t>(method);
}

bool detail::RouteTable::isRoutableMethod(HttpMethod method) noexcept {
    return methodIndex(method) < kRoutableMethodCount;
}

bool detail::RouteTable::isDynamicPath(std::string_view path) noexcept {
    while (!path.empty()) {
        std::string_view segment;
        std::string_view rest;
        if (!splitSegment(path, segment, rest)) {
            return false;
        }
        if (segment == "*" || (!segment.empty() && segment.front() == ':')) {
            return true;
        }
        path = rest;
    }

    return false;
}

std::uint64_t detail::RouteTable::routeHash(HttpMethod method, std::string_view path, std::uint64_t seed) noexcept {
    auto hash = kFnvOffset ^ seed;
    hash ^= static_cast<std::uint64_t>(method);
    hash *= kFnvPrime;

    for (const unsigned char c : path) {
        hash ^= c;
        hash *= kFnvPrime;
    }

    return hash;
}

std::size_t detail::RouteTable::nextPowerOfTwo(std::size_t value) noexcept {
    std::size_t result = 1;
    while (result < value) {
        result <<= 1U;
    }
    return result;
}

std::size_t detail::RouteTable::commonPrefixLength(std::string_view left, std::string_view right) noexcept {
    const auto length = std::min(left.size(), right.size());
    std::size_t index = 0;
    while (index < length && left[index] == right[index]) {
        ++index;
    }
    return index;
}

void detail::RouteTable::insertRadix(RadixNode& node, std::string_view path, const RouteEntry& route) {
    if (path.empty()) {
        node.route = &route;
        return;
    }

    for (auto& child : node.children) {
        const auto prefixLength = commonPrefixLength(child.label, path);
        if (prefixLength == 0) {
            continue;
        }

        if (prefixLength == child.label.size()) {
            insertRadix(child, path.substr(prefixLength), route);
            return;
        }

        auto oldLabel = std::move(child.label);
        auto oldChildren = std::move(child.children);
        auto* oldRoute = child.route;

        RadixNode suffix;
        suffix.label.assign(oldLabel.data() + prefixLength, oldLabel.size() - prefixLength);
        suffix.children = std::move(oldChildren);
        suffix.route = oldRoute;

        child.label.assign(oldLabel.data(), prefixLength);
        child.children.clear();
        child.children.push_back(std::move(suffix));
        child.route = nullptr;

        if (prefixLength == path.size()) {
            child.route = &route;
        } else {
            RadixNode branch;
            branch.label.assign(path.data() + prefixLength, path.size() - prefixLength);
            branch.route = &route;
            child.children.push_back(std::move(branch));
        }
        return;
    }

    RadixNode child;
    child.label.assign(path.data(), path.size());
    child.route = &route;
    node.children.push_back(std::move(child));
}

const detail::RouteEntry* detail::RouteTable::findRadixNode(
    const RadixNode& root,
    std::string_view path) noexcept {
    const auto* node = &root;
    while (!path.empty()) {
        const RadixNode* next = nullptr;
        for (const auto& child : node->children) {
            if (path.starts_with(child.label)) {
                next = &child;
                path.remove_prefix(child.label.size());
                break;
            }
        }

        if (next == nullptr) {
            return nullptr;
        }
        node = next;
    }

    return node->route;
}

void detail::RouteTable::collectDynamicParamNames(RouteEntry& route) {
    route.paramCount = 0;
    auto path = std::string_view(route.path);

    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!splitSegment(path, segment, rest)) {
            return;
        }
        if (segment.empty()) {
            throw std::invalid_argument("dynamic route path must not contain empty segments");
        }
        if (segment == "*") {
            if (!rest.empty()) {
                throw std::invalid_argument("wildcard route segment must be final");
            }
            if (route.paramCount >= route.paramNames.size()) {
                throw std::invalid_argument("route has too many parameters");
            }
            route.paramNames[route.paramCount++] = "*";
            return;
        }
        if (segment.front() == ':') {
            if (segment.size() == 1) {
                throw std::invalid_argument("route parameter name must not be empty");
            }
            if (route.paramCount >= route.paramNames.size()) {
                throw std::invalid_argument("route has too many parameters");
            }
            route.paramNames[route.paramCount++] = segment.substr(1);
        }

        if (rest.empty()) {
            return;
        }
        path = rest;
    }
}

void detail::RouteTable::insertDynamic(DynamicNode& root, const RouteEntry& route) {
    auto path = std::string_view(route.path);
    auto* node = &root;

    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!splitSegment(path, segment, rest)) {
            node->route = &route;
            return;
        }
        if (segment.empty()) {
            throw std::invalid_argument("dynamic route path must not contain empty segments");
        }
        if (segment == "*") {
            if (!rest.empty()) {
                throw std::invalid_argument("wildcard route segment must be final");
            }
            node->wildcardRoute = &route;
            return;
        }
        if (segment.front() == ':') {
            if (segment.size() == 1) {
                throw std::invalid_argument("route parameter name must not be empty");
            }

            if (!node->paramChild) {
                node->paramChild = std::make_unique<DynamicNode>();
            }
            node = node->paramChild.get();
        } else {
            auto* childNode = static_cast<DynamicNode*>(nullptr);
            for (auto& child : node->staticChildren) {
                if (child.segment == segment) {
                    childNode = child.node.get();
                    break;
                }
            }
            if (childNode == nullptr) {
                auto child = DynamicStaticChild{std::pmr::string(segment, startupResource()), std::make_unique<DynamicNode>()};
                childNode = child.node.get();
                node->staticChildren.push_back(std::move(child));
            }
            node = childNode;
        }

        if (rest.empty()) {
            node->route = &route;
            return;
        }
        path = rest;
    }
}

const detail::RouteEntry* detail::RouteTable::findDynamicNode(
    const DynamicNode& node,
    std::string_view path,
    RouteMatch& match) noexcept {
    std::string_view segment;
    std::string_view rest;
    if (!splitSegment(path, segment, rest)) {
        if (node.route != nullptr) {
            return node.route;
        }
        if (node.wildcardRoute != nullptr && addParam(match, "*", {})) {
            return node.wildcardRoute;
        }
        return nullptr;
    }

    const auto originalParamCount = match.paramCount;
    for (const auto& child : node.staticChildren) {
        if (child.segment == segment) {
            if (const auto* route = findDynamicNode(*child.node, rest, match); route != nullptr) {
                return route;
            }
            match.paramCount = originalParamCount;
            break;
        }
    }

    if (node.paramChild && !segment.empty() && addParam(match, {}, segment)) {
        if (const auto* route = findDynamicNode(*node.paramChild, rest, match); route != nullptr) {
            return route;
        }
        match.paramCount = originalParamCount;
    }

    if (node.wildcardRoute != nullptr) {
        auto capture = path;
        if (capture.starts_with('/')) {
            capture.remove_prefix(1);
        }
        if (addParam(match, "*", capture)) {
            return node.wildcardRoute;
        }
        match.paramCount = originalParamCount;
    }

    return nullptr;
}

bool detail::RouteTable::addParam(RouteMatch& match, std::string_view name, std::string_view value) noexcept {
    if (match.paramCount >= match.params.size()) {
        return false;
    }

    match.params[match.paramCount++] = RouteParamView{name, value};
    return true;
}

bool detail::RouteTable::splitSegment(
    std::string_view path,
    std::string_view& segment,
    std::string_view& rest) noexcept {
    if (path.starts_with('/')) {
        path.remove_prefix(1);
    }
    if (path.empty()) {
        segment = {};
        rest = {};
        return false;
    }

    const auto slash = path.find('/');
    if (slash == std::string_view::npos) {
        segment = path;
        rest = {};
        return true;
    }

    segment = path.substr(0, slash);
    rest = path.substr(slash + 1);
    return true;
}

bool detail::RouteTable::sameDynamicShape(std::string_view left, std::string_view right) noexcept {
    std::size_t depth = 0;
    for (;;) {
        std::string_view leftSegment;
        std::string_view leftRest;
        std::string_view rightSegment;
        std::string_view rightRest;
        const auto hasLeft = splitSegment(left, leftSegment, leftRest);
        const auto hasRight = splitSegment(right, rightSegment, rightRest);
        if (!hasLeft || !hasRight) {
            return hasLeft == hasRight;
        }

        if (leftSegment == "*" || rightSegment == "*") {
            if (depth == 0 && leftSegment != rightSegment) {
                return false;
            }
            return true;
        }

        const auto leftParam = !leftSegment.empty() && leftSegment.front() == ':';
        const auto rightParam = !rightSegment.empty() && rightSegment.front() == ':';
        if (!leftParam && !rightParam && leftSegment != rightSegment) {
            return false;
        }

        left = leftRest;
        right = rightRest;
        ++depth;
    }
}

const detail::RouteEntry* detail::RouteTable::findStaticRoute(
    HttpMethod method,
    std::string_view path) const noexcept {
    if (!isRoutableMethod(method)) {
        return nullptr;
    }

    if (const auto* route = findPerfect(method, path); route != nullptr) {
        return route;
    }
    if (!exactSlots_.empty()) {
        return nullptr;
    }
    if (const auto* route = findRadix(method, path); route != nullptr) {
        return route;
    }

    return nullptr;
}

detail::RouteMatch detail::RouteTable::findDynamicRoute(HttpMethod method, std::string_view path) const noexcept {
    RouteMatch match;
    if (!isRoutableMethod(method) || !hasDynamicRoutes_[methodIndex(method)]) {
        return match;
    }

    match = findDynamic(method, path);
    return match;
}

const detail::RouteEntry* detail::RouteTable::findPerfect(HttpMethod method, std::string_view path) const noexcept {
    if (exactSlots_.empty()) {
        return nullptr;
    }

    const auto index = static_cast<std::size_t>(routeHash(method, path, exactSeed_)) & exactMask_;
    const auto* route = exactSlots_[index].route;
    if (route != nullptr && route->method == method && route->path == path) {
        return route;
    }

    return nullptr;
}

const detail::RouteEntry* detail::RouteTable::findRadix(HttpMethod method, std::string_view path) const noexcept {
    return findRadixNode(radixRoots_[methodIndex(method)], path);
}

detail::RouteMatch detail::RouteTable::findDynamic(HttpMethod method, std::string_view path) const noexcept {
    RouteMatch match;
    match.route = findDynamicNode(dynamicRoots_[methodIndex(method)], path, match);
    if (match.route == nullptr || match.paramCount != match.route->paramCount) {
        match.route = nullptr;
        match.paramCount = 0;
    } else {
        for (std::size_t i = 0; i < match.paramCount; ++i) {
            match.params[i].name = match.route->paramNames[i];
        }
    }
    return match;
}

std::uint32_t detail::RouteTable::allowedMethods(std::string_view path) const noexcept {
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < kRoutableMethodCount; ++i) {
        const auto method = static_cast<HttpMethod>(i);
        if (method == HttpMethod::kOptions || (allowedMethodMask_ & (1U << i)) == 0) {
            continue;
        }
        if (findStaticRoute(method, path) != nullptr || findDynamicRoute(method, path).route != nullptr) {
            mask |= 1U << i;
        }
    }
    return mask;
}

std::uint32_t detail::RouteTable::allowedMethodsForServer() const noexcept {
    return allowedMethodMask_;
}

}  // namespace ruvia
