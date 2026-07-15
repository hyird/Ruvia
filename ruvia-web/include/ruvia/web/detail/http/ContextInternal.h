#pragma once

#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/ContextServices.h"
#include "ruvia/web/detail/router/RouteLimits.h"

#include <cstddef>
#include <exception>
#include <utility>

namespace ruvia {

inline Context::Context(
    RequestMemory& memory,
    const HttpRequest& request,
    detail::ContextServices services) noexcept
    : Context(memory, request, {}, nullptr, nullptr, 0, 0, services) {}

inline Context::Context(
    RequestMemory& memory,
    const HttpRequest& request,
    std::string_view routePath,
    const std::string_view* paramNames,
    const std::string_view* paramValues,
    std::size_t paramCount,
    std::uintptr_t routeRateLimitScope,
    detail::ContextServices services) noexcept
    : memory_(memory),
      request_(request),
      connInfo_(services.connInfo()),
      worker_(services.worker()),
      routePath_(routePath),
      paramNames_(paramNames),
      paramValues_(paramValues),
      paramCount_(
          paramCount < detail::kMaxRouteParams
              ? paramCount
              : detail::kMaxRouteParams),
      db_(services.db()),
      redis_(services.redis()),
      rateLimiter_(services.rateLimiter()),
      errorHandler_(services.errorHandler()),
      notFoundHandler_(services.notFoundHandler()),
      routeRateLimitScope_(routeRateLimitScope),
      maxDecodedBodyBytes_(services.maxDecodedBodyBytes()),
      requestBodySource_(services.requestBodySource()),
      responseOutput_(services.responseOutput()),
      responseState_(memory.resource()),
      sessionState_(memory.resource()) {}

}  // namespace ruvia

namespace ruvia::detail {

class ContextWebSocketBinding;

struct ContextAccess final {
    [[nodiscard]] static Context make(
        RequestMemory& memory,
        const HttpRequest& request,
        ContextServices services = {}) noexcept {
        return Context(memory, request, services);
    }

    [[nodiscard]] static Context make(
        RequestMemory& memory,
        const HttpRequest& request,
        std::uintptr_t routeRateLimitScope,
        ContextServices services = {}) noexcept {
        return Context(memory, request, {}, nullptr, nullptr, 0, routeRateLimitScope, services);
    }

    [[nodiscard]] static Context make(
        RequestMemory& memory,
        const HttpRequest& request,
        std::string_view routePath,
        std::uintptr_t routeRateLimitScope,
        ContextServices services = {}) noexcept {
        return Context(
            memory,
            request,
            routePath,
            nullptr,
            nullptr,
            0,
            routeRateLimitScope,
            services);
    }

    [[nodiscard]] static Context make(
        RequestMemory& memory,
        const HttpRequest& request,
        std::string_view routePath,
        const std::string_view* paramNames,
        const std::string_view* paramValues,
        std::size_t paramCount,
        std::uintptr_t routeRateLimitScope,
        ContextServices services = {}) noexcept {
        return Context(
            memory,
            request,
            routePath,
            paramNames,
            paramValues,
            paramCount,
            routeRateLimitScope,
            services);
    }

    [[nodiscard]] static RateLimiter* rateLimiter(Context& context) noexcept {
        return context.rateLimiter_;
    }

    [[nodiscard]] static std::uintptr_t routeRateLimitScope(const Context& context) noexcept {
        return context.routeRateLimitScope_;
    }

    [[nodiscard]] static bool requestCookiesMaterialized(const Context& context) noexcept {
        return context.requestCookies_ != nullptr;
    }

    [[nodiscard]] static bool requestQueryMaterialized(const Context& context) noexcept {
        return context.requestQuery_ != nullptr || context.requestQueries_ != nullptr;
    }

    [[nodiscard]] static bool routeParamsMaterialized(const Context& context) noexcept {
        return context.routeParams_ != nullptr;
    }

    static void setResponse(Context& context, HttpResponse&& response) {
        context.storeResponse(std::move(response));
    }

    [[nodiscard]] static HttpResponse& responseStorage(Context& context) {
        return context.responseStorage();
    }

    [[nodiscard]] static bool hasResponseHeader(
        const Context& context,
        std::string_view name) noexcept {
        return context.responseState_.activeResponse().header(name).has_value();
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
        const Context& context,
        std::string_view contentType = {}) {
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
        const Context& context,
        std::string_view valuePrefix) noexcept {
        for (const auto& header : context.responseState_.activeResponse().headers()) {
            if (header.name() == "Set-Cookie" && header.value().starts_with(valuePrefix)) {
                return true;
            }
        }
        return false;
    }

private:
    friend class ContextWebSocketBinding;

    [[nodiscard]] static ContextResponseOutput bindWebSocket(
        Context& context,
        WebSocket& webSocket) noexcept {
        auto previous = context.responseOutput_;
        context.responseOutput_ = ContextResponseOutput::webSocket(webSocket);
        return previous;
    }

    static void restoreResponseOutput(
        Context& context,
        ContextResponseOutput output) noexcept {
        context.responseOutput_ = output;
    }
};

// The facade borrowed by Context is valid only while the established session
// owns its connection. Restoring the previous output capability on every exit
// prevents onion middleware post-processing from observing a dangling facade.
class ContextWebSocketBinding final {
public:
    ContextWebSocketBinding(
        Context& context,
        WebSocket& webSocket) noexcept
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
