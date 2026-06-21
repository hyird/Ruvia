#include "../RouterInternal.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "RouterUtils.h"

namespace ruvia {

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
            if (RouteTable::sameDynamicShape(left.path, right.path)) {
                throw std::invalid_argument("conflicting dynamic route shape");
            }
        }
    }
}

detail::RouteTable detail::RouterImpl::buildRouteTable() const {
    RouteTable table;
    std::size_t headShadowCandidateCount = 0;
    std::size_t middlewareCount = 0;
    for (const auto& route : pendingRoutes_) {
        if (route.method == HttpMethod::kGet && route.responseMode == ResponseBodyMode::kBuffered) {
            ++headShadowCandidateCount;
        }
        middlewareCount += route.middlewares.size();
    }
    table.routes_.reserve(pendingRoutes_.size() + headShadowCandidateCount);
    table.middlewareFrames_.reserve(middlewareCount);

    for (const auto& pending : pendingRoutes_) {
        RouteEntry route{
            .method = pending.method,
            .path = pending.path,
            .handler = pending.handler,
            .streamHandler = pending.streamHandler,
            .bodyMode = pending.bodyMode,
            .responseMode = pending.responseMode,
            .dynamic = pending.dynamic,
            .paramNames = {},
            .paramCount = 0,
            .middlewareOffset = 0,
            .middlewareCount = 0,
            .webSocketSubprotocols = pending.webSocketSubprotocols,
            .webSocketHeartbeat = pending.webSocketHeartbeat};
        route.middlewareOffset = table.middlewareFrames_.size();
        route.middlewareCount = pending.middlewares.size();
        table.middlewareFrames_.insert(
            table.middlewareFrames_.end(),
            pending.middlewares.begin(),
            pending.middlewares.end());
        table.routes_.push_back(std::move(route));
    }

    const auto originalRouteCount = table.routes_.size();
    const auto conflictsWithHeadRoute = [](const RouteEntry& source, const RouteEntry& headRoute) noexcept {
        if (source.dynamic && headRoute.dynamic) {
            return RouteTable::sameDynamicShape(source.path, headRoute.path);
        }
        if (!source.dynamic && !headRoute.dynamic) {
            return headRoute.path == source.path;
        }
        return false;
    };

    std::pmr::vector<const RouteEntry*> explicitHeadRoutes(detail::startupResource());
    for (std::size_t i = 0; i < originalRouteCount; ++i) {
        if (table.routes_[i].method == HttpMethod::kHead) {
            explicitHeadRoutes.push_back(&table.routes_[i]);
        }
    }

    for (std::size_t i = 0; i < originalRouteCount; ++i) {
        const auto& source = table.routes_[i];
        if (source.method != HttpMethod::kGet || source.responseMode != ResponseBodyMode::kBuffered) {
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
        RouteEntry shadow{
            .method = HttpMethod::kHead,
            .path = source.path,
            .handler = source.handler,
            .streamHandler = source.streamHandler,
            .bodyMode = source.bodyMode,
            .responseMode = source.responseMode,
            .dynamic = source.dynamic,
            .paramNames = source.paramNames,
            .paramCount = source.paramCount,
            .middlewareOffset = source.middlewareOffset,
            .middlewareCount = source.middlewareCount,
            .webSocketSubprotocols = source.webSocketSubprotocols,
            .webSocketHeartbeat = source.webSocketHeartbeat};
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
