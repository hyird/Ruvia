#include <algorithm>
#include <bit>

#include "ruvia/http/HttpConnectUdp.h"
#include "ruvia/http/HttpRequestTarget.h"

#include "router/RouteTable.h"

namespace ruvia {
namespace {

constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

}  // namespace

detail::RouteResolution detail::RouteTable::resolve(const HttpRequest& request) const noexcept {
    // HTTP/1 Upgrade detection remains in its adapter; only normalized routing
    // facts enter the shared classification policy.
    if (isHttpConnectUdpUpgradeRequest(request)) {
        return resolve(route_request_view{HttpKnownMethod::kConnect, request.method(),
            request.path(), request.path(), "connect-udp"});
    }
    return resolve(route_request_view{request.knownMethod(), request.method(),
        request.path(), request.path(), {}});
}

detail::RouteResolution detail::RouteTable::resolve(route_request_view request) const noexcept {
    if (request.known_method == HttpKnownMethod::kConnect) {
        if (request.extended_protocol == "websocket") {
            return request.path.empty() ? RouteResolution{} : resolve(HttpKnownMethod::kGet, request.path);
        }
        return resolveConnect(request.extended_protocol,
            request.extended_protocol.empty() ? request.authority : request.path);
    }
    if (request.path.empty()) {
        return {};
    }
    if (request.known_method == HttpKnownMethod::kUnknown) {
        return resolveExtensionMethod(request.method_token, request.path);
    }
    return resolve(request.known_method, request.path);
}

detail::RouteResolution detail::RouteTable::resolveConnect(std::string_view protocol, std::string_view target) const noexcept {
    const RouteEntry* fallback = nullptr;
    for (const auto index : plan_->connectRouteIndices_) {
        const auto& route = routes_[index];
        if (route.endpoint().tunnel()->protocol() != protocol) {
            continue;
        }
        if (!route.dynamic() && (protocol.empty() ? httpAuthoritiesEqual(route.path(), target, 0) : route.path() == target)) {
            return RouteResolution::resolved(route);
        }
        if (protocol.empty() && route.path() == "*") {
            fallback = &route;
        }
    }
    if (!protocol.empty()) {
        for (const auto& index : plan_->connectProtocols_) {
            if (index.protocol != protocol) {
                continue;
            }
            RouteMatch match;
            const auto found = findDynamicNode(index.root, target, match);
            if (found != kNoRouteIndex) {
                return RouteResolution::resolved(routes_[found], match);
            }
            break;
        }
    }
    return fallback == nullptr ? RouteResolution{} : RouteResolution::resolved(*fallback);
}

detail::RouteResolution detail::RouteTable::resolveExtensionMethod(
    std::string_view methodToken, std::string_view path) const noexcept {
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
    const auto hash = plan_->staticMethodMask_ != 0 ? path_hash(path) : 0;
    auto methodMask = allowedMethods(path, HttpKnownMethod::kUnknown, hash);
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

std::span<const std::string_view> detail::RouteTable::extensionMethodsFor(
    std::string_view path, std::span<std::string_view> buffer) const noexcept {
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

detail::RouteResolution detail::RouteTable::resolve(
    HttpKnownMethod method, std::string_view path) const noexcept {
    if (method == HttpKnownMethod::kConnect) {
        return resolveConnect({}, path);
    }
    // RFC 9110 7.1 / 9.3.7: the asterisk-form target ("OPTIONS *") applies to the
    // server as a whole, not any resource, so it must not bind to a route -- a
    // catch-all such as RUVIA_ALL("/*") would otherwise capture it through the
    // wildcard node. Leave it unresolved so dispatch emits the server-wide response.
    if (method == HttpKnownMethod::kOptions && path == "*") {
        return RouteResolution{};
    }
    const bool has_static_method = isRoutableMethod(method) &&
                                   (plan_->staticMethodMask_ & (1U << methodIndex(method))) != 0;
    auto hash = has_static_method ? path_hash(path) : 0;
    if (has_static_method) {
        if (const auto* route = findStaticRoute(method, path, hash); route != nullptr) {
            return RouteResolution::resolved(*route);
        }
    }

    RouteMatch match;
    const auto* dynamicRoute = findDynamicRoute(method, path, match);
    if (dynamicRoute != nullptr) {
        return RouteResolution::resolved(*dynamicRoute, std::move(match));
    }

    if (!has_static_method && plan_->staticMethodMask_ != 0) {
        hash = path_hash(path);
    }
    if (method == HttpKnownMethod::kHead) {
        const auto* fallback = (plan_->staticMethodMask_ & (1U << methodIndex(HttpKnownMethod::kGet))) != 0
                                   ? findStaticRoute(HttpKnownMethod::kGet, path, hash)
                                   : nullptr;
        if (fallback == nullptr) {
            fallback = findDynamicRoute(HttpKnownMethod::kGet, path, match);
        }
        if (fallback != nullptr && supports_head_fallback(*fallback)) {
            return RouteResolution::resolved(*fallback, std::move(match));
        }
    }
    auto methodMask = allowedMethods(path, method, hash);
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

bool detail::RouteTable::supports_head_fallback(const RouteEntry& route) noexcept {
    // Streaming, SSE and WebSocket endpoints require an explicit HEAD handler;
    // the ordinary GET handler borrows the original HEAD request unchanged.
    return route.method() == HttpKnownMethod::kGet && route.endpoint().buffered() != nullptr;
}

std::uint64_t detail::RouteTable::path_hash(std::string_view path) noexcept {
    auto hash = fnv_offset;

    for (const unsigned char c : path) {
        hash ^= c;
        hash *= fnv_prime;
    }

    // MurmurHash3's public-domain fmix64 spreads common path prefixes across
    // the low bits used by the power-of-two table.
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return hash;
}

std::uint64_t detail::RouteTable::route_hash(
    HttpKnownMethod method, std::uint64_t hash) noexcept {
    return hash ^ (static_cast<std::uint64_t>(method) * fnv_prime);
}

const detail::RouteEntry* detail::RouteTable::findStaticRoute(
    HttpKnownMethod method, std::string_view path, std::uint64_t hash) const noexcept {
    const auto key_hash = route_hash(method, hash);
    auto slot_index = static_cast<std::size_t>(key_hash) & plan_->static_slot_mask_;
    for (;;) {
        const auto& slot = plan_->static_slots_[slot_index];
        if (slot.route_index_ == kNoRouteIndex) {
            return nullptr;
        }
        const auto& route = routes_[slot.route_index_];
        if (slot.hash_ == key_hash && route.method() == method && route.path() == path) {
            return &route;
        }
        slot_index = (slot_index + 1) & plan_->static_slot_mask_;
    }
}

std::uint32_t detail::RouteTable::allowedMethods(
    std::string_view path, HttpKnownMethod requestedMethod, std::uint64_t hash) const noexcept {
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
        const auto* const staticRoute = hasStaticRoutes ? findStaticRoute(method, path, hash) : nullptr;
        const auto dynamicRouteIndex = staticRoute == nullptr && (plan_->dynamicMethodMask_ & methodBit) != 0
                                           ? findDynamicNodeNoParams(plan_->dynamicRoots_[i], path)
                                           : kNoRouteIndex;
        const auto* route = staticRoute != nullptr ? staticRoute
                                                   : (dynamicRouteIndex != kNoRouteIndex ? &routes_[dynamicRouteIndex] : nullptr);
        if (route != nullptr) {
            mask |= 1U << i;
            if (supports_head_fallback(*route)) {
                mask |= 1U << methodIndex(HttpKnownMethod::kHead);
            }
        }
    }
    return mask;
}

std::uint32_t detail::RouteTable::allowedMethodsForServer() const noexcept {
    return plan_->allowedMethodMask_;
}

}  // namespace ruvia
