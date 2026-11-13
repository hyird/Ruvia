#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/worker_state.h"

// Internal startup-time middleware descriptor.

#include "ruvia/core/task.h"
#include "ruvia/web/detail/callback_ref.h"
#include "ruvia/web/middleware.h"

namespace ruvia {

class context;

namespace detail {

class controller_middleware_descriptor;

template <middleware middleware_type, typename args_type>
[[nodiscard]] controller_middleware_descriptor describe_middleware(const void* args) noexcept;

template <route_policy policy_type>
[[nodiscard]] constexpr controller_middleware_descriptor describe_route_policy() noexcept;

class controller_middleware_descriptor final {
public:
    using invoke_type = route_middleware::invoke_type;
    // `args` points at the argument tuple captured by use<T>(args...), or is
    // null when the middleware is default constructed. The tuple belongs to the
    // owned_middleware_descriptor the app configuration keeps, which outlives
    // every plan compiled from it and every instance a worker's route table
    // materializes from this descriptor.
    using create_type = void* (*)(const void* args);
    using destroy_type = void (*)(void*) noexcept;

    [[nodiscard]] invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] create_type create() const noexcept {
        return create_;
    }

    [[nodiscard]] const void* args() const noexcept {
        return args_;
    }

    [[nodiscard]] destroy_type destroy() const noexcept {
        return destroy_;
    }

    [[nodiscard]] const void* validated_model_type_key() const noexcept {
        return validated_model_type_key_;
    }

    // Empty means app-wide. Otherwise the middleware runs exactly on requests
    // whose path is under this prefix. The compiled route plan decides that per
    // route and only checks the request path at dispatch for a route serving
    // paths on both sides of the scope. The text is registration-owned and
    // outlives the plan.
    [[nodiscard]] std::string_view prefix() const noexcept {
        return prefix_;
    }

    [[nodiscard]] controller_middleware_descriptor scoped_to(std::string_view prefix) const noexcept {
        auto scoped = *this;
        scoped.prefix_ = prefix;
        return scoped;
    }

    // See middleware_runs_on_unmatched_requests(): true means this middleware also
    // wraps the 404/405/501 terminal, not just matched routes.
    [[nodiscard]] bool runs_on_unmatched_requests() const noexcept {
        return runs_on_unmatched_requests_;
    }

    // 0 when this middleware declares no ceiling.
    [[nodiscard]] std::size_t request_body_limit() const noexcept {
        return request_body_limit_;
    }

    [[nodiscard]] bool uses_route_rate_limit() const noexcept {
        return uses_route_rate_limit_;
    }

    [[nodiscard]] bool stream_request_body() const noexcept {
        return stream_request_body_;
    }

private:
    template <middleware middleware_type, typename args_type>
    friend controller_middleware_descriptor describe_middleware(const void* args) noexcept;
    template <route_policy policy_type>
    friend constexpr controller_middleware_descriptor describe_route_policy() noexcept;

    constexpr controller_middleware_descriptor(invoke_type invoke, create_type create, destroy_type destroy,
        const void* args, const void* validated_model_type_key, bool uses_route_rate_limit,
        bool runs_on_unmatched_requests, std::size_t request_body_limit,
        bool stream_request_body = false) noexcept
        : invoke_(invoke),
          create_(create),
          destroy_(destroy),
          args_(args),
          validated_model_type_key_(validated_model_type_key),
          uses_route_rate_limit_(uses_route_rate_limit),
          request_body_limit_(request_body_limit),
          runs_on_unmatched_requests_(runs_on_unmatched_requests),
          stream_request_body_(stream_request_body) {}
    invoke_type invoke_{nullptr};
    create_type create_{nullptr};
    destroy_type destroy_{nullptr};
    const void* args_{nullptr};
    const void* validated_model_type_key_{nullptr};
    std::string_view prefix_{};
    bool uses_route_rate_limit_{false};
    std::size_t request_body_limit_{0};
    bool runs_on_unmatched_requests_{false};
    bool stream_request_body_{false};
};

// An app-level use<T>(args...) registration: the descriptor plus ownership of
// the argument tuple it points at. The app configuration holds it for its own
// lifetime, which spans every route plan compiled from it; descriptors copied
// into plans and route tables only borrow the tuple. A registration without
// arguments owns nothing.
class owned_middleware_descriptor final {
public:
    using release_args_type = void (*)(const void* args) noexcept;

    owned_middleware_descriptor(controller_middleware_descriptor descriptor, release_args_type release_args) noexcept
        : descriptor_(descriptor),
          release_args_(release_args) {}

    owned_middleware_descriptor(const owned_middleware_descriptor&) = delete;
    owned_middleware_descriptor& operator=(const owned_middleware_descriptor&) = delete;

    owned_middleware_descriptor(owned_middleware_descriptor&& other) noexcept
        : prefix_(std::move(other.prefix_)),
          descriptor_(other.descriptor_.scoped_to(prefix_)),
          release_args_(std::exchange(other.release_args_, nullptr)) {}
    owned_middleware_descriptor& operator=(owned_middleware_descriptor&&) = delete;

    ~owned_middleware_descriptor() {
        if (auto* const release = std::exchange(release_args_, nullptr)) {
            release(descriptor_.args());
        }
    }

    [[nodiscard]] const controller_middleware_descriptor& descriptor() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] owned_middleware_descriptor scoped_to(std::string_view prefix) && {
        prefix_.assign(prefix);
        descriptor_ = descriptor_.scoped_to(prefix_);
        return std::move(*this);
    }

private:
    std::string prefix_{};
    controller_middleware_descriptor descriptor_;
    release_args_type release_args_{nullptr};
};

}  // namespace detail

}  // namespace ruvia

namespace ruvia::detail {

template <middleware middleware_type>
[[nodiscard]] task<void> invoke_middleware(void* target, context& context_value, next& next_value) {
    if constexpr (requires {
                      static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::invoke);
                  }) {
        constexpr auto method =
            static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::invoke);
        return (static_cast<middleware_type*>(target)->*method)(context_value, next_value);
    } else {
        constexpr auto method =
            static_cast<task<void> (middleware_type::*)(context&, next&) const>(&middleware_type::invoke);
        return (static_cast<const middleware_type*>(target)->*method)(context_value, next_value);
    }
}

// Constructs the middleware from the arguments use<T>(args...) captured. The
// tuple is owned by the app configuration's owned_middleware_descriptor, so it
// is still readable every time a worker's route table materializes an instance
// -- including across a stop()/run() cycle, which rebuilds instances from the
// same descriptors. Registering without arguments stores no tuple at all: the
// descriptor's args are null and the default constructor runs.
template <typename middleware_type, typename args_type>
[[nodiscard]] void* create_middleware(const void* args) {
    if constexpr (std::tuple_size_v<args_type> == 0) {
        return static_cast<void*>(new middleware_type());
    } else {
        return std::apply(
            [](const auto&... values) {
                return static_cast<void*>(new middleware_type(values...));
            },
            *static_cast<const args_type*>(args));
    }
}

template <typename middleware_type>
void destroy_middleware(void* target) noexcept {
    delete static_cast<middleware_type*>(target);
}

template <typename args_type>
void release_middleware_args(const void* args) noexcept {
    delete static_cast<const args_type*>(args);
}

template <typename middleware_type>
[[nodiscard]] const void* middleware_validated_model_type_key() noexcept {
    if constexpr (requires { typename middleware_type::ruvia_validation_body_type; }) {
        return type_key<typename middleware_type::ruvia_validation_body_type>();
    } else {
        return nullptr;
    }
}

template <typename middleware_type>
[[nodiscard]] constexpr bool middleware_uses_route_rate_limit() noexcept {
    if constexpr (requires { middleware_type::ruvia_uses_route_rate_limit; }) {
        return middleware_type::ruvia_uses_route_rate_limit;
    } else {
        return false;
    }
}

// Whether this middleware is meaningful on a request that matched no route.
// A property of the middleware, not of where it is registered: security headers
// and request ids belong on a 404 response just as much as on a 200, while a
// validator or an authorization check has nothing to act on. Declared as
//     static constexpr bool ruvia_runs_on_unmatched_requests = true;
template <typename middleware_type>
[[nodiscard]] constexpr bool middleware_runs_on_unmatched_requests() noexcept {
    if constexpr (requires { middleware_type::ruvia_runs_on_unmatched_requests; }) {
        return middleware_type::ruvia_runs_on_unmatched_requests;
    } else {
        return false;
    }
}

// A request-body ceiling this middleware declares for the routes it is on, or 0
// for none. Read by the server BEFORE the body is accepted, which is why it
// cannot simply be code inside invoke(): by the time a middleware runs, the
// bytes it would have rejected are already buffered.
template <typename middleware_type>
[[nodiscard]] constexpr std::size_t middleware_request_body_limit() noexcept {
    if constexpr (requires { middleware_type::ruvia_request_body_limit_bytes; }) {
        return middleware_type::ruvia_request_body_limit_bytes;
    } else {
        return 0;
    }
}

template <typename middleware_type>
[[nodiscard]] constexpr bool middleware_stream_request_body() noexcept {
    if constexpr (requires { middleware_type::ruvia_stream_request_body; }) {
        return middleware_type::ruvia_stream_request_body;
    } else {
        return false;
    }
}

// The one description of a middleware type: its entry points and declared
// route properties, bound to the argument tuple (null when there is none).
template <middleware middleware_type, typename args_type>
[[nodiscard]] controller_middleware_descriptor describe_middleware(const void* args) noexcept {
    return controller_middleware_descriptor(&invoke_middleware<middleware_type>,
        &create_middleware<middleware_type, args_type>, &destroy_middleware<middleware_type>, args,
        middleware_validated_model_type_key<middleware_type>(), middleware_uses_route_rate_limit<middleware_type>(),
        middleware_runs_on_unmatched_requests<middleware_type>(), middleware_request_body_limit<middleware_type>(),
        middleware_stream_request_body<middleware_type>());
}

// A default-constructed middleware, as controller and route middleware lists
// declare it. It captures nothing, so it allocates nothing and owns nothing:
// route registration reruns on every route plan compile.
template <middleware middleware_type>
[[nodiscard]] controller_middleware_descriptor make_middleware_descriptor() noexcept {
    static_assert(std::is_default_constructible_v<middleware_type>,
        "middleware registered without arguments must be default constructible");
    return describe_middleware<middleware_type, std::tuple<>>(nullptr);
}

template <route_policy policy_type>
[[nodiscard]] constexpr controller_middleware_descriptor describe_route_policy() noexcept {
    return controller_middleware_descriptor(nullptr, nullptr, nullptr, nullptr, nullptr, false, false,
        middleware_request_body_limit<policy_type>(), middleware_stream_request_body<policy_type>());
}

template <route_modifier modifier_type>
[[nodiscard]] controller_middleware_descriptor make_route_modifier_descriptor() noexcept {
    if constexpr (route_policy<modifier_type>) {
        return describe_route_policy<modifier_type>();
    } else {
        return make_middleware_descriptor<modifier_type>();
    }
}

// Registers one app-level middleware type together with the arguments every
// instance of it is constructed from. Arguments are decayed and copied once,
// at registration; a middleware is built per worker route table, so they must
// stay readable for the app configuration's lifetime rather than the caller's
// scope. The returned owner destroys them. Registering without arguments
// stores no tuple.
template <middleware middleware_type, typename... args_types>
[[nodiscard]] owned_middleware_descriptor make_owned_middleware_descriptor(args_types&&... args) {
    static_assert(std::is_constructible_v<middleware_type, const std::decay_t<args_types>&...>,
        "middleware is not constructible from the arguments passed to use<T>(); a middleware "
        "registered without arguments must be default constructible");

    if constexpr (sizeof...(args_types) == 0) {
        return owned_middleware_descriptor(make_middleware_descriptor<middleware_type>(), nullptr);
    } else {
        using args_tuple_type = std::tuple<std::decay_t<args_types>...>;
        const auto* stored = new args_tuple_type(std::forward<args_types>(args)...);
        return owned_middleware_descriptor(
            describe_middleware<middleware_type, args_tuple_type>(stored), &release_middleware_args<args_tuple_type>);
    }
}

}  // namespace ruvia::detail
