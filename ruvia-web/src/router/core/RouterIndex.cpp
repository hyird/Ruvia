#include "../RouteTable.h"

#include <algorithm>
#include <bit>

namespace ruvia {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

// Snapshot the RouteEntry metadata the transport sessions need into a
// Context-agnostic RouteDisposition, so RouteResolution carries it and the
// sessions never dereference RouteEntry.
[[nodiscard]] detail::RouteDisposition dispositionOf(const detail::RouteEntry& route) noexcept {
    return detail::RouteDisposition{
        route.bodyMode(),
        route.responseMode(),
        route.webSocketSubprotocols(),
        route.webSocketHeartbeat()};
}

}  // namespace

detail::RouteResolution detail::RouteTable::resolve(const HttpRequest& request, RouteMatch& match) const noexcept {
    return resolve(request.method(), request.path(), match);
}

detail::RouteResolution detail::RouteTable::resolve(
    HttpMethod method,
    std::string_view path,
    RouteMatch& match) const noexcept {
    // RFC 9110 7.1 / 9.3.7: the asterisk-form target ("OPTIONS *") applies to the
    // server as a whole, not any resource, so it must not bind to a route -- a
    // catch-all such as RUVIA_ALL("/*") would otherwise capture it through the
    // wildcard node. Leave it unresolved so dispatch emits the server-wide response.
    if (method == HttpMethod::kOptions && path == "*") {
        return RouteResolution{};
    }
    if (const auto* route = findStaticRoute(method, path); route != nullptr) {
        return RouteResolution::foundStatic(route, dispositionOf(*route));
    }

    const auto* dynamicRoute = findDynamicRoute(method, path, match);
    if (dynamicRoute != nullptr) {
        return RouteResolution::foundDynamic(dynamicRoute, match, dispositionOf(*dynamicRoute));
    }

    auto methodMask = allowedMethods(path, method);
    if (methodMask != 0) {
        methodMask |= 1U << methodIndex(HttpMethod::kOptions);
        return RouteResolution::methodNotAllowed(methodMask);
    }

    return RouteResolution{};
}

std::size_t detail::RouteTable::methodIndex(HttpMethod method) noexcept {
    return static_cast<std::size_t>(method);
}

bool detail::RouteTable::isRoutableMethod(HttpMethod method) noexcept {
    return methodIndex(method) < kRoutableMethodCount;
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

const detail::RouteEntry* detail::RouteTable::findStaticRoute(
    HttpMethod method,
    std::string_view path) const noexcept {
    if (!isRoutableMethod(method)) {
        return nullptr;
    }
    if ((staticMethodMask_ & (1U << methodIndex(method))) == 0) {
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

const detail::RouteEntry* detail::RouteTable::findPerfect(HttpMethod method, std::string_view path) const noexcept {
    if (exactSlots_.empty()) {
        return nullptr;
    }

    const auto index = static_cast<std::size_t>(routeHash(method, path, exactSeed_)) & exactMask_;
    const auto* route = exactSlots_[index].route;
    if (route != nullptr && route->method() == method && route->path() == path) {
        return route;
    }

    return nullptr;
}

const detail::RouteEntry* detail::RouteTable::findRadix(HttpMethod method, std::string_view path) const noexcept {
    return findRadixNode(radixRoots_[methodIndex(method)], path);
}

std::uint32_t detail::RouteTable::allowedMethods(std::string_view path, HttpMethod requestedMethod) const noexcept {
    std::uint32_t mask = 0;
    auto candidateMask = staticMethodMask_ | dynamicMethodMask_;
    // Keep OPTIONS in the candidate mask: a path whose only registered method is
    // OPTIONS must yield a non-zero Allow set so resolve() answers 405 (method known
    // but unsupported, RFC 9110 15.5.6) instead of 404. Clearing it here made such a
    // path resolve to not-found. resolve() still ORs OPTIONS into any non-zero mask,
    // so GET-only paths keep advertising OPTIONS.
    if (isRoutableMethod(requestedMethod)) {
        candidateMask &= ~(1U << methodIndex(requestedMethod));
    }

    while (candidateMask != 0) {
        const auto i = static_cast<std::size_t>(std::countr_zero(candidateMask));
        const auto method = static_cast<HttpMethod>(i);
        const auto methodBit = 1U << i;
        candidateMask &= ~methodBit;

        // resolve() already proved requestedMethod has no route for this path, so it
        // was cleared from the candidate mask above.
        const bool hasStaticRoutes = (staticMethodMask_ & methodBit) != 0;
        const auto* const staticRoute = hasStaticRoutes ? findStaticRoute(method, path) : nullptr;
        const auto* const dynamicRoute = (dynamicMethodMask_ & methodBit) != 0
            ? findDynamicNodeNoParams(dynamicRoots_[i], path)
            : nullptr;
        if (staticRoute != nullptr || dynamicRoute != nullptr) {
            mask |= 1U << i;
        }
    }
    return mask;
}

std::uint32_t detail::RouteTable::allowedMethodsForServer() const noexcept {
    return allowedMethodMask_;
}

}  // namespace ruvia
