#pragma once

#include <memory_resource>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/detail/http/context/request_bindings.h"
#include "ruvia/web/detail/middleware/middleware_descriptor.h"
#include "ruvia/web/detail/util/registration_resource.h"
#include "ruvia/web/middleware.h"

namespace ruvia::detail {

// context::respond() needs a complete context. This header is included by
// application.h and by session.h while context is still incomplete, so the call lives
// in a .cpp that already has the class definition.
void apply_middleware_response(context& context_value, http_response&& response);

template <typename middleware_t_type>
concept void_handle_middleware =
    requires { static_cast<task<void> (middleware_t_type::*)(context&, next&)>(&middleware_t_type::handle); };

template <typename middleware_t_type>
concept response_handle_middleware = requires {
    static_cast<task<http_response> (middleware_t_type::*)(context&, next&)>(&middleware_t_type::handle);
};

template <typename middleware_t_type>
[[nodiscard]] task<void> invoke_response_middleware(void* target, context& context_value, next& next_value) {
    auto* middleware_value = static_cast<middleware_t_type*>(target);
    auto response = co_await middleware_value->handle(context_value, next_value);
    apply_middleware_response(context_value, std::move(response));
}

template <typename middleware_t_type>
[[nodiscard]] task<void> invoke_middleware(void* target, context& context_value, next& next_value) {
    auto* middleware_value = static_cast<middleware_t_type*>(target);
    if constexpr (void_handle_middleware<middleware_t_type>) {
        return middleware_value->handle(context_value, next_value);
    } else if constexpr (response_handle_middleware<middleware_t_type>) {
        return invoke_response_middleware<middleware_t_type>(target, context_value, next_value);
    } else {
        static_assert(void_handle_middleware<middleware_t_type> || response_handle_middleware<middleware_t_type>,
            "middleware must implement async Task<void> or Task<HttpResponse> handle(Context&, "
            "ruvia::Next&)");
    }
}

// Constructs the middleware from the arguments use<T>(args...) captured. The
// tuple lives on the registration resource for the process lifetime, so it is
// still readable every time the router materializes an instance -- including
// across a stop()/run() cycle, which rebuilds instances from the same
// descriptors. Registering without arguments is the empty-tuple case, not a
// separate path: std::apply then calls the default constructor.
template <typename middleware_t_type, typename args_t_type>
[[nodiscard]] void* create_middleware(const void* args) {
    return std::apply(
        [](const auto&... values) {
            return static_cast<void*>(
                construct_pmr_object<middleware_t_type>(registration_resource(), values...));
        },
        *static_cast<const args_t_type*>(args));
}

template <typename middleware_t_type>
void destroy_middleware(void* target) noexcept {
    destroy_pmr_object(static_cast<middleware_t_type*>(target), registration_resource());
}

template <typename middleware_t_type>
[[nodiscard]] const void* middleware_validated_model_type_key() noexcept {
    if constexpr (requires { typename middleware_t_type::ruvia_validation_body_type; }) {
        return request_binding_key<typename middleware_t_type::ruvia_validation_body_type>();
    } else {
        return nullptr;
    }
}

template <typename middleware_t_type>
[[nodiscard]] constexpr bool middleware_uses_route_rate_limit() noexcept {
    if constexpr (requires { middleware_t_type::ruvia_uses_route_rate_limit; }) {
        return middleware_t_type::ruvia_uses_route_rate_limit;
    } else {
        return false;
    }
}

// Whether this middleware is meaningful on a request that matched no route.
// A property of the middleware, not of where it is registered: security headers
// and request ids belong on a 404 response just as much as on a 200, while a
// validator or an authorization check has nothing to act on. Declared as
//     static constexpr bool ruvia_runs_on_unmatched_requests = true;
template <typename middleware_t_type>
[[nodiscard]] constexpr bool middleware_runs_on_unmatched_requests() noexcept {
    if constexpr (requires { middleware_t_type::ruvia_runs_on_unmatched_requests; }) {
        return middleware_t_type::ruvia_runs_on_unmatched_requests;
    } else {
        return false;
    }
}

// A request-body ceiling this middleware declares for the routes it is on, or 0
// for none. Read by the server BEFORE the body is accepted, which is why it
// cannot simply be code inside handle(): by the time a middleware runs, the
// bytes it would have rejected are already buffered.
template <typename middleware_t_type>
[[nodiscard]] constexpr std::size_t middleware_request_body_limit() noexcept {
    if constexpr (requires { middleware_t_type::ruvia_request_body_limit_bytes; }) {
        return middleware_t_type::ruvia_request_body_limit_bytes;
    } else {
        return 0;
    }
}

// A handler deadline this middleware declares for the routes it is on, in
// milliseconds, or 0 for none. Read by the server before dispatch, for the same
// reason the body limit is: by the time a middleware's handle() runs, the clock
// it wants to start has already been running.
template <typename middleware_t_type>
[[nodiscard]] constexpr bool middleware_replay_safe() noexcept {
    if constexpr (requires { middleware_t_type::ruvia_replay_safe; }) {
        return middleware_t_type::ruvia_replay_safe;
    } else {
        return false;
    }
}

template <typename middleware_t_type>
[[nodiscard]] constexpr std::int64_t middleware_deadline_ms() noexcept {
    if constexpr (requires { middleware_t_type::ruvia_deadline_ms; }) {
        return middleware_t_type::ruvia_deadline_ms;
    } else {
        return 0;
    }
}

// Registers one middleware type together with the arguments every instance of
// it is constructed from. Arguments are decayed and copied once, at
// registration; a middleware is built per router materialization, so they must
// stay readable for the process lifetime rather than the caller's scope.
// Registering without arguments is the zero-argument case of this, so there is
// one descriptor shape and one construction path.
template <typename middleware_t_type, typename... args_type>
[[nodiscard]] controller_middleware_descriptor make_middleware_descriptor(args_type&&... args) {
    static_assert(std::is_base_of_v<middleware, middleware_t_type>,
        "middleware must derive from ruvia::Middleware");
    static_assert(std::is_final_v<middleware_t_type>, "middleware must be final");
    static_assert(std::is_constructible_v<middleware_t_type, const std::decay_t<args_type>&...>,
        "middleware is not constructible from the arguments passed to use<T>(); a middleware "
        "registered without arguments must be default constructible");

    using args_t_type = std::tuple<std::decay_t<args_type>...>;
    const auto* stored =
        construct_pmr_object<args_t_type>(registration_resource(), std::forward<args_type>(args)...);
    return controller_middleware_descriptor(&invoke_middleware<middleware_t_type>,
        &create_middleware<middleware_t_type, args_t_type>, &destroy_middleware<middleware_t_type>, stored,
        middleware_validated_model_type_key<middleware_t_type>(), middleware_uses_route_rate_limit<middleware_t_type>(),
        middleware_runs_on_unmatched_requests<middleware_t_type>(), middleware_request_body_limit<middleware_t_type>(),
        middleware_deadline_ms<middleware_t_type>(), middleware_replay_safe<middleware_t_type>());
}

}  // namespace ruvia::detail
