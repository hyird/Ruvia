#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../http/ContextServices.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/detail/CallableRef.h"
#include "RouteResolution.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/Next.h"
#include "ruvia/http/WebSocket.h"
#include "ruvia/memory/PmrResource.h"
#include "ruvia/router/Router.h"

namespace ruvia::detail {

class DbRegistry;
class HttpClientRegistry;
class RedisRegistry;
class RouterImpl;

struct NextAccess final {
    [[nodiscard]] static constexpr Next make(
        Next::State state,
        Next::Invoke invoke) noexcept {
        return Next(state, invoke);
    }
};

using RouteHandler = CallableRef<HttpResponse, Context&>;
using RouteStreamHandler = CallableRef<void, Context&>;
using RouteMiddleware = CallableRef<void, Context&, Next&>;

enum class RouteStreamDispatchOutcome {
    kBufferedResponse,
    kStreamHandled
};

class StreamDispatchResult final {
public:
    StreamDispatchResult(HttpResponse response, RouteStreamDispatchOutcome outcome)
        : response_(std::move(response)),
          outcome_(outcome) {}

    [[nodiscard]] bool streamHandled() const noexcept {
        return outcome_ == RouteStreamDispatchOutcome::kStreamHandled;
    }

    [[nodiscard]] bool bufferedResponse() const noexcept {
        return outcome_ == RouteStreamDispatchOutcome::kBufferedResponse;
    }

    [[nodiscard]] HttpResponse takeResponse() noexcept {
        return std::move(response_);
    }

private:
    HttpResponse response_;
    RouteStreamDispatchOutcome outcome_{RouteStreamDispatchOutcome::kBufferedResponse};
};

class RouteEntry final {
public:
    struct Init final {
        HttpMethod method;
        std::string_view path;
        RouteHandler handler;
        RouteStreamHandler streamHandler;
        RequestBodyMode bodyMode{RequestBodyMode::kBuffered};
        ResponseBodyMode responseMode{ResponseBodyMode::kBuffered};
        bool dynamic{false};
        std::size_t middlewareOffset{0};
        std::size_t middlewareCount{0};
        std::string_view webSocketSubprotocols{};
        WebSocketHeartbeatOptions webSocketHeartbeat{};
    };

    RouteEntry(std::pmr::memory_resource* resource, Init init);
    RouteEntry(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource, Init init);
    RouteEntry(const RouteEntry&) = delete;
    RouteEntry& operator=(const RouteEntry&) = delete;
    RouteEntry(RouteEntry&&) noexcept = default;
    RouteEntry& operator=(RouteEntry&&) noexcept = default;

    [[nodiscard]] HttpMethod method() const noexcept {
        return method_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] const RouteHandler& handler() const noexcept {
        return handler_;
    }

    [[nodiscard]] const RouteStreamHandler& streamHandler() const noexcept {
        return streamHandler_;
    }

    [[nodiscard]] RequestBodyMode bodyMode() const noexcept {
        return bodyMode_;
    }

    [[nodiscard]] ResponseBodyMode responseMode() const noexcept {
        return responseMode_;
    }

    [[nodiscard]] bool usesStreamRequestBody() const noexcept {
        return bodyMode_ == RequestBodyMode::kStream;
    }

    [[nodiscard]] bool isBufferedResponse() const noexcept {
        return responseMode_ == ResponseBodyMode::kBuffered;
    }

    [[nodiscard]] bool isDynamicResponse() const noexcept {
        return responseMode_ == ResponseBodyMode::kDynamic;
    }

    [[nodiscard]] bool isWebSocketResponse() const noexcept {
        return responseMode_ == ResponseBodyMode::kWebSocket;
    }

    [[nodiscard]] bool usesResponseStream() const noexcept {
        return responseMode_ == ResponseBodyMode::kStream ||
            responseMode_ == ResponseBodyMode::kSse ||
            responseMode_ == ResponseBodyMode::kDynamic;
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

    [[nodiscard]] std::string_view webSocketSubprotocols() const noexcept {
        return webSocketSubprotocols_;
    }

    [[nodiscard]] const WebSocketHeartbeatOptions& webSocketHeartbeat() const noexcept {
        return webSocketHeartbeat_;
    }

    void setMiddlewareRange(std::size_t offset, std::size_t count) noexcept {
        middlewareOffset_ = offset;
        middlewareCount_ = count;
    }

    void setParamNames(std::span<const std::string_view> names) noexcept {
        paramNames_ = names;
    }

private:
    HttpMethod method_;
    std::pmr::string path_;
    RouteHandler handler_;
    RouteStreamHandler streamHandler_;
    RequestBodyMode bodyMode_{RequestBodyMode::kBuffered};
    ResponseBodyMode responseMode_{ResponseBodyMode::kBuffered};
    bool dynamic_{false};
    std::span<const std::string_view> paramNames_{};
    std::size_t middlewareOffset_{0};
    std::size_t middlewareCount_{0};
    std::pmr::string webSocketSubprotocols_;
    WebSocketHeartbeatOptions webSocketHeartbeat_{};
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
    [[nodiscard]] RouteResolution resolve(const HttpRequest& request, RouteMatch& match) const noexcept;
    [[nodiscard]] RouteResolution resolve(HttpMethod method, std::string_view path, RouteMatch& match) const noexcept;
    Task<HttpResponse> dispatch(
        const HttpRequest& request,
        RequestMemory& memory,
        ContextServices services = {}) const;
    Task<HttpResponse> dispatch(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ContextServices services = {}) const;
    // Never throws: dispatches a resolved route and turns any failure — a
    // handler exception (already handled inside dispatch) or one escaping the
    // routing machinery itself — into an error response. Both the HTTP/1.1 and
    // HTTP/2 buffered paths funnel through this, so the dispatch→error-response
    // policy lives in the routing layer rather than being re-implemented by each
    // transport. closeConnectionOnError only governs the rare escaping-failure
    // case (handler exceptions keep dispatch's own close semantics).
    Task<HttpResponse> dispatchBuffered(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        bool closeConnectionOnError,
        ContextServices services = {}) const;
    Task<HttpResponse> handleError(
        const HttpRequest& request,
        RequestMemory& memory,
        HttpErrorInfo error,
        bool closeConnection,
        ContextServices services = {}) const;
    Task<HttpResponse> handleException(
        const HttpRequest& request,
        RequestMemory& memory,
        std::exception_ptr exception,
        bool closeConnection,
        ContextServices services = {}) const;
    Task<StreamDispatchResult> dispatchResponseStream(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ResponseStreamWriter& responseStream,
        ContextServices services = {}) const;
    Task<StreamDispatchResult> dispatchWebSocket(
        const HttpRequest& request,
        const RouteResolution& resolution,
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

    [[nodiscard]] static std::size_t methodIndex(HttpMethod method) noexcept;
    [[nodiscard]] static bool isRoutableMethod(HttpMethod method) noexcept;
    [[nodiscard]] static bool isDynamicPath(std::string_view path) noexcept;
    [[nodiscard]] static std::uint64_t routeHash(
        HttpMethod method,
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
    [[nodiscard]] static bool sameDynamicShape(std::string_view left, std::string_view right) noexcept;

    [[nodiscard]] const RouteEntry* findStaticRoute(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findDynamicRoute(
        HttpMethod method,
        std::string_view path,
        RouteMatch& match) const noexcept;
    [[nodiscard]] const RouteEntry* findPerfect(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findRadix(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findDynamic(
        HttpMethod method,
        std::string_view path,
        RouteMatch& match) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethods(std::string_view path, HttpMethod requestedMethod) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethodsForServer() const noexcept;
    [[nodiscard]] Task<HttpResponse> invokeRoute(const RouteEntry& route, Context& context) const;
    [[nodiscard]] Task<HttpResponse> invokeRouteWithMiddleware(const RouteEntry& route, Context& context) const;
    [[nodiscard]] Task<void> invokeMiddlewareAt(
        const RouteEntry& route,
        std::size_t index,
        Context& context) const;
    [[nodiscard]] static Task<void> invokeMiddlewareContinuation(Next::State state);
    [[nodiscard]] Task<StreamDispatchResult> dispatchStreamRoute(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ContextServices services) const;
    [[nodiscard]] Task<void> invokeStreamMiddlewareAt(
        const RouteEntry& route,
        std::size_t index,
        Context& context,
        RouteStreamDispatchOutcome& outcome) const;
    [[nodiscard]] static Task<void> invokeStreamMiddlewareContinuation(Next::State state);
    [[nodiscard]] Task<void> storeMiddlewareExceptionResponse(
        Context& context,
        std::exception_ptr exception) const;
    [[nodiscard]] Task<HttpResponse> handleError(
        Context& context,
        HttpErrorInfo error,
        bool closeConnection) const;
    [[nodiscard]] Task<HttpResponse> handleNotFound(
        const HttpRequest& request,
        RequestMemory& memory,
        bool closeConnection,
        ContextServices services) const;
    [[nodiscard]] Task<HttpResponse> handleException(
        Context& context,
        std::exception_ptr exception,
        bool closeConnection) const;

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
