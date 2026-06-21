#include "../RouteTable.h"

#include <algorithm>

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
    if (route != nullptr && route->method == method && route->path == path) {
        return route;
    }

    return nullptr;
}

const detail::RouteEntry* detail::RouteTable::findRadix(HttpMethod method, std::string_view path) const noexcept {
    return findRadixNode(radixRoots_[methodIndex(method)], path);
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
