#include "../RouteTable.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "RouterUtils.h"

namespace ruvia {

void detail::RouteTable::buildDynamicRoutes() {
    hasDynamicRoutes_.fill(false);
    dynamicNodeArena_.clear();
    for (auto& root : dynamicRoots_) {
        root = DynamicNode{};
    }

    std::size_t dynamicNodeCapacity = 0;
    for (const auto& route : routes_) {
        if (route.dynamic) {
            dynamicNodeCapacity += dynamicNodeUpperBound(route.path);
        }
    }
    dynamicNodeArena_.reserve(dynamicNodeCapacity);

    for (auto& route : routes_) {
        if (route.dynamic) {
            hasDynamicRoutes_[methodIndex(route.method)] = true;
            collectDynamicParamNames(route);
            insertDynamic(dynamicRoots_[methodIndex(route.method)], route);
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
        if (!splitSegment(path, segment, rest)) {
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
        if (!splitSegment(path, segment, rest)) {
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

void detail::RouteTable::collectDynamicParamNames(RouteEntry& route) {
    route.paramCount = 0;
    auto path = std::string_view(route.path);

    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!splitSegment(path, segment, rest)) {
            return;
        }
        if (segment.empty()) {
            throw std::invalid_argument("dynamic route path must not contain empty segments");
        }
        if (segment == "*") {
            if (!rest.empty()) {
                throw std::invalid_argument("wildcard route segment must be final");
            }
            if (route.paramCount >= route.paramNames.size()) {
                throw std::invalid_argument("route has too many parameters");
            }
            route.paramNames[route.paramCount++] = "*";
            return;
        }
        if (segment.front() == ':') {
            if (segment.size() == 1) {
                throw std::invalid_argument("route parameter name must not be empty");
            }
            if (route.paramCount >= route.paramNames.size()) {
                throw std::invalid_argument("route has too many parameters");
            }
            route.paramNames[route.paramCount++] = segment.substr(1);
        }

        if (rest.empty()) {
            return;
        }
        path = rest;
    }
}

void detail::RouteTable::insertDynamic(DynamicNode& root, const RouteEntry& route) {
    auto path = std::string_view(route.path);
    auto* node = &root;

    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!splitSegment(path, segment, rest)) {
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
            node->wildcardRoute = &route;
            return;
        }
        if (segment.front() == ':') {
            if (segment.size() == 1) {
                throw std::invalid_argument("route parameter name must not be empty");
            }

            if (!node->paramChild) {
                dynamicNodeArena_.emplace_back();
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
                dynamicNodeArena_.emplace_back();
                childNode = &dynamicNodeArena_.back();
                auto child = DynamicStaticChild{std::pmr::string(segment, startupResource()), childNode};
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
    std::sort(
        node.staticChildren.begin(),
        node.staticChildren.end(),
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
    std::size_t depth = 0;
    for (;;) {
        std::string_view leftSegment;
        std::string_view leftRest;
        std::string_view rightSegment;
        std::string_view rightRest;
        const auto hasLeft = splitSegment(left, leftSegment, leftRest);
        const auto hasRight = splitSegment(right, rightSegment, rightRest);
        if (!hasLeft || !hasRight) {
            return hasLeft == hasRight;
        }

        if (leftSegment == "*" || rightSegment == "*") {
            if (depth == 0 && leftSegment != rightSegment) {
                return false;
            }
            return true;
        }

        const auto leftParam = !leftSegment.empty() && leftSegment.front() == ':';
        const auto rightParam = !rightSegment.empty() && rightSegment.front() == ':';
        if (!leftParam && !rightParam && leftSegment != rightSegment) {
            return false;
        }

        left = leftRest;
        right = rightRest;
        ++depth;
    }
}

}  // namespace ruvia
