#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/detail/integration/WorkerState.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"
#include "ruvia/web/detail/router/PrefixFallback.h"
#include "ruvia/web/detail/util/RegistrationResource.h"

namespace ruvia::detail {

class RouterImpl;

template <typename middleware_type, typename... args_types>
[[nodiscard]] ControllerMiddlewareDescriptor make_scoped_app_middleware(
    const MiddlewareScopeOptions& options, args_types&&... args) {
    static_assert(!middlewareRunsOnUnmatchedRequests<middleware_type>(),
        "a middleware declaring ruviaRunsOnUnmatchedRequests cannot be path-scoped with "
        "useAt(); register it app-wide with use<T>()");
    const auto normalized = normalizeFallbackPrefix(options.prefix);
    return makeMiddlewareDescriptor<middleware_type>(std::forward<args_types>(args)...)
        .scopedTo(retainRegistrationText(normalized));
}

template <typename state_type>
[[nodiscard]] WorkerStateDefinition make_default_worker_state() {
    static_assert(std::is_default_constructible_v<state_type>,
        "useWorkerState<T>() without a factory requires T to be default "
        "constructible; pass a factory otherwise");
    return WorkerStateDefinition::make<state_type>([] { return state_type(); });
}

// App and TestApp own this same dispatch configuration. Their lifecycle guards
// remain outside the component; validation, callback ownership, registration
// storage and router binding have one implementation. Bound callback views
// borrow this immovable owner until its routers have retired.
class app_configuration final {
public:
    explicit app_configuration(std::pmr::memory_resource* resource);

    app_configuration(const app_configuration&) = delete;
    app_configuration& operator=(const app_configuration&) = delete;
    app_configuration(app_configuration&&) = delete;
    app_configuration& operator=(app_configuration&&) = delete;

    void add_middleware(ControllerMiddlewareDescriptor descriptor);
    void add_worker_state(WorkerStateDefinition definition);
    void on_error(HttpErrorHandler handler);
    void on_error(ScopedErrorHandlerOptions options);
    void on_not_found(HttpNotFoundHandler handler);
    void on_not_found(ScopedNotFoundHandlerOptions options);

    [[nodiscard]] std::span<const WorkerStateDefinition> worker_states() const noexcept {
        return worker_states_;
    }

    void apply(RouterImpl& routes, std::pmr::memory_resource* temporary_resource) const;

private:
    HttpErrorHandler error_handler_{nullptr};
    HttpNotFoundHandler not_found_handler_{nullptr};
    std::pmr::vector<std::pair<std::pmr::string, HttpErrorHandler>> prefix_error_handlers_;
    std::pmr::vector<std::pair<std::pmr::string, HttpNotFoundHandler>> prefix_not_found_handlers_;
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares_;
    std::pmr::vector<WorkerStateDefinition> worker_states_;
};

}  // namespace ruvia::detail
