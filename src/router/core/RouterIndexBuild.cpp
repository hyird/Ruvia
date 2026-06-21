#include "../RouteTable.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "RouterUtils.h"

namespace ruvia {

void detail::RouteTable::buildPerfectHash() {
    exactSlots_.clear();
    exactSeed_ = 0;
    exactMask_ = 0;

    std::pmr::vector<const RouteEntry*> exactRoutes(startupResource());
    exactRoutes.reserve(routes_.size());
    for (const auto& route : routes_) {
        if (!route.dynamic) {
            exactRoutes.push_back(&route);
        }
    }

    if (exactRoutes.empty()) {
        return;
    }

    auto slotCount = nextPowerOfTwo(exactRoutes.size());
    std::pmr::vector<const RouteEntry*> candidate(startupResource());

    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        const auto mask = slotCount - 1;
        for (std::uint64_t seed = 0; seed < 4096; ++seed) {
            candidate.assign(slotCount, nullptr);
            bool collision = false;

            for (const auto* route : exactRoutes) {
                const auto index = static_cast<std::size_t>(routeHash(route->method, route->path, seed)) & mask;
                if (candidate[index] != nullptr) {
                    collision = true;
                    break;
                }
                candidate[index] = route;
            }

            if (!collision) {
                exactSlots_.resize(slotCount);
                for (std::size_t i = 0; i < slotCount; ++i) {
                    exactSlots_[i].route = candidate[i];
                }
                exactSeed_ = seed;
                exactMask_ = mask;
                return;
            }
        }

        slotCount <<= 1U;
    }
}

void detail::RouteTable::buildRadix() {
    radixRoots_ = {};
    for (const auto& route : routes_) {
        if (route.dynamic) {
            continue;
        }
        insertRadix(radixRoots_[methodIndex(route.method)], route.path, route);
    }
}

void detail::RouteTable::buildAllowedMethodMask() noexcept {
    allowedMethodMask_ = 0;
    for (const auto& route : routes_) {
        if (isRoutableMethod(route.method)) {
            allowedMethodMask_ |= 1U << methodIndex(route.method);
        }
    }
    allowedMethodMask_ |= 1U << methodIndex(HttpMethod::kOptions);
}

void detail::RouteTable::insertRadix(RadixNode& node, std::string_view path, const RouteEntry& route) {
    if (path.empty()) {
        node.route = &route;
        return;
    }

    for (auto& child : node.children) {
        const auto prefixLength = commonPrefixLength(child.label, path);
        if (prefixLength == 0) {
            continue;
        }

        if (prefixLength == child.label.size()) {
            insertRadix(child, path.substr(prefixLength), route);
            return;
        }

        auto oldLabel = std::move(child.label);
        auto oldChildren = std::move(child.children);
        auto* oldRoute = child.route;

        RadixNode suffix;
        suffix.label.assign(oldLabel.data() + prefixLength, oldLabel.size() - prefixLength);
        suffix.children = std::move(oldChildren);
        suffix.route = oldRoute;

        child.label.assign(oldLabel.data(), prefixLength);
        child.children.clear();
        child.children.push_back(std::move(suffix));
        child.route = nullptr;

        if (prefixLength == path.size()) {
            child.route = &route;
        } else {
            RadixNode branch;
            branch.label.assign(path.data() + prefixLength, path.size() - prefixLength);
            branch.route = &route;
            child.children.push_back(std::move(branch));
        }
        return;
    }

    RadixNode child;
    child.label.assign(path.data(), path.size());
    child.route = &route;
    node.children.push_back(std::move(child));
}

}  // namespace ruvia
