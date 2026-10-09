#include <algorithm>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "router/route_table.h"

namespace ruvia {

void detail::route_table::build_static_index() {
    auto& plan = *owned_plan_;
    plan.static_slots_.clear();
    plan.static_slot_mask_ = 0;

    // Extension tokens and CONNECT have separate routing semantics.
    const auto is_indexed = [](const route_entry& route) {
        return !route.dynamic() && is_routable_method(route.method());
    };
    const auto route_count = static_cast<std::size_t>(std::ranges::count_if(routes_, is_indexed));
    if (route_count == 0) {
        return;
    }
    if (route_count > plan.static_slots_.max_size() / 2) {
        throw std::length_error("too many static routes");
    }

    // At most half full: storage stays linear and a probe always reaches an
    // empty slot. Finalization owns the only allocation; lookups only borrow.
    plan.static_slots_.resize(std::bit_ceil(route_count * 2));
    plan.static_slot_mask_ = plan.static_slots_.size() - 1;
    for (std::size_t route_index = 0; route_index < routes_.size(); ++route_index) {
        const auto& route = routes_[route_index];
        if (!is_indexed(route)) {
            continue;
        }
        const auto hash = route_hash(route.method(), path_hash(route.path()));
        auto slot_index = static_cast<std::size_t>(hash) & plan.static_slot_mask_;
        while (plan.static_slots_[slot_index].route_index_ != no_route_index) {
            slot_index = (slot_index + 1) & plan.static_slot_mask_;
        }
        plan.static_slots_[slot_index] = {hash, route_index};
    }
}

void detail::route_table::build_allowed_method_mask() {
    auto& plan = *owned_plan_;
    plan.allowed_method_mask_ = 0;
    plan.static_method_mask_ = 0;
    for (const auto& route : routes_) {
        if (is_routable_method(route.method())) {
            const auto method_bit = 1U << method_index(route.method());
            plan.allowed_method_mask_ |= method_bit;
            if (supports_head_fallback(route)) {
                plan.allowed_method_mask_ |= 1U << method_index(http_known_method::head);
            }
            if (!route.dynamic()) {
                plan.static_method_mask_ |= method_bit;
            }
        }
    }
    plan.allowed_method_mask_ |= 1U << method_index(http_known_method::options);
    build_server_extension_method_tokens();
}

}  // namespace ruvia
