#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/router/PathSegments.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace ruvia {

void detail::RouteTable::buildDynamicRoutes() {
    dynamicMethodMask_ = 0;
    dynamicNodeArena_.clear();
    dynamicParamNames_.clear();
    for (auto& root : dynamicRoots_) {
        root = DynamicNode(resource_);
    }

    std::size_t dynamicNodeCapacity = 0;
    std::size_t dynamicParamNameCapacity = 0;
    for (const auto& route : routes_) {
        if (route.dynamic()) {
            dynamicNodeCapacity += dynamicNodeUpperBound(route.path());
            dynamicParamNameCapacity += dynamicParamNameUpperBound(route.path());
        }
    }
    dynamicNodeArena_.reserve(dynamicNodeCapacity);
    dynamicParamNames_.reserve(dynamicParamNameCapacity);

    for (auto& route : routes_) {
        if (route.dynamic()) {
            dynamicMethodMask_ |= 1U << methodIndex(route.method());
            insertDynamic(dynamicRoots_[methodIndex(route.method())], route);
        }
    }
    for (auto& root : dynamicRoots_) {
        sortDynamicNode(root);
    }
}

bool detail::RouteTable::isDynamicPath(std::string_view path) noexcept {
    while (!path.empty()) {
        std::string_view segment;
        std::string_view rest;
        if (!splitRoutePathSegment(path, segment, rest)) {
            return false;
        }
        if (segment == "*" || (!segment.empty() && segment.front() == ':')) {
            return true;
        }
        path = rest;
    }

    return false;
}

std::size_t detail::RouteTable::dynamicNodeUpperBound(std::string_view path) noexcept {
    std::size_t count = 0;
    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!splitRoutePathSegment(path, segment, rest)) {
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

std::size_t detail::RouteTable::dynamicParamNameUpperBound(std::string_view path) noexcept {
    std::size_t count = 0;
    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!splitRoutePathSegment(path, segment, rest)) {
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

void detail::RouteTable::appendDynamicParamName(RouteEntry& route, std::string_view name) {
    const auto names = route.paramNames();
    if (names.size() >= kMaxRouteParams) {
        throw std::invalid_argument("route has too many parameters");
    }

    const auto offset = names.empty()
        ? dynamicParamNames_.size()
        : static_cast<std::size_t>(names.data() - dynamicParamNames_.data());
    dynamicParamNames_.push_back(name);
    route.setParamNames(std::span<const std::string_view>(
        dynamicParamNames_.data() + offset,
        names.size() + 1));
}

void detail::RouteTable::insertDynamic(DynamicNode& root, RouteEntry& route) {
    auto path = route.path();
    auto* node = &root;
    route.setParamNames({});

    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!splitRoutePathSegment(path, segment, rest)) {
            node->route = &route;
            return;
        }
        if (segment.empty()) {
            throw std::invalid_argument("dynamic route path must not contain empty segments");
        }
        if (segment == "*") {
            if (!rest.empty()) {
                throw std::invalid_argument("wildcard route segment must be final");
            }
            appendDynamicParamName(route, "*");
            node->wildcardRoute = &route;
            return;
        }
        if (segment.front() == ':') {
            if (segment.size() == 1) {
                throw std::invalid_argument("route parameter name must not be empty");
            }
            appendDynamicParamName(route, segment.substr(1));

            if (!node->paramChild) {
                dynamicNodeArena_.emplace_back(resource_);
                node->paramChild = &dynamicNodeArena_.back();
            }
            node = node->paramChild;
        } else {
            auto* childNode = static_cast<DynamicNode*>(nullptr);
            for (auto& child : node->staticChildren) {
                if (child.segment == segment) {
                    childNode = child.node;
                    break;
                }
            }
            if (childNode == nullptr) {
                dynamicNodeArena_.emplace_back(resource_);
                childNode = &dynamicNodeArena_.back();
                auto child = DynamicStaticChild{std::pmr::string(segment, resource_), childNode};
                node->staticChildren.push_back(std::move(child));
            }
            node = childNode;
        }

        if (rest.empty()) {
            node->route = &route;
            return;
        }
        path = rest;
    }
}

void detail::RouteTable::sortDynamicNode(DynamicNode& node) {
    std::ranges::sort(
        node.staticChildren,
        [](const DynamicStaticChild& left, const DynamicStaticChild& right) {
            return std::string_view(left.segment) < std::string_view(right.segment);
        });
    for (auto& child : node.staticChildren) {
        sortDynamicNode(*child.node);
    }
    if (node.paramChild) {
        sortDynamicNode(*node.paramChild);
    }
}

bool detail::RouteTable::sameDynamicShape(std::string_view left, std::string_view right) noexcept {
    enum class ForkPriority : std::uint8_t {
        kShared,
        kLeftStatic,
        kRightStatic
    };

    // Tracks the first static-vs-param fork. After that fork, the static side has runtime priority
    // for overlapping paths because findDynamicNode tries static children before param children.
    auto priority = ForkPriority::kShared;
    for (;;) {
        std::string_view leftSegment;
        std::string_view leftRest;
        std::string_view rightSegment;
        std::string_view rightRest;
        const auto hasLeft = splitRoutePathSegment(left, leftSegment, leftRest);
        const auto hasRight = splitRoutePathSegment(right, rightSegment, rightRest);
        if (!hasLeft || !hasRight) {
            return hasLeft == hasRight && priority == ForkPriority::kShared;
        }

        if (leftSegment == "*" || rightSegment == "*") {
            if (leftSegment == rightSegment) {
                return priority == ForkPriority::kShared;
            }

            if (priority == ForkPriority::kShared) {
                // At a shared node a wildcard is distinguished by a STATIC sibling (findDynamicNode
                // tries the static child before the wildcard), so it is not a conflict — unless both
                // sides are dynamic, in which case the wildcard shadows the sibling param. This holds
                // at any depth, not just the root: e.g. "/files/*" + "/files/public/:id" is fine, but
                // "/a/*" + "/a/:x" conflicts.
                const auto leftDynamic =
                    leftSegment == "*" || (!leftSegment.empty() && leftSegment.front() == ':');
                const auto rightDynamic =
                    rightSegment == "*" || (!rightSegment.empty() && rightSegment.front() == ':');
                if (!(leftDynamic && rightDynamic)) {
                    return false;
                }
            }

            const auto leftWildcard = leftSegment == "*";
            if (priority == ForkPriority::kLeftStatic) {
                return leftWildcard;
            }
            if (priority == ForkPriority::kRightStatic) {
                return !leftWildcard;
            }
            return true;
        }

        const auto leftParam = !leftSegment.empty() && leftSegment.front() == ':';
        const auto rightParam = !rightSegment.empty() && rightSegment.front() == ':';
        if (!leftParam && !rightParam && leftSegment != rightSegment) {
            return false;
        }
        if (priority == ForkPriority::kShared && leftParam != rightParam) {
            priority = leftParam ? ForkPriority::kRightStatic : ForkPriority::kLeftStatic;
        }

        left = leftRest;
        right = rightRest;
    }
}

}  // namespace ruvia
