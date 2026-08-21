#pragma once

#include "ruvia/web/detail/util/CallableRef.h"

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
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/RouteEntry.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/http/static/StaticFileVariant.h"
#include "ruvia/web/detail/server/DocumentRootBinding.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/Next.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/router/Router.h"

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
    HttpErrorHandlerRef handler{nullptr};
};

struct HttpPrefixNotFoundHandler final {
    std::string_view prefix;
    HttpNotFoundHandlerRef handler{nullptr};
};

class RouteTable final {
public:
    explicit RouteTable(std::pmr::memory_resource* resource);
    RouteTable(const RouteTable&) = delete;
    RouteTable& operator=(const RouteTable&) = delete;
    RouteTable(RouteTable&&) = delete;
    RouteTable& operator=(RouteTable&&) = delete;

    void setErrorHandler(HttpErrorHandlerRef handler) noexcept;
    void setNotFoundHandler(HttpNotFoundHandlerRef handler) noexcept;
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

    // Extension-method routing, kept off every enum-indexed structure. The
    // request's exact token is compared against a small cold list, which costs
    // a known-method request nothing: both protocol drivers only reach it when
    // classifyHttpMethod() returned kUnknown.
    [[nodiscard]] RouteResolution resolveExtensionMethod(std::string_view methodToken, std::string_view path) const noexcept;

    // Tokens of the extension routes registered on `path`, for the Allow header
    // of a 405. Written into caller storage so no allocation outlives the call.
    [[nodiscard]] std::span<const std::string_view> extensionMethodsFor(std::string_view path, std::span<std::string_view> buffer) const noexcept;
    // Unique extension method tokens registered anywhere on the server, for the
    // server-wide Allow header of OPTIONS *.
    [[nodiscard]] std::span<const std::string_view> extensionMethodsForServer() const noexcept;
    [[nodiscard]] bool hasExtensionRoutesFor(std::string_view path) const noexcept;

    // Whether ANY route in the table uses this exact token. RFC 9110 15.5.6
    // makes 405 conditional on the method being "known by the origin server",
    // so a token nobody registered is 501 no matter what the target path holds.
    [[nodiscard]] bool recognizesMethodToken(std::string_view methodToken) const noexcept;
    Task<HttpResponse> dispatch(const HttpRequest& request, RequestMemory& memory, ContextServices services = {}) const;
    Task<HttpResponse> dispatch(const HttpRequest& request, const RouteResolution& resolution, RequestMemory& memory, ContextServices services = {}) const;
    // Canonical buffered-response application dispatch for every server
    // protocol. An unresolved route first consults the configured document
    // root, then falls through to 404/405/OPTIONS handling. Any failure escaping
    // the routing machinery becomes an error response. Connection persistence
    // and wire framing remain the protocol driver's responsibility. Pass
    // DocumentRootBinding::none() when no root is configured,
    // DocumentRootBinding::standalone(root) for an immutable root, or
    // DocumentRootBinding::configured(root, runtimeOptions) for a server-owned
    // root with live-reload policy.
    Task<HttpResponse> dispatchBufferedResponse(const HttpRequest& request, const RouteResolution& resolution, RequestMemory& memory, DocumentRootBinding documentRoot, ContextServices services = {}, StaticFileSelectionMode staticFileMode = StaticFileSelectionMode::kIdentityOnly) const;
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
    void buildDynamicRoutes();
    void buildAllowedMethodMask();


    [[nodiscard]] static std::size_t methodIndex(HttpKnownMethod method) noexcept;
    [[nodiscard]] static bool isRoutableMethod(HttpKnownMethod method) noexcept;
    [[nodiscard]] static bool isDynamicPath(std::string_view path) noexcept;
    [[nodiscard]] static std::uint64_t routeHash(HttpKnownMethod method, std::string_view path, std::uint64_t seed) noexcept;
    [[nodiscard]] static std::size_t nextPowerOfTwo(std::size_t value) noexcept;
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
    [[nodiscard]] const RouteEntry* findDynamic(HttpKnownMethod method, std::string_view path, RouteMatch& match) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethods(std::string_view path, HttpKnownMethod requestedMethod) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethodsForServer() const noexcept;
    [[nodiscard]] Task<HttpResponse> dispatchRequest(const HttpRequest& request, const RouteResolution& resolution, RequestMemory& memory, ContextServices services, DocumentRootBinding documentRoot, DispatchFailure failure, StaticFileSelectionMode staticFileMode) const;
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

    // Runs the unmatched-request middleware chain around `terminal`, which
    // produces the 404/405/501 response. Falls straight through to the terminal
    // when nothing declared itself for unmatched requests.
    using UnmatchedTerminal = CallableRef<HttpResponse, Context&>;
    [[nodiscard]] Task<HttpResponse> runUnmatchedChain(Context& context, const UnmatchedTerminal& terminal) const;
    [[nodiscard]] Task<void> invokeUnmatchedMiddlewareAt(std::size_t index, Context& context, const UnmatchedTerminal& terminal) const;
    [[nodiscard]] static Task<void> invokeUnmatchedMiddlewareContinuation(NextState state);
    [[nodiscard]] Task<HttpResponse> handleException(Context& context, std::exception_ptr exception) const;

    template <typename Handler>
    struct StoredPrefixHandler final {
        StoredPrefixHandler(std::pmr::memory_resource* resource, std::string_view prefixValue, Handler handlerValue)
            : prefix(prefixValue, resource),
              handler(handlerValue) {}

        std::pmr::string prefix;
        Handler handler{nullptr};
    };

    [[nodiscard]] HttpErrorHandlerRef errorHandlerFor(std::string_view path) const noexcept;
    [[nodiscard]] HttpNotFoundHandlerRef notFoundHandlerFor(std::string_view path) const noexcept;

    std::pmr::memory_resource* resource_;
    std::pmr::vector<RouteEntry> routes_;
    std::pmr::vector<RouteMiddleware> middlewareFrames_;
    // A contiguous block at the end of middlewareFrames_: the app-wide
    // middlewares that declared they also run when no route matched. Kept as a
    // range rather than a separate vector so the fallback chain indexes frames
    // exactly the way a route's chain does.
    // Indices into routes_ rather than pointers: routes_ is populated in two
    // passes and this is built after both.
    std::pmr::vector<std::size_t> extensionRouteIndices_;
    std::pmr::vector<std::string_view> serverExtensionMethodTokens_;
    std::size_t unmatchedMiddlewareOffset_{0};
    std::size_t unmatchedMiddlewareCount_{0};
    std::pmr::vector<PerfectSlot> exactSlots_;
    std::array<DynamicNode, kRoutableMethodCount> dynamicRoots_{};
    std::pmr::vector<DynamicNode> dynamicNodeArena_;
    std::pmr::vector<std::string_view> dynamicParamNames_;
    std::uint32_t staticMethodMask_{0};
    std::uint32_t dynamicMethodMask_{0};
    std::uint32_t allowedMethodMask_{0};
    std::uint64_t exactSeed_{0};
    std::size_t exactMask_{0};
    HttpErrorHandlerRef errorHandler_{nullptr};
    HttpNotFoundHandlerRef notFoundHandler_{nullptr};
    std::pmr::vector<StoredPrefixHandler<HttpErrorHandlerRef>> prefixErrorHandlers_{resource_};
    std::pmr::vector<StoredPrefixHandler<HttpNotFoundHandlerRef>> prefixNotFoundHandlers_{resource_};
    bool hasRouteRateLimit_{false};
};

}  // namespace ruvia::detail
