#pragma once

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/web/detail/controller/controller_descriptors.h"

#include "router/route_table.h"
#include "router/router.h"

namespace ruvia::detail {

class controller_route_builder::impl_type final {
public:
    impl_type(detail::router& router_value, std::pmr::string prefix_value,
        std::pmr::vector<controller_middleware_descriptor> middleware_values)
        : router_(router_value),
          prefix_(std::move(prefix_value)),
          middlewares_(std::move(middleware_values)) {}

    [[nodiscard]] detail::router& router() const noexcept {
        return router_;
    }

    [[nodiscard]] std::string_view prefix() const noexcept {
        return prefix_;
    }

    [[nodiscard]] const std::pmr::vector<controller_middleware_descriptor>& middlewares()
        const noexcept {
        return middlewares_;
    }

private:
    detail::router& router_;
    std::pmr::string prefix_;
    std::pmr::vector<controller_middleware_descriptor> middlewares_;
};

class router_impl final {
public:
    router& owner_;

    explicit router_impl(router& router_value);

    router_impl(const router_impl&) = delete;
    router_impl& operator=(const router_impl&) = delete;

    [[nodiscard]] static router_impl& from(router& router_value) noexcept {
        return *router_value.impl_;
    }

    [[nodiscard]] static const router_impl& from(const router& router_value) noexcept {
        return *router_value.impl_;
    }

    router& set_error_handler(http_error_handler_ref_type handler) noexcept;
    router& set_not_found_handler(http_not_found_handler_ref_type handler) noexcept;
    // Path-prefix-scoped fallbacks (Hono sub-app scoping analog): wholesale
    // replacement, owned copies; applied to the table at finalize or, when the
    // table already exists, immediately (both are idempotent for restarts).
    router& set_prefix_error_handlers(std::span<const http_prefix_error_handler> handlers);
    router& set_prefix_not_found_handlers(std::span<const http_prefix_not_found_handler> handlers);
    // application-wide middleware, prepended to every route's chain at finalize. Each
    // descriptor is materialized exactly once per worker route graph; that
    // worker-local instance serves all routes in the graph.
    void set_global_middlewares(std::span<const controller_middleware_descriptor> descriptors);
    void finalize(const compiled_route_plan* compiled_plan = nullptr);
    [[nodiscard]] const detail::route_table& route_table() const;
    [[nodiscard]] compiled_route_plan_ptr_type release_compiled_plan();

    void register_route(http_known_method method, std::pmr::string path, route_handler_type handler,
        request_body_mode body_mode,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);
    void register_response_stream_route(http_known_method method, std::pmr::string path,
        route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);
    void register_sse_route(http_known_method method, std::pmr::string path, route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);
    void register_tunnel_route(std::string_view protocol, std::pmr::string target,
        route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares, http_tunnel_route_config config = {});
    void register_websocket_route(http_known_method method, std::pmr::string path,
        route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares,
        websocket_route_config websocket_config = {});

    // An extension method is routed by its exact wire token. Kept off the
    // enum-indexed structures entirely: extension routes are rare, so they get
    // a separate cold list rather than widening every dense per-method array.
    void register_extension_method_route(std::string_view method_token, std::pmr::string path,
        route_handler_type handler, request_body_mode body_mode,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);

private:
    void register_endpoint(http_known_method method, std::pmr::string path, route_endpoint endpoint,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);
    void register_endpoint_with_token(http_known_method method, std::string_view method_token,
        std::pmr::string path, route_endpoint endpoint,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);

    class pending_route_type final {
    public:
        struct init_type final {
            http_known_method method_;
            std::pmr::string method_token_;
            std::pmr::string path_;
            route_endpoint endpoint_;
            bool dynamic_{false};
            std::size_t max_request_body_bytes_{0};
            std::int64_t deadline_ms_{0};
            std::pmr::vector<route_middleware_type> middlewares_;
        };

        pending_route_type(std::pmr::memory_resource* resource, init_type init);
        pending_route_type(const pending_route_type&) = delete;
        pending_route_type& operator=(const pending_route_type&) = delete;
        pending_route_type(pending_route_type&&) noexcept = default;
        pending_route_type& operator=(pending_route_type&&) = delete;

        [[nodiscard]] http_known_method method() const noexcept {
            return method_;
        }

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

        [[nodiscard]] std::span<const route_middleware_type> middlewares() const noexcept {
            return middlewares_;
        }

        [[nodiscard]] std::size_t max_request_body_bytes() const noexcept {
            return max_request_body_bytes_;
        }

        [[nodiscard]] std::int64_t deadline_ms() const noexcept {
            return deadline_ms_;
        }

        void set_dynamic(bool dynamic) noexcept {
            dynamic_ = dynamic;
        }

    private:
        http_known_method method_;
        std::pmr::string method_token_;
        std::pmr::string path_;
        route_endpoint endpoint_;
        bool dynamic_{false};
        std::size_t max_request_body_bytes_{0};
        std::int64_t deadline_ms_{0};
        std::pmr::vector<route_middleware_type> middlewares_;
    };

    void append_pending_route(pending_route_type route);

    class middleware_lifetime_type {
    public:
        middleware_lifetime_type() noexcept = default;
        middleware_lifetime_type(void* target, controller_middleware_descriptor::destroy_type destroy) noexcept;
        middleware_lifetime_type(const middleware_lifetime_type&) = delete;
        middleware_lifetime_type& operator=(const middleware_lifetime_type&) = delete;
        middleware_lifetime_type(middleware_lifetime_type&& other) noexcept;
        middleware_lifetime_type& operator=(middleware_lifetime_type&& other) noexcept;
        ~middleware_lifetime_type();

    private:
        void reset() noexcept;

        void* target_{nullptr};
        controller_middleware_descriptor::destroy_type destroy_{nullptr};
    };

    static void validate_no_dynamic_route_conflict(std::span<const pending_route_type> routes);
    void validate_route_target(
        http_known_method method, std::string_view method_token, std::string_view path, const route_endpoint& endpoint) const;
    [[nodiscard]] route_middleware_type materialize_middleware(controller_middleware_descriptor middleware);
    void append_materialized_middlewares(std::pmr::vector<route_middleware_type>& frames,
        std::span<const controller_middleware_descriptor> descriptors);
    [[nodiscard]] std::pmr::vector<route_middleware_type> materialize_middlewares(
        std::span<const controller_middleware_descriptor> first,
        std::span<const controller_middleware_descriptor> second = {});
    void build_route_table(detail::route_table& table, const compiled_route_plan* compiled_plan) const;

    struct route_table_deleter_type final {
        std::pmr::memory_resource* resource_{nullptr};
        void operator()(detail::route_table* table) const noexcept;
    };

    std::pmr::memory_resource* resource_{nullptr};
    std::pmr::vector<pending_route_type> pending_routes_;
    std::optional<std::pmr::unordered_multimap<std::uint64_t, std::size_t>> pending_route_indices_;
    std::pmr::vector<middleware_lifetime_type> middleware_lifetimes_;
    std::pmr::vector<controller_middleware_descriptor> global_middleware_descriptors_;
    std::pmr::vector<route_middleware_type> global_middleware_frames_;
    std::unique_ptr<detail::route_table, route_table_deleter_type> route_table_;
    http_error_handler_ref_type error_handler_{nullptr};
    http_not_found_handler_ref_type not_found_handler_{nullptr};
    std::pmr::vector<std::pair<std::pmr::string, http_error_handler_ref_type>> prefix_error_handlers_{
        registration_resource()};
    std::pmr::vector<std::pair<std::pmr::string, http_not_found_handler_ref_type>> prefix_not_found_handlers_{
        registration_resource()};
    bool has_route_rate_limit_{false};
};

}  // namespace ruvia::detail
