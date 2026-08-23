#pragma once

#include <utility>
#include <variant>

#include "ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeFields.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Next.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/web/detail/util/CallableRef.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/web/detail/router/RouteModes.h"
// What a registered route runs, as one closed set of alternatives. Each
// alternative carries the handler shape together with the route metadata only
// that shape may have, so a route cannot claim a streaming or WebSocket mode
// while holding a buffered handler.

namespace ruvia::detail {

using RouteHandler = CallableRef<HttpResponse, Context&>;
using RouteStreamHandler = CallableRef<void, Context&>;
using RouteMiddleware = CallableRef<void, Context&, Next&>;

class RouteEndpoint;

class BufferedRouteEndpoint final {
public:
    [[nodiscard]] const RouteHandler& handler() const noexcept {
        return handler_;
    }

    [[nodiscard]] RequestBodyMode requestBodyMode() const noexcept {
        return requestBodyMode_;
    }

private:
    friend class RouteEndpoint;

    BufferedRouteEndpoint(RouteHandler handler, RequestBodyMode requestBodyMode) noexcept
        : handler_(handler),
          requestBodyMode_(requestBodyMode) {}

    RouteHandler handler_;
    RequestBodyMode requestBodyMode_;
};

class ResponseStreamRouteEndpoint final {
public:
    [[nodiscard]] const RouteStreamHandler& handler() const noexcept {
        return handler_;
    }

    [[nodiscard]] ResponseStreamKind kind() const noexcept {
        return kind_;
    }

private:
    friend class RouteEndpoint;

    ResponseStreamRouteEndpoint(RouteStreamHandler handler, ResponseStreamKind kind) noexcept
        : handler_(handler),
          kind_(kind) {}

    RouteStreamHandler handler_;
    ResponseStreamKind kind_;
};

class WebSocketRouteEndpoint final {
public:
    [[nodiscard]] const RouteStreamHandler& handler() const noexcept {
        return handler_;
    }

    [[nodiscard]] std::string_view subprotocols() const noexcept {
        return subprotocols_;
    }

    [[nodiscard]] const WebSocketLifecycleOptions& lifecycle() const noexcept {
        return lifecycle_;
    }

private:
    friend class RouteEndpoint;

    WebSocketRouteEndpoint(std::pmr::memory_resource* resource, RouteStreamHandler handler, WebSocketRouteConfig options)
        : handler_(handler),
          subprotocols_(options.subprotocols, resource),
          lifecycle_(options.lifecycle) {}

    RouteStreamHandler handler_;
    std::pmr::string subprotocols_;
    WebSocketLifecycleOptions lifecycle_;
};

// Startup-built endpoint contract. The handler shape and its only legal route
// metadata live in the same alternative, so a route cannot claim a streaming or
// WebSocket mode while carrying only a buffered handler (or vice versa).
class RouteEndpoint final {
public:
    RouteEndpoint(const RouteEndpoint&) = delete;
    RouteEndpoint& operator=(const RouteEndpoint&) = delete;
    RouteEndpoint(RouteEndpoint&&) noexcept = default;
    RouteEndpoint& operator=(RouteEndpoint&&) = delete;

    [[nodiscard]] static RouteEndpoint buffered(RouteHandler handler, RequestBodyMode requestBodyMode) {
        if (!handler.valid()) {
            throw std::invalid_argument("route handler must not be empty");
        }
        if (requestBodyMode != RequestBodyMode::kBuffered && requestBodyMode != RequestBodyMode::kStream) {
            throw std::invalid_argument("invalid route request-body mode");
        }
        return RouteEndpoint(BufferedRouteEndpoint(handler, requestBodyMode));
    }

    [[nodiscard]] static RouteEndpoint responseStream(RouteStreamHandler handler, ResponseStreamKind kind) {
        if (!handler.valid()) {
            throw std::invalid_argument("route stream handler must not be empty");
        }
        if (kind != ResponseStreamKind::kGeneric && kind != ResponseStreamKind::kSse) {
            throw std::invalid_argument("invalid response-stream kind");
        }
        return RouteEndpoint(ResponseStreamRouteEndpoint(handler, kind));
    }

    [[nodiscard]] static RouteEndpoint webSocket(std::pmr::memory_resource* resource, RouteStreamHandler handler, WebSocketRouteConfig options = {}) {
        if (!handler.valid()) {
            throw std::invalid_argument("websocket route handler must not be empty");
        }
        if (options.lifecycle.closeHandshakeTimeout.has_value() && options.lifecycle.closeHandshakeTimeout->count() <= 0) {
            throw std::invalid_argument("websocket close-handshake timeout must be greater than zero");
        }
        if (!options.lifecycle.heartbeat.pingInterval.has_value()) {
            if (options.lifecycle.heartbeat.pongTimeout.has_value()) {
                throw std::invalid_argument("websocket pong timeout requires a ping interval");
            }
        } else {
            if (options.lifecycle.heartbeat.pingInterval->count() <= 0) {
                throw std::invalid_argument("websocket heartbeat intervals must be greater than zero");
            }
            auto& pongTimeout = options.lifecycle.heartbeat.pongTimeout;
            if (!pongTimeout.has_value()) {
                pongTimeout = options.lifecycle.heartbeat.pingInterval;
            }
            if (pongTimeout->count() <= 0) {
                throw std::invalid_argument("websocket heartbeat intervals must be greater than zero");
            }
        }
        if (!options.subprotocols.empty() && !isValidWebSocketSubprotocolList(options.subprotocols)) {
            throw std::invalid_argument("websocket subprotocols must be a list of at most 64 unique HTTP tokens");
        }
        return RouteEndpoint(WebSocketRouteEndpoint(pmrResourceOrDefault(resource), handler, options));
    }

    [[nodiscard]] RouteEndpoint clone(std::pmr::memory_resource* resource) const {
        if (const auto* endpoint = buffered()) {
            return RouteEndpoint::buffered(endpoint->handler(), endpoint->requestBodyMode());
        }
        if (const auto* endpoint = responseStream()) {
            return RouteEndpoint::responseStream(endpoint->handler(), endpoint->kind());
        }
        const auto& endpoint = *webSocket();
        return RouteEndpoint::webSocket(resource, endpoint.handler(), WebSocketRouteConfig{.subprotocols = std::string(endpoint.subprotocols()), .lifecycle = endpoint.lifecycle()});
    }

    [[nodiscard]] const BufferedRouteEndpoint* buffered() const& noexcept {
        return std::get_if<BufferedRouteEndpoint>(&value_);
    }
    [[nodiscard]] const BufferedRouteEndpoint* buffered() const&& = delete;

    [[nodiscard]] const ResponseStreamRouteEndpoint* responseStream() const& noexcept {
        return std::get_if<ResponseStreamRouteEndpoint>(&value_);
    }
    [[nodiscard]] const ResponseStreamRouteEndpoint* responseStream() const&& = delete;

    [[nodiscard]] const WebSocketRouteEndpoint* webSocket() const& noexcept {
        return std::get_if<WebSocketRouteEndpoint>(&value_);
    }
    [[nodiscard]] const WebSocketRouteEndpoint* webSocket() const&& = delete;

    // Every non-buffered endpoint has a buffered request body contract. Only a
    // buffered-response endpoint may opt into the explicit stream-body route.
    [[nodiscard]] RequestBodyMode requestBodyMode() const noexcept {
        const auto* endpoint = buffered();
        return endpoint == nullptr ? RequestBodyMode::kBuffered : endpoint->requestBodyMode();
    }

private:
    using Value = std::variant<BufferedRouteEndpoint, ResponseStreamRouteEndpoint, WebSocketRouteEndpoint>;

    template <typename Endpoint>
    explicit RouteEndpoint(Endpoint endpoint) noexcept
        : value_(std::move(endpoint)) {}

    Value value_;
};

}  // namespace ruvia::detail
