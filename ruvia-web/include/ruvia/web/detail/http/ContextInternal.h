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
    detail::ContextServices services,
    HttpKnownMethod routeMethod,
    std::size_t routeMiddlewareCount) noexcept
    : memory_(memory),
      request_(request),
      connInfo_(services.connInfo()),
      routePath_(routePath),
      routeMethod_(routeMethod),
      paramNames_(paramNames),
      paramValues_(paramValues),
      paramCount_(
          paramCount < detail::kMaxRouteParams
              ? paramCount
              : detail::kMaxRouteParams),
      routeMiddlewareCount_(routeMiddlewareCount),
      db_(services.db()),
      redis_(services.redis()),
      rateLimiter_(services.rateLimiter()),
      errorHandler_(services.errorHandler()),
      notFoundHandler_(services.notFoundHandler()),
      routeRateLimitScope_(routeRateLimitScope),
      maxDecodedBodyBytes_(services.maxDecodedBodyBytes()),
      requestBodySource_(services.requestBodySource()),
      responseOutput_(services.responseOutput()),
      responseHeaders_(memory.resource()) {}

}  // namespace ruvia

namespace ruvia::detail {

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
        HttpKnownMethod routeMethod,
        std::size_t routeMiddlewareCount,
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
            services,
            routeMethod,
            routeMiddlewareCount);
    }

    [[nodiscard]] static Context make(
        RequestMemory& memory,
        const HttpRequest& request,
        std::string_view routePath,
        const std::string_view* paramNames,
        const std::string_view* paramValues,
        std::size_t paramCount,
        HttpKnownMethod routeMethod,
        std::size_t routeMiddlewareCount,
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
            services,
            routeMethod,
            routeMiddlewareCount);
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
        for (const auto& header : context.responseHeaders_) {
            if (header.name() == "Set-Cookie" && header.value().starts_with(valuePrefix)) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace ruvia::detail
