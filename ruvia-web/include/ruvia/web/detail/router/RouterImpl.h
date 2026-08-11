#pragma once

#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/controller/ControllerDescriptors.h"
#include "ruvia/web/detail/router/Router.h"

namespace ruvia::detail {

class ControllerRouteBuilder::Impl final {
public:
    Impl(Router& routerValue, std::pmr::string prefixValue, std::pmr::vector<ControllerMiddlewareDescriptor> middlewareValues)
        : router_(routerValue),
          prefix_(std::move(prefixValue)),
          middlewares_(std::move(middlewareValues)) {}

    [[nodiscard]] Router& router() const noexcept {
        return router_;
    }

    [[nodiscard]] std::string_view prefix() const noexcept {
        return prefix_;
    }

    [[nodiscard]] const std::pmr::vector<ControllerMiddlewareDescriptor>& middlewares() const noexcept {
        return middlewares_;
    }

private:
    Router& router_;
    std::pmr::string prefix_;
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares_;
};

class RouterImpl final {
public:
    Router& owner;

    explicit RouterImpl(Router& router) noexcept;

    RouterImpl(const RouterImpl&) = delete;
    RouterImpl& operator=(const RouterImpl&) = delete;

    [[nodiscard]] static RouterImpl& from(Router& router) noexcept {
        return *router.impl_;
    }

    [[nodiscard]] static const RouterImpl& from(const Router& router) noexcept {
        return *router.impl_;
    }

    Router& setErrorHandler(HttpErrorHandlerRef handler) noexcept;
    Router& setNotFoundHandler(HttpNotFoundHandlerRef handler) noexcept;
    // Path-prefix-scoped fallbacks (Hono sub-app scoping analog): wholesale
    // replacement, owned copies; applied to the table at finalize or, when the
    // table already exists, immediately (both are idempotent for restarts).
    Router& setPrefixErrorHandlers(std::span<const HttpPrefixErrorHandler> handlers);
    Router& setPrefixNotFoundHandlers(std::span<const HttpPrefixNotFoundHandler> handlers);
    // App-wide middleware, prepended to every route's chain at finalize. Each
    // descriptor is materialized exactly once per worker route graph; that
    // worker-local instance serves all routes in the graph.
    void setGlobalMiddlewares(std::span<const ControllerMiddlewareDescriptor> descriptors);
    void finalize();
    [[nodiscard]] const RouteTable& routeTable() const;

    void registerRoute(HttpKnownMethod method, std::pmr::string path, RouteHandler handler, RequestBodyMode bodyMode, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares);
    void registerResponseStreamRoute(HttpKnownMethod method, std::pmr::string path, RouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares);
    void registerSseRoute(HttpKnownMethod method, std::pmr::string path, RouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares);
    void registerWebSocketRoute(HttpKnownMethod method, std::pmr::string path, RouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares, WebSocketRouteOptions webSocketOptions = {});

    // An extension method is routed by its exact wire token. Kept off the
    // enum-indexed structures entirely: extension routes are rare, so they get
    // a separate cold list rather than widening every dense per-method array.
    void registerExtensionMethodRoute(std::string_view methodToken, std::pmr::string path, RouteHandler handler, RequestBodyMode bodyMode, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares);

private:
    void registerEndpoint(HttpKnownMethod method, std::pmr::string path, RouteEndpoint endpoint, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares);
    void registerEndpointWithToken(HttpKnownMethod method, std::string_view methodToken, std::pmr::string path, RouteEndpoint endpoint, std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares, std::span<const ControllerMiddlewareDescriptor> routeMiddlewares);

    class PendingRoute final {
    public:
        struct Init final {
            HttpKnownMethod method;
            std::pmr::string methodToken;
            std::pmr::string path;
            RouteEndpoint endpoint;
            bool dynamic{false};
            std::size_t maxRequestBodyBytes{0};
            std::pmr::vector<RouteMiddleware> middlewares;
        };

        PendingRoute(std::pmr::memory_resource* resource, Init init);
        PendingRoute(const PendingRoute&) = delete;
        PendingRoute& operator=(const PendingRoute&) = delete;
        PendingRoute(PendingRoute&&) noexcept = default;
        PendingRoute& operator=(PendingRoute&&) = delete;

        [[nodiscard]] HttpKnownMethod method() const noexcept {
            return method_;
        }

        [[nodiscard]] std::string_view methodToken() const noexcept {
            return methodToken_;
        }

        [[nodiscard]] std::string_view path() const noexcept {
            return path_;
        }

        [[nodiscard]] const RouteEndpoint& endpoint() const noexcept {
            return endpoint_;
        }

        [[nodiscard]] bool dynamic() const noexcept {
            return dynamic_;
        }

        [[nodiscard]] std::span<const RouteMiddleware> middlewares() const noexcept {
            return middlewares_;
        }

        [[nodiscard]] std::size_t maxRequestBodyBytes() const noexcept {
            return maxRequestBodyBytes_;
        }

        void setDynamic(bool dynamic) noexcept {
            dynamic_ = dynamic;
        }

    private:
        HttpKnownMethod method_;
        std::pmr::string methodToken_;
        std::pmr::string path_;
        RouteEndpoint endpoint_;
        bool dynamic_{false};
        std::size_t maxRequestBodyBytes_{0};
        std::pmr::vector<RouteMiddleware> middlewares_;
    };

    void appendPendingRoute(PendingRoute route);

    class MiddlewareLifetime {
    public:
        MiddlewareLifetime() noexcept = default;
        MiddlewareLifetime(void* target, ControllerMiddlewareDescriptor::Destroy destroy) noexcept;
        MiddlewareLifetime(const MiddlewareLifetime&) = delete;
        MiddlewareLifetime& operator=(const MiddlewareLifetime&) = delete;
        MiddlewareLifetime(MiddlewareLifetime&& other) noexcept;
        MiddlewareLifetime& operator=(MiddlewareLifetime&& other) noexcept;
        ~MiddlewareLifetime();

    private:
        void reset() noexcept;

        void* target_{nullptr};
        ControllerMiddlewareDescriptor::Destroy destroy_{nullptr};
    };

    static void validateNoDynamicRouteConflict(std::span<const PendingRoute> routes);
    void validateRouteTarget(HttpKnownMethod method, std::string_view methodToken, std::string_view path) const;
    [[nodiscard]] RouteMiddleware materializeMiddleware(ControllerMiddlewareDescriptor middleware);
    void appendMaterializedMiddlewares(std::pmr::vector<RouteMiddleware>& frames, std::span<const ControllerMiddlewareDescriptor> descriptors);
    [[nodiscard]] std::pmr::vector<RouteMiddleware> materializeMiddlewares(std::span<const ControllerMiddlewareDescriptor> first, std::span<const ControllerMiddlewareDescriptor> second = {});
    void buildRouteTable(RouteTable& table) const;

    struct RouteTableDeleter final {
        std::pmr::memory_resource* resource{nullptr};
        void operator()(RouteTable* table) const noexcept;
    };

    std::pmr::memory_resource* resource_{nullptr};
    std::pmr::vector<PendingRoute> pendingRoutes_;
    std::pmr::vector<MiddlewareLifetime> middlewareLifetimes_;
    std::pmr::vector<ControllerMiddlewareDescriptor> globalMiddlewareDescriptors_;
    std::pmr::vector<RouteMiddleware> globalMiddlewareFrames_;
    std::unique_ptr<RouteTable, RouteTableDeleter> routeTable_;
    HttpErrorHandlerRef errorHandler_{nullptr};
    HttpNotFoundHandlerRef notFoundHandler_{nullptr};
    std::pmr::vector<std::pair<std::pmr::string, HttpErrorHandlerRef>> prefixErrorHandlers_{registrationResource()};
    std::pmr::vector<std::pair<std::pmr::string, HttpNotFoundHandlerRef>> prefixNotFoundHandlers_{registrationResource()};
    bool hasRouteRateLimit_{false};
};

}  // namespace ruvia::detail
