#include "ruvia/web/detail/router/RouteTable.h"

#include <algorithm>

namespace ruvia {

const detail::RouteEntry* detail::RouteTable::findDynamicNode(
    const DynamicNode& node,
    std::string_view path,
    RouteMatch& match) noexcept {
    std::string_view segment;
    std::string_view rest;
    if (!splitSegmentStrict(path, segment, rest)) {
        if (node.route != nullptr) {
            return node.route;
        }
        if (node.wildcardRoute != nullptr && addParam(match, {})) {
            return node.wildcardRoute;
        }
        return nullptr;
    }

    const auto originalParamCount = match.size();
    if (const auto* staticChild = findDynamicStaticChild(node, segment); staticChild != nullptr) {
        if (const auto* route = findDynamicNode(*staticChild->node, rest, match); route != nullptr) {
            return route;
        }
        match.truncate(originalParamCount);
    }

    if (node.paramChild && !segment.empty() && addParam(match, segment)) {
        if (const auto* route = findDynamicNode(*node.paramChild, rest, match); route != nullptr) {
            return route;
        }
        match.truncate(originalParamCount);
    }

    if (node.wildcardRoute != nullptr) {
        auto capture = path;
        if (capture.starts_with('/')) {
            capture.remove_prefix(1);
        }
        if (addParam(match, capture)) {
            return node.wildcardRoute;
        }
        match.truncate(originalParamCount);
    }

    return nullptr;
}

const detail::RouteEntry* detail::RouteTable::findDynamicNodeNoParams(
    const DynamicNode& node,
    std::string_view path) noexcept {
    std::string_view segment;
    std::string_view rest;
    if (!splitSegmentStrict(path, segment, rest)) {
        return node.route != nullptr ? node.route : node.wildcardRoute;
    }

    if (const auto* staticChild = findDynamicStaticChild(node, segment); staticChild != nullptr) {
        if (const auto* route = findDynamicNodeNoParams(*staticChild->node, rest); route != nullptr) {
            return route;
        }
    }

    if (node.paramChild != nullptr && !segment.empty()) {
        if (const auto* route = findDynamicNodeNoParams(*node.paramChild, rest); route != nullptr) {
            return route;
        }
    }

    return node.wildcardRoute;
}

const detail::RouteTable::DynamicStaticChild* detail::RouteTable::findDynamicStaticChild(
    const DynamicNode& node,
    std::string_view segment) noexcept {
    if (node.staticChildren.size() <= 4) {
        for (const auto& child : node.staticChildren) {
            if (child.segment == segment) {
                return &child;
            }
        }
        return nullptr;
    }

    const auto iter = std::lower_bound(
        node.staticChildren.begin(),
        node.staticChildren.end(),
        segment,
        [](const DynamicStaticChild& child, std::string_view value) {
            return std::string_view(child.segment) < value;
        });
    if (iter != node.staticChildren.end() && std::string_view(iter->segment) == segment) {
        return &*iter;
    }
    return nullptr;
}

bool detail::RouteTable::addParam(RouteMatch& match, std::string_view value) noexcept {
    return match.add(value);
}

bool detail::RouteTable::splitSegment(
    std::string_view path,
    std::string_view& segment,
    std::string_view& rest) noexcept {
    if (path.starts_with('/')) {
        path.remove_prefix(1);
    }
    if (path.empty()) {
        segment = {};
        rest = {};
        return false;
    }

    const auto slash = path.find('/');
    if (slash == std::string_view::npos) {
        segment = path;
        rest = {};
        return true;
    }

    segment = path.substr(0, slash);
    rest = path.substr(slash + 1);
    return true;
}

bool detail::RouteTable::splitSegmentStrict(
    std::string_view path,
    std::string_view& segment,
    std::string_view& rest) noexcept {
    if (path.empty()) {
        segment = {};
        rest = {};
        return false;
    }
    if (path.front() == '/') {
        path.remove_prefix(1);
    }

    const auto slash = path.find('/');
    if (slash == std::string_view::npos) {
        segment = path;
        rest = {};
        return true;
    }

    segment = path.substr(0, slash);
    rest = path.substr(slash);  // keep the leading '/' so empty segments survive
    return true;
}

const detail::RouteEntry* detail::RouteTable::findDynamicRoute(
    HttpMethod method,
    std::string_view path,
    RouteMatch& match) const noexcept {
    match.clear();
    if (!isRoutableMethod(method)) {
        return nullptr;
    }

    const auto methodBit = 1U << methodIndex(method);
    return (dynamicMethodMask_ & methodBit) != 0 ? findDynamic(method, path, match) : nullptr;
}

const detail::RouteEntry* detail::RouteTable::findDynamic(
    HttpMethod method,
    std::string_view path,
    RouteMatch& match) const noexcept {
    match.clear();
    const auto* route = findDynamicNode(dynamicRoots_[methodIndex(method)], path, match);
    if (route == nullptr || match.size() != route->paramNames().size()) {
        match.clear();
        return nullptr;
    }
    return route;
}

}  // namespace ruvia
