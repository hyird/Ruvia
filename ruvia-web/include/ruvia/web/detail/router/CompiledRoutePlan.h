#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/detail/router/RouteEndpoint.h"

namespace ruvia::detail {

class RouteTable;
class RouterImpl;

// Process-level immutable lookup metadata. Route handlers and middleware
// instances remain in each worker's RouteTable; this plan stores only stable
// route indices and owned path structure, so every worker can share it safely.
class CompiledRoutePlan final {
public:
    explicit CompiledRoutePlan(std::pmr::memory_resource* resource)
        : resource_(pmrResourceOrDefault(resource)),
          identities_(resource_),
          extensionRouteIndices_(resource_),
          exactSlots_(resource_),
          dynamicRoots_{DynamicNode(resource_), DynamicNode(resource_), DynamicNode(resource_), DynamicNode(resource_), DynamicNode(resource_), DynamicNode(resource_), DynamicNode(resource_)},
          dynamicNodeArena_(resource_),
          unmatchedMiddlewareInvokes_(resource_) {}

    CompiledRoutePlan(const CompiledRoutePlan&) = delete;
    CompiledRoutePlan& operator=(const CompiledRoutePlan&) = delete;
    CompiledRoutePlan(CompiledRoutePlan&&) = delete;
    CompiledRoutePlan& operator=(CompiledRoutePlan&&) = delete;

private:
    friend class RouteTable;
    friend class RouterImpl;

    static constexpr std::size_t kRoutableMethodCount = 7;
    static constexpr std::size_t kNoRouteIndex = std::numeric_limits<std::size_t>::max();

    enum class EndpointKind : std::uint8_t {
        kBuffered,
        kResponseStream,
        kWebSocket,
    };

    struct RouteIdentity final {
        explicit RouteIdentity(std::pmr::memory_resource* resource)
            : methodToken(resource),
              path(resource),
              webSocketSubprotocols(resource),
              middlewareInvokes(resource) {}

        HttpKnownMethod method{HttpKnownMethod::kUnknown};
        std::pmr::string methodToken;
        std::pmr::string path;
        bool dynamic{false};
        EndpointKind endpointKind{EndpointKind::kBuffered};
        RequestBodyMode requestBodyMode{RequestBodyMode::kBuffered};
        ResponseStreamKind responseStreamKind{ResponseStreamKind::kGeneric};
        RouteHandler::Invoke bufferedInvoke{nullptr};
        RouteStreamHandler::Invoke streamInvoke{nullptr};
        std::pmr::vector<std::pmr::string> webSocketSubprotocols;
        std::int64_t webSocketPingIntervalMs{-1};
        std::int64_t webSocketPongTimeoutMs{-1};
        std::int64_t webSocketCloseTimeoutMs{-1};
        std::size_t maxRequestBodyBytes{0};
        std::int64_t deadlineMs{0};
        std::pmr::vector<RouteMiddleware::Invoke> middlewareInvokes;
    };

    struct PerfectSlot final {
        std::size_t routeIndex{kNoRouteIndex};
    };

    struct DynamicNode;

    struct DynamicStaticChild final {
        std::pmr::string segment;
        DynamicNode* node{nullptr};
    };

    struct DynamicNode final {
        explicit DynamicNode(std::pmr::memory_resource* resource)
            : staticChildren(resource) {}

        std::pmr::vector<DynamicStaticChild> staticChildren;
        DynamicNode* paramChild{nullptr};
        std::size_t routeIndex{kNoRouteIndex};
        std::size_t wildcardRouteIndex{kNoRouteIndex};
    };

    std::pmr::memory_resource* resource_;
    std::pmr::vector<RouteIdentity> identities_;
    std::pmr::vector<std::size_t> extensionRouteIndices_;
    std::pmr::vector<PerfectSlot> exactSlots_;
    std::array<DynamicNode, kRoutableMethodCount> dynamicRoots_;
    std::pmr::vector<DynamicNode> dynamicNodeArena_;
    std::pmr::vector<RouteMiddleware::Invoke> unmatchedMiddlewareInvokes_;
    std::uint32_t staticMethodMask_{0};
    std::uint32_t dynamicMethodMask_{0};
    std::uint32_t allowedMethodMask_{0};
    std::uint64_t exactSeed_{0};
    std::size_t exactMask_{0};
    bool hasRouteRateLimit_{false};
};

using CompiledRoutePlanPtr = std::unique_ptr<CompiledRoutePlan, PmrObjectDeleter<CompiledRoutePlan>>;

}  // namespace ruvia::detail
