#include <algorithm>

#include "router/path_segments.h"
#include "router/route_table.h"

namespace ruvia {

std::size_t detail::route_table::find_dynamic_node(
    const dynamic_node_type& node_value, std::string_view path, route_match& match) noexcept {
    std::string_view segment;
    std::string_view rest;
    if (!split_request_path_segment(path, segment, rest)) {
        if (node_value.route_index_ != no_route_index) {
            return node_value.route_index_;
        }
        if (node_value.wildcard_route_index_ != no_route_index && add_param(match, {})) {
            return node_value.wildcard_route_index_;
        }
        return no_route_index;
    }

    const auto original_param_count = match.size();
    if (const auto* static_child = find_dynamic_static_child(node_value, segment); static_child != nullptr) {
        if (const auto route_index = find_dynamic_node(*static_child->node_, rest, match);
            route_index != no_route_index) {
            return route_index;
        }
        match.truncate(original_param_count);
    }

    if (node_value.param_child_ && !segment.empty() && add_param(match, segment)) {
        if (const auto route_index = find_dynamic_node(*node_value.param_child_, rest, match);
            route_index != no_route_index) {
            return route_index;
        }
        match.truncate(original_param_count);
    }

    if (node_value.wildcard_route_index_ != no_route_index) {
        auto capture_value = path;
        if (capture_value.starts_with('/')) {
            capture_value.remove_prefix(1);
        }
        if (add_param(match, capture_value)) {
            return node_value.wildcard_route_index_;
        }
        match.truncate(original_param_count);
    }

    return no_route_index;
}

std::size_t detail::route_table::find_dynamic_node_no_params(
    const dynamic_node_type& node_value, std::string_view path) noexcept {
    std::string_view segment;
    std::string_view rest;
    if (!split_request_path_segment(path, segment, rest)) {
        return node_value.route_index_ != no_route_index ? node_value.route_index_ : node_value.wildcard_route_index_;
    }

    if (const auto* static_child = find_dynamic_static_child(node_value, segment); static_child != nullptr) {
        if (const auto route_index = find_dynamic_node_no_params(*static_child->node_, rest);
            route_index != no_route_index) {
            return route_index;
        }
    }

    if (node_value.param_child_ != nullptr && !segment.empty()) {
        if (const auto route_index = find_dynamic_node_no_params(*node_value.param_child_, rest);
            route_index != no_route_index) {
            return route_index;
        }
    }

    return node_value.wildcard_route_index_;
}

const detail::route_table::dynamic_static_child_type* detail::route_table::find_dynamic_static_child(
    const dynamic_node_type& node_value, std::string_view segment) noexcept {
    if (node_value.static_children_.size() <= 4) {
        for (const auto& child : node_value.static_children_) {
            if (child.segment_ == segment) {
                return &child;
            }
        }
        return nullptr;
    }

    const auto iter = std::ranges::lower_bound(node_value.static_children_, segment, std::ranges::less{},
        [](const dynamic_static_child_type& child_value) noexcept { return std::string_view(child_value.segment_); });
    if (iter != node_value.static_children_.end() && std::string_view(iter->segment_) == segment) {
        return &*iter;
    }
    return nullptr;
}

bool detail::route_table::add_param(route_match& match, std::string_view value) noexcept {
    return match.add(value);
}

const detail::route_entry* detail::route_table::find_dynamic_route(
    http_known_method method, std::string_view path, route_match& match) const noexcept {
    match.clear();
    if (!is_routable_method(method)) {
        return nullptr;
    }

    const auto method_bit = 1U << method_index(method);
    return (plan_->dynamic_method_mask_ & method_bit) != 0 ? find_dynamic(method, path, match)
                                                           : nullptr;
}

const detail::route_entry* detail::route_table::find_dynamic(
    http_known_method method, std::string_view path, route_match& match) const noexcept {
    match.clear();
    const auto route_index = find_dynamic_node(plan_->dynamic_roots_[method_index(method)], path, match);
    if (route_index == no_route_index || match.size() != routes_[route_index].param_names().size()) {
        match.clear();
        return nullptr;
    }
    return &routes_[route_index];
}

}  // namespace ruvia
