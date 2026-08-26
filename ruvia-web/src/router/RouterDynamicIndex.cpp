#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/router/PathSegments.h"

#include <algorithm>

namespace ruvia {

std::size_t detail::RouteTable::findDynamicNode(
    const DynamicNode& node, std::string_view path, RouteMatch& match) noexcept {
    std::string_view segment;
    std::string_view rest;
    if (!splitRequestPathSegment(path, segment, rest)) {
        if (node.routeIndex != kNoRouteIndex) {
            return node.routeIndex;
        }
        if (node.wildcardRouteIndex != kNoRouteIndex && addParam(match, {})) {
            return node.wildcardRouteIndex;
        }
        return kNoRouteIndex;
    }

    const auto originalParamCount = match.size();
    if (const auto* staticChild = findDynamicStaticChild(node, segment); staticChild != nullptr) {
        if (const auto routeIndex = findDynamicNode(*staticChild->node, rest, match);
            routeIndex != kNoRouteIndex) {
            return routeIndex;
        }
        match.truncate(originalParamCount);
    }

    if (node.paramChild && !segment.empty() && addParam(match, segment)) {
        if (const auto routeIndex = findDynamicNode(*node.paramChild, rest, match);
            routeIndex != kNoRouteIndex) {
            return routeIndex;
        }
        match.truncate(originalParamCount);
    }

    if (node.wildcardRouteIndex != kNoRouteIndex) {
        auto capture = path;
        if (capture.starts_with('/')) {
            capture.remove_prefix(1);
        }
        if (addParam(match, capture)) {
            return node.wildcardRouteIndex;
        }
        match.truncate(originalParamCount);
    }

    return kNoRouteIndex;
}

std::size_t detail::RouteTable::findDynamicNodeNoParams(
    const DynamicNode& node, std::string_view path) noexcept {
    std::string_view segment;
    std::string_view rest;
    if (!splitRequestPathSegment(path, segment, rest)) {
        return node.routeIndex != kNoRouteIndex ? node.routeIndex : node.wildcardRouteIndex;
    }

    if (const auto* staticChild = findDynamicStaticChild(node, segment); staticChild != nullptr) {
        if (const auto routeIndex = findDynamicNodeNoParams(*staticChild->node, rest);
            routeIndex != kNoRouteIndex) {
            return routeIndex;
        }
    }

    if (node.paramChild != nullptr && !segment.empty()) {
        if (const auto routeIndex = findDynamicNodeNoParams(*node.paramChild, rest);
            routeIndex != kNoRouteIndex) {
            return routeIndex;
        }
    }

    return node.wildcardRouteIndex;
}

const detail::RouteTable::DynamicStaticChild* detail::RouteTable::findDynamicStaticChild(
    const DynamicNode& node, std::string_view segment) noexcept {
    if (node.staticChildren.size() <= 4) {
        for (const auto& child : node.staticChildren) {
            if (child.segment == segment) {
                return &child;
            }
        }
        return nullptr;
    }

    const auto iter = std::ranges::lower_bound(node.staticChildren, segment, std::ranges::less{},
        [](const DynamicStaticChild& child) noexcept { return std::string_view(child.segment); });
    if (iter != node.staticChildren.end() && std::string_view(iter->segment) == segment) {
        return &*iter;
    }
    return nullptr;
}

bool detail::RouteTable::addParam(RouteMatch& match, std::string_view value) noexcept {
    return match.add(value);
}

const detail::RouteEntry* detail::RouteTable::findDynamicRoute(
    HttpKnownMethod method, std::string_view path, RouteMatch& match) const noexcept {
    match.clear();
    if (!isRoutableMethod(method)) {
        return nullptr;
    }

    const auto methodBit = 1U << methodIndex(method);
    return (plan_->dynamicMethodMask_ & methodBit) != 0 ? findDynamic(method, path, match)
                                                        : nullptr;
}

const detail::RouteEntry* detail::RouteTable::findDynamic(
    HttpKnownMethod method, std::string_view path, RouteMatch& match) const noexcept {
    match.clear();
    const auto routeIndex = findDynamicNode(plan_->dynamicRoots_[methodIndex(method)], path, match);
    if (routeIndex == kNoRouteIndex || match.size() != routes_[routeIndex].paramNames().size()) {
        match.clear();
        return nullptr;
    }
    return &routes_[routeIndex];
}

}  // namespace ruvia
