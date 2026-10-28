#pragma once

#include <chrono>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/detail/router/route_handlers.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/websocket.h"

#include "websocket/websocket_heartbeat_config_validation.h"

// What a registered route runs, as one closed set of alternatives. Each
// alternative carries the handler's invoke thunk together with the route
// metadata only that shape may have, so a route cannot claim a streaming or
// websocket mode while holding a buffered handler. The handler's target is
// worker-bound and lives in each worker's route_table, never here.

namespace ruvia::detail {

class route_endpoint;

class buffered_route_endpoint final {
public:
    [[nodiscard]] route_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] detail::request_body_mode request_body_mode() const noexcept {
        return request_body_mode_;
    }

private:
    friend class route_endpoint;

    buffered_route_endpoint(route_handler::invoke_type invoke,
        detail::request_body_mode request_body_mode_value) noexcept
        : invoke_(invoke),
          request_body_mode_(request_body_mode_value) {}

    route_handler::invoke_type invoke_;
    detail::request_body_mode request_body_mode_;
};

class response_stream_route_endpoint final {
public:
    [[nodiscard]] route_stream_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] detail::request_body_mode request_body_mode() const noexcept {
        return request_body_mode_;
    }

private:
    friend class route_endpoint;

    explicit response_stream_route_endpoint(route_stream_handler::invoke_type invoke,
        detail::request_body_mode request_body_mode_value = detail::request_body_mode::buffered) noexcept
        : invoke_(invoke),
          request_body_mode_(request_body_mode_value) {}

    route_stream_handler::invoke_type invoke_;
    detail::request_body_mode request_body_mode_;
};

class tunnel_route_endpoint final {
public:
    [[nodiscard]] route_stream_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }
    [[nodiscard]] std::string_view protocol() const noexcept {
        return protocol_;
    }
    [[nodiscard]] const http_tunnel_route_config& config() const noexcept {
        return config_;
    }

private:
    friend class route_endpoint;
    tunnel_route_endpoint(std::pmr::memory_resource* resource, route_stream_handler::invoke_type invoke,
        std::string_view protocol, http_tunnel_route_config config)
        : invoke_(invoke),
          protocol_(protocol, resource),
          config_(config) {}
    route_stream_handler::invoke_type invoke_;
    std::pmr::string protocol_;
    http_tunnel_route_config config_;
};

class websocket_route_endpoint final {
public:
    [[nodiscard]] route_stream_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] std::span<const std::string_view> subprotocols() const noexcept {
        return subprotocols_;
    }

    [[nodiscard]] websocket_handshake_validation_options handshake_validation() const noexcept {
        return {.supported_subprotocols_ = subprotocols_, .require_subprotocol_ = require_subprotocol_};
    }

    [[nodiscard]] const websocket_lifecycle_options& lifecycle() const noexcept {
        return lifecycle_;
    }

    [[nodiscard]] const websocket_deflate_config& deflate() const noexcept {
        return deflate_;
    }

private:
    friend class route_endpoint;

    websocket_route_endpoint(std::pmr::memory_resource* resource, route_stream_handler::invoke_type invoke,
        const websocket_route_config& options)
        : invoke_(invoke),
          subprotocol_storage_(resource),
          subprotocols_(resource),
          require_subprotocol_(options.require_subprotocol_),
          lifecycle_(options.lifecycle_),
          deflate_(options.deflate_) {
        if (!options.subprotocols_.empty()) {
            std::size_t total_bytes = 0;
            for (const auto& subprotocol : options.subprotocols_) {
                total_bytes += subprotocol.size();
            }
            subprotocol_storage_.reserve(total_bytes);
            subprotocols_.reserve(options.subprotocols_.size());
            for (const auto& subprotocol : options.subprotocols_) {
                const auto offset = subprotocol_storage_.size();
                subprotocol_storage_.insert(
                    subprotocol_storage_.end(), subprotocol.begin(), subprotocol.end());
                subprotocols_.emplace_back(
                    subprotocol_storage_.data() + offset, subprotocol.size());
            }
        }
    }

    route_stream_handler::invoke_type invoke_;
    std::pmr::vector<char> subprotocol_storage_;
    std::pmr::vector<std::string_view> subprotocols_;
    bool require_subprotocol_;
    websocket_lifecycle_options lifecycle_;
    websocket_deflate_config deflate_;
};

// Startup-built endpoint contract, validated once when the process route plan
// is compiled. The handler shape and its only legal route metadata live in the
// same alternative.
class route_endpoint final {
public:
    route_endpoint(const route_endpoint&) = delete;
    route_endpoint& operator=(const route_endpoint&) = delete;
    route_endpoint(route_endpoint&&) noexcept = default;
    route_endpoint& operator=(route_endpoint&&) = delete;

    // Handlers come from the controller macros' bind()/bind_stream(), which
    // always supply an invoker; body modes and stream kinds are the builder's
    // own enumerators.
    [[nodiscard]] static route_endpoint buffered(route_handler::invoke_type invoke,
        request_body_mode request_body_mode_value) {
        return route_endpoint(buffered_route_endpoint(invoke, request_body_mode_value));
    }

    [[nodiscard]] static route_endpoint response_stream(
        route_stream_handler::invoke_type invoke,
        request_body_mode request_body_mode_value = request_body_mode::buffered) {
        return route_endpoint(response_stream_route_endpoint(invoke, request_body_mode_value));
    }

    [[nodiscard]] static route_endpoint websocket(std::pmr::memory_resource* resource,
        route_stream_handler::invoke_type invoke, websocket_route_config options) {
        if (options.lifecycle_.close_handshake_timeout_.has_value() &&
            options.lifecycle_.close_handshake_timeout_->count() <= 0) {
            throw std::invalid_argument(
                "websocket close-handshake timeout must be greater than zero");
        }
        if (options.deflate_.compression_level_ < 0 || options.deflate_.compression_level_ > 9) {
            throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
        }
        if (options.deflate_.mem_level_ < 1 || options.deflate_.mem_level_ > 9) {
            throw std::invalid_argument("WebSocket deflate memory level must be between 1 and 9");
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
        if (options.require_subprotocol_ && options.subprotocols_.empty()) {
            throw std::invalid_argument("a websocket route requiring a subprotocol must list subprotocols");
        }
        return route_endpoint(
            websocket_route_endpoint(pmr_resource_or_default(resource), invoke, options));
    }

    [[nodiscard]] static route_endpoint tunnel(std::pmr::memory_resource* resource,
        route_stream_handler::invoke_type invoke, std::string_view protocol, http_tunnel_route_config config) {
        // Extended CONNECT protocols use HTTP/2; connect-udp also admits
        // the HTTP/1.1 Upgrade form.
        if (!protocol.empty() && !is_valid_http_header_name(protocol)) {
            throw std::invalid_argument("CONNECT protocol must be an HTTP token");
        }
        if (protocol == "connect-udp") {
            config.datagrams_ = true;
        }
        if (config.datagrams_ && protocol.empty()) {
            throw std::invalid_argument("HTTP Datagrams require an upgrade-protocol tunnel route");
        }
        if (config.peer_transport_fin_timeout_ <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("CONNECT peer transport FIN timeout must be greater than zero");
        }
        return route_endpoint(
            tunnel_route_endpoint(pmr_resource_or_default(resource), invoke, protocol, config));
    }

    [[nodiscard]] const tunnel_route_endpoint* tunnel() const& noexcept {
        return std::get_if<tunnel_route_endpoint>(&value_);
    }
    const tunnel_route_endpoint* tunnel() const&& = delete;

    [[nodiscard]] const buffered_route_endpoint* buffered() const& noexcept {
        return std::get_if<buffered_route_endpoint>(&value_);
    }
    [[nodiscard]] const buffered_route_endpoint* buffered() const&& = delete;

    [[nodiscard]] const response_stream_route_endpoint* response_stream() const& noexcept {
        return std::get_if<response_stream_route_endpoint>(&value_);
    }
    [[nodiscard]] const response_stream_route_endpoint* response_stream() const&& = delete;

    [[nodiscard]] const websocket_route_endpoint* websocket() const& noexcept {
        return std::get_if<websocket_route_endpoint>(&value_);
    }
    [[nodiscard]] const websocket_route_endpoint* websocket() const&& = delete;

    [[nodiscard]] ruvia::detail::request_body_mode request_body_mode() const noexcept {
        if (const auto* endpoint = buffered()) {
            return endpoint->request_body_mode();
        }
        if (const auto* endpoint = response_stream()) {
            return endpoint->request_body_mode();
        }
        return request_body_mode::buffered;
    }

    // The streaming handler thunk, or null for a buffered-response endpoint.
    [[nodiscard]] route_stream_handler::invoke_type stream_invoke() const noexcept {
        if (const auto* endpoint = response_stream()) {
            return endpoint->invoke();
        }
        if (const auto* endpoint = websocket()) {
            return endpoint->invoke();
        }
        if (const auto* endpoint = tunnel()) {
            return endpoint->invoke();
        }
        return nullptr;
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
