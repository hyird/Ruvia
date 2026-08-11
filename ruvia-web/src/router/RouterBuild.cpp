#include "ruvia/web/detail/router/PrefixFallback.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/router/PathSegments.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/PmrResource.h"

namespace ruvia {
namespace {

[[nodiscard]] bool dynamicRouteMatchesPath(std::string_view pattern, std::string_view path) noexcept {
    for (;;) {
        std::string_view patternSegment;
        std::string_view patternRest;
        const auto hasPattern = detail::splitRoutePathSegment(pattern, patternSegment, patternRest);
        if (!hasPattern) {
            std::string_view pathSegment;
            std::string_view pathRest;
            return !detail::splitRequestPathSegment(path, pathSegment, pathRest);
        }

        if (patternSegment == "*") {
            return patternRest.empty();
        }

        std::string_view pathSegment;
        std::string_view pathRest;
        if (!detail::splitRequestPathSegment(path, pathSegment, pathRest)) {
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
    : RouteEntry(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource), std::move(init)) {}

detail::RouteEntry::RouteEntry(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource, Init init)
    : method_(init.method),
      methodToken_(init.methodToken, resource),
      path_(init.path, resource),
      endpoint_(std::move(init.endpoint)),
      dynamic_(init.dynamic),
      maxRequestBodyBytes_(init.maxRequestBodyBytes),
      middlewareOffset_(init.middlewareOffset),
      middlewareCount_(init.middlewareCount) {}

detail::RouteTable::RouteTable(std::pmr::memory_resource* resource)
    : resource_(detail::pmrResourceOrDefault(resource)),
      routes_(resource_),
      middlewareFrames_(resource_),
      exactSlots_(resource_),
      dynamicRoots_{DynamicNode(detail::ResolvedPmrResourceTag{}, resource_), DynamicNode(detail::ResolvedPmrResourceTag{}, resource_), DynamicNode(detail::ResolvedPmrResourceTag{}, resource_), DynamicNode(detail::ResolvedPmrResourceTag{}, resource_), DynamicNode(detail::ResolvedPmrResourceTag{}, resource_), DynamicNode(detail::ResolvedPmrResourceTag{}, resource_), DynamicNode(detail::ResolvedPmrResourceTag{}, resource_)},
      extensionRouteIndices_(resource_),
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

// HEAD mirrors only buffered GET routes. Streaming, SSE, and WebSocket handlers
// have explicit stream lifecycles and must not be entered by an implicit HEAD
// shadow.
[[nodiscard]] static bool eligibleForHeadShadow(const detail::RouteEndpoint& endpoint) noexcept {
    return endpoint.buffered() != nullptr;
}

void detail::RouterImpl::buildRouteTable(RouteTable& table) const {
    std::size_t headShadowCandidateCount = 0;
    std::size_t middlewareCount = 0;
    for (const auto& route : pendingRoutes_) {
        if (route.method() == HttpKnownMethod::kGet && eligibleForHeadShadow(route.endpoint())) {
            ++headShadowCandidateCount;
        }
        middlewareCount += globalMiddlewareFrames_.size() + route.middlewares().size();
    }
    table.routes_.reserve(pendingRoutes_.size() + headShadowCandidateCount);
    table.middlewareFrames_.reserve(middlewareCount);

    for (const auto& pending : pendingRoutes_) {
        const auto pendingMiddlewares = pending.middlewares();
        RouteEntry route(detail::ResolvedPmrResourceTag{}, table.resource_, RouteEntry::Init{.method = pending.method(), .methodToken = pending.methodToken(), .path = pending.path(), .endpoint = pending.endpoint().clone(table.resource_), .dynamic = pending.dynamic(), .maxRequestBodyBytes = pending.maxRequestBodyBytes(), .middlewareOffset = 0, .middlewareCount = 0});
        // App-wide middleware runs before controller/route middleware on every
        // matched route: each route's contiguous frame range starts with the
        // shared global instances.
        //
        // A path-scoped registration (useAt) is filtered out HERE, when the
        // table is built, rather than tested per request: a route outside the
        // scope simply never receives the frame, so scoping costs the request
        // path nothing. globalMiddlewareFrames_ and globalMiddlewareDescriptors_
        // are parallel, so the descriptor at i carries frame i's scope.
        const auto middlewareOffset = table.middlewareFrames_.size();
        for (std::size_t i = 0; i < globalMiddlewareFrames_.size(); ++i) {
            if (!pathIsUnderPrefix(pending.path(), globalMiddlewareDescriptors_[i].prefix())) {
                continue;
            }
            table.middlewareFrames_.push_back(globalMiddlewareFrames_[i]);
        }
        table.middlewareFrames_.insert(table.middlewareFrames_.end(), pendingMiddlewares.begin(), pendingMiddlewares.end());
        route.setMiddlewareRange(middlewareOffset, table.middlewareFrames_.size() - middlewareOffset);
        table.routes_.push_back(std::move(route));
    }

    // The unmatched-request block, appended once after every route's range so
    // it stays contiguous and no route can accidentally include it.
    table.unmatchedMiddlewareOffset_ = table.middlewareFrames_.size();
    for (std::size_t i = 0; i < globalMiddlewareFrames_.size(); ++i) {
        if (globalMiddlewareDescriptors_[i].runsOnUnmatchedRequests()) {
            table.middlewareFrames_.push_back(globalMiddlewareFrames_[i]);
        }
    }
    table.unmatchedMiddlewareCount_ = table.middlewareFrames_.size() - table.unmatchedMiddlewareOffset_;

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
        if (table.routes_[i].method() == HttpKnownMethod::kHead) {
            explicitHeadRoutes.push_back(&table.routes_[i]);
        }
    }

    for (std::size_t i = 0; i < originalRouteCount; ++i) {
        const auto& source = table.routes_[i];
        if (source.method() != HttpKnownMethod::kGet || !eligibleForHeadShadow(source.endpoint())) {
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
        RouteEntry shadow(detail::ResolvedPmrResourceTag{}, table.resource_, RouteEntry::Init{.method = HttpKnownMethod::kHead, .path = source.path(), .endpoint = source.endpoint().clone(table.resource_), .dynamic = source.dynamic(), .maxRequestBodyBytes = source.maxRequestBodyBytes(), .middlewareOffset = source.middlewareOffset(), .middlewareCount = source.middlewareCount()});
        table.routes_.push_back(std::move(shadow));
    }

    // After both passes, so the indices are stable for the table's lifetime.
    for (std::size_t i = 0; i < table.routes_.size(); ++i) {
        if (table.routes_[i].method() == HttpKnownMethod::kUnknown) {
            table.extensionRouteIndices_.push_back(i);
        }
    }

    table.buildAllowedMethodMask();
    table.buildPerfectHash();
    table.buildDynamicRoutes();
}

}  // namespace ruvia
