#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/router.h"
#include "ruvia/web/websocket.h"

#include "context/context_access.h"
#include "http/http_websocket_connection.h"
#include "http/static_root_index.h"
#include "server/http_server_options.h"
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

// Lightweight, self-contained route-resolution result types. A materialized
// resolution is exactly one of resolved, method-not-allowed, not-found, or
// rejected; the payload for another alternative is not observable.

namespace ruvia::detail {

inline constexpr std::size_t max_route_params = 16;
class route_entry;

class route_match final {
public:
    constexpr route_match() noexcept = default;

    constexpr route_match(const route_match& other) noexcept
        : param_count_(other.param_count_) {
        for (std::size_t i = 0; i < param_count_; ++i) {
            param_values_[i] = other.param_values_[i];
        }
    }

    constexpr route_match& operator=(const route_match& other) noexcept {
        param_count_ = other.param_count_;
        for (std::size_t i = 0; i < param_count_; ++i) {
            param_values_[i] = other.param_values_[i];
        }
        return *this;
    }

    [[nodiscard]] std::span<const std::string_view> values() const& noexcept {
        return std::span<const std::string_view>(param_values_.data(), param_count_);
    }
    [[nodiscard]] std::span<const std::string_view> values() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return param_count_;
    }

    void clear() noexcept {
        param_count_ = 0;
    }

    void truncate(std::size_t count) noexcept {
        param_count_ = count <= param_count_ ? count : param_count_;
    }

    [[nodiscard]] bool add(std::string_view value) noexcept {
        if (param_count_ >= param_values_.size()) {
            return false;
        }

        param_values_[param_count_++] = value;
        return true;
    }

private:
    std::array<std::string_view, max_route_params> param_values_{};
    std::size_t param_count_{0};
};

class route_not_found final {
private:
    friend class route_resolution;
    constexpr route_not_found() noexcept = default;
};

// A target that names no resource at all (rootless or dot-segment path): no
// route and no document-root file may answer it, only the not-found path.
class route_rejected final {
private:
    friend class route_resolution;
    constexpr route_rejected() noexcept = default;
};

class route_method_not_allowed final {
public:
    [[nodiscard]] constexpr std::uint32_t allowed_methods() const noexcept {
        return allowed_methods_;
    }
    // The distinct extension tokens registered on the resource, for Allow.
    // Borrowed from the compiled route plan.
    [[nodiscard]] constexpr std::span<const std::string_view> extension_methods() const noexcept {
        return extension_methods_;
    }

private:
    friend class route_resolution;

    constexpr route_method_not_allowed(
        std::uint32_t allowed_methods, std::span<const std::string_view> extension_methods) noexcept
        : allowed_methods_(allowed_methods),
          extension_methods_(extension_methods) {}

    std::uint32_t allowed_methods_;
    std::span<const std::string_view> extension_methods_;
};

class resolved_route final {
public:
    // Process-invariant route data from the compiled plan.
    [[nodiscard]] const route_entry& route() const noexcept {
        return *route_;
    }

    // The resolving worker's handler. handler() requires a buffered-response
    // endpoint; stream_handler() a response-stream, websocket or tunnel one.
    [[nodiscard]] route_handler handler() const noexcept;
    [[nodiscard]] route_stream_handler stream_handler() const noexcept;

    // Static routes carry an empty match; dynamic routes carry their captured
    // values in the same value object. Callers never need a nullable match side
    // channel.
    [[nodiscard]] const route_match& match() const& noexcept {
        return match_;
    }
    [[nodiscard]] const route_match& match() const&& = delete;

private:
    friend class route_resolution;

    resolved_route(const route_entry& route, void* handler_target) noexcept
        : route_(&route),
          handler_target_(handler_target) {}

    resolved_route(const route_entry& route, void* handler_target, const route_match& match) noexcept
        : route_(&route),
          handler_target_(handler_target),
          match_(match) {}

    const route_entry* route_;
    void* handler_target_;
    route_match match_;
};

class route_resolution final {
public:
    route_resolution() noexcept
        : value_(route_not_found{}) {}

    [[nodiscard]] static route_resolution resolved(
        const route_entry& route, void* handler_target) noexcept {
        return route_resolution(resolved_route(route, handler_target));
    }

    [[nodiscard]] static route_resolution resolved(
        const route_entry& route, void* handler_target, const route_match& match) noexcept {
        return route_resolution(resolved_route(route, handler_target, match));
    }

    // The mask spans only classified methods, so it can be empty for a resource
    // that has extension-method routes. A non-empty `extension_methods` alone
    // still makes the resource exist, so an extension-only path answers 405
    // with an Allow built from its tokens instead of collapsing to 404.
    [[nodiscard]] static route_resolution method_not_allowed(
        std::uint32_t allowed_methods, std::span<const std::string_view> extension_methods) noexcept {
        if (allowed_methods == 0 && extension_methods.empty()) {
            return route_resolution();
        }
        return route_resolution(route_method_not_allowed(allowed_methods, extension_methods));
    }

    [[nodiscard]] static route_resolution reject() noexcept {
        return route_resolution(route_rejected{});
    }

    [[nodiscard]] const resolved_route* resolved() const& noexcept {
        return std::get_if<resolved_route>(&value_);
    }
    [[nodiscard]] const resolved_route* resolved() const&& = delete;

    [[nodiscard]] const route_method_not_allowed* method_not_allowed() const& noexcept {
        return std::get_if<route_method_not_allowed>(&value_);
    }
    [[nodiscard]] const route_method_not_allowed* method_not_allowed() const&& = delete;

    [[nodiscard]] const route_rejected* rejected() const& noexcept {
        return std::get_if<route_rejected>(&value_);
    }
    [[nodiscard]] const route_rejected* rejected() const&& = delete;

private:
    using value_type = std::variant<route_not_found, route_method_not_allowed, resolved_route, route_rejected>;

    template <typename alternative_type>
    explicit route_resolution(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class route_table;

struct middleware_chain final {
    using terminal_type = callback_ref<task<void>(context&)>;
    using failure_type = task<void> (*)(const route_table&, context&, std::exception_ptr);

    const route_table* table_{nullptr};
    std::span<const route_middleware> frames_;
    std::span<const std::string_view> scopes_;
    terminal_type terminal_;
    failure_type failure_{nullptr};
};

[[nodiscard]] task<void> run_chain(const middleware_chain& chain, context& context_value);
struct route_request_view final {
    http_known_method known_method_{http_known_method::unknown};
    std::string_view method_{};
    std::string_view path_{};
    std::string_view connect_target_{};
    std::string_view extended_protocol_{};
};

class controller_store;
class router;

// The framework's own answer to a failed request, consulting no application
// error handler: the status an http_error or protocol failure carries, else a
// generic 500 that names nothing internal. An inbound transport failure
// propagates instead.
struct default_exception_response final {
    http_response response_;
    // The failure maps to no HTTP error: a fault for the caller to report.
    bool unexpected_;
};
[[nodiscard]] default_exception_response make_default_exception_response(const std::exception_ptr& exception);

// One worker's binding of the process route plan: the worker's controller
// instance behind every route and the worker's middleware instance behind
// every frame, indexed exactly like the plan. Everything else -- lookup,
// endpoint metadata, limits, fallbacks -- is read from the shared plan.
class route_table final {
public:
    // The plan and the controllers must outlive the table. Middleware
    // instances are created here and destroyed with the table.
    route_table(const compiled_route_plan& plan, const controller_store& controllers);
    route_table(const compiled_route_plan& plan, std::span<void* const> targets);
    ~route_table();
    route_table(const route_table&) = delete;
    route_table& operator=(const route_table&) = delete;
    route_table(route_table&&) = delete;
    route_table& operator=(route_table&&) = delete;
    // Owner thread, before this worker admits its first routed listener.
    // Both plans and controller stores retain their existing lifetimes.
    void swap(route_table& other) noexcept;

    [[nodiscard]] bool has_route_rate_limit() const noexcept {
        return plan_->has_route_rate_limit_;
    }
    // Builds a request path from a registered route pattern: ":name" segments
    // take the next value (percent-encoded, non-empty), a trailing "*" takes
    // the final value (slashes preserved, may be empty). The pattern is the
    // route's identity -- an unregistered pattern, a value-count mismatch, a
    // "."/".." value, or a pattern starting with an empty segment (a "//"
    // network-path reference) is a programming error and throws
    // std::invalid_argument. A root "*" value's leading slash becomes "%2F".
    [[nodiscard]] std::string url_for(std::string_view pattern, std::span<const std::string_view> values) const;
    [[nodiscard]] route_resolution resolve(const http_request& request) const noexcept;
    [[nodiscard]] route_resolution resolve(route_request_view request) const noexcept;

    // Whether ANY route in the table uses this exact token. RFC 9110 15.5.6
    // makes 405 conditional on the method being "known by the origin server",
    // so a token nobody registered is 501 no matter what the target path holds.
    [[nodiscard]] bool recognizes_method_token(std::string_view method_token) const noexcept;
    // Canonical buffered-response application dispatch for unresolved requests
    // and callers that need it to own the route context. An unresolved route
    // first consults the configured document root, then falls through to
    // 404/405/OPTIONS handling. Any failure escaping the routing machinery
    // becomes an error response, except an inbound transport failure, which
    // propagates. Connection persistence and wire framing remain the HTTP/1
    // driver's responsibility. Pass document_root_binding::none() when no root
    // is configured, or the worker's configured(generation) lease otherwise.
    // The caller owns response and keeps it alive until dispatch completes;
    // successful completion assigns the selected final response directly.
    task<void> dispatch_buffered_response(const http_request& request,
        const route_resolution& resolution, document_root_binding document_root,
        const context_services& services, static_file_selection_mode static_file_mode,
        http_response& response) const;
    // The one matched-route invocation every transport awaits: builds the
    // context in this frame, runs the route chain, applies the response policy
    // for routes without middleware, finalizes a failure, and falls back to the
    // request-only handler when the context cannot be built or finalization
    // fails. `body_failure`, when given, is a request-body failure the transport
    // records outside the handler's reads (HTTP/2 DATA): a handler that returns
    // without surfacing it gets that failure instead. Returns whether the
    // response accounts for that failure. The context dies before this
    // returns, so body completion and the request arena outlive it.
    [[nodiscard]] task<bool> dispatch_resolved_route(const http_request& request,
        const resolved_route& resolved, const context_services& services,
        const std::optional<http_request_body_failure>* body_failure, http_response& response) const;
    task<http_response> handle_error(const http_request& request,
        http_error_info error, const context_services& services) const;
    task<http_response> handle_exception(const http_request& request,
        std::exception_ptr exception, const context_services& services) const;
    // Absence means the bound output handled the request; a value is the one
    // buffered response produced before a response-stream commit.
    task<std::optional<http_response>> dispatch_response_stream(const http_request& request,
        const resolved_route& route, response_stream_writer& response_stream,
        const context_services& services) const;
    // A CONNECT tunnel or WebSocket upgrade: `handler` is the terminal that
    // commits the upgrade. Absence means the terminal took the request.
    task<std::optional<http_response>> dispatch_upgrade(const http_request& request,
        const resolved_route& route, const route_stream_handler& handler,
        const context_services& services) const;
    // The not-found handler that applies to `path`: the tightest prefix scope
    // covering it, else the plan-wide one; null when neither is configured.
    [[nodiscard]] http_not_found_handler_ref not_found_handler_for(std::string_view path) const noexcept;
    // Reuses the transport's already-bound context and response writer.
    task<std::optional<http_response>> dispatch_bound_stream(const resolved_route& resolved,
        context& context_value, route_stream_handler handler) const;
    task<http_response> dispatch_bound_buffered(const resolved_route& resolved, context& context_value,
        const std::optional<http_request_body_failure>* body_failure = nullptr) const;

private:
    friend class router;

    [[nodiscard]] task<http_response> invoke_route(const resolved_route& resolved, context& context) const;
    // Failed buffered/stream invocations preserve only the explicit policy;
    // discarded representation fields never enter the replacement error.
    // If finalization fails, destroy the context before the request-only fallback.
    [[nodiscard]] task<http_response> finalize_failure(context& context, std::exception_ptr exception) const;

    [[nodiscard]] route_resolution resolve(
        http_known_method method, std::string_view path) const noexcept;
    [[nodiscard]] route_resolution resolve_connect(std::string_view protocol, std::string_view target) const noexcept;
    // Extension-method routing, kept off every enum-indexed structure. The
    // request's exact token is compared against a small cold list, which costs
    // a known-method request nothing: the HTTP/1 driver only reaches it when
    // classify_http_method() returned unknown.
    [[nodiscard]] route_resolution resolve_extension_method(
        std::string_view method_token, std::string_view path) const noexcept;
    // Distinct tokens of the extension routes on `path`, precomputed in the
    // plan; empty when none is registered there.
    [[nodiscard]] std::span<const std::string_view> extension_methods_for(std::string_view path) const noexcept;
    [[nodiscard]] route_resolution resolved(const route_entry& route) const noexcept;
    [[nodiscard]] route_resolution resolved(const route_entry& route, const route_match& match) const noexcept;

    static constexpr std::size_t routable_method_count = compiled_route_plan::routable_method_count;
    static constexpr std::size_t no_route_index = compiled_route_plan::no_route_index;
    using dynamic_node_type = compiled_route_plan::dynamic_node_type;
    using dynamic_static_child_type = compiled_route_plan::dynamic_static_child_type;

    [[nodiscard]] static std::size_t method_index(http_known_method method) noexcept;
    [[nodiscard]] static bool is_routable_method(http_known_method method) noexcept;
    [[nodiscard]] static bool supports_head_fallback(const route_entry& route) noexcept;
    [[nodiscard]] static std::uint64_t path_hash(std::string_view path) noexcept;
    [[nodiscard]] static std::uint64_t route_hash(
        http_known_method method, std::uint64_t hash) noexcept;
    [[nodiscard]] static std::size_t find_dynamic_node(
        const dynamic_node_type& node, std::string_view path, route_match& match) noexcept;
    [[nodiscard]] static std::size_t find_dynamic_node_no_params(
        const dynamic_node_type& node, std::string_view path) noexcept;
    [[nodiscard]] static const dynamic_static_child_type* find_dynamic_static_child(
        const dynamic_node_type& node, std::string_view segment) noexcept;

    [[nodiscard]] const route_entry* find_static_route(
        http_known_method method, std::string_view path, std::uint64_t hash) const noexcept;
    [[nodiscard]] const route_entry* find_dynamic_route(
        http_known_method method, std::string_view path, route_match& match) const noexcept;
    [[nodiscard]] std::uint32_t allowed_methods(std::string_view path, std::uint64_t hash,
        std::uint32_t excluded_mask = 0, std::uint32_t initial_mask = 0) const noexcept;
    // The handler is copied into the coroutine frame: callers may pass one
    // bound from the resolved route as a temporary.
    [[nodiscard]] task<std::optional<http_response>> dispatch_stream_route(const http_request& request,
        const resolved_route& route, route_stream_handler handler,
        const context_services& services, response_stream_writer* response_stream = nullptr) const;

    // A route's own frames and, only when some are conditional, their scopes.
    [[nodiscard]] middleware_chain route_chain(const route_entry& route) const noexcept;
    // Runs `frames` around `terminal`, whose response becomes the result; the
    // completion check shared by matched buffered routes and the unmatched
    // chain.
    [[nodiscard]] task<http_response> respond_through_chain(
        middleware_chain chain, context& context, route_handler terminal) const;
    // Runs the unmatched-request middleware chain around `terminal`, which
    // produces the 404/405/501 response. Falls straight through to the terminal
    // when nothing declared itself for unmatched requests.
    [[nodiscard]] task<http_response> run_unmatched_chain(context& context, route_handler terminal) const;
    [[nodiscard]] static task<void> contain_failure(
        const route_table& table, context& context, std::exception_ptr exception);
    [[nodiscard]] task<http_response> handle_not_found(
        const http_request& request, const context_services& services) const;
    [[nodiscard]] task<http_response> handle_exception(
        context& context, std::exception_ptr exception) const;
    [[nodiscard]] task<http_response> handle_exception(context& context, std::exception_ptr exception,
        http_error_handler_ref error_handler) const;
    [[nodiscard]] task<http_response> handle_exception(const http_request& request,
        std::exception_ptr exception, const context_services& services,
        http_error_handler_ref error_handler) const;

    [[nodiscard]] http_error_handler_ref error_handler_for(std::string_view path) const noexcept;
    explicit route_table(const compiled_route_plan& plan);
    void build_middleware_instances();

    const compiled_route_plan* plan_;
    // Worker-bound, indexed like the plan: the controller instance behind each
    // route, the middleware instances this table owns, and each frame bound to
    // its instance.
    std::vector<void*> handler_targets_;
    std::vector<void*> middleware_instances_;
    std::vector<route_middleware> middleware_frames_;
};

}  // namespace ruvia::detail

namespace ruvia {

[[nodiscard]] inline context make_route_context(const http_request& request,
    const detail::resolved_route& resolved, const detail::context_services& services) {
    const auto& route = resolved.route();
    const auto values = resolved.match().values();
    return detail::context_access::make(request, route.path(), route.param_names().data(),
        values.data(), values.size(), reinterpret_cast<std::uintptr_t>(&route), services);
}

}  // namespace ruvia

namespace ruvia::detail {

class app_configuration;

// Builds the process route plan once per run: every controller instance in
// `controllers` (one per registrar, registrar order) declares its routes, then
// `configuration` applies the app-wide fallbacks and middleware. Route
// conflicts and invalid registrations throw here, before any worker starts.
[[nodiscard]] compiled_route_plan_ptr_type compile_route_plan(
    std::span<const controller_registrar> registrars, const controller_store& controllers,
    const app_configuration& configuration);

// Startup builder shared by controller declarations and owned public routes.
class router final {
public:
    router();
    ~router();

    router(const router&) = delete;
    router& operator=(const router&) = delete;
    router(router&&) = delete;
    router& operator=(router&&) = delete;

    void set_error_handler(http_error_handler_ref handler) noexcept;
    void set_not_found_handler(http_not_found_handler_ref handler) noexcept;
    // Copies normalized, de-duplicated prefixes into the plan, deepest first.
    // Callback views borrow their app_configuration owners until the plan retires.
    void set_prefix_error_handlers(std::span<const std::pair<std::string, callback_registration<http_error_handler>>> handlers);
    void set_prefix_not_found_handlers(std::span<const std::pair<std::string, callback_registration<http_not_found_handler>>> handlers);
    // Application-wide middleware, prepended to every route's chain. Each
    // descriptor becomes exactly one instance per worker, serving all routes.
    void set_global_middlewares(std::span<const controller_middleware_descriptor> descriptors);

    // Runs every registrar against its controller instance, in slot order.
    void declare_controllers(
        std::span<const controller_registrar> registrars, const controller_store& controllers);
    // The single registration path. `handler_target` must be the instance of
    // the controller currently declaring its routes. `method_token` is non-empty
    // only for an extension method, with `method` unknown.
    void register_endpoint(http_known_method method, std::string_view method_token, std::string_view path,
        route_endpoint endpoint, void* handler_target,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);
    // Explicit, owned handler slots use the same registration and compilation
    // path without a controller declaration in progress.
    void register_owned_endpoint(std::size_t slot, http_known_method method, std::string_view method_token,
        std::string_view path, route_endpoint endpoint,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);

    [[nodiscard]] compiled_route_plan_ptr_type compile();

private:
    // A route's own (controller + route) middleware descriptors, as a slice of
    // route_middleware_descriptors_. Parallel to the plan's routes.
    struct route_middleware_range_type final {
        std::size_t offset_{0};
        std::size_t count_{0};
    };

    void validate_route_target(http_known_method method, std::string_view method_token, std::string_view path,
        const route_endpoint& endpoint) const;
    void validate_no_dynamic_route_conflict() const;
    [[nodiscard]] std::size_t append_route_middlewares(
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);
    void build_middleware_frames();
    void build_static_index();
    void build_static_method_mask();
    void build_extension_index();
    void build_dynamic_routes();
    void append_dynamic_param_name(route_entry& route, std::string_view name);
    void insert_dynamic(compiled_route_plan::dynamic_node_type& root, route_entry& route);
    void build_url_patterns();

    [[nodiscard]] static bool is_dynamic_path(std::string_view path) noexcept;

    compiled_route_plan_ptr_type plan_;
    std::vector<std::uint64_t> route_hashes_;
    std::vector<controller_middleware_descriptor> route_middleware_descriptors_;
    std::vector<route_middleware_range_type> route_middleware_ranges_;
    std::vector<controller_middleware_descriptor> global_middleware_descriptors_;
    std::size_t controller_slot_{0};
    void* controller_target_{nullptr};
};

}  // namespace ruvia::detail
