#pragma once

#include "ruvia/http/Context.h"
#include "ContextServices.h"

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
    HttpMethod routeMethod,
    std::size_t routeMiddlewareCount) noexcept
    : memory_(memory),
      request_(request),
      routePath_(routePath),
      routeMethod_(routeMethod),
      paramNames_(paramNames),
      paramValues_(paramValues),
      paramCount_(paramCount < kMaxRouteParams ? paramCount : kMaxRouteParams),
      routeMiddlewareCount_(routeMiddlewareCount),
      db_(services.db()),
      redis_(services.redis()),
      httpClients_(services.httpClients()),
      rateLimiter_(services.rateLimiter()),
      errorHandler_(services.errorHandler()),
      notFoundHandler_(services.notFoundHandler()),
      routeRateLimitScope_(routeRateLimitScope),
      bodyReader_(services.bodyReader()),
      bodyLoader_(services.bodyLoader()),
      webSocket_(services.webSocket()),
      responseStream_(services.responseStream()),
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
        HttpMethod routeMethod,
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
        HttpMethod routeMethod,
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
};

}  // namespace ruvia::detail
