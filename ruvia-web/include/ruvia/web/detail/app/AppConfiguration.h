#pragma once

#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/web/Middleware.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"
#include "ruvia/web/detail/integration/WorkerState.h"
#include "ruvia/web/detail/router/PrefixFallback.h"

namespace ruvia::detail {

// The configuration calls App and TestApp answer identically. TestApp cannot
// reuse App -- controllers register into one process-wide table that App owns --
// so the two run separate route tables, and anything defined per class would be
// free to drift between what a test exercises and what a server does. These
// forward to storage each class owns; the shapes live here once.
template <typename Derived>
class AppConfiguration {
public:
    // Registers one app-wide middleware that runs before every matched route's
    // controller and route middlewares, in registration order. It participates
    // only in routed dispatch; requests that end in 404/405 without matching a
    // route never enter a middleware chain. Validator middlewares
    // (RUVIA_VALIDATE_*) bind one model to one route and are rejected here.
    //
    // Arguments configure the middleware: they are copied once at registration
    // and every instance is constructed from them, so a configured middleware
    // does not need to be default constructible. Route- and controller-level
    // middleware lists (the trailing arguments of RUVIA_GET and friends) name
    // types only, so a configured middleware is registered here.
    template <typename MiddlewareT, typename... Args>
    Derived& use(Args&&... args) {
        return self().useMiddleware(makeMiddlewareDescriptor<MiddlewareT>(std::forward<Args>(args)...));
    }

    // The same registration, scoped to a path prefix: the middleware runs only
    // on routes under it. Prefixes use the same whole-segment rule and trailing
    // slash normalization as onError({.prefix = ...}) -- "/api" scopes "/api" and
    // "/api/x", never "/apix".
    //
    // Scoping is resolved when the route table is built, so a scoped middleware
    // costs a matched route exactly what an app-wide one does and costs a route
    // outside the scope nothing at all. There is no per-request path matching.
    //
    // Deliberately a separate name rather than a use<T>(prefix, args...)
    // overload: the argument pack can itself begin with a string, and a
    // registration silently changing meaning based on its first argument's type
    // is not a mistake worth allowing.
    template <typename MiddlewareT, typename... Args>
    Derived& useAt(MiddlewareScopeOptions options, Args&&... args) {
        // The unmatched-request chain is one contiguous block shared by every
        // 404/405/501, so it cannot carry per-prefix membership. Rejecting the
        // combination is better than silently dropping either half of it.
        static_assert(!middlewareRunsOnUnmatchedRequests<MiddlewareT>(),
            "a middleware declaring ruviaRunsOnUnmatchedRequests cannot be path-scoped with useAt(); register it app-wide with use<T>()");
        const auto normalized = normalizeFallbackPrefix(options.prefix.view());
        return self().useMiddleware(makeMiddlewareDescriptor<MiddlewareT>(std::forward<Args>(args)...).scopedTo(retainRegistrationText(normalized)));
    }

    // Worker-local user state: every worker builds its own T from the registered
    // factory at startup, and Context::workerState<T>() /
    // WebWorkerContext::workerState<T>() return that worker's instance. Workers
    // are single-threaded, so the instance needs no synchronization; it must not
    // be shared across workers by the application. One registration per type.
    template <typename T, typename Factory>
    Derived& useWorkerState(Factory&& factory) {
        return self().useWorkerStateDefinition(WorkerStateDefinition::make<T>(std::forward<Factory>(factory)));
    }

    template <typename T>
    Derived& useWorkerState() {
        static_assert(std::is_default_constructible_v<T>,
            "useWorkerState<T>() without a factory requires T to be default "
            "constructible; pass a factory otherwise");
        return useWorkerState<T>([] { return T(); });
    }

protected:
    constexpr AppConfiguration() noexcept = default;
    ~AppConfiguration() = default;

private:
    [[nodiscard]] Derived& self() noexcept {
        return static_cast<Derived&>(*this);
    }
};

// The rule a prefix-scoped fallback registration follows, shared for the same
// reason: a prefix that normalizes differently between App and TestApp would
// make a test pass against a scope the server never selects. Storage stays with
// each class -- App keeps its handlers on the app resource, TestApp on the heap
// -- so this validates and normalizes, and the caller appends.
//
// Prefix-less setters are explicit single slots and may be overwritten; a scoped
// fallback is a route-like registration, so a duplicate normalized scope is a
// configuration error rather than an order-dependent last-wins mutation.
template <typename Handlers, typename Handler>
[[nodiscard]] std::string_view validateFallbackPrefix(const Handlers& handlers, std::string_view prefix, const Handler& handler) {
    if (!handler) {
        throw std::invalid_argument("fallback handler must not be null");
    }
    const auto normalized = normalizeFallbackPrefix(prefix);
    for (const auto& existing : handlers) {
        if (std::string_view(existing.first) == normalized) {
            throw std::invalid_argument("duplicate fallback prefix");
        }
    }
    return normalized;
}

}  // namespace ruvia::detail
