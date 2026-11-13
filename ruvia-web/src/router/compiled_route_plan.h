#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/detail/callback_ref.h"
#include "ruvia/web/detail/middleware_registration.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/websocket.h"

#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

class router;

// What a registered route runs, as one closed set of alternatives. Each
// alternative carries the handler's invoke thunk together with the route
// metadata only that shape may have, so a route cannot claim a streaming or
// websocket mode while holding a buffered handler. The handler's target is
// worker-bound and lives in each worker's route_table, never here.

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
    tunnel_route_endpoint(route_stream_handler::invoke_type invoke,
        std::string_view protocol, http_tunnel_route_config config)
        : invoke_(invoke),
          protocol_(protocol),
          config_(config) {}
    route_stream_handler::invoke_type invoke_;
    std::string protocol_;
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
    [[nodiscard]] protocol_byte_limit message_limit(std::size_t server_limit) const {
        const auto local = max_message_bytes_.maximum();
        return protocol_byte_limit::limited(local && *local < server_limit ? *local : server_limit);
    }

private:
    friend class route_endpoint;

    websocket_route_endpoint(route_stream_handler::invoke_type invoke,
        const websocket_route_config& options)
        : invoke_(invoke),
          require_subprotocol_(options.require_subprotocol_),
          lifecycle_(options.lifecycle_),
          deflate_(options.deflate_),
          max_message_bytes_(options.max_message_bytes_) {
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
    std::vector<char> subprotocol_storage_;
    std::vector<std::string_view> subprotocols_;
    bool require_subprotocol_;
    websocket_lifecycle_options lifecycle_;
    websocket_deflate_config deflate_;
    protocol_byte_limit max_message_bytes_;
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

    [[nodiscard]] static route_endpoint websocket(route_stream_handler::invoke_type invoke, websocket_route_config options) {
        options.lifecycle_ = normalize_websocket_lifecycle_options(options.lifecycle_);
        validate_websocket_deflater_config(options.deflate_.deflater_);
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
            websocket_route_endpoint(invoke, options));
    }

    [[nodiscard]] static http_tunnel_route_config validate_tunnel_config(
        bool upgrade_protocol, bool udp, http_tunnel_route_config config) {
        if (udp) {
            config.datagrams_ = true;
        }
        if (config.datagrams_ && !upgrade_protocol) {
            throw std::invalid_argument("HTTP Datagrams require an upgrade-protocol tunnel route");
        }
        if (config.peer_transport_fin_timeout_.value() <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("CONNECT peer transport FIN timeout must be greater than zero");
        }
        return config;
    }

    [[nodiscard]] static route_endpoint tunnel(route_stream_handler::invoke_type invoke, std::string_view protocol, http_tunnel_route_config config) {
        if (!protocol.empty() && !is_valid_http_header_name(protocol)) {
            throw std::invalid_argument("CONNECT protocol must be an HTTP token");
        }
        config = validate_tunnel_config(!protocol.empty(), protocol == "connect-udp", config);
        return route_endpoint(
            tunnel_route_endpoint(invoke, protocol, config));
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

class route_entry final {
public:
    struct init_type final {
        std::size_t index_{0};
        // Registration-order slot of the controller whose instance is the
        // handler target on every worker.
        std::size_t controller_{0};
        http_known_method method_;
        // Non-empty only for an extension-method route, where it is the exact,
        // case-sensitive wire token (RFC 9110 9.1) and `method` is unknown.
        // Known methods keep the enum as their identity so the routing fast
        // path never compares strings.
        std::string_view method_token_{};
        std::string_view path_;
        route_endpoint endpoint_;
        bool dynamic_{false};
        // 0 = no route-declared ceiling; the server default applies.
        std::size_t max_request_body_bytes_{0};
    };

    route_entry(init_type init)
        : index_(init.index_),
          controller_(init.controller_),
          method_(init.method_),
          method_token_(init.method_token_),
          path_(init.path_),
          endpoint_(std::move(init.endpoint_)),
          dynamic_(init.dynamic_),
          max_request_body_bytes_(init.max_request_body_bytes_) {}
    route_entry(const route_entry&) = delete;
    route_entry& operator=(const route_entry&) = delete;
    route_entry(route_entry&&) noexcept = default;
    route_entry& operator=(route_entry&&) = delete;

    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }

    [[nodiscard]] std::size_t controller() const noexcept {
        return controller_;
    }

    [[nodiscard]] http_known_method method() const noexcept {
        return method_;
    }

    // Empty unless this is an extension-method route.
    [[nodiscard]] std::string_view method_token() const noexcept {
        return method_token_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] const route_endpoint& endpoint() const noexcept {
        return endpoint_;
    }

    [[nodiscard]] bool dynamic() const noexcept {
        return dynamic_;
    }

    // A ceiling declared by one of this route's middlewares, or 0 for none.
    // Read before the body is accepted, so it bounds what is buffered rather
    // than what a handler later sees.
    [[nodiscard]] std::size_t max_request_body_bytes() const noexcept {
        return max_request_body_bytes_;
    }

    [[nodiscard]] std::span<const std::string_view> param_names() const noexcept {
        return param_names_;
    }

    [[nodiscard]] std::size_t middleware_offset() const noexcept {
        return middleware_offset_;
    }

    [[nodiscard]] std::size_t middleware_count() const noexcept {
        return middleware_count_;
    }

    [[nodiscard]] bool has_middleware() const noexcept {
        return middleware_count_ != 0;
    }

    // True when at least one frame in this route's range is a path-scoped
    // middleware whose scope covers only part of the route's request paths;
    // dispatch then checks the request path before running such a frame.
    [[nodiscard]] bool has_conditional_middleware() const noexcept {
        return conditional_middleware_;
    }

private:
    friend class router;

    std::size_t index_;
    std::size_t controller_;
    http_known_method method_;
    std::string method_token_;
    std::string path_;
    route_endpoint endpoint_;
    bool dynamic_{false};
    std::size_t max_request_body_bytes_{0};
    std::span<const std::string_view> param_names_{};
    std::size_t middleware_offset_{0};
    std::size_t middleware_count_{0};
    bool conditional_middleware_{false};
};
class route_table;

// The one immutable route plan of a process run, compiled once at startup and
// shared by every worker: every route with its endpoint metadata, limits and
// parameter names, the middleware frame layout, the lookup indices, and the
// error/not-found fallbacks. Workers bind only their own handler targets and
// middleware instances to it (route_table), by route and frame index.
class compiled_route_plan final {
public:
    compiled_route_plan()
        : routes_(),
          middleware_instances_(),
          middleware_frame_instances_(),
          middleware_scopes_(),
          param_names_(),
          extension_route_indices_(),
          extension_paths_(),
          extension_methods_(),
          connect_route_indices_(),
          connect_protocols_(),
          static_slots_(),
          dynamic_node_arena_(),
          url_patterns_(),
          prefix_error_handlers_(),
          prefix_not_found_handlers_() {}

    compiled_route_plan(const compiled_route_plan&) = delete;
    compiled_route_plan& operator=(const compiled_route_plan&) = delete;
    compiled_route_plan(compiled_route_plan&&) = delete;
    compiled_route_plan& operator=(compiled_route_plan&&) = delete;

private:
    friend class route_table;
    friend class router;

    static constexpr std::size_t routable_method_count = 7;
    static constexpr std::size_t no_route_index = std::numeric_limits<std::size_t>::max();

    struct static_route_slot final {
        std::uint64_t hash_{};
        std::size_t route_index_{no_route_index};
    };

    struct extension_path_type final {
        std::string_view path_;
        std::span<const std::string_view> methods_;
    };

    struct dynamic_node_type;

    struct dynamic_static_child_type final {
        std::string_view segment_;
        dynamic_node_type* node_{nullptr};
    };

    struct dynamic_node_type final {
        dynamic_node_type()
            : static_children_() {}

        std::vector<dynamic_static_child_type> static_children_;
        dynamic_node_type* param_child_{nullptr};
        std::size_t route_index_{no_route_index};
        std::size_t wildcard_route_index_{no_route_index};
    };

    struct connect_protocol_index_type final {
        explicit connect_protocol_index_type(std::string_view value)
            : protocol_(value) {}
        std::string protocol_;
        dynamic_node_type root_;
    };

    // Normalized prefix, deepest first: selection is a first-match scan.
    template <typename handler_type>
    struct prefix_handler_type final {
        prefix_handler_type(std::string_view prefix_value,
            handler_type handler_value)
            : prefix_(prefix_value),
              handler_(handler_value) {}

        std::string prefix_;
        handler_type handler_{nullptr};
    };

    std::vector<route_entry> routes_;
    // One descriptor per middleware instance a worker materializes. Frames
    // name their instance by index, so an app-wide middleware is one instance
    // shared by every route of a worker.
    std::vector<controller_middleware_descriptor> middleware_instances_;
    std::vector<std::size_t> middleware_frame_instances_;
    // Parallel to middleware_frame_instances_: the path scope a conditional
    // frame checks at dispatch, or empty for an unconditional frame. Only
    // routes flagged has_conditional_middleware() read it. Views borrow the
    // registration-owned use_at prefixes, which outlive every plan.
    std::vector<std::string_view> middleware_scopes_;
    // A contiguous block at the end of the frames: the app-wide middlewares
    // that declared they also run when no route matched.
    std::size_t unmatched_middleware_offset_{0};
    std::size_t unmatched_middleware_count_{0};
    std::vector<std::string_view> param_names_;
    // Sorted by case-sensitive method token, then canonical path.
    std::vector<std::size_t> extension_route_indices_;
    // Sorted distinct canonical paths, naming every registered token for
    // 405/OPTIONS Allow in registration order. The spans slice
    // extension_methods_ and borrow the routes' own path and token storage.
    std::vector<extension_path_type> extension_paths_;
    std::vector<std::string_view> extension_methods_;
    std::vector<std::size_t> connect_route_indices_;
    std::vector<connect_protocol_index_type> connect_protocols_;
    std::vector<static_route_slot> static_slots_;
    std::array<dynamic_node_type, routable_method_count> dynamic_roots_;
    std::vector<dynamic_node_type> dynamic_node_arena_;
    // Distinct registered route patterns, sorted, so url_for finds its pattern
    // by binary search.
    std::vector<std::string_view> url_patterns_;
    http_error_handler_ref error_handler_{nullptr};
    http_not_found_handler_ref not_found_handler_{nullptr};
    std::vector<prefix_handler_type<http_error_handler_ref>> prefix_error_handlers_;
    std::vector<prefix_handler_type<http_not_found_handler_ref>> prefix_not_found_handlers_;
    std::uint32_t static_method_mask_{0};
    std::uint32_t dynamic_method_mask_{0};
    std::size_t static_slot_mask_{};
    bool has_route_rate_limit_{false};
};

using compiled_route_plan_ptr_type =
    std::unique_ptr<compiled_route_plan>;

}  // namespace ruvia::detail
