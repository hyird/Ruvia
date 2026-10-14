#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

// Internal startup-time middleware descriptor.

#include "ruvia/core/task.h"
#include "ruvia/web/detail/router/route_handlers.h"
#include "ruvia/web/middleware.h"

namespace ruvia {

class context;

namespace detail {

class controller_middleware_descriptor;

template <middleware middleware_t_type, typename args_t_type>
[[nodiscard]] controller_middleware_descriptor describe_middleware(const void* args) noexcept;

class controller_middleware_descriptor final {
public:
    using invoke_type = route_middleware_type::invoke_type;
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

private:
    template <middleware middleware_t_type, typename args_t_type>
    friend controller_middleware_descriptor describe_middleware(const void* args) noexcept;

    constexpr controller_middleware_descriptor(invoke_type invoke, create_type create, destroy_type destroy,
        const void* args, const void* validated_model_type_key, bool uses_route_rate_limit,
        bool runs_on_unmatched_requests, std::size_t request_body_limit) noexcept
        : invoke_(invoke),
          create_(create),
          destroy_(destroy),
          args_(args),
          validated_model_type_key_(validated_model_type_key),
          uses_route_rate_limit_(uses_route_rate_limit),
          request_body_limit_(request_body_limit),
          runs_on_unmatched_requests_(runs_on_unmatched_requests) {}

    invoke_type invoke_{nullptr};
    create_type create_{nullptr};
    destroy_type destroy_{nullptr};
    const void* args_{nullptr};
    const void* validated_model_type_key_{nullptr};
    std::string_view prefix_{};
    bool uses_route_rate_limit_{false};
    std::size_t request_body_limit_{0};
    bool runs_on_unmatched_requests_{false};
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
        : descriptor_(other.descriptor_),
          release_args_(std::exchange(other.release_args_, nullptr)) {}
    owned_middleware_descriptor& operator=(owned_middleware_descriptor&&) = delete;

    ~owned_middleware_descriptor() {
        if (release_args_ != nullptr) {
            release_args_(descriptor_.args());
        }
    }

    [[nodiscard]] const controller_middleware_descriptor& descriptor() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] owned_middleware_descriptor scoped_to(std::string_view prefix) && noexcept {
        descriptor_ = descriptor_.scoped_to(prefix);
        return std::move(*this);
    }

private:
    controller_middleware_descriptor descriptor_;
    release_args_type release_args_{nullptr};
};

}  // namespace detail

}  // namespace ruvia
