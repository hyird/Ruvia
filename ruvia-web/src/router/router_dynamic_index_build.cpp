#include <algorithm>
#include <stdexcept>
#include <utility>

#include "router/path_segments.h"
#include "router/route_table.h"

namespace ruvia {

void detail::route_table::build_dynamic_routes() {
    auto& plan = *owned_plan_;
    plan.dynamic_method_mask_ = 0;
    plan.dynamic_node_arena_.clear();
    plan.connect_protocols_.clear();
    plan.connect_protocols_.reserve(plan.connect_route_indices_.size());
    for (auto& root : plan.dynamic_roots_) {
        root = dynamic_node_type(plan.resource_);
    }

    std::size_t dynamic_node_capacity = 0;
    for (const auto& route : routes_) {
        if (route.dynamic()) {
            dynamic_node_capacity += dynamic_node_upper_bound(route.path());
        }
    }
    plan.dynamic_node_arena_.reserve(dynamic_node_capacity);
    bind_dynamic_param_names();

    for (std::size_t route_index = 0; route_index < routes_.size(); ++route_index) {
        auto& route = routes_[route_index];
        if (route.dynamic() && route.endpoint().tunnel() != nullptr) {
            const auto protocol = route.endpoint().tunnel()->protocol();
            auto found = std::ranges::find(plan.connect_protocols_, protocol, &compiled_route_plan::connect_protocol_index_type::protocol_);
            if (found == plan.connect_protocols_.end()) {
                found = plan.connect_protocols_.emplace(plan.connect_protocols_.end(), plan.resource_, protocol);
            }
            insert_dynamic(found->root_, route, route_index);
        } else if (route.dynamic()) {
            plan.dynamic_method_mask_ |= 1U << method_index(route.method());
            insert_dynamic(plan.dynamic_roots_[method_index(route.method())], route, route_index);
        }
    }
    for (auto& protocol : plan.connect_protocols_) {
        sort_dynamic_node(protocol.root_);
    }
    for (auto& root : plan.dynamic_roots_) {
        sort_dynamic_node(root);
    }
}

void detail::route_table::bind_dynamic_param_names() {
    dynamic_param_names_.clear();
    std::size_t capacity = 0;
    for (const auto& route : routes_) {
        if (route.dynamic()) {
            capacity += dynamic_param_name_upper_bound(route.path());
        }
    }
    dynamic_param_names_.reserve(capacity);
    for (auto& route : routes_) {
        if (route.dynamic()) {
            bind_dynamic_param_names(route);
        }
    }
}

void detail::route_table::bind_dynamic_param_names(route_entry& route) {
    route.set_param_names({});
    auto path = route.path();
    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!split_path_segment(path, segment, rest)) {
            return;
        }
        if (segment == "*") {
            if (!rest.empty()) {
                throw std::invalid_argument("wildcard route segment must be final");
            }
            append_dynamic_param_name(route, "*");
            return;
        }
        if (!segment.empty() && segment.front() == ':') {
            if (segment.size() == 1) {
                throw std::invalid_argument("route parameter name must not be empty");
            }
            append_dynamic_param_name(route, segment.substr(1));
        }
        if (rest.empty()) {
            return;
        }
        path = rest;
    }
}

bool detail::route_table::is_dynamic_path(std::string_view path) noexcept {
    while (!path.empty()) {
        std::string_view segment;
        std::string_view rest;
        if (!split_path_segment(path, segment, rest)) {
            return false;
        }
        if (segment == "*" || (!segment.empty() && segment.front() == ':')) {
            return true;
        }
        path = rest;
    }

    return false;
}

std::size_t detail::route_table::dynamic_node_upper_bound(std::string_view path) noexcept {
    std::size_t count = 0;
    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!split_path_segment(path, segment, rest)) {
            return count;
        }
        if (segment != "*") {
            ++count;
        }
        if (rest.empty()) {
            return count;
        }
        path = rest;
    }
}

std::size_t detail::route_table::dynamic_param_name_upper_bound(std::string_view path) noexcept {
    std::size_t count = 0;
    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!split_path_segment(path, segment, rest)) {
            return count;
        }
        if (segment == "*" || (!segment.empty() && segment.front() == ':')) {
            ++count;
        }
        if (rest.empty()) {
            return count;
        }
        path = rest;
    }
}

void detail::route_table::append_dynamic_param_name(route_entry& route, std::string_view name) {
    const auto names = route.param_names();
    if (names.size() >= max_route_params) {
        throw std::invalid_argument("route has too many parameters");
    }

    const auto offset = names.empty()
                            ? dynamic_param_names_.size()
                            : static_cast<std::size_t>(names.data() - dynamic_param_names_.data());
    dynamic_param_names_.push_back(name);
    route.set_param_names(
        std::span<const std::string_view>(dynamic_param_names_.data() + offset, names.size() + 1));
}

void detail::route_table::insert_dynamic(
    dynamic_node_type& root, route_entry& route, std::size_t route_index) {
    auto path = route.path();
    auto* node_value = &root;
    auto& plan = *owned_plan_;

    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!split_path_segment(path, segment, rest)) {
            node_value->route_index_ = route_index;
            return;
        }
        if (segment == "*") {
            if (!rest.empty()) {
                throw std::invalid_argument("wildcard route segment must be final");
            }
            node_value->wildcard_route_index_ = route_index;
            return;
        }
        if (!segment.empty() && segment.front() == ':') {
            if (segment.size() == 1) {
                throw std::invalid_argument("route parameter name must not be empty");
            }
            if (!node_value->param_child_) {
                plan.dynamic_node_arena_.emplace_back(plan.resource_);
                node_value->param_child_ = &plan.dynamic_node_arena_.back();
            }
            node_value = node_value->param_child_;
        } else {
            auto* child_node = static_cast<dynamic_node_type*>(nullptr);
            for (auto& child_value : node_value->static_children_) {
                if (child_value.segment_ == segment) {
                    child_node = child_value.node_;
                    break;
                }
            }
            if (child_node == nullptr) {
                plan.dynamic_node_arena_.emplace_back(plan.resource_);
                child_node = &plan.dynamic_node_arena_.back();
                auto child_value =
                    dynamic_static_child_type{std::pmr::string(segment, plan.resource_), child_node};
                node_value->static_children_.push_back(std::move(child_value));
            }
            node_value = child_node;
        }

        if (rest.empty()) {
            node_value->route_index_ = route_index;
            return;
        }
        path = rest;
    }
}

void detail::route_table::sort_dynamic_node(dynamic_node_type& node_value) {
    std::ranges::sort(
        node_value.static_children_, [](const dynamic_static_child_type& left, const dynamic_static_child_type& right) {
            return std::string_view(left.segment_) < std::string_view(right.segment_);
        });
    for (auto& child : node_value.static_children_) {
        sort_dynamic_node(*child.node_);
    }
    if (node_value.param_child_) {
        sort_dynamic_node(*node_value.param_child_);
    }
}

// find_dynamic_node orders every pair of distinct trie branches -- a literal child before the
// parameter child before the wildcard, backtracking on failure -- so two dynamic routes are only
// ambiguous when they occupy the same trie slot: segment by segment both are the same literal, both
// are parameters (whatever their names), or both are the wildcard, and both end at the same depth.
// Any other pair overlaps at most with a deterministic winner ("/a/*" vs "/:x/:y", "/a/*" vs
// "/a/:x"), which is legal routing, not a conflict.
bool detail::route_table::same_dynamic_shape(std::string_view left, std::string_view right) noexcept {
    const auto is_param = [](std::string_view segment) noexcept {
        return !segment.empty() && segment.front() == ':';
    };
    for (;;) {
        std::string_view left_segment;
        std::string_view left_rest;
        std::string_view right_segment;
        std::string_view right_rest;
        const auto has_left = split_path_segment(left, left_segment, left_rest);
        const auto has_right = split_path_segment(right, right_segment, right_rest);
        if (!has_left || !has_right) {
            return has_left == has_right;
        }

        const auto both_params = is_param(left_segment) && is_param(right_segment);
        if (!both_params && left_segment != right_segment) {
            return false;
        }

        left = left_rest;
        right = right_rest;
    }
}

}  // namespace ruvia
