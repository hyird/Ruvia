#include "ruvia/web/detail/router/RouteTable.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace ruvia {

void detail::RouteTable::buildPerfectHash() {
    auto& plan = *ownedPlan_;
    plan.exactSlots_.clear();
    plan.exactSeed_ = 0;
    plan.exactMask_ = 0;

    std::pmr::vector<std::size_t> exactRoutes(resource_);
    exactRoutes.reserve(routes_.size());
    for (std::size_t routeIndex = 0; routeIndex < routes_.size(); ++routeIndex) {
        const auto& route = routes_[routeIndex];
        // Extension routes are resolved by the cold token scan, never by this
        // index. They must also stay out of it: the hash is keyed on the method
        // ENUM and the path, so two extension routes on one path -- which is
        // ordinary, e.g. PROPFIND and PURGE on the same resource -- would be
        // indistinguishable and no seed could ever separate them.
        if (!route.dynamic() && route.method() != HttpKnownMethod::kUnknown) {
            exactRoutes.push_back(routeIndex);
        }
    }

    if (exactRoutes.empty()) {
        return;
    }

    auto slotCount = nextPowerOfTwo(exactRoutes.size());
    std::pmr::vector<std::size_t> candidate(resource_);
    std::pmr::vector<std::uint32_t> candidateMarks(resource_);
    std::uint32_t generation = 0;

    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        const auto mask = slotCount - 1;
        candidate.resize(slotCount);
        candidateMarks.resize(slotCount);
        for (std::uint64_t seed = 0; seed < 4096; ++seed) {
            ++generation;
            bool collision = false;

            for (const auto routeIndex : exactRoutes) {
                const auto& route = routes_[routeIndex];
                const auto index =
                    static_cast<std::size_t>(routeHash(route.method(), route.path(), seed)) & mask;
                if (candidateMarks[index] == generation) {
                    collision = true;
                    break;
                }
                candidateMarks[index] = generation;
                candidate[index] = routeIndex;
            }

            if (!collision) {
                plan.exactSlots_.resize(slotCount);
                for (std::size_t i = 0; i < slotCount; ++i) {
                    plan.exactSlots_[i].routeIndex =
                        candidateMarks[i] == generation ? candidate[i] : kNoRouteIndex;
                }
                plan.exactSeed_ = seed;
                plan.exactMask_ = mask;
                return;
            }
        }

        slotCount <<= 1U;
    }

    // The static index has no fallback, so an unbuildable table must fail the
    // build rather than leave every static route silently unroutable. Reaching
    // here needs 16 doublings (a table 32768x the route count) to have collided
    // under all 4096 seeds, which no real route set can do.
    throw std::logic_error("failed to build the static route index");
}

void detail::RouteTable::buildAllowedMethodMask() {
    auto& plan = *ownedPlan_;
    plan.allowedMethodMask_ = 0;
    plan.staticMethodMask_ = 0;
    for (const auto& route : routes_) {
        if (isRoutableMethod(route.method())) {
            const auto methodBit = 1U << methodIndex(route.method());
            plan.allowedMethodMask_ |= methodBit;
            if (!route.dynamic()) {
                plan.staticMethodMask_ |= methodBit;
            }
        }
    }
    plan.allowedMethodMask_ |= 1U << methodIndex(HttpKnownMethod::kOptions);
    buildServerExtensionMethodTokens();
}

}  // namespace ruvia
