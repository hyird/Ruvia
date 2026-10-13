#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/web/detail/middleware/middleware_descriptor.h"
#include "ruvia/web/error_handlers.h"

#include "router/route_entry.h"

namespace ruvia::detail {

class route_table;
class router_impl;

// The one immutable route plan of a process run, compiled once at startup and
// shared by every worker: every route with its endpoint metadata, limits and
// parameter names, the middleware frame layout, the lookup indices, and the
// error/not-found fallbacks. Workers bind only their own handler targets and
// middleware instances to it (route_table), by route and frame index.
class compiled_route_plan final {
public:
    explicit compiled_route_plan(std::pmr::memory_resource* resource)
        : resource_(pmr_resource_or_default(resource)),
          routes_(resource_),
          middleware_instances_(resource_),
          middleware_frame_instances_(resource_),
          middleware_scopes_(resource_),
          param_names_(resource_),
          extension_route_indices_(resource_),
          connect_route_indices_(resource_),
          connect_protocols_(resource_),
          static_slots_(resource_),
          dynamic_roots_{dynamic_node_type(resource_), dynamic_node_type(resource_), dynamic_node_type(resource_),
              dynamic_node_type(resource_), dynamic_node_type(resource_), dynamic_node_type(resource_),
              dynamic_node_type(resource_)},
          dynamic_node_arena_(resource_),
          url_patterns_(resource_),
          prefix_error_handlers_(resource_),
          prefix_not_found_handlers_(resource_) {}

    compiled_route_plan(const compiled_route_plan&) = delete;
    compiled_route_plan& operator=(const compiled_route_plan&) = delete;
    compiled_route_plan(compiled_route_plan&&) = delete;
    compiled_route_plan& operator=(compiled_route_plan&&) = delete;

private:
    friend class route_table;
    friend class router_impl;

    static constexpr std::size_t routable_method_count = 7;
    static constexpr std::size_t no_route_index = std::numeric_limits<std::size_t>::max();

    struct static_route_slot final {
        std::uint64_t hash_{};
        std::size_t route_index_{no_route_index};
    };

    struct dynamic_node_type;

    struct dynamic_static_child_type final {
        std::pmr::string segment_;
        dynamic_node_type* node_{nullptr};
    };

    struct dynamic_node_type final {
        explicit dynamic_node_type(std::pmr::memory_resource* resource)
            : static_children_(resource) {}

        std::pmr::vector<dynamic_static_child_type> static_children_;
        dynamic_node_type* param_child_{nullptr};
        std::size_t route_index_{no_route_index};
        std::size_t wildcard_route_index_{no_route_index};
    };

    struct connect_protocol_index_type final {
        connect_protocol_index_type(std::pmr::memory_resource* resource, std::string_view value)
            : protocol_(value, resource),
              root_(resource) {}
        std::pmr::string protocol_;
        dynamic_node_type root_;
    };

    // Normalized prefix, deepest first: selection is a first-match scan.
    template <typename handler_type>
    struct prefix_handler_type final {
        prefix_handler_type(std::pmr::memory_resource* resource, std::string_view prefix_value,
            handler_type handler_value)
            : prefix_(prefix_value, resource),
              handler_(handler_value) {}

        std::pmr::string prefix_;
        handler_type handler_{nullptr};
    };

    std::pmr::memory_resource* resource_;
    std::pmr::vector<route_entry> routes_;
    // One descriptor per middleware instance a worker materializes. Frames
    // name their instance by index, so an app-wide middleware is one instance
    // shared by every route of a worker.
    std::pmr::vector<controller_middleware_descriptor> middleware_instances_;
    std::pmr::vector<std::size_t> middleware_frame_instances_;
    // Parallel to middleware_frame_instances_: the path scope a conditional
    // frame checks at dispatch, or empty for an unconditional frame. Only
    // routes flagged has_conditional_middleware() read it. Views borrow the
    // registration-owned use_at prefixes, which outlive every plan.
    std::pmr::vector<std::string_view> middleware_scopes_;
    // A contiguous block at the end of the frames: the app-wide middlewares
    // that declared they also run when no route matched.
    std::size_t unmatched_middleware_offset_{0};
    std::size_t unmatched_middleware_count_{0};
    std::pmr::vector<std::string_view> param_names_;
    std::pmr::vector<std::size_t> extension_route_indices_;
    std::pmr::vector<std::size_t> connect_route_indices_;
    std::pmr::vector<connect_protocol_index_type> connect_protocols_;
    std::pmr::vector<static_route_slot> static_slots_;
    std::array<dynamic_node_type, routable_method_count> dynamic_roots_;
    std::pmr::vector<dynamic_node_type> dynamic_node_arena_;
    // Distinct registered route patterns, sorted, so url_for finds its pattern
    // by binary search.
    std::pmr::vector<std::string_view> url_patterns_;
    http_error_handler_ref_type error_handler_{nullptr};
    http_not_found_handler_ref_type not_found_handler_{nullptr};
    std::pmr::vector<prefix_handler_type<http_error_handler_ref_type>> prefix_error_handlers_;
    std::pmr::vector<prefix_handler_type<http_not_found_handler_ref_type>> prefix_not_found_handlers_;
    std::uint32_t static_method_mask_{0};
    std::uint32_t dynamic_method_mask_{0};
    std::size_t static_slot_mask_{};
    bool has_route_rate_limit_{false};
};

using compiled_route_plan_ptr_type =
    std::unique_ptr<compiled_route_plan, pmr_object_deleter<compiled_route_plan>>;

}  // namespace ruvia::detail
