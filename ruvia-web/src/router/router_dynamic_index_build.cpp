#include <algorithm>
#include <stdexcept>
#include <utility>

#include "ruvia/http/url_encoding.h"

#include "router/path_segments.h"
#include "router/router_impl.h"

namespace ruvia {
namespace {

struct dynamic_route_upper_bounds final {
    std::size_t nodes_{0};
    std::size_t param_names_{0};
};

[[nodiscard]] dynamic_route_upper_bounds dynamic_upper_bounds(std::string_view path) noexcept {
    dynamic_route_upper_bounds bounds;
    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!detail::split_path_segment(path, segment, rest)) {
            return bounds;
        }
        const auto kind = detail::classify_route_segment(segment);
        if (kind == detail::route_segment_kind::wildcard) {
            ++bounds.param_names_;
        } else {
            ++bounds.nodes_;
            if (kind == detail::route_segment_kind::param) {
                ++bounds.param_names_;
            }
        }
        if (rest.empty()) {
            return bounds;
        }
        path = rest;
    }
}

// Templated so the plan's private node type is never named outside its friends.
// Literal children sort by canonical form, the order find_dynamic_static_child
// searches in.
template <typename node_type>
void sort_dynamic_node(node_type& node_value) {
    std::ranges::sort(node_value.static_children_, [](const auto& left, const auto& right) {
        return compare_uri_paths(left.segment_, right.segment_) < 0;
    });
    for (auto& child : node_value.static_children_) {
        sort_dynamic_node(*child.node_);
    }
    if (node_value.param_child_) {
        sort_dynamic_node(*node_value.param_child_);
    }
}

}  // namespace

void detail::router::build_dynamic_routes() {
    auto& plan = *plan_;
    plan.connect_protocols_.reserve(plan.connect_route_indices_.size());

    std::size_t dynamic_node_capacity = 0;
    std::size_t param_name_capacity = 0;
    for (const auto& route : plan.routes_) {
        if (route.dynamic()) {
            const auto bounds = dynamic_upper_bounds(route.path());
            dynamic_node_capacity += bounds.nodes_;
            param_name_capacity += bounds.param_names_;
        }
    }
    // Both reserved up front: trie nodes and parameter-name views are borrowed
    // by address, so neither vector may reallocate while it is filled.
    plan.dynamic_node_arena_.reserve(dynamic_node_capacity);
    plan.param_names_.reserve(param_name_capacity);

    for (auto& route : plan.routes_) {
        if (!route.dynamic()) {
            continue;
        }
        if (const auto* tunnel = route.endpoint().tunnel()) {
            const auto protocol = tunnel->protocol();
            auto found = std::ranges::find(
                plan.connect_protocols_, protocol, &compiled_route_plan::connect_protocol_index_type::protocol_);
            if (found == plan.connect_protocols_.end()) {
                found = plan.connect_protocols_.emplace(plan.connect_protocols_.end(), protocol);
            }
            insert_dynamic(found->root_, route);
        } else {
            const auto method = route_table::method_index(route.method());
            plan.dynamic_method_mask_ |= 1U << method;
            insert_dynamic(plan.dynamic_roots_[method], route);
        }
    }
    for (auto& protocol : plan.connect_protocols_) {
        sort_dynamic_node(protocol.root_);
    }
    for (auto& root : plan.dynamic_roots_) {
        sort_dynamic_node(root);
    }
}

bool detail::router::is_dynamic_path(std::string_view path) noexcept {
    while (!path.empty()) {
        std::string_view segment;
        std::string_view rest;
        if (!split_path_segment(path, segment, rest)) {
            return false;
        }
        if (classify_route_segment(segment) != route_segment_kind::literal) {
            return true;
        }
        path = rest;
    }

    return false;
}

void detail::router::append_dynamic_param_name(route_entry& route, std::string_view name) {
    auto& names_storage = plan_->param_names_;
    const auto names = route.param_names();
    if (names.size() >= max_route_params) {
        throw std::invalid_argument("route has too many parameters");
    }

    const auto offset =
        names.empty() ? names_storage.size() : static_cast<std::size_t>(names.data() - names_storage.data());
    names_storage.push_back(name);
    route.param_names_ = std::span<const std::string_view>(names_storage.data() + offset, names.size() + 1);
}

void detail::router::insert_dynamic(compiled_route_plan::dynamic_node_type& root, route_entry& route) {
    auto path = route.path();
    auto* node_value = &root;
    auto& plan = *plan_;

    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!split_path_segment(path, segment, rest)) {
            node_value->route_index_ = route.index();
            return;
        }
        const auto kind = classify_route_segment(segment);
        if (kind == route_segment_kind::wildcard) {
            if (!rest.empty()) {
                throw std::invalid_argument("wildcard route segment must be final");
            }
            append_dynamic_param_name(route, "*");
            node_value->wildcard_route_index_ = route.index();
            return;
        }
        if (kind == route_segment_kind::param) {
            if (segment.size() == 1) {
                throw std::invalid_argument("route parameter name must not be empty");
            }
            append_dynamic_param_name(route, segment.substr(1));
            if (!node_value->param_child_) {
                plan.dynamic_node_arena_.emplace_back();
                node_value->param_child_ = &plan.dynamic_node_arena_.back();
            }
            node_value = node_value->param_child_;
        } else {
            compiled_route_plan::dynamic_node_type* child_node = nullptr;
            for (auto& child_value : node_value->static_children_) {
                if (uri_paths_equivalent(child_value.segment_, segment)) {
                    child_node = child_value.node_;
                    break;
                }
            }
            if (child_node == nullptr) {
                plan.dynamic_node_arena_.emplace_back();
                child_node = &plan.dynamic_node_arena_.back();
                node_value->static_children_.push_back(
                    compiled_route_plan::dynamic_static_child_type{segment, child_node});
            }
            node_value = child_node;
        }

        if (rest.empty()) {
            node_value->route_index_ = route.index();
            return;
        }
        path = rest;
    }
}

}  // namespace ruvia
