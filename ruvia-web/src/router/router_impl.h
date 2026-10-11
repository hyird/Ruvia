#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/web/detail/controller/controller_descriptors.h"

#include "router/compiled_route_plan.h"
#include "router/route_table.h"
#include "router/router.h"

namespace ruvia::detail {

class app_configuration;

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

// Builds the process route plan once per run: every controller instance in
// `controllers` (one per registrar, registrar order) declares its routes, then
// `configuration` applies the app-wide fallbacks and middleware. Route
// conflicts and invalid registrations throw here, before any worker starts.
[[nodiscard]] compiled_route_plan_ptr_type compile_route_plan(std::pmr::memory_resource* resource,
    std::span<const controller_registrar_type> registrars, const controller_store& controllers,
    const app_configuration& configuration);

// The plan under construction. Lives only inside compile_route_plan().
class router_impl final {
public:
    router& owner_;

    router_impl(router& router_value, std::pmr::memory_resource* plan_resource);

    router_impl(const router_impl&) = delete;
    router_impl& operator=(const router_impl&) = delete;

    [[nodiscard]] static router_impl& from(router& router_value) noexcept {
        return *router_value.impl_;
    }

    void set_error_handler(http_error_handler_ref_type handler) noexcept;
    void set_not_found_handler(http_not_found_handler_ref_type handler) noexcept;
    // Path-prefix-scoped fallbacks (Hono sub-app scoping analog): wholesale
    // replacement, normalized and ordered deepest prefix first.
    void set_prefix_error_handlers(std::span<const http_prefix_error_handler> handlers);
    void set_prefix_not_found_handlers(std::span<const http_prefix_not_found_handler> handlers);
    // Application-wide middleware, prepended to every route's chain. Each
    // descriptor becomes exactly one instance per worker, serving all routes.
    void set_global_middlewares(std::span<const controller_middleware_descriptor> descriptors);

    // Owns endpoint text (tunnel protocol, websocket subprotocols).
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return plan_->resource_;
    }

    // Runs every registrar against its controller instance, in slot order.
    void declare_controllers(
        std::span<const controller_registrar_type> registrars, const controller_store& controllers);

    // The single registration path. `handler_target` must be the instance of
    // the controller currently declaring its routes. `method_token` is non-empty
    // only for an extension method, with `method` unknown.
    void register_endpoint(http_known_method method, std::string_view method_token, std::string_view path,
        route_endpoint endpoint, void* handler_target,
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
    void append_route_middlewares(std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);
    void build_middleware_frames();
    void build_static_index();
    void build_allowed_method_mask();
    void build_dynamic_routes();
    void bind_dynamic_param_names(route_entry& route);
    void append_dynamic_param_name(route_entry& route, std::string_view name);
    void insert_dynamic(compiled_route_plan::dynamic_node_type& root, const route_entry& route);
    void build_server_extension_method_tokens();
    void build_url_patterns();

    [[nodiscard]] static bool is_dynamic_path(std::string_view path) noexcept;
    [[nodiscard]] static bool same_dynamic_shape(std::string_view left, std::string_view right) noexcept;

    compiled_route_plan_ptr_type plan_;
    std::pmr::unordered_multimap<std::uint64_t, std::size_t> route_indices_;
    std::pmr::vector<controller_middleware_descriptor> route_middleware_descriptors_;
    std::pmr::vector<route_middleware_range_type> route_middleware_ranges_;
    std::pmr::vector<controller_middleware_descriptor> global_middleware_descriptors_;
    std::size_t controller_slot_{0};
    void* controller_target_{nullptr};
};

}  // namespace ruvia::detail
