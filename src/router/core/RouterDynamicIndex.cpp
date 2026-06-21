#include "../RouteTable.h"

#include <algorithm>

namespace ruvia {

const detail::RouteEntry* detail::RouteTable::findDynamicNode(
    const DynamicNode& node,
    std::string_view path,
    RouteMatch& match) noexcept {
    std::string_view segment;
    std::string_view rest;
    if (!splitSegment(path, segment, rest)) {
        if (node.route != nullptr) {
            return node.route;
        }
        if (node.wildcardRoute != nullptr && addParam(match, "*", {})) {
            return node.wildcardRoute;
        }
        return nullptr;
    }

    const auto originalParamCount = match.paramCount;
    const DynamicStaticChild* staticChild = nullptr;
    if (node.staticChildren.size() <= 4) {
        for (const auto& child : node.staticChildren) {
            if (child.segment == segment) {
                staticChild = &child;
                break;
            }
        }
    } else {
        const auto iter = std::lower_bound(
            node.staticChildren.begin(),
            node.staticChildren.end(),
            segment,
            [](const DynamicStaticChild& child, std::string_view value) {
                return std::string_view(child.segment) < value;
            });
        if (iter != node.staticChildren.end() && std::string_view(iter->segment) == segment) {
            staticChild = &*iter;
        }
    }
    if (staticChild != nullptr) {
        if (const auto* route = findDynamicNode(*staticChild->node, rest, match); route != nullptr) {
            return route;
        }
        match.paramCount = originalParamCount;
    }

    if (node.paramChild && !segment.empty() && addParam(match, {}, segment)) {
        if (const auto* route = findDynamicNode(*node.paramChild, rest, match); route != nullptr) {
            return route;
        }
        match.paramCount = originalParamCount;
    }

    if (node.wildcardRoute != nullptr) {
        auto capture = path;
        if (capture.starts_with('/')) {
            capture.remove_prefix(1);
        }
        if (addParam(match, "*", capture)) {
            return node.wildcardRoute;
        }
        match.paramCount = originalParamCount;
    }

    return nullptr;
}

bool detail::RouteTable::addParam(RouteMatch& match, std::string_view name, std::string_view value) noexcept {
    if (match.paramCount >= match.params.size()) {
        return false;
    }

    match.params[match.paramCount++] = RouteParamView{name, value};
    return true;
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

detail::RouteMatch detail::RouteTable::findDynamicRoute(HttpMethod method, std::string_view path) const noexcept {
    RouteMatch match;
    if (!isRoutableMethod(method) || !hasDynamicRoutes_[methodIndex(method)]) {
        return match;
    }

    match = findDynamic(method, path);
    return match;
}

detail::RouteMatch detail::RouteTable::findDynamic(HttpMethod method, std::string_view path) const noexcept {
    RouteMatch match;
    match.route = findDynamicNode(dynamicRoots_[methodIndex(method)], path, match);
    if (match.route == nullptr || match.paramCount != match.route->paramCount) {
        match.route = nullptr;
        match.paramCount = 0;
    } else {
        for (std::size_t i = 0; i < match.paramCount; ++i) {
            match.params[i].name = match.route->paramNames[i];
        }
    }
    return match;
}

}  // namespace ruvia
