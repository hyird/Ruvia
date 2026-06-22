#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/Context.h"
#include "RouteResolution.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/Next.h"
#include "ruvia/http/WebSocket.h"
#include "ruvia/router/Router.h"

namespace ruvia::detail {

class DbRegistry;
class HttpClientRegistry;
class RedisRegistry;
class RouterImpl;

struct NextAccess final {
    [[nodiscard]] static constexpr Next make(void* target, Next::Invoke invoke) noexcept {
        return Next(target, invoke);
    }
};

struct RouteHandler final {
    void* target{nullptr};
    Next::Invoke invoke{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr;
    }

    [[nodiscard]] Task<HttpResponse> operator()(Context& context) const {
        if (invoke == nullptr) {
            throw std::logic_error("route handler is empty");
        }
        return invoke(target, context);
    }
};

struct RouteStreamHandler final {
    using Invoke = Task<void> (*)(void*, Context&);

    void* target{nullptr};
    Invoke invoke{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr;
    }
    [[nodiscard]] Task<void> operator()(Context& context) const {
        if (invoke == nullptr) {
            throw std::logic_error("route stream handler is empty");
        }
        return invoke(target, context);
    }
};

struct RouteMiddleware final {
    using Invoke = Task<HttpResponse> (*)(void*, Context&, const Next&);
    using Create = void* (*)();
    using Destroy = void (*)(void*) noexcept;

    void* target{nullptr};
    Invoke invoke{nullptr};
    Create create{nullptr};
    Destroy destroy{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr && (target != nullptr || create != nullptr);
    }

    [[nodiscard]] Task<HttpResponse> operator()(Context& context, const Next& next) const {
        return invoke(target, context, next);
    }
};

struct StreamDispatchResult final {
    HttpResponse response;
    bool streamHandled{false};
};

struct RouteServices final {
    DbRegistry* db{nullptr};
    RedisRegistry* redis{nullptr};
    BodyReader* bodyReader{nullptr};
    RequestBodyLoader* bodyLoader{nullptr};
    HttpClientRegistry* httpClients{nullptr};

    [[nodiscard]] RouteServices withBodyReader(BodyReader* value) const noexcept {
        auto services = *this;
        services.bodyReader = value;
        return services;
    }

    [[nodiscard]] RouteServices withBodyLoader(RequestBodyLoader* value) const noexcept {
        auto services = *this;
        services.bodyLoader = value;
        return services;
    }
};

struct RouteEntry final {
    HttpMethod method;
    std::pmr::string path;
    RouteHandler handler;
    RouteStreamHandler streamHandler;
    RequestBodyMode bodyMode{RequestBodyMode::kBuffered};
    ResponseBodyMode responseMode{ResponseBodyMode::kBuffered};
    bool dynamic{false};
    std::array<std::string_view, kMaxRouteParams> paramNames{};
    std::size_t paramCount{0};
    std::size_t middlewareOffset{0};
    std::size_t middlewareCount{0};
    std::pmr::string webSocketSubprotocols;
    WebSocketHeartbeatOptions webSocketHeartbeat{};
};

class RouteTable final {
public:
    RouteTable() = default;
    RouteTable(const RouteTable&) = delete;
    RouteTable& operator=(const RouteTable&) = delete;
    RouteTable(RouteTable&&) noexcept = default;
    RouteTable& operator=(RouteTable&&) noexcept = default;

    void setErrorHandler(HttpErrorHandler handler) noexcept;
    [[nodiscard]] RouteResolution resolve(const HttpRequest& request) const noexcept;
    [[nodiscard]] RouteResolution resolve(HttpMethod method, std::string_view path) const noexcept;
    Task<HttpResponse> dispatch(
        const HttpRequest& request,
        RequestMemory& memory,
        RouteServices services = {}) const;
    Task<HttpResponse> dispatch(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        RouteServices services = {}) const;
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
        RouteServices services = {}) const;
    Task<HttpResponse> handleError(
        const HttpRequest& request,
        RequestMemory& memory,
        HttpErrorInfo error,
        bool closeConnection,
        RouteServices services = {}) const;
    Task<HttpResponse> handleException(
        const HttpRequest& request,
        RequestMemory& memory,
        std::exception_ptr exception,
        bool closeConnection,
        RouteServices services = {}) const;
    Task<StreamDispatchResult> dispatchResponseStream(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        ResponseStreamWriter& responseStream,
        RouteServices services = {}) const;
    Task<StreamDispatchResult> dispatchWebSocket(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        WebSocket& webSocket,
        RouteServices services = {}) const;

private:
    friend class RouterImpl;

    static constexpr std::size_t kRoutableMethodCount = 7;

    struct PerfectSlot {
        const RouteEntry* route{nullptr};
    };

    struct RadixNode {
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
        std::pmr::vector<DynamicStaticChild> staticChildren;
        DynamicNode* paramChild{nullptr};
        const RouteEntry* route{nullptr};
        const RouteEntry* wildcardRoute{nullptr};
    };

    struct MiddlewareContinuation {
        const RouteTable* table{nullptr};
        const RouteEntry* route{nullptr};
        std::size_t index{0};
    };

    struct StreamMiddlewareContinuation {
        const RouteTable* table{nullptr};
        const RouteEntry* route{nullptr};
        std::size_t index{0};
        bool* streamHandled{nullptr};
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
    void insertDynamic(DynamicNode& root, RouteEntry& route);
    static void sortDynamicNode(DynamicNode& node);
    [[nodiscard]] static const RouteEntry* findDynamicNode(
        const DynamicNode& node,
        std::string_view path,
        RouteMatch& match) noexcept;
    [[nodiscard]] static bool addParam(
        RouteMatch& match,
        std::string_view name,
        std::string_view value) noexcept;
    [[nodiscard]] static bool splitSegment(
        std::string_view path,
        std::string_view& segment,
        std::string_view& rest) noexcept;
    [[nodiscard]] static bool sameDynamicShape(std::string_view left, std::string_view right) noexcept;

    [[nodiscard]] const RouteEntry* findStaticRoute(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] RouteMatch findDynamicRoute(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findPerfect(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] const RouteEntry* findRadix(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] RouteMatch findDynamic(HttpMethod method, std::string_view path) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethods(std::string_view path) const noexcept;
    [[nodiscard]] std::uint32_t allowedMethodsForServer() const noexcept;
    [[nodiscard]] Task<HttpResponse> invokeRoute(const RouteEntry& route, Context& context) const;
    [[nodiscard]] Task<HttpResponse> invokeMiddlewareAt(
        const RouteEntry& route,
        std::size_t index,
        Context& context) const;
    [[nodiscard]] static Task<HttpResponse> invokeMiddlewareContinuation(void* target, Context& context);
    [[nodiscard]] Task<HttpResponse> invokeStreamRoute(
        const RouteEntry& route,
        Context& context,
        bool& streamHandled) const;
    [[nodiscard]] Task<StreamDispatchResult> dispatchStreamRoute(
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& memory,
        RouteServices services,
        ResponseStreamWriter* responseStream,
        WebSocket* webSocket) const;
    [[nodiscard]] Task<HttpResponse> invokeStreamMiddlewareAt(
        const RouteEntry& route,
        std::size_t index,
        Context& context,
        bool& streamHandled) const;
    [[nodiscard]] static Task<HttpResponse> invokeStreamMiddlewareContinuation(void* target, Context& context);
    [[nodiscard]] Task<HttpResponse> handleError(
        Context& context,
        HttpErrorInfo error,
        bool closeConnection) const;
    [[nodiscard]] Task<HttpResponse> handleException(
        Context& context,
        std::exception_ptr exception,
        bool closeConnection) const;

    std::pmr::vector<RouteEntry> routes_;
    std::pmr::vector<RouteMiddleware> middlewareFrames_;
    std::pmr::vector<PerfectSlot> exactSlots_;
    std::array<RadixNode, kRoutableMethodCount> radixRoots_{};
    std::array<DynamicNode, kRoutableMethodCount> dynamicRoots_{};
    std::pmr::vector<DynamicNode> dynamicNodeArena_{std::pmr::get_default_resource()};
    std::array<bool, kRoutableMethodCount> hasDynamicRoutes_{};
    std::uint32_t allowedMethodMask_{0};
    std::uint64_t exactSeed_{0};
    std::size_t exactMask_{0};
    HttpErrorHandler errorHandler_{nullptr};
};

}  // namespace ruvia::detail
