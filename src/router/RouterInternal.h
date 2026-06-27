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

class ControllerRouteBuilder::Impl final {
public:
    Impl(
        Router& routerValue,
        std::pmr::string prefixValue,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewareValues)
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

    Router& setErrorHandler(HttpErrorHandler handler) noexcept;
    void finalize();
    [[nodiscard]] const RouteTable& routeTable() const;

    void registerRoute(
        HttpMethod method,
        std::pmr::string path,
        RouteHandler handler,
        RequestBodyMode bodyMode,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
        ResponseBodyMode responseMode = ResponseBodyMode::kBuffered);
    void registerStreamRoute(
        HttpMethod method,
        std::pmr::string path,
        RouteStreamHandler handler,
        ResponseBodyMode responseMode,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
        WebSocketRouteOptions webSocketOptions = {});
    void prependMiddlewares(std::span<const ControllerMiddlewareDescriptor> middlewares);

private:
    class PendingRoute final {
    public:
        struct Init final {
            HttpMethod method;
            std::pmr::string path;
            RouteHandler handler;
            RouteStreamHandler streamHandler;
            RequestBodyMode bodyMode{RequestBodyMode::kBuffered};
            ResponseBodyMode responseMode{ResponseBodyMode::kBuffered};
            bool dynamic{false};
            std::pmr::vector<RouteMiddleware> middlewares;
            std::string_view webSocketSubprotocols{};
            WebSocketHeartbeatOptions webSocketHeartbeat{};
        };

        explicit PendingRoute(Init init);
        PendingRoute(const PendingRoute&) = delete;
        PendingRoute& operator=(const PendingRoute&) = delete;
        PendingRoute(PendingRoute&&) noexcept = default;
        PendingRoute& operator=(PendingRoute&&) noexcept = default;

        [[nodiscard]] HttpMethod method() const noexcept {
            return method_;
        }

        [[nodiscard]] std::string_view path() const noexcept {
            return path_;
        }

        [[nodiscard]] const RouteHandler& handler() const noexcept {
            return handler_;
        }

        [[nodiscard]] const RouteStreamHandler& streamHandler() const noexcept {
            return streamHandler_;
        }

        [[nodiscard]] RequestBodyMode bodyMode() const noexcept {
            return bodyMode_;
        }

        [[nodiscard]] ResponseBodyMode responseMode() const noexcept {
            return responseMode_;
        }

        [[nodiscard]] bool isBufferedResponse() const noexcept {
            return responseMode_ == ResponseBodyMode::kBuffered;
        }

        [[nodiscard]] bool dynamic() const noexcept {
            return dynamic_;
        }

        [[nodiscard]] std::span<const RouteMiddleware> middlewares() const noexcept {
            return middlewares_;
        }

        [[nodiscard]] std::string_view webSocketSubprotocols() const noexcept {
            return webSocketSubprotocols_;
        }

        [[nodiscard]] const WebSocketHeartbeatOptions& webSocketHeartbeat() const noexcept {
            return webSocketHeartbeat_;
        }

        void setDynamic(bool dynamic) noexcept {
            dynamic_ = dynamic;
        }

        void setMiddlewares(std::pmr::vector<RouteMiddleware> middlewares) {
            middlewares_ = std::move(middlewares);
        }

    private:
        HttpMethod method_;
        std::pmr::string path_;
        RouteHandler handler_;
        RouteStreamHandler streamHandler_;
        RequestBodyMode bodyMode_{RequestBodyMode::kBuffered};
        ResponseBodyMode responseMode_{ResponseBodyMode::kBuffered};
        bool dynamic_{false};
        std::pmr::vector<RouteMiddleware> middlewares_;
        std::pmr::string webSocketSubprotocols_;
        WebSocketHeartbeatOptions webSocketHeartbeat_{};
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
    void validateRouteTarget(HttpMethod method, std::string_view path) const;
    [[nodiscard]] RouteMiddleware materializeMiddleware(ControllerMiddlewareDescriptor middleware);
    [[nodiscard]] std::pmr::vector<RouteMiddleware> materializeMiddlewares(
        std::span<const ControllerMiddlewareDescriptor> middlewares);
    [[nodiscard]] RouteTable buildRouteTable() const;

    struct RouteTableDeleter final {
        std::pmr::memory_resource* resource{nullptr};
        void operator()(RouteTable* table) const noexcept;
    };

    std::pmr::vector<PendingRoute> pendingRoutes_;
    std::pmr::vector<MiddlewareLifetime> middlewareLifetimes_;
    std::unique_ptr<RouteTable, RouteTableDeleter> routeTable_;
    HttpErrorHandler errorHandler_{nullptr};
    bool finalized_{false};
};

}  // namespace ruvia::detail
