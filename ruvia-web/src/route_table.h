#pragma once

#if defined(RUVIA_ENABLE_HTTP3)
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/error.hpp>
#include <asio/fwd.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "blocking_pool.h"
#include "borrowed_text.h"
#include "event_loop.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "worker_ref.h"
#include "worker_wait.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3_connection.h"
#endif
#include "app.h"
#include "connection_lifecycle.h"
#include "context.h"
#include "context_access.h"
#include "controller.h"
#include "http1_server_response.h"
#include "http_client_pool.h"
#include "http_connect_udp.h"
#include "http_content_coding.h"
#include "http_content_negotiation.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"
#include "middleware.h"
#include "rate_limit.h"
#include "streaming.h"
#include "streaming_session.h"
#include "websocket_handshake.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "db_pool.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis_pool.h"
#endif

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

    buffered_route_endpoint(route_handler::invoke_type invoke, detail::request_body_mode request_body_mode_value) noexcept :
        invoke_(invoke),
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

    explicit response_stream_route_endpoint(route_stream_handler::invoke_type invoke, detail::request_body_mode request_body_mode_value = detail::request_body_mode::buffered) noexcept :
        invoke_(invoke),
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
    tunnel_route_endpoint(route_stream_handler::invoke_type invoke, std::string_view protocol, http_tunnel_route_config config) :
        invoke_(invoke),
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

    websocket_route_endpoint(route_stream_handler::invoke_type invoke, const websocket_route_config& options) :
        invoke_(invoke),
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
                subprotocol_storage_.insert(subprotocol_storage_.end(), subprotocol.begin(), subprotocol.end());
                subprotocols_.emplace_back(subprotocol_storage_.data() + offset, subprotocol.size());
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
    [[nodiscard]] static route_endpoint buffered(route_handler::invoke_type invoke, request_body_mode request_body_mode_value) {
        return route_endpoint(buffered_route_endpoint(invoke, request_body_mode_value));
    }

    [[nodiscard]] static route_endpoint response_stream(route_stream_handler::invoke_type invoke, request_body_mode request_body_mode_value = request_body_mode::buffered) {
        return route_endpoint(response_stream_route_endpoint(invoke, request_body_mode_value));
    }

    [[nodiscard]] static route_endpoint websocket(route_stream_handler::invoke_type invoke, websocket_route_config options) {
        options.lifecycle_ = normalize_websocket_lifecycle_options(options.lifecycle_);
        validate_websocket_deflater_config(options.deflate_.deflater_);
        ruvia::websocket_subprotocol_set subprotocols;
        for (const auto& subprotocol : options.subprotocols_) {
            if (!subprotocols.append(subprotocol)) {
                throw std::invalid_argument("websocket subprotocols must contain at most 64 unique HTTP tokens");
            }
        }
        if (options.require_subprotocol_ && options.subprotocols_.empty()) {
            throw std::invalid_argument("a websocket route requiring a subprotocol must list subprotocols");
        }
        return route_endpoint(websocket_route_endpoint(invoke, options));
    }

    [[nodiscard]] static http_tunnel_route_config validate_tunnel_config(bool upgrade_protocol, bool udp, http_tunnel_route_config config) {
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
        return route_endpoint(tunnel_route_endpoint(invoke, protocol, config));
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
    using value_type = std::variant<buffered_route_endpoint, response_stream_route_endpoint, websocket_route_endpoint, tunnel_route_endpoint>;

    template <typename endpoint_type>
    explicit route_endpoint(endpoint_type endpoint) noexcept :
        value_(std::move(endpoint)) {}

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

    route_entry(init_type init) :
        index_(init.index_),
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
    compiled_route_plan() :
        routes_(),
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
        dynamic_node_type() :
            static_children_() {}

        std::vector<dynamic_static_child_type> static_children_;
        dynamic_node_type* param_child_{nullptr};
        std::size_t route_index_{no_route_index};
        std::size_t wildcard_route_index_{no_route_index};
    };

    struct connect_protocol_index_type final {
        explicit connect_protocol_index_type(std::string_view value) :
            protocol_(value) {}
        std::string protocol_;
        dynamic_node_type root_;
    };

    // Normalized prefix, deepest first: selection is a first-match scan.
    template <typename handler_type>
    struct prefix_handler_type final {
        prefix_handler_type(std::string_view prefix_value, handler_type handler_value) :
            prefix_(prefix_value),
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

using compiled_route_plan_ptr_type = std::unique_ptr<compiled_route_plan>;

// Lightweight, self-contained route-resolution result types. A materialized
// resolution is exactly one of resolved, method-not-allowed, not-found, or
// rejected; the payload for another alternative is not observable.

inline constexpr std::size_t max_route_params = 16;
class route_entry;

class route_match final {
public:
    constexpr route_match() noexcept = default;

    constexpr route_match(const route_match& other) noexcept :
        param_count_(other.param_count_) {
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

    constexpr route_method_not_allowed(std::uint32_t allowed_methods, std::span<const std::string_view> extension_methods) noexcept :
        allowed_methods_(allowed_methods),
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

    resolved_route(const route_entry& route, void* handler_target) noexcept :
        route_(&route),
        handler_target_(handler_target) {}

    resolved_route(const route_entry& route, void* handler_target, const route_match& match) noexcept :
        route_(&route),
        handler_target_(handler_target),
        match_(match) {}

    const route_entry* route_;
    void* handler_target_;
    route_match match_;
};

class route_resolution final {
public:
    route_resolution() noexcept :
        value_(route_not_found{}) {}

    [[nodiscard]] static route_resolution resolved(const route_entry& route, void* handler_target) noexcept {
        return route_resolution(resolved_route(route, handler_target));
    }

    [[nodiscard]] static route_resolution resolved(const route_entry& route, void* handler_target, const route_match& match) noexcept {
        return route_resolution(resolved_route(route, handler_target, match));
    }

    // The mask spans only classified methods, so it can be empty for a resource
    // that has extension-method routes. A non-empty `extension_methods` alone
    // still makes the resource exist, so an extension-only path answers 405
    // with an Allow built from its tokens instead of collapsing to 404.
    [[nodiscard]] static route_resolution method_not_allowed(std::uint32_t allowed_methods, std::span<const std::string_view> extension_methods) noexcept {
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
    explicit route_resolution(alternative_type alternative) noexcept :
        value_(std::move(alternative)) {}

    value_type value_;
};

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
    task<void> dispatch_buffered_response(const http_request& request, const route_resolution& resolution, document_root_binding document_root, const context_services& services, static_file_selection_mode static_file_mode, http_response& response) const;
    // The one matched-route invocation every transport awaits: builds the
    // context in this frame, runs the route chain, applies the response policy
    // for routes without middleware, finalizes a failure, and falls back to the
    // request-only handler when the context cannot be built or finalization
    // fails. `body_failure`, when given, is a request-body failure the transport
    // records outside the handler's reads (HTTP/2 DATA): a handler that returns
    // without surfacing it gets that failure instead. Returns whether the
    // response accounts for that failure. The context dies before this
    // returns, so body completion and the request arena outlive it.
    [[nodiscard]] task<bool> dispatch_resolved_route(const http_request& request, const resolved_route& resolved, const context_services& services, const std::optional<http_request_body_failure>* body_failure, http_response& response) const;
    task<http_response> handle_error(const http_request& request, http_error_info error, const context_services& services) const;
    task<http_response> handle_exception(const http_request& request, std::exception_ptr exception, const context_services& services) const;
    // Absence means the bound output handled the request; a value is the one
    // buffered response produced before a response-stream commit.
    task<std::optional<http_response>> dispatch_response_stream(const http_request& request, const resolved_route& route, response_stream_writer& response_stream, const context_services& services) const;
    // A CONNECT tunnel or WebSocket upgrade: `handler` is the terminal that
    // commits the upgrade. Absence means the terminal took the request.
    task<std::optional<http_response>> dispatch_upgrade(const http_request& request, const resolved_route& route, const route_stream_handler& handler, const context_services& services) const;
    // The not-found handler that applies to `path`: the tightest prefix scope
    // covering it, else the plan-wide one; null when neither is configured.
    [[nodiscard]] http_not_found_handler_ref not_found_handler_for(std::string_view path) const noexcept;
    // Reuses the transport's already-bound context and response writer.
    task<std::optional<http_response>> dispatch_bound_stream(const resolved_route& resolved, context& context_value, route_stream_handler handler) const;
    task<http_response> dispatch_bound_buffered(const resolved_route& resolved, context& context_value, const std::optional<http_request_body_failure>* body_failure = nullptr) const;

private:
    friend class router;

    [[nodiscard]] task<http_response> invoke_route(const resolved_route& resolved, context& context) const;
    // Failed buffered/stream invocations preserve only the explicit policy;
    // discarded representation fields never enter the replacement error.
    // If finalization fails, destroy the context before the request-only fallback.
    [[nodiscard]] task<http_response> finalize_failure(context& context, std::exception_ptr exception) const;

    [[nodiscard]] route_resolution resolve(http_known_method method, std::string_view path) const noexcept;
    [[nodiscard]] route_resolution resolve_connect(std::string_view protocol, std::string_view target) const noexcept;
    // Extension-method routing, kept off every enum-indexed structure. The
    // request's exact token is compared against a small cold list, which costs
    // a known-method request nothing: the HTTP/1 driver only reaches it when
    // classify_http_method() returned unknown.
    [[nodiscard]] route_resolution resolve_extension_method(std::string_view method_token, std::string_view path) const noexcept;
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
    [[nodiscard]] static std::uint64_t route_hash(http_known_method method, std::uint64_t hash) noexcept;
    [[nodiscard]] static std::size_t find_dynamic_node(const dynamic_node_type& node, std::string_view path, route_match& match) noexcept;
    [[nodiscard]] static std::size_t find_dynamic_node_no_params(const dynamic_node_type& node, std::string_view path) noexcept;
    [[nodiscard]] static const dynamic_static_child_type* find_dynamic_static_child(const dynamic_node_type& node, std::string_view segment) noexcept;

    [[nodiscard]] const route_entry* find_static_route(http_known_method method, std::string_view path, std::uint64_t hash) const noexcept;
    [[nodiscard]] const route_entry* find_dynamic_route(http_known_method method, std::string_view path, route_match& match) const noexcept;
    [[nodiscard]] std::uint32_t allowed_methods(std::string_view path, std::uint64_t hash, std::uint32_t excluded_mask = 0, std::uint32_t initial_mask = 0) const noexcept;
    // The handler is copied into the coroutine frame: callers may pass one
    // bound from the resolved route as a temporary.
    [[nodiscard]] task<std::optional<http_response>> dispatch_stream_route(const http_request& request, const resolved_route& route, route_stream_handler handler, const context_services& services, response_stream_writer* response_stream = nullptr) const;

    // A route's own frames and, only when some are conditional, their scopes.
    [[nodiscard]] middleware_chain route_chain(const route_entry& route) const noexcept;
    // Runs `frames` around `terminal`, whose response becomes the result; the
    // completion check shared by matched buffered routes and the unmatched
    // chain.
    [[nodiscard]] task<http_response> respond_through_chain(middleware_chain chain, context& context, route_handler terminal) const;
    // Runs the unmatched-request middleware chain around `terminal`, which
    // produces the 404/405/501 response. Falls straight through to the terminal
    // when nothing declared itself for unmatched requests.
    [[nodiscard]] task<http_response> run_unmatched_chain(context& context, route_handler terminal) const;
    [[nodiscard]] static task<void> contain_failure(const route_table& table, context& context, std::exception_ptr exception);
    [[nodiscard]] task<http_response> handle_not_found(const http_request& request, const context_services& services) const;
    [[nodiscard]] task<http_response> handle_exception(context& context, std::exception_ptr exception) const;
    [[nodiscard]] task<http_response> handle_exception(context& context, std::exception_ptr exception, http_error_handler_ref error_handler) const;
    [[nodiscard]] task<http_response> handle_exception(const http_request& request, std::exception_ptr exception, const context_services& services, http_error_handler_ref error_handler) const;

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

[[nodiscard]] inline context make_route_context(const http_request& request, const detail::resolved_route& resolved, const detail::context_services& services) {
    const auto& route = resolved.route();
    const auto values = resolved.match().values();
    return detail::context_access::make(request, route.path(), route.param_names().data(), values.data(), values.size(), reinterpret_cast<std::uintptr_t>(&route), services);
}

}  // namespace ruvia

namespace ruvia::detail {

class app_configuration;

// Builds the process route plan once per run: every controller instance in
// `controllers` (one per registrar, registrar order) declares its routes, then
// `configuration` applies the app-wide fallbacks and middleware. Route
// conflicts and invalid registrations throw here, before any worker starts.
[[nodiscard]] compiled_route_plan_ptr_type compile_route_plan(std::span<const controller_registrar> registrars, const controller_store& controllers, const app_configuration& configuration);

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
    void declare_controllers(std::span<const controller_registrar> registrars, const controller_store& controllers);
    // The single registration path. `handler_target` must be the instance of
    // the controller currently declaring its routes. `method_token` is non-empty
    // only for an extension method, with `method` unknown.
    void register_endpoint(http_known_method method, std::string_view method_token, std::string_view path, route_endpoint endpoint, void* handler_target, std::span<const controller_middleware_descriptor> controller_middlewares, std::span<const controller_middleware_descriptor> route_middlewares);
    // Explicit, owned handler slots use the same registration and compilation
    // path without a controller declaration in progress.
    void register_owned_endpoint(std::size_t slot, http_known_method method, std::string_view method_token, std::string_view path, route_endpoint endpoint, std::span<const controller_middleware_descriptor> controller_middlewares, std::span<const controller_middleware_descriptor> route_middlewares);

    [[nodiscard]] compiled_route_plan_ptr_type compile();

private:
    // A route's own (controller + route) middleware descriptors, as a slice of
    // route_middleware_descriptors_. Parallel to the plan's routes.
    struct route_middleware_range_type final {
        std::size_t offset_{0};
        std::size_t count_{0};
    };

    void validate_route_target(http_known_method method, std::string_view method_token, std::string_view path, const route_endpoint& endpoint) const;
    void validate_no_dynamic_route_conflict() const;
    [[nodiscard]] std::size_t append_route_middlewares(std::span<const controller_middleware_descriptor> controller_middlewares, std::span<const controller_middleware_descriptor> route_middlewares);
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

#if defined(RUVIA_ENABLE_HTTP2)

namespace ruvia::detail {

// What an HTTP/2 session needs from the server that accepted the connection.
// context_services is copied per session; the remaining dependencies are stable
// server-owned borrows that cannot be absent or rebound.
class http2_sans_io_session_context final {
public:
    http2_sans_io_session_context(context_services services, const http_server_options& options, connection_scanner::entry& scanner_entry) :
        services_(std::move(services)),
        options_(options),
        scanner_entry_(scanner_entry) {}

    [[nodiscard]] const http_server_options& options() const& noexcept RUVIA_LIFETIMEBOUND {
        return options_;
    }
    const http_server_options& options() const&& = delete;

    [[nodiscard]] connection_scanner::entry& scanner_entry() const& noexcept RUVIA_LIFETIMEBOUND {
        return scanner_entry_;
    }
    connection_scanner::entry& scanner_entry() const&& = delete;

    [[nodiscard]] const context_services& services() const& noexcept RUVIA_LIFETIMEBOUND {
        return services_;
    }
    const context_services& services() const&& = delete;

private:
    context_services services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
};

// HTTP/2 supports exactly the two transports owned by the Web server. Keeping
// these concrete overloads out of headers prevents every server/test consumer
// from instantiating the complete session coroutine again. The result is
// notify when the peer ended its input cleanly, linger when the session ended
// with GOAWAY (a completed drain or a connection error), otherwise abort.
[[nodiscard]] task<http_connection_close> run_http2_sans_io_session(inbound_socket_stream& stream, const route_table& routes_value, http2_sans_io_session_context session_value, std::string_view initial_bytes = {});

[[nodiscard]] task<http_connection_close> run_http2_sans_io_session(server_tls_stream& stream, const route_table& routes_value, http2_sans_io_session_context session_value, std::string_view initial_bytes = {});

}  // namespace ruvia::detail

#endif

// Shared admission for HTTP/1 and in-process subrequests. The HTTP/1 session
// reads and frames messages; admission decides coding, client identity,
// expectations, rate limits, body ceilings and endpoint kind.

namespace ruvia::detail {

class http1_deferred_access_log;

// The deployment limits that shape admission. Servers build it from their
// options; a subrequest builds it from its parent (no rate limit and no
// target-origin check: the parent request already passed both).
struct http_request_admission_policy final {
    std::optional<std::size_t> max_stream_body_bytes_{};
    std::size_t max_buffered_body_bytes_{};
    bool apply_rate_limit_{true};
    // RFC 9110 §7.4: answer 421 when the connection cannot carry the target
    // (decide_http_target_origin against the services' transport facts).
    bool verify_target_origin_{true};
    [[nodiscard]] static http_request_admission_policy server(const http_server_options& options) noexcept;
};

struct http_request_admission_input final {
    const http_request& request_;
    const route_resolution& resolution_;
    // The parser or subrequest supplies the response-coding selection.
    http_response_coding_selection coding_;
    // Request arena for copied rejection diagnostics.
    // The protocol's Expect plan, when it defers the rejection to Web.
    const http_server_expectation_plan* expectation_{};
    // Start of the request for the access-log duration.
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
    // The arrival instant rate-limit decisions use, pinned by an owner that
    // replays requests at chosen instants (test_app); empty reads the steady
    // clock at each decision.
    std::optional<std::chrono::steady_clock::time_point> pinned_arrival_{};
};

// Effective request-body ceiling. A route may only tighten the deployment limit.
[[nodiscard]] protocol_byte_limit http_request_body_limit(const route_resolution& resolution, request_body_mode mode, const http_request_admission_policy& policy) noexcept;

// A server-layer rejection decided before any handler ran. The diagnostic is
// borrowed from static storage or the request arena.
class http_request_rejection final {
public:
    [[nodiscard]] static constexpr http_request_rejection error(http_error_info error) noexcept {
        return http_request_rejection(error, std::nullopt);
    }

    [[nodiscard]] static http_request_rejection rate_limited(rate_limit_rejection rejection) noexcept;

    [[nodiscard]] constexpr const http_error_info& error() const& noexcept RUVIA_LIFETIMEBOUND {
        return error_;
    }
    const http_error_info& error() const&& = delete;

    [[nodiscard]] constexpr const rate_limit_rejection* rate_limit() const& noexcept RUVIA_LIFETIMEBOUND {
        return rate_limit_ ? &*rate_limit_ : nullptr;
    }
    const rate_limit_rejection* rate_limit() const&& = delete;

private:
    constexpr http_request_rejection(http_error_info error, std::optional<rate_limit_rejection> rate_limit) noexcept :
        error_(error),
        rate_limit_(rate_limit) {}

    http_error_info error_;
    std::optional<rate_limit_rejection> rate_limit_;
};

// Answers a rejection through the application's error handler, adding the
// rate-limit headers when that is why the request was refused. The lazy task
// owns this snapshot; its diagnostic text still borrows static/request storage.
[[nodiscard]] task<http_response> respond_http_request_rejection(const route_table& routes, const http_request& request, http_request_rejection rejection, const context_services& services);

// Result of the synchronous first preparation pass. ready() is null only when
// a blocking-pool encode or the application error handler must be awaited
// through http_response_tail::finish_prepare().
class http_response_preparation final {
public:
    [[nodiscard]] const prepared_application_response* ready() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<prepared_application_response>(&state_);
    }
    const prepared_application_response* ready() const&& = delete;

    http_response_preparation(const http_response_preparation&) = default;
    http_response_preparation(http_response_preparation&&) = default;
    http_response_preparation& operator=(const http_response_preparation&) = delete;
    http_response_preparation& operator=(http_response_preparation&&) = delete;

private:
    friend class http_response_tail;

    struct deferred_type final {
        http_response_coding_policy policy_;
        buffered_response_recovery recovery_;
        // Without a saved recovery step, encode on the blocking pool first.
        std::optional<buffered_response_recovery_step> pending_step_;
    };

    explicit http_response_preparation(prepared_application_response ready) noexcept :
        state_(ready) {}
    explicit http_response_preparation(http_response_coding_policy policy) noexcept :
        state_(deferred_type{policy, {}, std::nullopt}) {}
    http_response_preparation(http_response_coding_policy policy, buffered_response_recovery recovery, buffered_response_recovery_step pending_step) noexcept :
        state_(deferred_type{policy, recovery, pending_step}) {}

    std::variant<prepared_application_response, deferred_type> state_;
};

// The post-response tail: final representation preparation (CORS,
// compression, 406/500 recovery through the application error handler) and
// the access-log record of the committed status.
class http_response_tail final {
public:
    http_response_tail(const http_request& request, http_response_coding_policy coding_policy, ip_address remote_address, ip_address client_address, std::chrono::steady_clock::time_point start) noexcept :
        request_(&request),
        coding_policy_(coding_policy),
        remote_address_(remote_address),
        client_address_(client_address),
        start_(start) {}

    [[nodiscard]] const http_response_coding_policy& coding_policy() const& noexcept RUVIA_LIFETIMEBOUND {
        return coding_policy_;
    }
    const http_response_coding_policy& coding_policy() const&& = delete;

    [[nodiscard]] request_io_metrics& io_metrics() const& noexcept RUVIA_LIFETIMEBOUND {
        return io_metrics_;
    }
    request_io_metrics& io_metrics() const&& = delete;

    // A generated terminal response must not inherit the negotiated coding
    // promise (and must not be transformed again).
    void disable_response_coding() noexcept {
        coding_policy_ = http_response_coding_policy::disabled();
    }
    void bind_addresses(ip_address remote_address, ip_address client_address) noexcept {
        remote_address_ = remote_address;
        client_address_ = client_address;
    }
    // The listener's Alt-Svc value (RFC 7838 §3), added to every prepared
    // response that does not carry its own; it borrows listener storage.
    void bind_alternative_service(std::string_view value) noexcept {
        alternative_service_ = value;
    }
    // The CORS policy the response streams and buffered responses of this
    // request apply; null when none does.
    [[nodiscard]] static const http_cors_policy* cors(const http_server_options& options) noexcept {
        return options.cors_.has_value() ? &*options.cors_ : nullptr;
    }

    // All arguments borrow the current request owner through completion. Only
    // an uncommitted response may enter. The common response (no blocking-pool
    // encode, no recovery) completes here without a coroutine frame.
    [[nodiscard]] http_response_preparation prepare(http_response& response, const http_server_options& options, const worker_ref& worker) const;

    // Completes a preparation whose ready() is null, with the arguments given
    // to prepare(). The application error handler runs at most once here.
    [[nodiscard]] task<prepared_application_response> finish_prepare(const http_response_preparation& preparation, http_response& response, const http_server_options& options, const route_table& routes, const context_services& services) const;

    [[nodiscard]] task<prepared_application_response> prepare_or_finish(http_response& response, const http_server_options& options, const worker_ref& worker, const route_table& routes, const context_services& services) const {
        const auto first = prepare(response, options, worker);
        if (const auto* ready = first.ready()) {
            co_return *ready;
        }
        co_return co_await finish_prepare(first, response, options, routes, services);
    }

    [[nodiscard]] task<prepared_application_response> prepare_or_finish(http_response& response, const http_server_options& options, const route_table& routes, const context_services& services) const {
        return prepare_or_finish(response, options, services.worker(), routes, services);
    }

    // Records a terminal outcome whose final status was committed. A single
    // null check when no access log is configured.
    void record_access(const access_log_sink& access_log, http_status_code status) const noexcept;

    // Keeps the record of a pipelined response held back for a coalesced
    // write whose head completes at output offset `head_end`; the log reports
    // it once that write commits the head. A no-op without an access log.
    void defer_access(const access_log_sink& access_log, http1_deferred_access_log& deferred, std::size_t head_end, http_status_code status) const;

private:
    const http_request* request_;
    http_response_coding_policy coding_policy_;
    ip_address remote_address_{};
    ip_address client_address_{};
    std::chrono::steady_clock::time_point start_;
    std::string_view alternative_service_;
    mutable request_io_metrics io_metrics_{};
};

enum class http_request_endpoint : std::uint8_t {
    // Rejections: answer rejection() through respond_http_request_rejection.
    rejection,
    // Admitted endpoints (resolved() is non-null for all but buffered).
    websocket,
    tunnel,
    response_stream,
    stream_body,
    buffered,
};

class http_request_admission final {
public:
    [[nodiscard]] http_request_endpoint endpoint() const noexcept {
        return endpoint_;
    }

    [[nodiscard]] const http_request_rejection* rejection() const& noexcept RUVIA_LIFETIMEBOUND {
        return rejection_ ? &*rejection_ : nullptr;
    }
    const http_request_rejection* rejection() const&& = delete;

    // Null for unresolved requests (404/405/OPTIONS/document root).
    [[nodiscard]] const resolved_route* resolved() const& noexcept RUVIA_LIFETIMEBOUND {
        return resolved_;
    }
    const resolved_route* resolved() const&& = delete;

    // CONNECT-UDP (RFC 9298) tunnel; meaningful for endpoint() == tunnel.
    [[nodiscard]] bool udp_tunnel() const noexcept {
        return udp_tunnel_;
    }

    // The connection's request services after applying admission policy.
    [[nodiscard]] const context_services& services() const& noexcept RUVIA_LIFETIMEBOUND {
        return services_;
    }
    const context_services& services() const&& = delete;

    [[nodiscard]] protocol_byte_limit body_limit() const noexcept {
        return body_limit_;
    }

    [[nodiscard]] static_file_selection_mode static_file_mode() const noexcept {
        return services_.precompressed_static_files() ? static_file_selection_mode::precompressed : static_file_selection_mode::identity_only;
    }

    [[nodiscard]] http_response_tail& tail() & noexcept RUVIA_LIFETIMEBOUND {
        return tail_;
    }
    http_response_tail& tail() && = delete;

    http_request_admission(const http_request_admission& other) noexcept :
        tail_(other.tail_),
        services_(other.services_),
        rejection_(other.rejection_),
        resolved_(other.resolved_),
        body_limit_(other.body_limit_),
        endpoint_(other.endpoint_),
        udp_tunnel_(other.udp_tunnel_) {
        if (services_.io_metrics() == &other.tail_.io_metrics()) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }

    http_request_admission& operator=(const http_request_admission& other) noexcept {
        if (this != &other) {
            tail_ = other.tail_;
            services_ = other.services_;
            if (services_.io_metrics() == &other.tail_.io_metrics()) {
                services_.bind_io_metrics(&tail_.io_metrics());
            }
            rejection_ = other.rejection_;
            resolved_ = other.resolved_;
            body_limit_ = other.body_limit_;
            endpoint_ = other.endpoint_;
            udp_tunnel_ = other.udp_tunnel_;
        }
        return *this;
    }

    http_request_admission(http_request_admission&& other) noexcept :
        tail_(std::move(other.tail_)),
        services_(other.services_),
        rejection_(std::move(other.rejection_)),
        resolved_(other.resolved_),
        body_limit_(other.body_limit_),
        endpoint_(other.endpoint_),
        udp_tunnel_(other.udp_tunnel_) {
        if (services_.io_metrics() == &other.tail_.io_metrics()) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }

    http_request_admission& operator=(http_request_admission&& other) noexcept {
        if (this != &other) {
            tail_ = std::move(other.tail_);
            services_ = other.services_;
            if (services_.io_metrics() == &other.tail_.io_metrics()) {
                services_.bind_io_metrics(&tail_.io_metrics());
            }
            rejection_ = std::move(other.rejection_);
            resolved_ = other.resolved_;
            body_limit_ = other.body_limit_;
            endpoint_ = other.endpoint_;
            udp_tunnel_ = other.udp_tunnel_;
        }
        return *this;
    }

private:
    friend http_request_admission admit_http_request(const http_request_admission_input& input, const http_request_admission_policy& policy, const context_services& services);

    http_request_admission(http_response_tail tail, const context_services& services) noexcept :
        tail_(tail),
        services_(services) {
        if (services_.io_metrics() == nullptr) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }
    http_response_tail tail_;
    context_services services_;
    std::optional<http_request_rejection> rejection_;
    const resolved_route* resolved_{};
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http_request_endpoint endpoint_{http_request_endpoint::buffered};
    bool udp_tunnel_{false};
};

// Runs the admission policy once for one request. `services` are the
// connection's request services (stop token, interim output, trailers, ...).
// Admission borrows the input only during this synchronous call and owns its
// request-local service view; binding it never changes the connection's view.
[[nodiscard]] http_request_admission admit_http_request(const http_request_admission_input& input, const http_request_admission_policy& policy, const context_services& services);

// Admits a buffered in-memory request and dispatches it through the route
// table or the rejection handler.
[[nodiscard]] task<http_request_admission> admit_and_dispatch_buffered_request(const route_table& routes, const http_request_admission_input& input, const http_request_admission_policy& policy, const context_services& services, http_response& response);

}  // namespace ruvia::detail

namespace ruvia {
class context;  // only used as context* in a type-erased bind thunk; web supplies the definition
}

namespace ruvia::detail {

// A normal response writer may reject a tunnel but may never establish one.
// Establishment transfers ownership through the protocol's acceptance driver.
inline void validate_response_stream_status(const http_request& request, const http_response& response) {
    if (request.known_method() == http_known_method::connect && response.status().is_successful()) {
        throw std::logic_error("a 2xx response to CONNECT establishes the tunnel; use accept_tunnel()");
    }
    if (response.status() == http_status::switching_protocols) {
        throw std::logic_error("a 101 response requires accept_tunnel() or accept_websocket()");
    }
}

// Plain forwarding functions, not coroutines: each returns the sink's own task,
// so a streamed chunk costs exactly the sink's frame and no wrapper frame.
template <typename sink>
task<stream_commit_result> response_stream_commit_thunk(void* target) {
    return static_cast<sink*>(target)->commit();
}

template <typename sink>
task<void> response_stream_write_thunk(void* target, std::string_view chunk) {
    return static_cast<sink*>(target)->write(chunk);
}

template <typename sink>
task<void> response_stream_end_thunk(void* target, std::span<const http_header_view> trailers) {
    return static_cast<sink*>(target)->end(trailers);
}

template <typename sink>
task<timer_sleep_result> response_stream_sleep_thunk(void* target, std::chrono::milliseconds duration, const stop_token& stop_token_value) {
    return static_cast<sink*>(target)->sleep(duration, stop_token_value);
}

template <typename sink>
bool response_stream_aborted_thunk(void* target) noexcept {
    return static_cast<sink*>(target)->aborted();
}

template <typename sink>
void response_stream_bind_context_thunk(void* target, context* context_value) {
    static_cast<sink*>(target)->bind_context(context_value);
}

template <typename sink>
void response_stream_release_context_thunk(void* target) noexcept {
    static_cast<sink*>(target)->release_context();
}

template <typename sink>
bool response_stream_committed_thunk(void* target) noexcept {
    return static_cast<sink*>(target)->committed();
}

template <typename sink>
[[nodiscard]] response_stream_writer make_response_stream_writer(sink& sink_value) noexcept {
    return streaming_access::make_response_stream_writer(&sink_value, sink_value.output_interrupts(), &response_stream_commit_thunk<sink>, &response_stream_write_thunk<sink>, &response_stream_end_thunk<sink>, &response_stream_sleep_thunk<sink>, &response_stream_bind_context_thunk<sink>, &response_stream_release_context_thunk<sink>, &response_stream_committed_thunk<sink>, &response_stream_aborted_thunk<sink>);
}

class response_stream_completed final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class response_stream_dispatch_result;

    explicit constexpr response_stream_completed(http_status_code status) noexcept :
        status_(status) {}

    http_status_code status_;
};

class response_stream_aborted_before_commit final {
private:
    friend class response_stream_dispatch_result;
    constexpr response_stream_aborted_before_commit() noexcept = default;
};

class response_stream_aborted_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class response_stream_dispatch_result;
    explicit constexpr response_stream_aborted_after_commit(http_status_code status) noexcept :
        status_(status) {}
    http_status_code status_;
};

// The handler failed after its head was already on the wire. The status is what
// the peer was told; the exception is why it will never receive the rest. The
// status alone cannot be reported to anyone -- it says 200 -- so the exception
// travels with it to the transport, which is the last owner able to report it.
class response_stream_failed_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    // Never null.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }

private:
    friend class response_stream_dispatch_result;

    response_stream_failed_after_commit(http_status_code status, std::exception_ptr exception) noexcept :
        status_(status),
        exception_(std::move(exception)) {}

    http_status_code status_;
    std::exception_ptr exception_;
};

class response_stream_route_response final {
public:
    [[nodiscard]] http_response take_response() && noexcept {
        return std::move(response_);
    }

private:
    friend class response_stream_dispatch_result;

    explicit response_stream_route_response(http_response response) noexcept :
        response_(std::move(response)) {}

    http_response response_;
};

class response_stream_recovered_failure final {
public:
    [[nodiscard]] http_response take_response() && noexcept {
        return std::move(response_);
    }

private:
    friend class response_stream_dispatch_result;

    explicit response_stream_recovered_failure(http_response response) noexcept :
        response_(std::move(response)) {}

    http_response response_;
};

// Each route terminal is one alternative. Commit-bearing alternatives own the
// exact wire status; response-bearing alternatives own the movable response.
// No separate outcome discriminator is needed.
class response_stream_dispatch_result final {
public:
    [[nodiscard]] static response_stream_dispatch_result make_completed(http_status_code status) noexcept {
        return response_stream_dispatch_result(response_stream_completed(status));
    }

    [[nodiscard]] static response_stream_dispatch_result make_aborted_before_commit() noexcept {
        return response_stream_dispatch_result(response_stream_aborted_before_commit{});
    }

    [[nodiscard]] static response_stream_dispatch_result make_aborted_after_commit(http_status_code status) noexcept {
        return response_stream_dispatch_result(response_stream_aborted_after_commit(status));
    }

    [[nodiscard]] static response_stream_dispatch_result make_failed_after_commit(http_status_code status, std::exception_ptr exception) noexcept {
        return response_stream_dispatch_result(response_stream_failed_after_commit(status, std::move(exception)));
    }

    [[nodiscard]] static response_stream_dispatch_result make_route_response(http_response response) noexcept {
        return response_stream_dispatch_result(response_stream_route_response(std::move(response)));
    }

    [[nodiscard]] static response_stream_dispatch_result make_recovered_failure(http_response response) noexcept {
        return response_stream_dispatch_result(response_stream_recovered_failure(std::move(response)));
    }

    [[nodiscard]] const response_stream_completed* completed() const& noexcept {
        return std::get_if<response_stream_completed>(&value_);
    }
    const response_stream_completed* completed() const&& = delete;

    [[nodiscard]] const response_stream_aborted_before_commit* aborted_before_commit() const& noexcept {
        return std::get_if<response_stream_aborted_before_commit>(&value_);
    }
    const response_stream_aborted_before_commit* aborted_before_commit() const&& = delete;

    [[nodiscard]] const response_stream_aborted_after_commit* aborted_after_commit() const& noexcept {
        return std::get_if<response_stream_aborted_after_commit>(&value_);
    }
    const response_stream_aborted_after_commit* aborted_after_commit() const&& = delete;

    [[nodiscard]] const response_stream_failed_after_commit* failed_after_commit() const& noexcept {
        return std::get_if<response_stream_failed_after_commit>(&value_);
    }
    const response_stream_failed_after_commit* failed_after_commit() const&& = delete;

    [[nodiscard]] response_stream_route_response* route_response() & noexcept {
        return std::get_if<response_stream_route_response>(&value_);
    }
    response_stream_route_response* route_response() && = delete;

    [[nodiscard]] response_stream_recovered_failure* recovered_failure() & noexcept {
        return std::get_if<response_stream_recovered_failure>(&value_);
    }
    response_stream_recovered_failure* recovered_failure() && = delete;

    [[nodiscard]] std::optional<http_status_code> committed_status() const noexcept {
        if (const auto* value = completed()) {
            return value->status();
        }
        if (const auto* value = aborted_after_commit()) {
            return value->status();
        }
        if (const auto* value = failed_after_commit()) {
            return value->status();
        }
        return std::nullopt;
    }

private:
    using value_type = std::variant<response_stream_completed, response_stream_aborted_before_commit, response_stream_aborted_after_commit, response_stream_failed_after_commit, response_stream_route_response, response_stream_recovered_failure>;

    template <typename alternative_type>
    explicit response_stream_dispatch_result(alternative_type alternative) noexcept :
        value_(std::move(alternative)) {}

    value_type value_;
};

// Only called once sink_value.committed() holds.
template <typename sink>
[[nodiscard]] http_status_code committed_response_stream_status(const sink& sink_value) {
    return sink_value.commit_plan()->response_status();
}

struct response_stream_no_peer_abort final {
    [[nodiscard]] constexpr bool operator()() const noexcept {
        return false;
    }
};

// Drives a response-stream route over an already-constructed sink. Connection
// persistence remains a transport concern when a buffered response is returned.
template <typename sink, typename peer_aborted_type = response_stream_no_peer_abort>
task<response_stream_dispatch_result> dispatch_response_stream_with(sink& sink_value, const route_table& routes_value, const http_request& request, const resolved_route& route, const context_services& services, peer_aborted_type peer_aborted = {}) {
    auto response_stream = make_response_stream_writer(sink_value);

    std::exception_ptr exception;
    try {
        auto result_value = co_await routes_value.dispatch_response_stream(request, route, response_stream, services);
        if (services.connection_aborted() || services.request_reset() || peer_aborted()) {
            if (!sink_value.committed()) {
                co_return response_stream_dispatch_result::make_aborted_before_commit();
            }
            co_return response_stream_dispatch_result::make_aborted_after_commit(committed_response_stream_status(sink_value));
        }
        if (!result_value.has_value()) {
            if (!sink_value.committed()) {
                throw std::logic_error("handled response stream has no committed protocol plan");
            }
            co_return response_stream_dispatch_result::make_completed(committed_response_stream_status(sink_value));
        }
        if (sink_value.committed()) {
            throw std::logic_error("buffered stream result followed a committed response head");
        }
        co_return response_stream_dispatch_result::make_route_response(std::move(*result_value));
    } catch (const inbound_transport_error&) {
        if (!services.request_reset() && (services.connection_aborted() || peer_aborted())) {
            if (!sink_value.committed()) {
                co_return response_stream_dispatch_result::make_aborted_before_commit();
            }
            co_return response_stream_dispatch_result::make_aborted_after_commit(committed_response_stream_status(sink_value));
        }
        exception = std::current_exception();
    } catch (...) {
        exception = std::current_exception();
    }
    if (services.connection_aborted() || services.request_reset()) {
        auto result = sink_value.committed() ? response_stream_dispatch_result::make_aborted_after_commit(committed_response_stream_status(sink_value)) : response_stream_dispatch_result::make_aborted_before_commit();
        // The transport is already closed, but a real callback failure still
        // belongs to its configured outlet. Snapshot before invoking user code.
        const auto failure = services.connection_failure();
        const auto remote = services.remote_address();
        failure.invoke(remote, exception);
        co_return result;
    }

    if (sink_value.committed()) {
        // Past the point of no return: the status cannot be changed and no
        // error body can be appended, so the exception is handed to the
        // transport rather than dropped with the connection.
        co_return response_stream_dispatch_result::make_failed_after_commit(committed_response_stream_status(sink_value), std::move(exception));
    }
    auto response = co_await routes_value.handle_exception(request, exception, services);
    co_return response_stream_dispatch_result::make_recovered_failure(std::move(response));
}

// listener_dispatch::request_stream: what a request stream connection resolves
// every request to in place of the route table, and the one run of the
// application's request_stream callback every transport reaches through it.

// A request stream route runs no middleware and declares no body limit. A
// tunnel request -- CONNECT, extended CONNECT, or an HTTP/1.1 Upgrade without
// content -- resolves to an upgrade route whose callback commits the tunnel
// itself (accept_tunnel()); any other request to a response stream route with
// a streamed body.
[[nodiscard]] route_resolution resolve_request_stream_route(bool tunnel) noexcept;

// Whether `route` is the tunnel route resolve_request_stream_route() yields.
[[nodiscard]] bool is_request_stream_tunnel_route(const route_entry& route) noexcept;

// Whether an HTTP/1 request of a request stream connection is a tunnel
// request; `content` is whether it carries request content.
[[nodiscard]] bool http1_request_stream_tunnel(const http_request& request, bool content) noexcept;

// Runs the connection's callback for a tunnel request in the context its
// upgrade route built. `accept` commits the success head and establishes the
// tunnel; a refusal writes through the protocol's ordinary response stream,
// which the upgrade driver must bind before dispatching this context.
[[nodiscard]] task<void> serve_request_stream_tunnel(context& context_value, const request_stream_tunnel_accept& accept, const request_stream_websocket_accept* accept_websocket = nullptr);

// The single writer of every HTTP/1 final head that hands the connection to
// another protocol: a WebSocket or CONNECT-UDP 101 and a CONNECT 2xx. RFC 9110
// section 7.8: when the head is 101 (Switching Protocols) and the request
// carried Expect: 100-continue, the HTTP-owned
// http_request_expectations::continue_before_switching_protocols() decision
// puts a 100 (Continue) head before it in the same write, even when no content
// follows. Any other status is written alone. `head` is the complete encoded
// final head; it must stay valid until the returned task completes.
template <typename stream_type>
[[nodiscard]] task<std::error_code> write_http1_upgrade_response_head(stream_type& stream, const http_request_expectations& expectations, http_status_code status, std::string_view head, connection_scanner::entry& scanner_entry) {
    constexpr std::size_t continue_head_capacity = 64;
    std::array<char, continue_head_capacity> continue_bytes;
    std::string_view continue_head;
    if (status == http_status::switching_protocols && expectations.continue_before_switching_protocols()) {
        const auto prepared = prepare_http1_interim_response_head(http_interim_response_head(http_status::continue_value), std::span<char>(continue_bytes));
        const auto* prepared_head = std::get_if<prepared_http1_interim_response>(&prepared);
        if (prepared_head == nullptr) {
            throw std::logic_error("cannot prepare HTTP/1 100 Continue head");
        }
        continue_head = prepared_head->head();
    }
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(continue_head), asio::buffer(head)};
    const connection_scanner::entry::write_scope write_stall_clock(scanner_entry);
    const auto written = co_await ruvia::async_asio<std::size_t>([&stream, &buffers, &scanner_entry](auto handler) {
        asio::async_write(stream, buffers, connection_write_progress(scanner_entry), std::move(handler));
    });
    co_return written.error_code();
}

// The context retains header storage through handshake construction. The
// protocol plan copies the views before this temporary pointer table is freed.
[[nodiscard]] inline inline_http_header_views<> websocket_response_headers(context& context_value) {
    return inline_http_header_views<>(context_access::handshake_headers(context_value));
}

// Flush the exact HTTP-owned 101 handshake prepared by the route. Negotiation is
// not recomputed and no compression bool is returned through a side channel.
// The shared HTTP/1 upgrade-head writer applies the request `expectations`
// (100 Continue before 101, RFC 9110 section 7.8).
template <typename stream_type>
task<std::error_code> write_websocket_handshake(stream_type& stream, const websocket_server_handshake& handshake, const http_request_expectations& expectations, connection_scanner::entry& scanner_entry) {
    http_response_head_buffer head;
    handshake.for_each_response_part([&](std::string_view part) {
        head.append(part);
    });
    co_return co_await write_http1_upgrade_response_head(stream, expectations, http_status::switching_protocols, head.view(), scanner_entry);
}

// The shared shape of every upgrade-capable route (CONNECT / CONNECT-UDP
// tunnels and WebSocket upgrades) across HTTP/1 and HTTP/2: run the
// application chain with a terminal that commits the upgrade and
// owns the resulting session; once a session exists it is always finished,
// otherwise a handler failure propagates and an uncommitted response falls
// back to the ordinary buffered tail.
[[nodiscard]] inline websocket_connection_options make_websocket_session_connection_options(const http_server_options& options, websocket_compression negotiated, const websocket_deflate_config& deflate, protocol_byte_limit receive_limit) noexcept {
    // Directions without context takeover borrow the worker's codecs, which
    // outlive every connection, so they allocate no codec state here.
    return websocket_connection_options{
        .receive_message_limit_ = receive_limit,
        .compression_ = negotiated,
        .deflater_ = deflate.deflater_,
        .shared_deflate_ = &options.codecs_->websocket_deflate(),
    };
}

// Selects the refusal response the server sends after the callback returns.
[[nodiscard]] inline websocket_rejection reject_request_stream_websocket(context& context_value, const websocket_handshake_failure& failure) {
    const auto error = failure.protocol_error();
    auto response = context_value.error(copy_http_protocol_error_info(error));
    failure.apply_required_response_headers(response);
    const websocket_rejection rejection{response.status(), error.code()};
    context_access::set_response(context_value, std::move(response));
    return rejection;
}

struct websocket_accept_access final {
    [[nodiscard]] static websocket_accept_result accepted(websocket& session) noexcept {
        return websocket_accept_result(session);
    }
    [[nodiscard]] static websocket_accept_result rejected(websocket_rejection rejection) noexcept {
        return websocket_accept_result(rejection);
    }
};

// `datagrams` is the route's http_tunnel_route_config::datagrams_, which
// CONNECT-UDP (`udp`) always sets.
[[nodiscard]] inline http_response prepare_tunnel_upgrade_response_head(context& context_value, http_protocol_version version, bool udp, bool datagrams) {
    auto head_response = context_access::streaming_head(context_value);
    if (udp) {
        auto negotiated = prepare_http_connect_udp_response(std::move(head_response), version);
        if (negotiated.index() != 0) {
            throw std::invalid_argument("invalid CONNECT-UDP response metadata");
        }
        head_response = std::move(std::get<0>(negotiated));
    } else if (datagrams && head_response.status().is_successful()) {
        // RFC 9297 section 3.4: the route's protocol uses the Capsule Protocol,
        // so its successful response says so with exactly one ?1.
        head_response.header_stable_view("Capsule-Protocol", "?1");
    }
    return head_response;
}

// `establish(context&) -> task<void>` commits the upgrade response head,
// emplaces `session` and runs the route's stream handler on it.
// `finish(session_type&, std::exception_ptr) -> task<void>` closes an
// established session and reports a handler failure; it always runs once a
// session exists, before any handler exception is surfaced.
// Returns a buffered response only when the application answered without
// upgrading; an established or abandoned upgrade returns std::nullopt.
template <typename session_type, typename establish_type, typename finish_type>
[[nodiscard]] task<std::optional<http_response>> run_http_upgrade_route(const route_table& routes, const http_request& request, const resolved_route& resolved, const context_services& services, std::optional<session_type>& session, establish_type& establish, finish_type& finish) {
    const auto terminal = callback_access::bind<task<void>(context&)>(establish);
    std::optional<http_response> buffered;
    std::exception_ptr failure;
    try {
        buffered = co_await routes.dispatch_upgrade(request, resolved, terminal, services);
    } catch (...) {
        failure = std::current_exception();
    }
    if (session.has_value()) {
        co_await finish(*session, failure);
        co_return std::nullopt;
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    co_return buffered;
}

template <typename stream_type>
class websocket_upgrade_commit final {
public:
    static constexpr bool native_stream = websocket_session_connection<stream_type>::native_stream;
    using stream_argument_type = typename websocket_session_connection<stream_type>::stream_argument_type;

    websocket_upgrade_commit(stream_argument_type stream, websocket_compression compression, std::string_view subprotocol) :
        stream_(hold_stream(std::forward<stream_argument_type>(stream))),
        compression_(compression) {
        try {
            subprotocol_.assign(subprotocol);
        } catch (...) {
            abort();
            throw;
        }
    }
    [[nodiscard]] stream_argument_type release_stream() noexcept {
        if constexpr (native_stream) {
            return *stream_;
        } else {
            return std::move(stream_);
        }
    }

    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] std::string&& release_subprotocol() noexcept {
        return std::move(subprotocol_);
    }

    void abort() noexcept {
        if constexpr (native_stream) {
            websocket_session_connection<stream_type>::close_stream(*stream_);
        } else {
            stream_.abort();
        }
    }

private:
    using held_stream_type = std::conditional_t<native_stream, stream_type*, stream_type>;

    [[nodiscard]] static held_stream_type hold_stream(stream_argument_type stream) noexcept(native_stream) {
        if constexpr (native_stream) {
            return &stream;
        } else {
            return std::move(stream);
        }
    }

    held_stream_type stream_;
    websocket_compression compression_{};
    std::string subprotocol_;
};

// A committed upgrade, or the refusal its handshake check selected.
template <typename stream_type>
using websocket_upgrade_attempt = std::variant<websocket_upgrade_commit<stream_type>, websocket_rejection>;

template <typename transport_type, typename commit_upgrade_type, typename commit_websocket_type>
struct tunnel_upgrade_session_operation final {
    using session_type = http_tunnel_session<transport_type>;

    const http_request& request_;
    const resolved_route& resolved_;
    const tunnel_route_endpoint& endpoint_;
    const context_services& services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
    inbound_buffer_account& inbound_;
    bool udp_;
    std::string_view pending_;
    commit_upgrade_type& commit_upgrade_;
    commit_websocket_type& commit_websocket_;
    std::optional<session_type>& session_;
    http_tunnel_route_config config_{endpoint_.config()};
    std::optional<websocket_session_connection<transport_type>> websocket_session_{};
    struct websocket_borrow final {
        explicit websocket_borrow(websocket_session_connection<transport_type>& connection) :
            value_(make_websocket_facade(connection)) {}
        websocket value_;
    };
    std::optional<websocket_borrow> websocket_{};

    // Commits the upgrade head the context's draft describes and opens the
    // session over the transport that head hands over.
    [[nodiscard]] task<void> establish(context& context_value) {
        auto head_response = prepare_tunnel_upgrade_response_head(context_value, request_.protocol_version(), udp_, config_.datagrams_);
        std::optional<transport_type> transport;
        if constexpr (requires { commit_upgrade_(context_value, head_response).operator co_await(); }) {
            transport.emplace(co_await commit_upgrade_(context_value, head_response));
        } else {
            transport.emplace(commit_upgrade_(context_value, head_response));
        }
        try {
            session_.emplace(std::move(*transport), services_.worker(), &inbound_, config_.datagrams_, pending_);
        } catch (...) {
            transport->abort();
            throw;
        }
        // A synchronous commit leaves no co_await in this instantiation;
        // co_return keeps the body a coroutine for every transport.
        co_return;
    }

    [[nodiscard]] task<http_tunnel*> accept_request_stream_tunnel(context& context_value, http_tunnel_route_config config) {
        config_ = route_endpoint::validate_tunnel_config(!request_.protocol().empty() || request_.known_method() != http_known_method::connect, udp_, config);
        co_await establish(context_value);
        scanner_entry_.enter_long_lived();
        co_return &session_->tunnel();
    }

    [[nodiscard]] task<websocket_accept_result> accept_request_stream_websocket(context& context_value, const websocket_route_endpoint& websocket_endpoint) {
        const auto headers = websocket_response_headers(context_value);
        auto attempt = co_await commit_websocket_(context_value, websocket_endpoint, headers);
        if (const auto* rejection = std::get_if<websocket_rejection>(&attempt)) {
            co_return websocket_accept_access::rejected(*rejection);
        }
        auto* committed = std::get_if<websocket_upgrade_commit<transport_type>>(&attempt);
        try {
            websocket_session_.emplace(committed->release_stream(), services_.worker(), scanner_entry_, websocket_endpoint.lifecycle(), protocol_byte_limit::limited(options_.server_.max_websocket_message_bytes_), pending_, make_websocket_session_connection_options(options_, committed->compression(), websocket_endpoint.deflate(), websocket_endpoint.message_limit(options_.server_.max_websocket_message_bytes_)), committed->release_subprotocol(), &inbound_);
        } catch (...) {
            committed->abort();
            throw;
        }
        websocket_.emplace(*websocket_session_);
        scanner_entry_.enter_long_lived();
        co_return websocket_accept_access::accepted(websocket_->value_);
    }

    [[nodiscard]] task<void> operator()(context& context_value) {
        if (is_request_stream_tunnel_route(resolved_.route())) {
            auto accept = [this, &context_value](http_status_code, http_tunnel_route_config config) {
                return accept_request_stream_tunnel(context_value, config);
            };
            const auto accept_ref = callback_access::bind<task<http_tunnel*>(http_status_code, http_tunnel_route_config)>(accept);
            auto accept_websocket = [this, &context_value](const websocket_route_endpoint& endpoint) {
                return accept_request_stream_websocket(context_value, endpoint);
            };
            const auto websocket_accept_ref = callback_access::bind<task<websocket_accept_result>(const websocket_route_endpoint&)>(accept_websocket);
            std::exception_ptr failure;
            try {
                co_await serve_request_stream_tunnel(context_value, accept_ref, &websocket_accept_ref);
            } catch (...) {
                failure = std::current_exception();
            }
            // Retire borrowed operations before draining the owned connection,
            // exactly as the route handler's stack facade does.
            websocket_.reset();
            if (websocket_session_) {
                co_await finish_websocket_session(*websocket_session_, failure, options_.connection_failure_, services_.remote_address());
            } else if (failure) {
                std::rethrow_exception(failure);
            }
            co_return;
        }
        co_await establish(context_value);
        co_await invoke_tunnel_handler(*session_, scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_tunnel_session(session_value, std::move(failure), options_.connection_failure_, services_.remote_address(), scanner_entry_, config_.peer_transport_fin_timeout_);
    }
};

template <typename transport_type, typename commit_upgrade_type, typename commit_websocket_type>
[[nodiscard]] task<std::optional<http_response>> execute_tunnel_upgrade_session(const route_table& routes, const http_request& request, const resolved_route& resolved, const context_services& services, const http_server_options& options, connection_scanner::entry& scanner_entry, inbound_buffer_account& inbound, bool udp, std::string_view pending, commit_upgrade_type commit_upgrade, commit_websocket_type commit_websocket) {
    const auto& endpoint = *resolved.route().endpoint().tunnel();
    using operation_type = tunnel_upgrade_session_operation<transport_type, commit_upgrade_type, commit_websocket_type>;
    std::optional<typename operation_type::session_type> session;
    operation_type operation{
        .request_ = request,
        .resolved_ = resolved,
        .endpoint_ = endpoint,
        .services_ = services,
        .options_ = options,
        .scanner_entry_ = scanner_entry,
        .inbound_ = inbound,
        .udp_ = udp,
        .pending_ = pending,
        .commit_upgrade_ = commit_upgrade,
        .commit_websocket_ = commit_websocket,
        .session_ = session,
    };
    co_return co_await run_http_upgrade_route(routes, request, resolved, services, session, operation, operation);
}

template <typename stream_type, typename commit_upgrade_type>
struct websocket_upgrade_session_operation final {
    using session_type = websocket_session_connection<stream_type>;

    const resolved_route& resolved_;
    const websocket_route_endpoint& websocket_endpoint_;
    const context_services& services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
    inbound_buffer_account& inbound_;
    std::string_view pending_frames_;
    commit_upgrade_type& commit_upgrade_;
    std::optional<session_type>& session_;

    [[nodiscard]] task<void> operator()(context& context_value) {
        const auto response_headers_value = websocket_response_headers(context_value);
        std::optional<websocket_upgrade_commit<stream_type>> committed;
        if constexpr (requires { commit_upgrade_(context_value, response_headers_value).operator co_await(); }) {
            committed.emplace(co_await commit_upgrade_(context_value, response_headers_value));
        } else {
            committed.emplace(commit_upgrade_(context_value, response_headers_value));
        }
        try {
            session_.emplace(committed->release_stream(), services_.worker(), scanner_entry_, websocket_endpoint_.lifecycle(), protocol_byte_limit::limited(options_.server_.max_websocket_message_bytes_), pending_frames_, make_websocket_session_connection_options(options_, committed->compression(), websocket_endpoint_.deflate(), websocket_endpoint_.message_limit(options_.server_.max_websocket_message_bytes_)), committed->release_subprotocol(), &inbound_);
        } catch (...) {
            committed->abort();
            throw;
        }
        co_await invoke_websocket_handler(*session_, scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_websocket_session(session_value, std::move(failure), options_.connection_failure_, services_.remote_address());
    }
};

template <typename stream_type, typename commit_upgrade_type>
[[nodiscard]] task<std::optional<http_response>> execute_websocket_upgrade_session(const route_table& routes, const http_request& request, const resolved_route& resolved, const context_services& services, const http_server_options& options, connection_scanner::entry& scanner_entry, inbound_buffer_account& inbound, std::string_view pending_frames, commit_upgrade_type commit_upgrade) {
    const auto& websocket_endpoint = *resolved.route().endpoint().websocket();
    using operation_type = websocket_upgrade_session_operation<stream_type, commit_upgrade_type>;
    std::optional<typename operation_type::session_type> session;
    operation_type operation{
        .resolved_ = resolved,
        .websocket_endpoint_ = websocket_endpoint,
        .services_ = services,
        .options_ = options,
        .scanner_entry_ = scanner_entry,
        .inbound_ = inbound,
        .pending_frames_ = pending_frames,
        .commit_upgrade_ = commit_upgrade,
        .session_ = session,
    };
    co_return co_await run_http_upgrade_route(routes, request, resolved, services, session, operation, operation);
}

}  // namespace ruvia::detail

namespace ruvia {

namespace detail {

class response_stream_state final {
public:
    [[nodiscard]] bool committed() const noexcept {
        return commit_plan() != nullptr;
    }

    [[nodiscard]] bool ended() const noexcept {
        return std::holds_alternative<ended_type>(state_);
    }

    [[nodiscard]] bool aborted() const noexcept {
        return std::holds_alternative<aborted_before_commit_type>(state_) || std::holds_alternative<aborted_after_commit_type>(state_);
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        if (const auto* value = std::get_if<body_open_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<ended_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<aborted_after_commit_type>(&state_)) {
            return &value->plan_;
        }
        return nullptr;
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    void bind_context(context* context_value) {
        if (!std::holds_alternative<unbound_type>(state_)) {
            throw std::logic_error("response stream context is already bound");
        }
        if (context_value == nullptr) {
            throw std::invalid_argument("response stream context binding is incomplete");
        }
        state_.emplace<bound_type>(context_value);
    }

    void release_context() noexcept {
        if (std::holds_alternative<bound_type>(state_)) {
            state_.emplace<detached_type>();
        }
    }

    [[nodiscard]] context& bound_context() const {
        const auto* bound = std::get_if<bound_type>(&state_);
        if (bound == nullptr) {
            if (committed()) {
                throw std::logic_error("response stream is already committed");
            }
            throw std::logic_error("response stream context is not bound");
        }
        return *bound->context_;
    }

    void mark_committed(http_response_stream_commit_plan plan) {
        if (committed() || aborted()) {
            throw std::logic_error("response stream is already committed");
        }
        switch (plan.head_disposition()) {
            case http_response_stream_head_disposition::body_open:
                state_.emplace<body_open_type>(plan);
                break;
            case http_response_stream_head_disposition::message_ended:
                state_.emplace<ended_type>(plan);
                break;
        }
    }

    void mark_ended() {
        if (ended()) {
            return;
        }
        if (auto* value = std::get_if<body_open_type>(&state_)) {
            auto plan = value->plan_;
            state_.emplace<ended_type>(plan);
            return;
        }
        throw std::logic_error("response stream is not committed");
    }

    void mark_aborted() noexcept {
        if (aborted()) {
            return;
        }
        if (const auto* committed_plan = commit_plan()) {
            // A body-suppressed commit is already logically ended, but its head
            // can still fail during transport delivery. Preserve that commit.
            const auto plan = *committed_plan;
            state_.emplace<aborted_after_commit_type>(plan);
            return;
        }
        state_.emplace<aborted_before_commit_type>();
    }

    void ensure_body_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (!std::holds_alternative<body_open_type>(state_)) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    // Trailers are optional metadata (RFC 9110 6.5): framing that cannot carry
    // them discards them, so only a status that can never carry content is a
    // handler error here. The caller writes trailers only if trailers_available().
    void ensure_trailers_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        const auto* plan = commit_plan();
        if (plan == nullptr || !plan->status_permits_trailers()) {
            throw std::logic_error("response status does not permit trailers");
        }
    }

private:
    struct unbound_type final {};

    struct detached_type final {};

    struct bound_type final {
        explicit bound_type(context* bound_context) noexcept :
            context_(bound_context) {}

        context* context_;
    };

    struct body_open_type final {
        explicit body_open_type(http_response_stream_commit_plan commit_plan) noexcept :
            plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct ended_type final {
        explicit ended_type(http_response_stream_commit_plan commit_plan) noexcept :
            plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct aborted_before_commit_type final {};

    struct aborted_after_commit_type final {
        explicit aborted_after_commit_type(http_response_stream_commit_plan commit_plan) noexcept :
            plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    using state_type = std::variant<unbound_type, bound_type, detached_type, body_open_type, ended_type, aborted_before_commit_type, aborted_after_commit_type>;

    state_type state_;
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail {

// Owns the response-coding lifecycle of HTTP/1 and HTTP/2 stream sinks.
// The transport only delivers encoded slices; it never reconstructs whether a
// coding was selected, whether the body is suppressed, or whether an encoder
// exists. The encoder itself is borrowed from the worker's codecs for the
// stream's lifetime, not constructed per response.
class http_streaming_response_compression final {
public:
    // Application input encoded per step. It bounds the encoded buffer and the
    // codec work between two transport writes independently of the size of
    // one application write.
    static constexpr std::size_t input_slice_bytes = 16 * 1024;

    // `compression` is the server's compression policy, or nullptr when the
    // server compresses nothing.
    http_streaming_response_compression(worker_codecs& codecs, http_response_coding_selection selection, const compression_config* compression) noexcept :
        codecs_(codecs),
        selection_(selection),
        compression_(compression) {}
    http_streaming_response_compression(worker_codecs&&, http_response_coding_selection, const compression_config*) = delete;

    http_streaming_response_compression(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression& operator=(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression(http_streaming_response_compression&&) = delete;
    http_streaming_response_compression& operator=(http_streaming_response_compression&&) = delete;

    // Applies the representation headers before the protocol-owned stream head
    // is committed. A forbidden identity fallback, including a negotiation the
    // selection already reports as failed, is rejected here, while the encoder
    // itself is delayed until activate() has a final body plan.
    void prepare(const http_request& request, http_response& response) {
        if (!std::holds_alternative<unprepared_type>(state_)) {
            throw std::logic_error("streaming response compression is already prepared");
        }
        const auto request_method = request.known_method();
        const auto decision = prepare_response_compression(selection_, request_method, response, http_response_compression_source::stream, compression_ != nullptr ? http_response_coding_availability::identity_and_compression : http_response_coding_availability::identity_only);
        if (decision != http_response_compression_decision::encode || !stream_length_allows_encoding(response)) {
            if (http_response_coding_fallback_forbidden(selection_, request_method, response, &request)) {
                throw http_error(http_not_acceptable_coding_error());
            }
            state_.emplace<inactive_type>();
            return;
        }

        response.apply_content_encoding(http_content_coding_token(selection_.coding()));
        state_.emplace<pending_type>();
    }

    // Binds encoder ownership to the protocol body plan. Body-suppressed
    // responses retain their selected representation metadata but never create
    // an encoder for bytes that the protocol will not send.
    void activate(http_response_body_plan body_plan) {
        if (std::holds_alternative<inactive_type>(state_)) {
            return;
        }
        const auto* pending = std::get_if<pending_type>(&state_);
        if (pending == nullptr) {
            throw std::logic_error("streaming response compression is not pending");
        }
        if (body_plan.body_suppressed()) {
            state_.emplace<inactive_type>();
            return;
        }
        state_.emplace<active_type>(codecs_.acquire_stream_encoder(selection_.coding()));
    }

    [[nodiscard]] bool active() const noexcept {
        return std::holds_alternative<active_type>(state_);
    }

    // A stream-head transaction may prepare representation metadata before the
    // protocol layer accepts the head. If that later preparation fails, the
    // sink is terminal before any body byte can be retried; keep the encoder
    // lifecycle in the same terminal state instead of leaving pending/inactive
    // behind for a second commit attempt.
    void abort() noexcept {
        encoded_chunk_.clear();
        state_.emplace<failed_type>();
    }

    // The one slicing loop of both protocols: encodes one application write in
    // input_slice_bytes steps and hands every non-empty encoded slice to
    // `deliver` (the sink's framed write, returning task<void>). Only the last
    // slice flushes, so each application write ends with exactly one codec
    // flush, which keeps SSE events prompt without flushing every slice.
    template <typename deliver_type>
    task<void> write(std::string_view chunk, deliver_type deliver) {
        std::size_t offset = 0;
        while (offset < chunk.size()) {
            const auto count = (std::min)(input_slice_bytes, chunk.size() - offset);
            const auto slice = chunk.substr(offset, count);
            offset += count;
            encode_slice(slice, offset == chunk.size());
            if (!encoded_chunk_.empty()) {
                co_await deliver(std::string_view(encoded_chunk_));
            }
            release_output();
        }
    }

    void finish() {
        encoded_chunk_.clear();
        if (std::holds_alternative<finished_type>(state_)) {
            return;
        }
        auto* active_state = std::get_if<active_type>(&state_);
        if (active_state == nullptr) {
            throw std::logic_error("streaming response encoder is not active");
        }
        try {
            active_state->lease_.encoder().finish(encoded_chunk_);
            state_.emplace<finished_type>();
        } catch (...) {
            fail_encoder(*active_state);
            throw;
        }
    }

    [[nodiscard]] std::string_view output() const& noexcept {
        return encoded_chunk_;
    }
    std::string_view output() const&& = delete;

    // A sent chunk no longer has a transport borrower. Keep ordinary stream
    // buffers reusable without retaining unusually large writes for the life
    // of a long-running handler.
    void release_output() {
        constexpr std::size_t retained_bytes = 64 * 1024;
        http_clear_string_retaining_small(encoded_chunk_, retained_bytes);
    }

private:
    struct unprepared_type final {};
    // Identity output and protocol-suppressed bodies both need no encoder.
    struct inactive_type final {};
    struct pending_type final {};
    struct finished_type final {};
    struct failed_type final {};

    struct active_type final {
        explicit active_type(stream_encoder_lease lease) noexcept :
            lease_(std::move(lease)) {}

        stream_encoder_lease lease_;
    };

    using state_type = std::variant<unprepared_type, inactive_type, pending_type, finished_type, failed_type, active_type>;

    // A length the head declares sizes the stream like a buffered body; an
    // undeclared length leaves the decision to the negotiated coding.
    [[nodiscard]] bool stream_length_allows_encoding(const http_response& response) const noexcept {
        const auto declared = declared_content_length(response);
        return !declared.has_value() || declared_length_permits_encoding(selection_, *compression_, *declared);
    }

    void encode_slice(std::string_view input, bool flush) {
        auto* active_state = std::get_if<active_type>(&state_);
        if (active_state == nullptr) {
            encoded_chunk_.clear();
            throw std::logic_error("streaming response encoder is not active");
        }
        encoded_chunk_.clear();
        try {
            active_state->lease_.encoder().write(input, encoded_chunk_, flush);
        } catch (...) {
            fail_encoder(*active_state);
            throw;
        }
    }

    // A codec that failed mid-stream is never returned to the worker pool.
    void fail_encoder(active_type& active_state) noexcept {
        active_state.lease_.discard();
        abort();
    }

    worker_codecs& codecs_;
    http_response_coding_selection selection_;
    const compression_config* compression_;
    std::string encoded_chunk_;
    state_type state_;
};

}  // namespace ruvia::detail

namespace ruvia {

class http_client;
class worker_ref;
#ifdef RUVIA_ENABLE_DATABASE
class db_client;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_client;
#endif

namespace detail {

// Caller-owned standalone clients a router lends to the requests it
// dispatches. Every entry borrows its client, which must run on the router's
// worker and outlive every dispatch; aliases are unique per client kind. The
// set is sealed with its router, so request-time lookup only reads it.
class borrowed_clients final {
public:
    borrowed_clients();
    borrowed_clients(const borrowed_clients&) = delete;
    borrowed_clients& operator=(const borrowed_clients&) = delete;

    [[nodiscard]] bool empty() const noexcept;

    void bind_http_client(const worker_ref& worker, std::string_view alias, const http_client& client);
    [[nodiscard]] const http_client* http_clients(std::string_view alias) const noexcept;

#ifdef RUVIA_ENABLE_DATABASE
    void bind_database(const worker_ref& worker, std::string_view alias, const db_client& client);
    [[nodiscard]] const db_client* database(std::string_view alias) const noexcept;
#endif

#ifdef RUVIA_ENABLE_REDIS
    void bind_redis(const worker_ref& worker, std::string_view alias, const redis_client& client);
    [[nodiscard]] const redis_client* redis(std::string_view alias) const noexcept;
#endif

private:
    template <typename client_type>
    struct alias_entry final {
        std::string alias_;
        const client_type* client_;
    };

    std::vector<alias_entry<http_client>> http_clients_;
#ifdef RUVIA_ENABLE_DATABASE
    std::vector<alias_entry<db_client>> databases_;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::vector<alias_entry<redis_client>> redis_;
#endif
};

}  // namespace detail
}  // namespace ruvia
