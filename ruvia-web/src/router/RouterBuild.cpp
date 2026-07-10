#include "ruvia/web/detail/router/RouterInternal.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/PmrResource.h"

namespace ruvia {
namespace {

[[nodiscard]] bool splitBuildSegment(
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

[[nodiscard]] bool splitRequestLikeSegment(
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
    rest = path.substr(slash);
    return true;
}

[[nodiscard]] bool dynamicRouteMatchesPath(std::string_view pattern, std::string_view path) noexcept {
    for (;;) {
        std::string_view patternSegment;
        std::string_view patternRest;
        const auto hasPattern = splitBuildSegment(pattern, patternSegment, patternRest);
        if (!hasPattern) {
            std::string_view pathSegment;
            std::string_view pathRest;
            return !splitRequestLikeSegment(path, pathSegment, pathRest);
        }

        if (patternSegment == "*") {
            return patternRest.empty();
        }

        std::string_view pathSegment;
        std::string_view pathRest;
        if (!splitRequestLikeSegment(path, pathSegment, pathRest)) {
            return false;
        }

        if (!patternSegment.empty() && patternSegment.front() == ':') {
            if (pathSegment.empty()) {
                return false;
            }
        } else if (patternSegment != pathSegment) {
            return false;
        }

        pattern = patternRest;
        path = pathRest;
    }
}

}  // namespace

detail::RouteEntry::RouteEntry(std::pmr::memory_resource* resource, Init init)
    : RouteEntry(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource), init) {}

detail::RouteEntry::RouteEntry(
    detail::ResolvedPmrResourceTag,
    std::pmr::memory_resource* resource,
    Init init)
    : method_(init.method),
      path_(init.path, resource),
      handler_(init.handler),
      streamHandler_(init.streamHandler),
      bodyMode_(init.bodyMode),
      responseMode_(init.responseMode),
      dynamic_(init.dynamic),
      middlewareOffset_(init.middlewareOffset),
      middlewareCount_(init.middlewareCount),
      webSocketSubprotocols_(init.webSocketSubprotocols, resource),
      webSocketHeartbeat_(init.webSocketHeartbeat) {}

detail::RouteTable::RouteTable(std::pmr::memory_resource* resource)
    : resource_(detail::pmrResourceOrDefault(resource)),
      routes_(resource_),
      middlewareFrames_(resource_),
      exactSlots_(resource_),
      radixRoots_{
          RadixNode(detail::ResolvedPmrResourceTag{}, resource_),
          RadixNode(detail::ResolvedPmrResourceTag{}, resource_),
          RadixNode(detail::ResolvedPmrResourceTag{}, resource_),
          RadixNode(detail::ResolvedPmrResourceTag{}, resource_),
          RadixNode(detail::ResolvedPmrResourceTag{}, resource_),
          RadixNode(detail::ResolvedPmrResourceTag{}, resource_),
          RadixNode(detail::ResolvedPmrResourceTag{}, resource_)},
      dynamicRoots_{
          DynamicNode(detail::ResolvedPmrResourceTag{}, resource_),
          DynamicNode(detail::ResolvedPmrResourceTag{}, resource_),
          DynamicNode(detail::ResolvedPmrResourceTag{}, resource_),
          DynamicNode(detail::ResolvedPmrResourceTag{}, resource_),
          DynamicNode(detail::ResolvedPmrResourceTag{}, resource_),
          DynamicNode(detail::ResolvedPmrResourceTag{}, resource_),
          DynamicNode(detail::ResolvedPmrResourceTag{}, resource_)},
      dynamicNodeArena_(resource_),
      dynamicParamNames_(resource_) {}

void detail::RouterImpl::validateNoDynamicRouteConflict(std::span<const PendingRoute> routes) {
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const auto& left = routes[i];
        if (!left.dynamic()) {
            continue;
        }
        for (std::size_t j = i + 1; j < routes.size(); ++j) {
            const auto& right = routes[j];
            if (!right.dynamic() || left.method() != right.method()) {
                continue;
            }
            if (RouteTable::sameDynamicShape(left.path(), right.path())) {
                throw std::invalid_argument("conflicting dynamic route shape");
            }
        }
    }
}

detail::RouteTable detail::RouterImpl::buildRouteTable() const {
    RouteTable table(resource_);
    std::size_t headShadowCandidateCount = 0;
    std::size_t middlewareCount = 0;
    for (const auto& route : pendingRoutes_) {
        if (route.method() == HttpMethod::kGet && route.isBufferedResponse()) {
            ++headShadowCandidateCount;
        }
        middlewareCount += route.middlewares().size();
    }
    table.routes_.reserve(pendingRoutes_.size() + headShadowCandidateCount);
    table.middlewareFrames_.reserve(middlewareCount);

    for (const auto& pending : pendingRoutes_) {
        const auto pendingMiddlewares = pending.middlewares();
        RouteEntry route(detail::ResolvedPmrResourceTag{}, table.resource_, RouteEntry::Init{
            .method = pending.method(),
            .path = pending.path(),
            .handler = pending.handler(),
            .streamHandler = pending.streamHandler(),
            .bodyMode = pending.bodyMode(),
            .responseMode = pending.responseMode(),
            .dynamic = pending.dynamic(),
            .middlewareOffset = 0,
            .middlewareCount = 0,
            .webSocketSubprotocols = pending.webSocketSubprotocols(),
            .webSocketHeartbeat = pending.webSocketHeartbeat()});
        route.setMiddlewareRange(table.middlewareFrames_.size(), pendingMiddlewares.size());
        table.middlewareFrames_.insert(
            table.middlewareFrames_.end(),
            pendingMiddlewares.begin(),
            pendingMiddlewares.end());
        table.routes_.push_back(std::move(route));
    }

    const auto originalRouteCount = table.routes_.size();
    const auto conflictsWithHeadRoute = [](const RouteEntry& source, const RouteEntry& headRoute) noexcept {
        if (source.dynamic() && headRoute.dynamic()) {
            return RouteTable::sameDynamicShape(source.path(), headRoute.path());
        }
        if (!source.dynamic() && headRoute.dynamic()) {
            return dynamicRouteMatchesPath(headRoute.path(), source.path());
        }
        if (!source.dynamic() && !headRoute.dynamic()) {
            return headRoute.path() == source.path();
        }
        return false;
    };

    std::pmr::vector<const RouteEntry*> explicitHeadRoutes(table.resource_);
    for (std::size_t i = 0; i < originalRouteCount; ++i) {
        if (table.routes_[i].method() == HttpMethod::kHead) {
            explicitHeadRoutes.push_back(&table.routes_[i]);
        }
    }

    for (std::size_t i = 0; i < originalRouteCount; ++i) {
        const auto& source = table.routes_[i];
        if (source.method() != HttpMethod::kGet || !source.isBufferedResponse()) {
            continue;
        }
        bool conflictsWithExistingHead = false;
        for (const auto* headRoute : explicitHeadRoutes) {
            if (conflictsWithHeadRoute(source, *headRoute)) {
                conflictsWithExistingHead = true;
                break;
            }
        }
        if (conflictsWithExistingHead) {
            continue;
        }
        RouteEntry shadow(detail::ResolvedPmrResourceTag{}, table.resource_, RouteEntry::Init{
            .method = HttpMethod::kHead,
            .path = source.path(),
            .handler = source.handler(),
            .streamHandler = source.streamHandler(),
            .bodyMode = source.bodyMode(),
            .responseMode = source.responseMode(),
            .dynamic = source.dynamic(),
            .middlewareOffset = source.middlewareOffset(),
            .middlewareCount = source.middlewareCount(),
            .webSocketSubprotocols = source.webSocketSubprotocols(),
            .webSocketHeartbeat = source.webSocketHeartbeat()});
        table.routes_.push_back(std::move(shadow));
    }

    table.buildAllowedMethodMask();
    table.buildPerfectHash();
    if (table.exactSlots_.empty()) {
        table.buildRadix();
    }
    table.buildDynamicRoutes();
    return table;
}

}  // namespace ruvia
