#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/url_encoding.h"

#include "router/router_impl.h"

namespace ruvia {

void detail::router::build_static_index() {
    auto& plan = *plan_;

    // Extension tokens and CONNECT have separate routing semantics.
    const auto is_indexed = [](const route_entry& route) {
        return !route.dynamic() && route_table::is_routable_method(route.method());
    };
    const auto route_count = static_cast<std::size_t>(std::ranges::count_if(plan.routes_, is_indexed));
    if (route_count == 0) {
        return;
    }
    if (route_count > plan.static_slots_.max_size() / 2) {
        throw std::length_error("too many static routes");
    }

    // At most half full: storage stays linear and a probe always reaches an
    // empty slot. Compilation owns the only allocation; lookups only borrow.
    plan.static_slots_.resize(std::bit_ceil(route_count * 2));
    plan.static_slot_mask_ = plan.static_slots_.size() - 1;
    for (const auto& route : plan.routes_) {
        if (!is_indexed(route)) {
            continue;
        }
        const auto hash = route_table::route_hash(route.method(), route_table::path_hash(route.path()));
        auto slot_index = static_cast<std::size_t>(hash) & plan.static_slot_mask_;
        while (plan.static_slots_[slot_index].route_index_ != compiled_route_plan::no_route_index) {
            slot_index = (slot_index + 1) & plan.static_slot_mask_;
        }
        plan.static_slots_[slot_index] = {hash, route.index()};
    }
}

void detail::router::build_static_method_mask() {
    auto& plan = *plan_;
    for (const auto& route : plan.routes_) {
        if (route_table::is_routable_method(route.method()) && !route.dynamic()) {
            plan.static_method_mask_ |= 1U << route_table::method_index(route.method());
        }
    }
}

void detail::router::build_extension_index() {
    auto& plan = *plan_;
    auto& indices = plan.extension_route_indices_;
    // Group canonical paths once; route indices preserve registration order
    // within each Allow set without a second index or temporary storage.
    std::ranges::sort(indices, [&plan](std::size_t left, std::size_t right) noexcept {
        const auto order = compare_uri_paths(plan.routes_[left].path(), plan.routes_[right].path());
        return order != 0 ? order < 0 : left < right;
    });
    // Every route contributes one token. Reserve before publishing spans.
    plan.extension_methods_.reserve(indices.size());
    for (std::size_t first = 0; first < indices.size();) {
        const auto path = plan.routes_[indices[first]].path();
        const auto offset = plan.extension_methods_.size();
        auto last = first;
        do {
            plan.extension_methods_.push_back(plan.routes_[indices[last]].method_token());
            ++last;
        } while (last < indices.size() && uri_paths_equivalent(plan.routes_[indices[last]].path(), path));
        plan.extension_paths_.push_back({.path_ = path,
            .methods_ = std::span<const std::string_view>(plan.extension_methods_).subspan(offset)});
        first = last;
    }
    // The same storage serves exact (case-sensitive token, canonical path)
    // lookups after compilation; the path groups above borrow route storage.
    std::ranges::sort(indices, [&plan](std::size_t left, std::size_t right) noexcept {
        const auto& left_route = plan.routes_[left];
        const auto& right_route = plan.routes_[right];
        if (left_route.method_token() != right_route.method_token()) {
            return left_route.method_token() < right_route.method_token();
        }
        return compare_uri_paths(left_route.path(), right_route.path()) < 0;
    });
}

}  // namespace ruvia
