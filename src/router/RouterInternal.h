#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/Controller.h"
#include "ruvia/router/Router.h"

#include "ruvia/db/Db.h"

namespace ruvia::detail {

class RouteTable;
class RedisRegistry;

struct ControllerRouteBuilder::Impl final {
    Impl(
        Router& routerValue,
        std::pmr::string prefixValue,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewareValues)
        : router(&routerValue),
          prefix(std::move(prefixValue)),
          middlewares(std::move(middlewareValues)) {}

    Router* router;
    std::pmr::string prefix;
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares;
};

struct RouteMatch final {
    const struct RouteEntry* route{nullptr};
    std::array<RouteParamView, kMaxRouteParams> params{};
    std::size_t paramCount{0};
};

enum class RouteResolveStatus {
    kFound,
    kNotFound,
    kMethodNotAllowed
};

struct RouteResolution final {
    RouteResolveStatus status{RouteResolveStatus::kNotFound};
    const struct RouteEntry* route{nullptr};
    RouteMatch match{};
    RequestBodyMode bodyMode{RequestBodyMode::kBuffered};
    std::uint32_t allowedMethods{0};
    bool dynamic{false};

    [[nodiscard]] bool found() const noexcept {
        return status == RouteResolveStatus::kFound && route != nullptr;
    }
};

struct RouteHandler final {
    void* target{nullptr};
    Next::Invoke invoke{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr;
    }

    [[nodiscard]] Task<HttpResponse> operator()(Context& context) const {
        if (invoke == nullptr) {
            throw std::logic_error("route handler is empty");
        }
        return invoke(target, context);
    }
};

struct RouteStreamHandler final {
    using Invoke = Task<void> (*)(void*, Context&);

    void* target{nullptr};
    Invoke invoke{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr;
    }
    [[nodiscard]] Task<void> operator()(Context& context) const {
        if (invoke == nullptr) {
            throw std::logic_error("route stream handler is empty");
        }
        return invoke(target, context);
    }
};

struct RouteMiddleware final {
    using Invoke = Task<HttpResponse> (*)(void*, Context&, const Next&);
    using Create = void* (*)();
    using Destroy = void (*)(void*) noexcept;

    void* target{nullptr};
    Invoke invoke{nullptr};
    Create create{nullptr};
    Destroy destroy{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr && (target != nullptr || create != nullptr);
    }

    [[nodiscard]] Task<HttpResponse> operator()(Context& context, const Next& next) const {
        return invoke(target, context, next);
    }
};

struct StreamDispatchResult final {
    HttpResponse response;
    bool streamHandled{false};
};

struct RouteEntry final {
    HttpMethod method;
    std::pmr::string path;
    RouteHandler handler;
    RouteStreamHandler streamHandler;
    RequestBodyMode bodyMode{RequestBodyMode::kBuffered};
    ResponseBodyMode responseMode{ResponseBodyMode::kBuffered};
    bool dynamic{false};
    std::array<std::string_view, kMaxRouteParams> paramNames{};
    std::size_t paramCount{0};
    std::size_t middlewareOffset{0};
    std::size_t middlewareCount{0};
    std::pmr::string webSocketSubprotocols;
    WebSocketHeartbeatOptions webSocketHeartbeat{};
};

class RouteTable final {
public:
    RouteTable() = default;
    RouteTable(const RouteTable&) = delete;
    RouteTable& operator=(const RouteTable&) = delete;
    RouteTable(RouteTable&&) noexcept = default;
    RouteTable& operator=(RouteTable&&) noexcept = default;

    void setErrorHandler(HttpErrorHandler handler) noexcept;
    [[nodiscard]] RouteResolution resolve(const HttpRequest& request) const noexcept;
    Task<HttpResponse> dispatch(
        const HttpRequest& request,
        RequestMemory& memory,
        DbRegistry* db = nullptr,
        RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr) const;
    Task<HttpResponse> dispatch(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        DbRegistry* db = nullptr,
        RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr) const;
    Task<HttpResponse> handleError(
        const HttpRequest& request,
        RequestMemory& memory,
        HttpErrorInfo error,
        bool closeConnection,
        DbRegistry* db = nullptr,
        RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr) const;
    Task<HttpResponse> handleException(
        const HttpRequest& request,
        RequestMemory& memory,
        std::exception_ptr exception,
        bool closeConnection,
        DbRegistry* db = nullptr,
        RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr) const;
    Task<StreamDispatchResult> dispatchResponseStream(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ResponseStreamWriter& responseStream,
        DbRegistry* db = nullptr,
        RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr) const;
    Task<StreamDispatchResult> dispatchWebSocket(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        WebSocket& webSocket,
        DbRegistry* db = nullptr,
        RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr) const;
    [[nodiscard]] RequestBodyMode bodyModeFor(const HttpRequest& request) const noexcept;

private:
    friend class RouterImpl;

    static constexpr std::size_t kRoutableMethodCount = 7;

    struct PerfectSlot {
        const RouteEntry* route{nullptr};
    };

    struct RadixNode {
        std::pmr::string label;
        std::pmr::vector<RadixNode> children;
        const RouteEntry* route{nullptr};
    };

    struct DynamicNode;

    struct DynamicStaticChild {
        std::pmr::string segment;
        std::unique_ptr<DynamicNode> node;
    };

    struct DynamicNode {
        std::pmr::vector<DynamicStaticChild> staticChildren;
        std::unique_ptr<DynamicNode> paramChild;
        const RouteEntry* route{nullptr};
        const RouteEntry* wildcardRoute{nullptr};
    };

    struct MiddlewareContinuation {
        const RouteTable* table{nullptr};
        const RouteEntry* route{nullptr};
        std::size_t index{0};
    };

    struct StreamMiddlewareContinuation {
        const RouteTable* table{nullptr};
        const RouteEntry* route{nullptr};
        std::size_t index{0};
        bool* streamHandled{nullptr};
    };

    void buildPerfectHash();
    void buildRadix();
    void buildDynamicRoutes();
    void buildAllowedMethodMask() noexcept;
    void validateNoDynamicRouteConflict() const;

    [[nodiscard]] static std::size_t methodIndex(HttpMethod method) noexcept;
    [[nodiscard]] static bool isRoutableMethod(HttpMethod method) noexcept;
    [[nodiscard]] static bool isDynamicPath(std::string_view path) noexcept;
    [[nodiscard]] static std::uint64_t routeHash(
        HttpMethod method,
        std::string_view path,
        std::uint64_t seed) noexcept;
    [[nodiscard]] static std::size_t nextPowerOfTwo(std::size_t value) noexcept;
    [[nodiscard]] static std::size_t commonPrefixLength(
        std::string_view left,
        std::string_view right) noexcept;
    static void insertRadix(RadixNode& node, std::string_view path, const RouteEntry& route);
    [[nodiscard]] static const RouteEntry* findRadixNode(const RadixNode& root, std::string_view path) noexcept;
    static void collectDynamicParamNames(RouteEntry& route);
    static void insertDynamic(DynamicNode& root, const RouteEntry& route);
    [[nodiscard]] static const RouteEntry* findDynamicNode(
        const DynamicNode& node,
        std::string_view path,
        RouteMatch& match) noexcept;
    [[nodiscard]] static bool addParam(
        RouteMatch& match,
        std::string_view name,
        std::string_view value) noexcept;
    [[nodiscard]] static bool splitSegment(
        std::string_view path,
        std::string_view& segment,
        std::string_view& rest) noexcept;
    [[nodiscard]] static bool sameDynamicShape(std::string_view left, std::string_view right) noexcept;

    [[nodiscard]] const RouteEntry* findStaticRoute(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] RouteMatch findDynamicRoute(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findPerfect(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findRadix(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] RouteMatch findDynamic(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethods(std::string_view path) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethodsForServer() const noexcept;
    [[nodiscard]] Task<HttpResponse> invokeRoute(const RouteEntry& route, Context& context) const;
    [[nodiscard]] Task<HttpResponse> invokeMiddlewareAt(
        const RouteEntry& route,
        std::size_t index,
        Context& context) const;
    [[nodiscard]] static Task<HttpResponse> invokeMiddlewareContinuation(void* target, Context& context);
    [[nodiscard]] Task<HttpResponse> invokeStreamRoute(
        const RouteEntry& route,
        Context& context,
        bool& streamHandled) const;
    [[nodiscard]] Task<HttpResponse> invokeStreamMiddlewareAt(
        const RouteEntry& route,
        std::size_t index,
        Context& context,
        bool& streamHandled) const;
    [[nodiscard]] static Task<HttpResponse> invokeStreamMiddlewareContinuation(void* target, Context& context);
    [[nodiscard]] Task<HttpResponse> handleError(
        Context& context,
        HttpErrorInfo error,
        bool closeConnection) const;
    [[nodiscard]] Task<HttpResponse> handleException(
        Context& context,
        std::exception_ptr exception,
        bool closeConnection) const;

    std::pmr::vector<RouteEntry> routes_;
    std::pmr::vector<RouteMiddleware> middlewareFrames_;
    std::pmr::vector<PerfectSlot> exactSlots_;
    std::array<RadixNode, kRoutableMethodCount> radixRoots_{};
    std::array<DynamicNode, kRoutableMethodCount> dynamicRoots_{};
    std::array<bool, kRoutableMethodCount> hasDynamicRoutes_{};
    std::uint32_t allowedMethodMask_{0};
    std::uint64_t exactSeed_{0};
    std::size_t exactMask_{0};
    HttpErrorHandler errorHandler_{nullptr};
};

class RouterImpl final {
public:
    Router& owner;

    explicit RouterImpl(Router& router) noexcept : owner(router) {}

    RouterImpl(const RouterImpl&) = delete;
    RouterImpl& operator=(const RouterImpl&) = delete;

    [[nodiscard]] static RouterImpl& from(Router& router) noexcept {
        return *router.impl_;
    }

    [[nodiscard]] static const RouterImpl& from(const Router& router) noexcept {
        return *router.impl_;
    }

    Router& setErrorHandler(HttpErrorHandler handler) noexcept;
    void finalize();
    [[nodiscard]] const RouteTable& routeTable() const;

    [[nodiscard]] RouteResolution resolve(const HttpRequest& request) const noexcept;
    Task<HttpResponse> dispatch(
        const HttpRequest& request,
        RequestMemory& memory,
        DbRegistry* db = nullptr,
        RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr) const;
    Task<HttpResponse> dispatch(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        DbRegistry* db = nullptr,
        RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr) const;
    Task<HttpResponse> handleError(
        const HttpRequest& request,
        RequestMemory& memory,
        HttpErrorInfo error,
        bool closeConnection,
        DbRegistry* db = nullptr,
        RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr) const;
    Task<HttpResponse> handleException(
        const HttpRequest& request,
        RequestMemory& memory,
        std::exception_ptr exception,
        bool closeConnection,
        DbRegistry* db = nullptr,
        RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr) const;
    Task<StreamDispatchResult> dispatchResponseStream(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ResponseStreamWriter& responseStream,
        DbRegistry* db = nullptr,
        RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr) const;
    [[nodiscard]] RequestBodyMode bodyModeFor(const HttpRequest& request) const noexcept;

    void registerRoute(
        HttpMethod method,
        std::pmr::string path,
        RouteHandler handler,
        RequestBodyMode bodyMode,
        std::pmr::vector<RouteMiddleware> middlewares = {});
    void registerStreamRoute(
        HttpMethod method,
        std::pmr::string path,
        RouteStreamHandler handler,
        ResponseBodyMode responseMode,
        std::pmr::vector<RouteMiddleware> middlewares = {},
        WebSocketRouteOptions webSocketOptions = {});
    void prependMiddlewares(std::span<const ControllerMiddlewareDescriptor> middlewares);

private:
    struct PendingRoute final {
        HttpMethod method;
        std::pmr::string path;
        RouteHandler handler;
        RouteStreamHandler streamHandler;
        RequestBodyMode bodyMode{RequestBodyMode::kBuffered};
        ResponseBodyMode responseMode{ResponseBodyMode::kBuffered};
        bool dynamic{false};
        std::pmr::vector<RouteMiddleware> middlewares;
        std::pmr::string webSocketSubprotocols;
        WebSocketHeartbeatOptions webSocketHeartbeat{};
    };

    struct MiddlewareLifetime {
        void* target{nullptr};
        RouteMiddleware::Destroy destroy{nullptr};

        MiddlewareLifetime() noexcept = default;
        MiddlewareLifetime(void* target, RouteMiddleware::Destroy destroy) noexcept;
        MiddlewareLifetime(const MiddlewareLifetime&) = delete;
        MiddlewareLifetime& operator=(const MiddlewareLifetime&) = delete;
        MiddlewareLifetime(MiddlewareLifetime&& other) noexcept;
        MiddlewareLifetime& operator=(MiddlewareLifetime&& other) noexcept;
        ~MiddlewareLifetime();

    private:
        void reset() noexcept;
    };

    [[nodiscard]] static bool isDynamicPath(std::string_view path) noexcept;
    [[nodiscard]] static bool splitSegment(
        std::string_view path,
        std::string_view& segment,
        std::string_view& rest) noexcept;
    [[nodiscard]] static bool sameDynamicShape(std::string_view left, std::string_view right) noexcept;
    static void validateNoDynamicRouteConflict(std::span<const PendingRoute> routes);
    void validateRouteTarget(HttpMethod method, std::string_view path) const;
    [[nodiscard]] RouteMiddleware materializeMiddleware(RouteMiddleware middleware);
    [[nodiscard]] std::pmr::vector<RouteMiddleware> materializeMiddlewares(std::pmr::vector<RouteMiddleware> middlewares);
    [[nodiscard]] RouteTable buildRouteTable() const;

    std::pmr::vector<PendingRoute> pendingRoutes_;
    std::pmr::vector<MiddlewareLifetime> middlewareLifetimes_;
    std::unique_ptr<RouteTable> routeTable_;
    HttpErrorHandler errorHandler_{nullptr};
    bool finalized_{false};
};

}  // namespace ruvia::detail
