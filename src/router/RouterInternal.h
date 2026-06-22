#pragma once

#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "RouteTable.h"
#include "ruvia/http/ControllerDescriptors.h"
#include "ruvia/router/Router.h"

namespace ruvia::detail {

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

class RouterImpl final {
public:
    Router& owner;

    explicit RouterImpl(Router& router) noexcept
        : owner(router),
          routeTable_(nullptr, RouteTableDeleter{std::pmr::get_default_resource()}) {}

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

    void appendPendingRoute(PendingRoute route);

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

    static void validateNoDynamicRouteConflict(std::span<const PendingRoute> routes);
    void validateRouteTarget(HttpMethod method, std::string_view path) const;
    [[nodiscard]] RouteMiddleware materializeMiddleware(RouteMiddleware middleware);
    void materializeMiddlewares(std::pmr::vector<RouteMiddleware>& middlewares);
    [[nodiscard]] RouteTable buildRouteTable() const;

    struct RouteTableDeleter final {
        std::pmr::memory_resource* resource{std::pmr::get_default_resource()};
        void operator()(RouteTable* table) const noexcept;
    };

    std::pmr::vector<PendingRoute> pendingRoutes_;
    std::pmr::vector<MiddlewareLifetime> middlewareLifetimes_;
    std::unique_ptr<RouteTable, RouteTableDeleter> routeTable_;
    HttpErrorHandler errorHandler_{nullptr};
    bool finalized_{false};
};

}  // namespace ruvia::detail
