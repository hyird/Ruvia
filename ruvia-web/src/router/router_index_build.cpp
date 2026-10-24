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

void detail::router::build_extension_paths() {
    auto& plan = *plan_;
    // Every token lands in exactly one group, so reserving the token count up
    // front keeps the spans into extension_methods_ valid while it is filled.
    plan.extension_methods_.reserve(plan.extension_route_indices_.size());
    for (std::size_t i = 0; i < plan.extension_route_indices_.size(); ++i) {
        const auto path = plan.routes_[plan.extension_route_indices_[i]].path();
        const auto seen = std::ranges::any_of(plan.extension_paths_,
            [path](const auto& entry) noexcept { return uri_paths_equivalent(entry.path_, path); });
        if (seen) {
            continue;
        }
        // Registration rejects a repeated (token, path) pair, so the tokens
        // collected for one path are already distinct.
        const auto offset = plan.extension_methods_.size();
        for (std::size_t j = i; j < plan.extension_route_indices_.size(); ++j) {
            const auto& route = plan.routes_[plan.extension_route_indices_[j]];
            if (uri_paths_equivalent(route.path(), path)) {
                plan.extension_methods_.push_back(route.method_token());
            }
        }
        plan.extension_paths_.push_back({.path_ = path,
            .methods_ = std::span<const std::string_view>(plan.extension_methods_).subspan(offset)});
    }
}

}  // namespace ruvia
