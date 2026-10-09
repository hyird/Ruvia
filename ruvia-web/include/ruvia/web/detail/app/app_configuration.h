#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/detail/integration/worker_state.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/detail/router/prefix_fallback.h"
#include "ruvia/web/detail/util/registration_resource.h"
#include "ruvia/web/error_handlers.h"
#include "ruvia/web/middleware.h"

namespace ruvia::detail {

class router_impl;

template <typename middleware_type, typename... args_types>
[[nodiscard]] controller_middleware_descriptor make_scoped_app_middleware(
    const middleware_scope_options& options, args_types&&... args) {
    static_assert(!middleware_runs_on_unmatched_requests<middleware_type>(),
        "a middleware declaring ruvia_runs_on_unmatched_requests cannot be path-scoped with "
        "use_at(); register it app-wide with use<T>()");
    const auto normalized = normalize_fallback_prefix(options.prefix_);
    return make_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...)
        .scoped_to(retain_registration_text(normalized));
}

template <typename state_type>
[[nodiscard]] worker_state_definition make_default_worker_state() {
    static_assert(std::is_default_constructible_v<state_type>,
        "use_worker_state<T>() without a factory requires T to be default "
        "constructible; pass a factory otherwise");
    return worker_state_definition::make<state_type>([] { return state_type(); });
}

// application and test_app own this same dispatch configuration. Their lifecycle guards
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

    void add_middleware(controller_middleware_descriptor descriptor);
    void add_worker_state(worker_state_definition definition);
    void on_error(http_error_handler_type handler);
    void on_error(scoped_error_handler_options options);
    void on_not_found(http_not_found_handler_type handler);
    void on_not_found(scoped_not_found_handler_options options);

    [[nodiscard]] std::span<const worker_state_definition> worker_states() const noexcept {
        return worker_states_;
    }

    void apply(router_impl& routes, std::pmr::memory_resource* temporary_resource) const;

private:
    http_error_handler_type error_handler_{nullptr};
    http_not_found_handler_type not_found_handler_{nullptr};
    std::pmr::vector<std::pair<std::pmr::string, http_error_handler_type>> prefix_error_handlers_;
    std::pmr::vector<std::pair<std::pmr::string, http_not_found_handler_type>> prefix_not_found_handlers_;
    std::pmr::vector<controller_middleware_descriptor> middlewares_;
    std::pmr::vector<worker_state_definition> worker_states_;
};

}  // namespace ruvia::detail
