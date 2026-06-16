#include "RouterInternal.h"

#include <algorithm>

#include "RouterUtils.h"

namespace ruvia {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

}  // namespace

detail::RouteResolution detail::RouteTable::resolve(const HttpRequest& request) const noexcept {
    const auto path = request.path();
    const auto method = request.method();

    if (const auto* route = findStaticRoute(method, path); route != nullptr) {
        return RouteResolution{
            .status = RouteResolveStatus::kFound,
            .route = route,
            .bodyMode = route->bodyMode};
    }

    auto match = findDynamicRoute(method, path);
    if (match.route != nullptr) {
        return RouteResolution{
            .status = RouteResolveStatus::kFound,
            .route = match.route,
            .match = match,
            .bodyMode = match.route->bodyMode,
            .dynamic = true};
    }

    auto methodMask = allowedMethods(path);
    if (methodMask != 0) {
        methodMask |= 1U << methodIndex(HttpMethod::kOptions);
        return RouteResolution{
            .status = RouteResolveStatus::kMethodNotAllowed,
            .allowedMethods = methodMask};
    }

    return RouteResolution{};
}

RequestBodyMode detail::RouteTable::bodyModeFor(const HttpRequest& request) const noexcept {
    return resolve(request).bodyMode;
}

void detail::RouterImpl::validateNoDynamicRouteConflict(std::span<const PendingRoute> routes) {
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const auto& left = routes[i];
        if (!left.dynamic) {
            continue;
        }
        for (std::size_t j = i + 1; j < routes.size(); ++j) {
            const auto& right = routes[j];
            if (!right.dynamic || left.method != right.method) {
                continue;
            }
            if (sameDynamicShape(left.path, right.path)) {
                throw std::invalid_argument("conflicting dynamic route shape");
            }
        }
    }
}

void detail::RouteTable::buildPerfectHash() {
    exactSlots_.clear();
    exactSeed_ = 0;
    exactMask_ = 0;

    std::pmr::vector<const RouteEntry*> exactRoutes;
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
    std::pmr::vector<const RouteEntry*> candidate;

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

void detail::RouteTable::buildDynamicRoutes() {
    hasDynamicRoutes_.fill(false);
    for (auto& root : dynamicRoots_) {
        root = DynamicNode{};
    }

    for (auto& route : routes_) {
        if (route.dynamic) {
            hasDynamicRoutes_[methodIndex(route.method)] = true;
            collectDynamicParamNames(route);
            insertDynamic(dynamicRoots_[methodIndex(route.method)], route);
        }
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

std::size_t detail::RouteTable::methodIndex(HttpMethod method) noexcept {
    return static_cast<std::size_t>(method);
}

bool detail::RouteTable::isRoutableMethod(HttpMethod method) noexcept {
    return methodIndex(method) < kRoutableMethodCount;
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

std::uint64_t detail::RouteTable::routeHash(HttpMethod method, std::string_view path, std::uint64_t seed) noexcept {
    auto hash = kFnvOffset ^ seed;
    hash ^= static_cast<std::uint64_t>(method);
    hash *= kFnvPrime;

    for (const unsigned char c : path) {
        hash ^= c;
        hash *= kFnvPrime;
    }

    return hash;
}

std::size_t detail::RouteTable::nextPowerOfTwo(std::size_t value) noexcept {
    std::size_t result = 1;
    while (result < value) {
        result <<= 1U;
    }
    return result;
}

std::size_t detail::RouteTable::commonPrefixLength(std::string_view left, std::string_view right) noexcept {
    const auto length = std::min(left.size(), right.size());
    std::size_t index = 0;
    while (index < length && left[index] == right[index]) {
        ++index;
    }
    return index;
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

const detail::RouteEntry* detail::RouteTable::findRadixNode(
    const RadixNode& root,
    std::string_view path) noexcept {
    const auto* node = &root;
    while (!path.empty()) {
        const RadixNode* next = nullptr;
        for (const auto& child : node->children) {
            if (path.starts_with(child.label)) {
                next = &child;
                path.remove_prefix(child.label.size());
                break;
            }
        }

        if (next == nullptr) {
            return nullptr;
        }
        node = next;
    }

    return node->route;
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
                node->paramChild = std::make_unique<DynamicNode>();
            }
            node = node->paramChild.get();
        } else {
            auto* childNode = static_cast<DynamicNode*>(nullptr);
            for (auto& child : node->staticChildren) {
                if (child.segment == segment) {
                    childNode = child.node.get();
                    break;
                }
            }
            if (childNode == nullptr) {
                auto child = DynamicStaticChild{std::pmr::string(segment, startupResource()), std::make_unique<DynamicNode>()};
                childNode = child.node.get();
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
    for (const auto& child : node.staticChildren) {
        if (child.segment == segment) {
            if (const auto* route = findDynamicNode(*child.node, rest, match); route != nullptr) {
                return route;
            }
            match.paramCount = originalParamCount;
            break;
        }
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

const detail::RouteEntry* detail::RouteTable::findStaticRoute(
    HttpMethod method,
    std::string_view path) const noexcept {
    if (!isRoutableMethod(method)) {
        return nullptr;
    }

    if (const auto* route = findPerfect(method, path); route != nullptr) {
        return route;
    }
    if (!exactSlots_.empty()) {
        return nullptr;
    }
    if (const auto* route = findRadix(method, path); route != nullptr) {
        return route;
    }

    return nullptr;
}

detail::RouteMatch detail::RouteTable::findDynamicRoute(HttpMethod method, std::string_view path) const noexcept {
    RouteMatch match;
    if (!isRoutableMethod(method) || !hasDynamicRoutes_[methodIndex(method)]) {
        return match;
    }

    match = findDynamic(method, path);
    return match;
}

const detail::RouteEntry* detail::RouteTable::findPerfect(HttpMethod method, std::string_view path) const noexcept {
    if (exactSlots_.empty()) {
        return nullptr;
    }

    const auto index = static_cast<std::size_t>(routeHash(method, path, exactSeed_)) & exactMask_;
    const auto* route = exactSlots_[index].route;
    if (route != nullptr && route->method == method && route->path == path) {
        return route;
    }

    return nullptr;
}

const detail::RouteEntry* detail::RouteTable::findRadix(HttpMethod method, std::string_view path) const noexcept {
    return findRadixNode(radixRoots_[methodIndex(method)], path);
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

std::uint32_t detail::RouteTable::allowedMethods(std::string_view path) const noexcept {
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < kRoutableMethodCount; ++i) {
        const auto method = static_cast<HttpMethod>(i);
        if (method == HttpMethod::kOptions || (allowedMethodMask_ & (1U << i)) == 0) {
            continue;
        }
        if (findStaticRoute(method, path) != nullptr || findDynamicRoute(method, path).route != nullptr) {
            mask |= 1U << i;
        }
    }
    return mask;
}

std::uint32_t detail::RouteTable::allowedMethodsForServer() const noexcept {
    return allowedMethodMask_;
}

}  // namespace ruvia
