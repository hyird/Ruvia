#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/detail/http/ContextServices.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/CallableRef.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/router/RouteModes.h"
#include "ruvia/web/detail/router/RouteStreamResult.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/Next.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/Router.h"

namespace ruvia::detail {

class DbRegistry;
class RedisRegistry;
class RouterImpl;

struct NextAccess final {
    [[nodiscard]] static constexpr Next make(
        NextState state,
        NextInvoke invoke) noexcept {
        return Next(state, invoke);
    }

    [[nodiscard]] static Next& makeIn(
        std::pmr::memory_resource* resource,
        NextState state,
        NextInvoke invoke) {
        auto* resolved = pmrResourceOrDefault(resource);
        auto* storage = resolved->allocate(sizeof(Next), alignof(Next));
        return *new (storage) Next(state, invoke);
    }
};

using RouteHandler = CallableRef<HttpResponse, Context&>;
using RouteStreamHandler = CallableRef<void, Context&>;
using RouteMiddleware = CallableRef<void, Context&, Next&>;

class RouteEndpoint;

class BufferedRouteEndpoint final {
public:
    [[nodiscard]] const RouteHandler& handler() const noexcept {
        return handler_;
    }

    [[nodiscard]] RequestBodyMode requestBodyMode() const noexcept {
        return requestBodyMode_;
    }

private:
    friend class RouteEndpoint;

    BufferedRouteEndpoint(
        RouteHandler handler,
        RequestBodyMode requestBodyMode) noexcept
        : handler_(handler), requestBodyMode_(requestBodyMode) {}

    RouteHandler handler_;
    RequestBodyMode requestBodyMode_;
};

class ResponseStreamRouteEndpoint final {
public:
    [[nodiscard]] const RouteStreamHandler& handler() const noexcept {
        return handler_;
    }

    [[nodiscard]] ResponseStreamKind kind() const noexcept {
        return kind_;
    }

private:
    friend class RouteEndpoint;

    ResponseStreamRouteEndpoint(
        RouteStreamHandler handler,
        ResponseStreamKind kind) noexcept
        : handler_(handler), kind_(kind) {}

    RouteStreamHandler handler_;
    ResponseStreamKind kind_;
};

class WebSocketRouteEndpoint final {
public:
    [[nodiscard]] const RouteStreamHandler& handler() const noexcept {
        return handler_;
    }

    [[nodiscard]] std::string_view subprotocols() const noexcept {
        return subprotocols_;
    }

    [[nodiscard]] const WebSocketLifecycleOptions& lifecycle() const noexcept {
        return lifecycle_;
    }

private:
    friend class RouteEndpoint;

    WebSocketRouteEndpoint(
        std::pmr::memory_resource* resource,
        RouteStreamHandler handler,
        WebSocketRouteOptions options)
        : handler_(handler),
          subprotocols_(options.subprotocols, resource),
          lifecycle_(options.lifecycle) {}

    RouteStreamHandler handler_;
    std::pmr::string subprotocols_;
    WebSocketLifecycleOptions lifecycle_;
};

// Startup-built endpoint contract. The handler shape and its only legal route
// metadata live in the same alternative, so a route cannot claim a streaming or
// WebSocket mode while carrying only a buffered handler (or vice versa).
class RouteEndpoint final {
public:
    RouteEndpoint(const RouteEndpoint&) = delete;
    RouteEndpoint& operator=(const RouteEndpoint&) = delete;
    RouteEndpoint(RouteEndpoint&&) noexcept = default;
    RouteEndpoint& operator=(RouteEndpoint&&) noexcept = default;

    [[nodiscard]] static RouteEndpoint buffered(
        RouteHandler handler,
        RequestBodyMode requestBodyMode) {
        if (!handler.valid()) {
            throw std::invalid_argument("route handler must not be empty");
        }
        if (requestBodyMode != RequestBodyMode::kBuffered &&
            requestBodyMode != RequestBodyMode::kStream) {
            throw std::invalid_argument("invalid route request-body mode");
        }
        return RouteEndpoint(BufferedRouteEndpoint(handler, requestBodyMode));
    }

    [[nodiscard]] static RouteEndpoint responseStream(
        RouteStreamHandler handler,
        ResponseStreamKind kind) {
        if (!handler.valid()) {
            throw std::invalid_argument("route stream handler must not be empty");
        }
        if (kind != ResponseStreamKind::kGeneric &&
            kind != ResponseStreamKind::kSse) {
            throw std::invalid_argument("invalid response-stream kind");
        }
        return RouteEndpoint(ResponseStreamRouteEndpoint(handler, kind));
    }

    [[nodiscard]] static RouteEndpoint webSocket(
        std::pmr::memory_resource* resource,
        RouteStreamHandler handler,
        WebSocketRouteOptions options = {}) {
        if (!handler.valid()) {
            throw std::invalid_argument("websocket route handler must not be empty");
        }
        if (options.lifecycle.closeHandshakeTimeout.has_value() &&
            options.lifecycle.closeHandshakeTimeout->count() <= 0) {
            throw std::invalid_argument(
                "websocket close-handshake timeout must be greater than zero");
        }
        return RouteEndpoint(WebSocketRouteEndpoint(
            pmrResourceOrDefault(resource), handler, options));
    }

    [[nodiscard]] RouteEndpoint clone(
        std::pmr::memory_resource* resource) const {
        if (const auto* endpoint = buffered()) {
            return RouteEndpoint::buffered(
                endpoint->handler(), endpoint->requestBodyMode());
        }
        if (const auto* endpoint = responseStream()) {
            return RouteEndpoint::responseStream(
                endpoint->handler(), endpoint->kind());
        }
        const auto& endpoint = *webSocket();
        return RouteEndpoint::webSocket(
            resource,
            endpoint.handler(),
            WebSocketRouteOptions{endpoint.subprotocols(), endpoint.lifecycle()});
    }

    [[nodiscard]] const BufferedRouteEndpoint* buffered() const noexcept {
        return std::get_if<BufferedRouteEndpoint>(&value_);
    }

    [[nodiscard]] const ResponseStreamRouteEndpoint* responseStream() const noexcept {
        return std::get_if<ResponseStreamRouteEndpoint>(&value_);
    }

    [[nodiscard]] const WebSocketRouteEndpoint* webSocket() const noexcept {
        return std::get_if<WebSocketRouteEndpoint>(&value_);
    }

    // Every non-buffered endpoint has a buffered request body contract. Only a
    // buffered-response endpoint may opt into the explicit stream-body route.
    [[nodiscard]] RequestBodyMode requestBodyMode() const noexcept {
        const auto* endpoint = buffered();
        return endpoint == nullptr
            ? RequestBodyMode::kBuffered
            : endpoint->requestBodyMode();
    }

private:
    using Value = std::variant<
        BufferedRouteEndpoint,
        ResponseStreamRouteEndpoint,
        WebSocketRouteEndpoint>;

    template <typename Endpoint>
    explicit RouteEndpoint(Endpoint endpoint) noexcept
        : value_(std::move(endpoint)) {}

    Value value_;
};

class RouteEntry final {
public:
    struct Init final {
        HttpKnownMethod method;
        std::string_view path;
        RouteEndpoint endpoint;
        bool dynamic{false};
        std::size_t middlewareOffset{0};
        std::size_t middlewareCount{0};
    };

    RouteEntry(std::pmr::memory_resource* resource, Init init);
    RouteEntry(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource, Init init);
    RouteEntry(const RouteEntry&) = delete;
    RouteEntry& operator=(const RouteEntry&) = delete;
    RouteEntry(RouteEntry&&) noexcept = default;
    RouteEntry& operator=(RouteEntry&&) noexcept = default;

    [[nodiscard]] HttpKnownMethod method() const noexcept {
        return method_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] const RouteEndpoint& endpoint() const noexcept {
        return endpoint_;
    }

    [[nodiscard]] bool dynamic() const noexcept {
        return dynamic_;
    }

    [[nodiscard]] std::span<const std::string_view> paramNames() const noexcept {
        return paramNames_;
    }

    [[nodiscard]] std::size_t middlewareOffset() const noexcept {
        return middlewareOffset_;
    }

    [[nodiscard]] std::size_t middlewareCount() const noexcept {
        return middlewareCount_;
    }

    [[nodiscard]] bool hasMiddleware() const noexcept {
        return middlewareCount_ != 0;
    }

    void setMiddlewareRange(std::size_t offset, std::size_t count) noexcept {
        middlewareOffset_ = offset;
        middlewareCount_ = count;
    }

    void setParamNames(std::span<const std::string_view> names) noexcept {
        paramNames_ = names;
    }

private:
    HttpKnownMethod method_;
    std::pmr::string path_;
    RouteEndpoint endpoint_;
    bool dynamic_{false};
    std::span<const std::string_view> paramNames_{};
    std::size_t middlewareOffset_{0};
    std::size_t middlewareCount_{0};
};

class RouteTable final {
public:
    explicit RouteTable(std::pmr::memory_resource* resource);
    RouteTable(const RouteTable&) = delete;
    RouteTable& operator=(const RouteTable&) = delete;
    RouteTable(RouteTable&&) noexcept = default;
    RouteTable& operator=(RouteTable&&) noexcept = default;

    void setErrorHandler(HttpErrorHandler handler) noexcept;
    void setNotFoundHandler(HttpNotFoundHandler handler) noexcept;
    [[nodiscard]] RouteResolution resolve(const HttpRequest& request) const noexcept;
    [[nodiscard]] RouteResolution resolve(
        HttpKnownMethod method,
        std::string_view path) const noexcept;
    Task<HttpResponse> dispatch(
        const HttpRequest& request,
        RequestMemory& memory,
        ContextServices services = {}) const;
    Task<HttpResponse> dispatch(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ContextServices services = {}) const;
    // Never throws: dispatches a resolved route and turns any failure -- a
    // handler exception (already handled inside dispatch) or one escaping the
    // routing machinery itself -- into an error response. It never decides
    // connection persistence; the HTTP/1 driver finalizes that after request-body
    // state is known, while HTTP/2 has no Connection header semantics.
    Task<HttpResponse> dispatchBuffered(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ContextServices services = {}) const;
    Task<HttpResponse> handleError(
        const HttpRequest& request,
        RequestMemory& memory,
        HttpErrorInfo error,
        ContextServices services = {}) const;
    Task<HttpResponse> handleException(
        const HttpRequest& request,
        RequestMemory& memory,
        std::exception_ptr exception,
        ContextServices services = {}) const;
    Task<StreamDispatchResult> dispatchResponseStream(
        const HttpRequest& request,
        const ResolvedRoute& route,
        RequestMemory& memory,
        ResponseStreamWriter& responseStream,
        ContextServices services = {}) const;
    Task<StreamDispatchResult> dispatchWebSocket(
        const HttpRequest& request,
        const ResolvedRoute& route,
        RequestMemory& memory,
        WebSocket& webSocket,
        ContextServices services = {}) const;

private:
    friend class RouterImpl;

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
    [[nodiscard]] static std::uint64_t routeHash(
        HttpKnownMethod method,
        std::string_view path,
        std::uint64_t seed) noexcept;
    [[nodiscard]] static std::size_t nextPowerOfTwo(std::size_t value) noexcept;
    [[nodiscard]] static std::size_t commonPrefixLength(
        std::string_view left,
        std::string_view right) noexcept;
    static void insertRadix(RadixNode& node, std::string_view path, const RouteEntry& route);
    [[nodiscard]] static const RouteEntry* findRadixNode(const RadixNode& root, std::string_view path) noexcept;
    [[nodiscard]] static std::size_t dynamicNodeUpperBound(std::string_view path) noexcept;
    [[nodiscard]] static std::size_t dynamicParamNameUpperBound(std::string_view path) noexcept;
    void insertDynamic(DynamicNode& root, RouteEntry& route);
    void appendDynamicParamName(RouteEntry& route, std::string_view name);
    static void sortDynamicNode(DynamicNode& node);
    [[nodiscard]] static const RouteEntry* findDynamicNode(
        const DynamicNode& node,
        std::string_view path,
        RouteMatch& match) noexcept;
    [[nodiscard]] static const RouteEntry* findDynamicNodeNoParams(
        const DynamicNode& node,
        std::string_view path) noexcept;
    [[nodiscard]] static const DynamicStaticChild* findDynamicStaticChild(
        const DynamicNode& node,
        std::string_view segment) noexcept;
    [[nodiscard]] static bool addParam(
        RouteMatch& match,
        std::string_view value) noexcept;
    [[nodiscard]] static bool splitSegment(
        std::string_view path,
        std::string_view& segment,
        std::string_view& rest) noexcept;
    // Strict segment split used only for REQUEST matching. Unlike splitSegment
    // (build-time), it preserves empty segments and a trailing slash so dynamic
    // matching is byte-exact like static matching: "/users/42/" and "/a//b" no
    // longer collapse to "/users/42" / "/a/b". `path` is expected to start with
    // '/' at each level; each returned `rest` keeps its leading '/'. Returns
    // false only at true end-of-path (empty `path`); a lone "/" yields an empty
    // segment that fails to match a param child.
    [[nodiscard]] static bool splitSegmentStrict(
        std::string_view path,
        std::string_view& segment,
        std::string_view& rest) noexcept;
    [[nodiscard]] static bool sameDynamicShape(std::string_view left, std::string_view right) noexcept;

    [[nodiscard]] const RouteEntry* findStaticRoute(HttpKnownMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findDynamicRoute(
        HttpKnownMethod method,
        std::string_view path,
        RouteMatch& match) const noexcept;
    [[nodiscard]] const RouteEntry* findPerfect(HttpKnownMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findRadix(HttpKnownMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findDynamic(
        HttpKnownMethod method,
        std::string_view path,
        RouteMatch& match) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethods(std::string_view path, HttpKnownMethod requestedMethod) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethodsForServer() const noexcept;
    [[nodiscard]] Task<HttpResponse> invokeRoute(const RouteEntry& route, Context& context) const;
    [[nodiscard]] Task<HttpResponse> invokeRouteWithMiddleware(const RouteEntry& route, Context& context) const;
    [[nodiscard]] Task<void> invokeMiddlewareAt(
        const RouteEntry& route,
        std::size_t index,
        Context& context) const;
    [[nodiscard]] static Task<void> invokeMiddlewareContinuation(NextState state);
    [[nodiscard]] Task<StreamDispatchResult> dispatchStreamRoute(
        const HttpRequest& request,
        const ResolvedRoute& route,
        RequestMemory& memory,
        ContextServices services) const;
    [[nodiscard]] Task<void> invokeStreamMiddlewareAt(
        const RouteEntry& route,
        std::size_t index,
        Context& context,
        StreamMiddlewareChainState& chain) const;
    [[nodiscard]] static Task<void> invokeStreamMiddlewareContinuation(NextState state);
    [[nodiscard]] Task<void> storeMiddlewareExceptionResponse(
        Context& context,
        std::exception_ptr exception) const;
    [[nodiscard]] Task<HttpResponse> handleError(
        Context& context,
        HttpErrorInfo error) const;
    [[nodiscard]] Task<HttpResponse> handleNotFound(
        const HttpRequest& request,
        RequestMemory& memory,
        ContextServices services) const;
    [[nodiscard]] Task<HttpResponse> handleException(
        Context& context,
        std::exception_ptr exception) const;

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
};

}  // namespace ruvia::detail
