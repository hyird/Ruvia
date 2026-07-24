#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/RouteEntry.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/Next.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/Router.h"

namespace ruvia {
class StaticRoot;
}

namespace ruvia::detail {

class DbRegistry;
class RedisRegistry;
class RouterImpl;

struct NextAccess final {
    [[nodiscard]] static constexpr Next make(NextState state, NextInvoke invoke) noexcept {
        return Next(state, invoke);
    }

    [[nodiscard]] static Next& makeIn(std::pmr::memory_resource* resource, NextState state, NextInvoke invoke) {
        auto* resolved = pmrResourceOrDefault(resource);
        auto* storage = resolved->allocate(sizeof(Next), alignof(Next));
        return *new (storage) Next(state, invoke);
    }
};

// One path-prefix-scoped fallback registration (Hono sub-app scoping analog).
// The prefix is a borrowed view during registration; RouteTable copies it into
// owned storage. Selection is longest-prefix-first on whole path segments.
struct HttpPrefixErrorHandler final {
    std::string_view prefix;
    HttpErrorHandler handler{nullptr};
};

struct HttpPrefixNotFoundHandler final {
    std::string_view prefix;
    HttpNotFoundHandler handler{nullptr};
};

class RouteTable final {
public:
    explicit RouteTable(std::pmr::memory_resource* resource);
    RouteTable(const RouteTable&) = delete;
    RouteTable& operator=(const RouteTable&) = delete;
    RouteTable(RouteTable&&) = delete;
    RouteTable& operator=(RouteTable&&) = delete;

    void setErrorHandler(HttpErrorHandler handler) noexcept;
    void setNotFoundHandler(HttpNotFoundHandler handler) noexcept;
    // Wholesale replacement (idempotent for an app stop()/run() cycle). The
    // stored set is normalized (trailing slash stripped) and ordered longest
    // prefix first so selection is a first-match scan.
    void setPrefixErrorHandlers(std::span<const HttpPrefixErrorHandler> handlers);
    void setPrefixNotFoundHandlers(std::span<const HttpPrefixNotFoundHandler> handlers);
    [[nodiscard]] bool hasRouteRateLimit() const noexcept {
        return hasRouteRateLimit_;
    }
    // Builds a request path from a registered route pattern: ":name" segments
    // take the next value (percent-encoded, non-empty), a trailing "*" takes
    // the final value (slashes preserved, may be empty). The pattern is the
    // route's identity -- an unregistered pattern or a value-count mismatch is
    // a programming error and throws std::invalid_argument.
    [[nodiscard]] std::pmr::string urlFor(std::string_view pattern, std::span<const std::string_view> values, std::pmr::memory_resource* resource) const;
    [[nodiscard]] RouteResolution resolve(const HttpRequest& request) const noexcept;
    [[nodiscard]] RouteResolution resolve(HttpKnownMethod method, std::string_view path) const noexcept;
    Task<HttpResponse> dispatch(const HttpRequest& request, RequestMemory& memory, ContextServices services = {}) const;
    Task<HttpResponse> dispatch(const HttpRequest& request, const RouteResolution& resolution, RequestMemory& memory, ContextServices services = {}) const;
    // Canonical buffered-response application dispatch for every server
    // protocol. An unresolved route first consults the configured document
    // root, then falls through to 404/405/OPTIONS handling. Any failure escaping
    // the routing machinery becomes an error response. Connection persistence
    // and wire framing remain the protocol driver's responsibility.
    Task<HttpResponse> dispatchBufferedResponse(const HttpRequest& request, const RouteResolution& resolution, RequestMemory& memory, const StaticRoot* documentRoot, ContextServices services = {}) const;
    Task<HttpResponse> handleError(const HttpRequest& request, RequestMemory& memory, HttpErrorInfo error, ContextServices services = {}) const;
    Task<HttpResponse> handleException(const HttpRequest& request, RequestMemory& memory, std::exception_ptr exception, ContextServices services = {}) const;
    // Absence means the bound output handled the request; a value is the one
    // buffered response produced before a response-stream commit.
    Task<std::optional<HttpResponse>> dispatchResponseStream(const HttpRequest& request, const ResolvedRoute& route, RequestMemory& memory, ResponseStreamWriter& responseStream, ContextServices services = {}) const;
    Task<std::optional<HttpResponse>> dispatchWebSocket(const HttpRequest& request, const ResolvedRoute& route, RequestMemory& memory, const RouteStreamHandler& handler, ContextServices services = {}) const;

private:
    friend class RouterImpl;

    // How dispatchRequest treats a failure escaping the routing machinery
    // itself. Handler exceptions are already converted inside the route path.
    enum class DispatchFailure : std::uint8_t {
        kPropagate,
        kRespond,
    };

    static constexpr std::size_t kRoutableMethodCount = 7;

    struct PerfectSlot {
        const RouteEntry* route{nullptr};
    };

    struct RadixNode {
        RadixNode()
            : RadixNode(nullptr) {}

        explicit RadixNode(std::pmr::memory_resource* resource)
            : RadixNode(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

        RadixNode(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
            : label(resource),
              children(resource) {}

        std::pmr::string label;
        std::pmr::vector<RadixNode> children;
        const RouteEntry* route{nullptr};
    };

    struct DynamicNode;

    struct DynamicStaticChild {
        std::pmr::string segment;
        DynamicNode* node{nullptr};
    };

    struct DynamicNode {
        DynamicNode()
            : DynamicNode(nullptr) {}

        explicit DynamicNode(std::pmr::memory_resource* resource)
            : DynamicNode(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

        DynamicNode(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
            : staticChildren(resource) {}

        std::pmr::vector<DynamicStaticChild> staticChildren;
        DynamicNode* paramChild{nullptr};
        const RouteEntry* route{nullptr};
        const RouteEntry* wildcardRoute{nullptr};
    };

    void buildPerfectHash();
    void buildRadix();
    void buildDynamicRoutes();
    void buildAllowedMethodMask() noexcept;

    [[nodiscard]] static std::size_t methodIndex(HttpKnownMethod method) noexcept;
    [[nodiscard]] static bool isRoutableMethod(HttpKnownMethod method) noexcept;
    [[nodiscard]] static bool isDynamicPath(std::string_view path) noexcept;
    [[nodiscard]] static std::uint64_t routeHash(HttpKnownMethod method, std::string_view path, std::uint64_t seed) noexcept;
    [[nodiscard]] static std::size_t nextPowerOfTwo(std::size_t value) noexcept;
    [[nodiscard]] static std::size_t commonPrefixLength(std::string_view left, std::string_view right) noexcept;
    static void insertRadix(RadixNode& node, std::string_view path, const RouteEntry& route);
    [[nodiscard]] static const RouteEntry* findRadixNode(const RadixNode& root, std::string_view path) noexcept;
    [[nodiscard]] static std::size_t dynamicNodeUpperBound(std::string_view path) noexcept;
    [[nodiscard]] static std::size_t dynamicParamNameUpperBound(std::string_view path) noexcept;
    void insertDynamic(DynamicNode& root, RouteEntry& route);
    void appendDynamicParamName(RouteEntry& route, std::string_view name);
    static void sortDynamicNode(DynamicNode& node);
    [[nodiscard]] static const RouteEntry* findDynamicNode(const DynamicNode& node, std::string_view path, RouteMatch& match) noexcept;
    [[nodiscard]] static const RouteEntry* findDynamicNodeNoParams(const DynamicNode& node, std::string_view path) noexcept;
    [[nodiscard]] static const DynamicStaticChild* findDynamicStaticChild(const DynamicNode& node, std::string_view segment) noexcept;
    [[nodiscard]] static bool addParam(RouteMatch& match, std::string_view value) noexcept;
    [[nodiscard]] static bool sameDynamicShape(std::string_view left, std::string_view right) noexcept;

    [[nodiscard]] const RouteEntry* findStaticRoute(HttpKnownMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findDynamicRoute(HttpKnownMethod method, std::string_view path, RouteMatch& match) const noexcept;
    [[nodiscard]] const RouteEntry* findPerfect(HttpKnownMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findRadix(HttpKnownMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findDynamic(HttpKnownMethod method, std::string_view path, RouteMatch& match) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethods(std::string_view path, HttpKnownMethod requestedMethod) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethodsForServer() const noexcept;
    [[nodiscard]] Task<HttpResponse> dispatchRequest(const HttpRequest& request, const RouteResolution& resolution, RequestMemory& memory, ContextServices services, const StaticRoot* documentRoot, DispatchFailure failure) const;
    [[nodiscard]] Task<HttpResponse> invokeRoute(const RouteEntry& route, Context& context) const;
    [[nodiscard]] Task<HttpResponse> invokeRouteWithMiddleware(const RouteEntry& route, Context& context) const;
    [[nodiscard]] Task<void> invokeMiddlewareAt(const RouteEntry& route, std::size_t index, Context& context) const;
    [[nodiscard]] static Task<void> invokeMiddlewareContinuation(NextState state);
    [[nodiscard]] Task<std::optional<HttpResponse>> dispatchStreamRoute(const HttpRequest& request, const ResolvedRoute& route, RequestMemory& memory, const RouteStreamHandler& handler, ContextServices services) const;
    [[nodiscard]] Task<void> invokeStreamMiddlewareAt(const RouteEntry& route, std::size_t index, Context& context, StreamMiddlewareChainState& chain, const RouteStreamHandler& handler) const;
    [[nodiscard]] static Task<void> invokeStreamMiddlewareContinuation(NextState state);
    [[nodiscard]] Task<void> storeMiddlewareExceptionResponse(Context& context, std::exception_ptr exception) const;
    [[nodiscard]] Task<HttpResponse> handleError(Context& context, HttpErrorInfo error) const;
    [[nodiscard]] Task<HttpResponse> handleNotFound(const HttpRequest& request, RequestMemory& memory, ContextServices services) const;
    [[nodiscard]] Task<HttpResponse> handleException(Context& context, std::exception_ptr exception) const;

    template <typename Handler>
    struct StoredPrefixHandler final {
        StoredPrefixHandler(std::pmr::memory_resource* resource, std::string_view prefixValue, Handler handlerValue)
            : prefix(prefixValue, resource),
              handler(handlerValue) {}

        std::pmr::string prefix;
        Handler handler{nullptr};
    };

    [[nodiscard]] HttpErrorHandler errorHandlerFor(std::string_view path) const noexcept;
    [[nodiscard]] HttpNotFoundHandler notFoundHandlerFor(std::string_view path) const noexcept;

    std::pmr::memory_resource* resource_;
    std::pmr::vector<RouteEntry> routes_;
    std::pmr::vector<RouteMiddleware> middlewareFrames_;
    std::pmr::vector<PerfectSlot> exactSlots_;
    std::array<RadixNode, kRoutableMethodCount> radixRoots_{};
    std::array<DynamicNode, kRoutableMethodCount> dynamicRoots_{};
    std::pmr::vector<DynamicNode> dynamicNodeArena_;
    std::pmr::vector<std::string_view> dynamicParamNames_;
    std::uint32_t staticMethodMask_{0};
    std::uint32_t dynamicMethodMask_{0};
    std::uint32_t allowedMethodMask_{0};
    std::uint64_t exactSeed_{0};
    std::size_t exactMask_{0};
    HttpErrorHandler errorHandler_{nullptr};
    HttpNotFoundHandler notFoundHandler_{nullptr};
    std::pmr::vector<StoredPrefixHandler<HttpErrorHandler>> prefixErrorHandlers_{resource_};
    std::pmr::vector<StoredPrefixHandler<HttpNotFoundHandler>> prefixNotFoundHandlers_{resource_};
    bool hasRouteRateLimit_{false};
};

}  // namespace ruvia::detail
