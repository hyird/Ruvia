#pragma once

#include <cstddef>
#include <exception>
#include <utility>

#include "ruvia/web/Context.h"

#include "context/ContextRequestStorage.h"
#include "context/ContextServices.h"
#include "http/StaticFileVariant.h"

namespace ruvia::detail {

class ContextWebSocketBinding;
class ContextTunnelBinding;

struct ContextAccess final {
    [[nodiscard]] static Context make(
        RequestMemory& memory, const HttpRequest& request, ContextServices services) {
        return Context(memory, request, services);
    }

    [[nodiscard]] static Context make(RequestMemory& memory, const HttpRequest& request,
        std::uintptr_t routeRateLimitScope, ContextServices services) {
        return Context(memory, request, {}, nullptr, nullptr, 0, routeRateLimitScope, services);
    }

    [[nodiscard]] static Context make(RequestMemory& memory, const HttpRequest& request,
        std::string_view routePath, std::uintptr_t routeRateLimitScope,
        ContextServices services) {
        return Context(
            memory, request, routePath, nullptr, nullptr, 0, routeRateLimitScope, services);
    }

    [[nodiscard]] static Context make(RequestMemory& memory, const HttpRequest& request,
        std::string_view routePath, const std::string_view* paramNames,
        const std::string_view* paramValues, std::size_t paramCount,
        std::uintptr_t routeRateLimitScope, ContextServices services) {
        return Context(memory, request, routePath, paramNames, paramValues, paramCount,
            routeRateLimitScope, services);
    }

    [[nodiscard]] static HttpResponse staticFileWithPrecompressedVariants(
        Context& context, const StaticRoot& root, StaticFileResponseOptions options) {
        return context.staticFile(root, options, StaticFileSelectionMode::kPrecompressed);
    }

    [[nodiscard]] static HttpInterimResponseOutput* interimOutput(Context& context) noexcept {
        return context.services().interimOutput();
    }

    [[nodiscard]] static const HttpRequest& request(const Context& context) noexcept {
        return context.request_;
    }

    [[nodiscard]] static RateLimiter* rateLimiter(Context& context) noexcept {
        return context.services().rateLimiter();
    }

    [[nodiscard]] static std::uintptr_t routeRateLimitScope(const Context& context) noexcept {
        return context.routeRateLimitScope_;
    }

    [[nodiscard]] static bool requestCookiesMaterialized(const Context& context) noexcept {
        return context.requestStorage_->cookies.has_value();
    }

    [[nodiscard]] static bool requestQueryMaterialized(const Context& context) noexcept {
        return context.requestStorage_->query.has_value();
    }

    [[nodiscard]] static bool routeParamsMaterialized(const Context& context) noexcept {
        return context.requestStorage_->routeParams.has_value();
    }

    [[nodiscard]] static const ContextRequestStorage* requestStorage(
        const Context& context) noexcept {
        return context.requestStorage_.get();
    }

    static void setResponse(Context& context, HttpResponse&& response) {
        context.storeResponse(std::move(response));
    }

    [[nodiscard]] static HttpResponse& responseStorage(Context& context) {
        return context.responseStorage();
    }

    [[nodiscard]] static bool hasResponseHeader(
        const Context& context, std::string_view name) noexcept {
        return context.responseState().activeResponse().header(name).has_value();
    }

    static void markWebSocketHandshakeStarted(Context& context) noexcept {
        context.requestStorage().webSocketHandshakeStarted = true;
    }

    [[nodiscard]] static bool webSocketHandshakeStarted(const Context& context) noexcept {
        return context.requestStorage().webSocketHandshakeStarted;
    }

    static void markTunnelHandshakeStarted(Context& context) noexcept {
        context.requestStorage().tunnelHandshakeStarted = true;
    }
    [[nodiscard]] static bool tunnelHandshakeStarted(const Context& context) noexcept {
        return context.requestStorage().tunnelHandshakeStarted;
    }
    static void setError(Context& context, std::exception_ptr exception) noexcept {
        context.storeError(std::move(exception));
    }

    [[nodiscard]] static bool hasResponse(const Context& context) noexcept {
        return context.hasResponse();
    }

    [[nodiscard]] static HttpResponse takeResponse(Context& context) {
        return context.takeResponse();
    }

    [[nodiscard]] static HttpResponse streamingHead(
        const Context& context, std::string_view contentType = {}) {
        return context.streamingHead(contentType);
    }

    // Sets a pending response header on the context, as a handler would before
    // streaming. Lets a test seed e.g. a caller-provided Cache-Control that the
    // stream-head builder must then honor.
    static void setResponseHeader(Context& context, std::string_view name, std::string_view value) {
        context.setStableResponseHeader(name, value);
    }

    // True if a Set-Cookie whose value begins with `valuePrefix` (e.g. a cookie
    // name plus '=') is already queued on the context's pending response headers.
    // Lets a test observe a cookie set by middleware before any response is built.
    [[nodiscard]] static bool hasPendingSetCookie(
        const Context& context, std::string_view valuePrefix) noexcept {
        return !pendingSetCookieValue(context, valuePrefix).empty();
    }

    [[nodiscard]] static std::string_view pendingSetCookieValue(
        const Context& context, std::string_view valuePrefix) noexcept {
        for (const auto& header : context.responseState().activeResponse().headers()) {
            if (header.name() == "Set-Cookie" && header.value().starts_with(valuePrefix)) {
                return header.value();
            }
        }
        return {};
    }

private:
    friend class ContextWebSocketBinding;
    friend class ContextTunnelBinding;
    [[nodiscard]] static ContextResponseOutput bindTunnel(Context& context, HttpTunnel& tunnel) noexcept {
        auto previous = context.responseOutput();
        context.responseOutput() = ContextResponseOutput::tunnel(tunnel);
        return previous;
    }

    [[nodiscard]] static ContextResponseOutput bindWebSocket(
        Context& context, WebSocket& webSocket) noexcept {
        auto previous = context.responseOutput();
        context.responseOutput() = ContextResponseOutput::webSocket(webSocket);
        return previous;
    }

    static void restoreResponseOutput(Context& context, ContextResponseOutput output) noexcept {
        context.responseOutput() = output;
    }
};

class ContextTunnelBinding final {
public:
    ContextTunnelBinding(Context& context, HttpTunnel& tunnel) noexcept
        : context_(context),
          previous_(ContextAccess::bindTunnel(context, tunnel)) {}
    ContextTunnelBinding(const ContextTunnelBinding&) = delete;
    ContextTunnelBinding& operator=(const ContextTunnelBinding&) = delete;
    ~ContextTunnelBinding() {
        ContextAccess::restoreResponseOutput(context_, previous_);
    }

private:
    Context& context_;
    ContextResponseOutput previous_;
};

// The facade borrowed by Context is valid only while the established session
// owns its connection. Restoring the previous output capability on every exit
// prevents onion middleware post-processing from observing a dangling facade.
class ContextWebSocketBinding final {
public:
    ContextWebSocketBinding(Context& context, WebSocket& webSocket) noexcept
        : context_(&context),
          previous_(ContextAccess::bindWebSocket(context, webSocket)) {}

    ContextWebSocketBinding(const ContextWebSocketBinding&) = delete;
    ContextWebSocketBinding& operator=(const ContextWebSocketBinding&) = delete;
    ContextWebSocketBinding(ContextWebSocketBinding&&) = delete;
    ContextWebSocketBinding& operator=(ContextWebSocketBinding&&) = delete;

    ~ContextWebSocketBinding() {
        ContextAccess::restoreResponseOutput(*context_, previous_);
    }

private:
    Context* context_;
    ContextResponseOutput previous_;
};

}  // namespace ruvia::detail
