#include "ruvia/web/detail/router/RouteTable.h"

#include <algorithm>
#include <bit>

namespace ruvia {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

}  // namespace

detail::RouteResolution detail::RouteTable::resolve(const HttpRequest& request) const noexcept {
    // An unclassified method can only be served by an extension route, and a
    // classified one can only be served by the enum-indexed structures, so the
    // two lookups never both run.
    if (request.knownMethod() == HttpKnownMethod::kUnknown) {
        return resolveExtensionMethod(request.method(), request.path());
    }
    return resolve(request.knownMethod(), request.path());
}

detail::RouteResolution detail::RouteTable::resolveExtensionMethod(std::string_view methodToken, std::string_view path) const noexcept {
    // Not registered anywhere means the server does not know this method, which
    // is 501 and not this function's business -- dispatch decides that before
    // asking about any particular resource.
    if (!recognizesMethodToken(methodToken)) {
        return RouteResolution{};
    }
    for (const auto index : plan_->extensionRouteIndices_) {
        const auto& route = routes_[index];
        // RFC 9110 9.1: the method token is case-sensitive.
        if (route.methodToken() == methodToken && route.path() == path) {
            return RouteResolution::resolved(route);
        }
    }

    // The path exists under other methods, so this is 405 rather than 404. The
    // mask cannot carry extension tokens; dispatch adds them to Allow from
    // extensionMethodsFor().
    auto methodMask = allowedMethods(path, HttpKnownMethod::kUnknown);
    const bool extensionRoutes = hasExtensionRoutesFor(path);
    if (methodMask != 0 || extensionRoutes) {
        methodMask |= 1U << methodIndex(HttpKnownMethod::kOptions);
    }
    return RouteResolution::methodNotAllowed(methodMask, extensionRoutes);
}

bool detail::RouteTable::recognizesMethodToken(std::string_view methodToken) const noexcept {
    for (const auto index : plan_->extensionRouteIndices_) {
        if (routes_[index].methodToken() == methodToken) {
            return true;
        }
    }
    return false;
}

bool detail::RouteTable::hasExtensionRoutesFor(std::string_view path) const noexcept {
    for (const auto index : plan_->extensionRouteIndices_) {
        if (routes_[index].path() == path) {
            return true;
        }
    }
    return false;
}

std::span<const std::string_view> detail::RouteTable::extensionMethodsFor(std::string_view path, std::span<std::string_view> buffer) const noexcept {
    std::size_t count = 0;
    for (const auto index : plan_->extensionRouteIndices_) {
        if (count == buffer.size()) {
            break;
        }
        const auto& route = routes_[index];
        if (route.path() != path) {
            continue;
        }
        bool duplicate = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (buffer[i] == route.methodToken()) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            buffer[count++] = route.methodToken();
        }
    }
    return buffer.first(count);
}

std::span<const std::string_view> detail::RouteTable::extensionMethodsForServer() const noexcept {
    return serverExtensionMethodTokens_;
}

detail::RouteResolution detail::RouteTable::resolve(HttpKnownMethod method, std::string_view path) const noexcept {
    // RFC 9110 7.1 / 9.3.7: the asterisk-form target ("OPTIONS *") applies to the
    // server as a whole, not any resource, so it must not bind to a route -- a
    // catch-all such as RUVIA_ALL("/*") would otherwise capture it through the
    // wildcard node. Leave it unresolved so dispatch emits the server-wide response.
    if (method == HttpKnownMethod::kOptions && path == "*") {
        return RouteResolution{};
    }
    if (const auto* route = findStaticRoute(method, path); route != nullptr) {
        return RouteResolution::resolved(*route);
    }

    RouteMatch match;
    const auto* dynamicRoute = findDynamicRoute(method, path, match);
    if (dynamicRoute != nullptr) {
        return RouteResolution::resolved(*dynamicRoute, std::move(match));
    }

    auto methodMask = allowedMethods(path, method);
    // A resource may be served only by extension methods. It still exists, so
    // an unsupported known method is 405 and OPTIONS must answer with Allow --
    // the mask alone cannot tell that apart from no resource at all.
    const bool extensionRoutes = hasExtensionRoutesFor(path);
    if (methodMask != 0 || extensionRoutes) {
        methodMask |= 1U << methodIndex(HttpKnownMethod::kOptions);
    }
    return RouteResolution::methodNotAllowed(methodMask, extensionRoutes);
}

std::size_t detail::RouteTable::methodIndex(HttpKnownMethod method) noexcept {
    return static_cast<std::size_t>(method);
}

bool detail::RouteTable::isRoutableMethod(HttpKnownMethod method) noexcept {
    return methodIndex(method) < kRoutableMethodCount;
}

std::uint64_t detail::RouteTable::routeHash(HttpKnownMethod method, std::string_view path, std::uint64_t seed) noexcept {
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
    return std::bit_ceil(value);
}

const detail::RouteEntry* detail::RouteTable::findStaticRoute(HttpKnownMethod method, std::string_view path) const noexcept {
    if (!isRoutableMethod(method)) {
        return nullptr;
    }
    if ((plan_->staticMethodMask_ & (1U << methodIndex(method))) == 0) {
        return nullptr;
    }

    // buildPerfectHash covers every static route or fails the build outright, so
    // a miss here is a miss: there is no second static index to consult.
    return findPerfect(method, path);
}

const detail::RouteEntry* detail::RouteTable::findPerfect(HttpKnownMethod method, std::string_view path) const noexcept {
    if (plan_->exactSlots_.empty()) {
        return nullptr;
    }

    const auto index = static_cast<std::size_t>(routeHash(method, path, plan_->exactSeed_)) & plan_->exactMask_;
    const auto routeIndex = plan_->exactSlots_[index].routeIndex;
    if (routeIndex != kNoRouteIndex) {
        const auto& route = routes_[routeIndex];
        if (route.method() == method && route.path() == path) {
            return &route;
        }
    }

    return nullptr;
}

std::uint32_t detail::RouteTable::allowedMethods(std::string_view path, HttpKnownMethod requestedMethod) const noexcept {
    std::uint32_t mask = 0;
    auto candidateMask = plan_->staticMethodMask_ | plan_->dynamicMethodMask_;
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
        const auto method = static_cast<HttpKnownMethod>(i);
        const auto methodBit = 1U << i;
        candidateMask &= ~methodBit;

        // resolve() already proved requestedMethod has no route for this path, so it
        // was cleared from the candidate mask above.
        const bool hasStaticRoutes = (plan_->staticMethodMask_ & methodBit) != 0;
        const auto* const staticRoute = hasStaticRoutes ? findStaticRoute(method, path) : nullptr;
        const auto dynamicRouteIndex = (plan_->dynamicMethodMask_ & methodBit) != 0 ? findDynamicNodeNoParams(plan_->dynamicRoots_[i], path) : kNoRouteIndex;
        if (staticRoute != nullptr || dynamicRouteIndex != kNoRouteIndex) {
            mask |= 1U << i;
        }
    }
    return mask;
}

std::uint32_t detail::RouteTable::allowedMethodsForServer() const noexcept {
    return plan_->allowedMethodMask_;
}

}  // namespace ruvia
