#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/websocket_subprotocol_set.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/router/route_modes.h"
#include "ruvia/web/detail/util/callable_ref.h"
#include "ruvia/web/http_tunnel_route_config.h"
#include "ruvia/web/next.h"
#include "ruvia/web/websocket.h"

#include "websocket/websocket_heartbeat_config_validation.h"
// What a registered route runs, as one closed set of alternatives. Each
// alternative carries the handler shape together with the route metadata only
// that shape may have, so a route cannot claim a streaming or websocket mode
// while holding a buffered handler.

namespace ruvia::detail {

using route_handler_type = callable_ref<http_response, context&>;
using route_stream_handler_type = callable_ref<void, context&>;
using route_middleware_type = callable_ref<void, context&, next&>;

class route_endpoint;

class buffered_route_endpoint final {
public:
    [[nodiscard]] const route_handler_type& handler() const noexcept {
        return handler_;
    }

    [[nodiscard]] detail::request_body_mode request_body_mode() const noexcept {
        return request_body_mode_;
    }

    [[nodiscard]] bool replay_safe() const noexcept {
        return replay_safe_;
    }

private:
    friend class route_endpoint;

    buffered_route_endpoint(route_handler_type handler, detail::request_body_mode request_body_mode_value,
        bool replay_safe) noexcept
        : handler_(handler),
          request_body_mode_(request_body_mode_value),
          replay_safe_(replay_safe) {}

    route_handler_type handler_;
    detail::request_body_mode request_body_mode_;
    bool replay_safe_{};
};

class response_stream_route_endpoint final {
public:
    [[nodiscard]] const route_stream_handler_type& handler() const noexcept {
        return handler_;
    }

    [[nodiscard]] http_response_stream_kind kind() const noexcept {
        return kind_;
    }

private:
    friend class route_endpoint;

    response_stream_route_endpoint(route_stream_handler_type handler, http_response_stream_kind kind) noexcept
        : handler_(handler),
          kind_(kind) {}

    route_stream_handler_type handler_;
    http_response_stream_kind kind_;
};

class tunnel_route_endpoint final {
public:
    [[nodiscard]] const route_stream_handler_type& handler() const noexcept {
        return handler_;
    }
    [[nodiscard]] std::string_view protocol() const noexcept {
        return protocol_;
    }
    [[nodiscard]] const http_tunnel_route_config& config() const noexcept {
        return config_;
    }

private:
    friend class route_endpoint;
    tunnel_route_endpoint(std::pmr::memory_resource* resource, route_stream_handler_type handler, std::string_view protocol, http_tunnel_route_config config)
        : handler_(handler),
          protocol_(protocol, resource),
          config_(config) {}
    route_stream_handler_type handler_;
    std::pmr::string protocol_;
    http_tunnel_route_config config_;
};

class websocket_route_endpoint final {
public:
    [[nodiscard]] const route_stream_handler_type& handler() const noexcept {
        return handler_;
    }

    [[nodiscard]] std::span<const std::string_view> subprotocols() const noexcept {
        return subprotocols_;
    }

    [[nodiscard]] const websocket_lifecycle_options& lifecycle() const noexcept {
        return lifecycle_;
    }

    [[nodiscard]] const websocket_deflate_config& deflate() const noexcept {
        return deflate_;
    }

private:
    friend class route_endpoint;

    websocket_route_endpoint(std::pmr::memory_resource* resource, route_stream_handler_type handler,
        const websocket_route_config& options)
        : handler_(handler),
          subprotocol_storage_(resource),
          subprotocols_(resource),
          lifecycle_(options.lifecycle_),
          deflate_(options.deflate_) {
        subprotocol_storage_.reserve(options.subprotocols_.size());
        for (const auto& subprotocol : options.subprotocols_) {
            subprotocol_storage_.emplace_back(subprotocol);
        }
        subprotocols_.reserve(subprotocol_storage_.size());
        for (const auto& subprotocol : subprotocol_storage_) {
            subprotocols_.emplace_back(subprotocol);
        }
    }

    route_stream_handler_type handler_;
    std::pmr::vector<std::pmr::string> subprotocol_storage_;
    std::pmr::vector<std::string_view> subprotocols_;
    websocket_lifecycle_options lifecycle_;
    websocket_deflate_config deflate_;
};

// Startup-built endpoint contract. The handler shape and its only legal route
// metadata live in the same alternative, so a route cannot claim a streaming or
// websocket mode while carrying only a buffered handler (or vice versa).
class route_endpoint final {
public:
    route_endpoint(const route_endpoint&) = delete;
    route_endpoint& operator=(const route_endpoint&) = delete;
    route_endpoint(route_endpoint&&) noexcept = default;
    route_endpoint& operator=(route_endpoint&&) = delete;

    [[nodiscard]] static route_endpoint buffered(
        route_handler_type handler, request_body_mode request_body_mode_value, bool replay_safe = false) {
        if (!handler.valid()) {
            throw std::invalid_argument("route handler must not be empty");
        }
        if (request_body_mode_value != request_body_mode::buffered &&
            request_body_mode_value != request_body_mode::stream) {
            throw std::invalid_argument("invalid route request-body mode");
        }
        return route_endpoint(buffered_route_endpoint(handler, request_body_mode_value, replay_safe));
    }

    [[nodiscard]] static route_endpoint response_stream(
        route_stream_handler_type handler, http_response_stream_kind kind) {
        if (!handler.valid()) {
            throw std::invalid_argument("route stream handler must not be empty");
        }
        if (kind != http_response_stream_kind::generic && kind != http_response_stream_kind::sse) {
            throw std::invalid_argument("invalid response-stream kind");
        }
        return route_endpoint(response_stream_route_endpoint(handler, kind));
    }

    [[nodiscard]] static route_endpoint get_websocket(std::pmr::memory_resource* resource,
        route_stream_handler_type handler, websocket_route_config options = {}) {
        if (!handler.valid()) {
            throw std::invalid_argument("websocket route handler must not be empty");
        }
        if (options.lifecycle_.close_handshake_timeout_.has_value() &&
            options.lifecycle_.close_handshake_timeout_->count() <= 0) {
            throw std::invalid_argument(
                "websocket close-handshake timeout must be greater than zero");
        }
        if (options.lifecycle_.peer_transport_fin_timeout_.count() <= 0) {
            throw std::invalid_argument(
                "websocket peer transport FIN timeout must be greater than zero");
        }
        if (options.deflate_.compression_level_ < 0 || options.deflate_.compression_level_ > 9) {
            throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
        }
        options.lifecycle_.heartbeat_ =
            normalize_websocket_heartbeat_config(options.lifecycle_.heartbeat_);
        ruvia::websocket_subprotocol_set subprotocols;
        for (const auto& subprotocol : options.subprotocols_) {
            if (!subprotocols.append(subprotocol)) {
                throw std::invalid_argument(
                    "websocket subprotocols must contain at most 64 unique HTTP tokens");
            }
        }
        return route_endpoint(
            websocket_route_endpoint(pmr_resource_or_default(resource), handler, options));
    }

    [[nodiscard]] static route_endpoint tunnel(std::pmr::memory_resource* resource,
        route_stream_handler_type handler, std::string_view protocol, http_tunnel_route_config config = {}) {
        if (!handler.valid() || (!protocol.empty() && !is_valid_http_method_token(protocol))) {
            throw std::invalid_argument("invalid CONNECT handler or protocol token");
        }
        if (protocol == "connect-udp") {
            config.datagrams_ = true;
        }
        if (config.datagrams_ && protocol.empty()) {
            throw std::invalid_argument("HTTP Datagrams require an Extended CONNECT protocol");
        }
        if (config.peer_transport_fin_timeout_ <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("CONNECT peer transport FIN timeout must be greater than zero");
        }
        return route_endpoint(tunnel_route_endpoint(pmr_resource_or_default(resource), handler, protocol, config));
    }
    [[nodiscard]] const tunnel_route_endpoint* tunnel() const& noexcept {
        return std::get_if<tunnel_route_endpoint>(&value_);
    }
    const tunnel_route_endpoint* tunnel() const&& = delete;

    [[nodiscard]] route_endpoint clone(std::pmr::memory_resource* resource) const {
        if (const auto* endpoint = tunnel()) {
            return route_endpoint::tunnel(resource, endpoint->handler(), endpoint->protocol(), endpoint->config());
        }
        if (const auto* endpoint = buffered()) {
            return route_endpoint::buffered(
                endpoint->handler(), endpoint->request_body_mode(), endpoint->replay_safe());
        }
        if (const auto* endpoint = response_stream()) {
            return route_endpoint::response_stream(endpoint->handler(), endpoint->kind());
        }
        const auto& endpoint = *get_websocket();
        websocket_route_config options{.lifecycle_ = endpoint.lifecycle(), .deflate_ = endpoint.deflate()};
        options.subprotocols_.reserve(endpoint.subprotocols().size());
        for (const auto subprotocol : endpoint.subprotocols()) {
            options.subprotocols_.emplace_back(subprotocol);
        }
        return route_endpoint::get_websocket(resource, endpoint.handler(), std::move(options));
    }

    [[nodiscard]] const buffered_route_endpoint* buffered() const& noexcept {
        return std::get_if<buffered_route_endpoint>(&value_);
    }
    [[nodiscard]] const buffered_route_endpoint* buffered() const&& = delete;

    [[nodiscard]] const response_stream_route_endpoint* response_stream() const& noexcept {
        return std::get_if<response_stream_route_endpoint>(&value_);
    }
    [[nodiscard]] const response_stream_route_endpoint* response_stream() const&& = delete;

    [[nodiscard]] const websocket_route_endpoint* get_websocket() const& noexcept {
        return std::get_if<websocket_route_endpoint>(&value_);
    }
    [[nodiscard]] const websocket_route_endpoint* get_websocket() const&& = delete;

    // Every non-buffered endpoint has a buffered request body contract. Only a
    // buffered-response endpoint may opt into the explicit stream-body route.
    [[nodiscard]] ruvia::detail::request_body_mode request_body_mode() const noexcept {
        const auto* endpoint = buffered();
        return endpoint == nullptr ? request_body_mode::buffered : endpoint->request_body_mode();
    }

private:
    using value_type =
        std::variant<buffered_route_endpoint, response_stream_route_endpoint, websocket_route_endpoint, tunnel_route_endpoint>;

    template <typename endpoint_type>
    explicit route_endpoint(endpoint_type endpoint) noexcept
        : value_(std::move(endpoint)) {}

    value_type value_;
};

}  // namespace ruvia::detail
