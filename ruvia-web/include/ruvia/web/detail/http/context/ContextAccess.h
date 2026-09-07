#pragma once

#include <cstddef>
#include <exception>
#include <utility>

#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextRequestStorage.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/http/static/StaticFileVariant.h"
#include "ruvia/web/detail/router/RouteLimits.h"

namespace ruvia {

inline Context::Context(
    RequestMemory& memory, const HttpRequest& request, detail::ContextServices services)
    : Context(memory, request, {}, nullptr, nullptr, 0, 0, services) {}

inline Context::Context(RequestMemory& memory, const HttpRequest& request,
    std::string_view routePath, const std::string_view* paramNames,
    const std::string_view* paramValues, std::size_t paramCount, std::uintptr_t routeRateLimitScope,
    detail::ContextServices services)
    : memory_(memory),
      request_(request),
      connInfo_(services.resolveConnInfo(request)),
      worker_(services.worker()),
      stopToken_(services.stopToken()),
      requestDeadline_(services.requestDeadline()),
      routePath_(routePath),
      paramNames_(paramNames),
      paramValues_(paramValues),
      paramCount_(paramCount < detail::kMaxRouteParams ? paramCount : detail::kMaxRouteParams),
      clientRegistries_(services.clientRegistries()),
      rateLimiter_(services.rateLimiter()),
      env_(services.env()),
      errorHandler_(services.errorHandler()),
      notFoundHandler_(services.notFoundHandler()),
      routes_(services.routes()),
      workerStates_(services.workerStates()),
      blockingPool_(services.blockingPool()),
      precompressedStaticFiles_(services.precompressedStaticFiles()),
      routeRateLimitScope_(routeRateLimitScope),
      maxDecodedBodyBytes_(services.maxDecodedBodyBytes()),
      requestStorage_(detail::makePmrObject<detail::ContextRequestStorage>(memory.resource(),
          services.requestBodySource(), services.responseOutput(), memory.resource())) {}

}  // namespace ruvia

namespace ruvia::detail {

class ContextWebSocketBinding;

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

    [[nodiscard]] static const HttpRequest& request(const Context& context) noexcept {
        return context.request_;
    }

    [[nodiscard]] static RateLimiter* rateLimiter(Context& context) noexcept {
        return context.rateLimiter_;
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
