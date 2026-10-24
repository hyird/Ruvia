#include <algorithm>
#include <stdexcept>
#include <utility>

#include "ruvia/http/url_encoding.h"

#include "router/path_segments.h"
#include "router/router_impl.h"

namespace ruvia {
namespace {

[[nodiscard]] std::size_t dynamic_node_upper_bound(std::string_view path) noexcept {
    std::size_t count = 0;
    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!detail::split_path_segment(path, segment, rest)) {
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

[[nodiscard]] std::size_t dynamic_param_name_upper_bound(std::string_view path) noexcept {
    std::size_t count = 0;
    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!detail::split_path_segment(path, segment, rest)) {
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
            dynamic_node_capacity += dynamic_node_upper_bound(route.path());
            param_name_capacity += dynamic_param_name_upper_bound(route.path());
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
        bind_dynamic_param_names(route);
        if (const auto* tunnel = route.endpoint().tunnel()) {
            const auto protocol = tunnel->protocol();
            auto found = std::ranges::find(
                plan.connect_protocols_, protocol, &compiled_route_plan::connect_protocol_index_type::protocol_);
            if (found == plan.connect_protocols_.end()) {
                found = plan.connect_protocols_.emplace(plan.connect_protocols_.end(), plan.resource_, protocol);
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

void detail::router::bind_dynamic_param_names(route_entry& route) {
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

bool detail::router::is_dynamic_path(std::string_view path) noexcept {
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

void detail::router::insert_dynamic(compiled_route_plan::dynamic_node_type& root, const route_entry& route) {
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
        if (segment == "*") {
            if (!rest.empty()) {
                throw std::invalid_argument("wildcard route segment must be final");
            }
            node_value->wildcard_route_index_ = route.index();
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
            compiled_route_plan::dynamic_node_type* child_node = nullptr;
            for (auto& child_value : node_value->static_children_) {
                if (uri_paths_equivalent(child_value.segment_, segment)) {
                    child_node = child_value.node_;
                    break;
                }
            }
            if (child_node == nullptr) {
                plan.dynamic_node_arena_.emplace_back(plan.resource_);
                child_node = &plan.dynamic_node_arena_.back();
                auto child_value = compiled_route_plan::dynamic_static_child_type{
                    std::pmr::string(segment, plan.resource_), child_node};
                node_value->static_children_.push_back(std::move(child_value));
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

// find_dynamic_node orders every pair of distinct trie branches -- a literal child before the
// parameter child before the wildcard, backtracking on failure -- so two dynamic routes are only
// ambiguous when they occupy the same trie slot: segment by segment both are the same literal, both
// are parameters (whatever their names), or both are the wildcard, and both end at the same depth.
// Any other pair overlaps at most with a deterministic winner ("/a/*" vs "/:x/:y", "/a/*" vs
// "/a/:x"), which is legal routing, not a conflict.
bool detail::router::same_dynamic_shape(std::string_view left, std::string_view right) noexcept {
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

        // Literal segments occupy one trie slot when their canonical forms
        // match; a parameter or the wildcard is a slot of its own kind.
        const auto left_literal = !is_param(left_segment) && left_segment != "*";
        const auto right_literal = !is_param(right_segment) && right_segment != "*";
        if (left_literal != right_literal) {
            return false;
        }
        if (left_literal ? !uri_paths_equivalent(left_segment, right_segment)
                         : is_param(left_segment) != is_param(right_segment)) {
            return false;
        }

        left = left_rest;
        right = right_rest;
    }
}

}  // namespace ruvia
