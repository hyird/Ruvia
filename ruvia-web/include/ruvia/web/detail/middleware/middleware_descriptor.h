#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Internal startup-time middleware descriptor.

#include "ruvia/core/task.h"
#include "ruvia/web/next.h"

namespace ruvia {

class context;

namespace detail {

class controller_middleware_descriptor;

template <typename middleware_t_type, typename... args_type>
[[nodiscard]] controller_middleware_descriptor make_middleware_descriptor(args_type&&... args);

class controller_middleware_descriptor final {
public:
    using invoke_type = task<void> (*)(void*, context&, next&);
    // `args` points at the registration-owned argument tuple captured by
    // use<T>(args...) -- empty when the middleware is default constructed. It
    // lives on the process registration resource, so it outlives every instance
    // the router materializes from this descriptor.
    using create_type = void* (*)(const void* args);
    using destroy_type = void (*)(void*) noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return invoke_ != nullptr;
    }

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

    // Two registrations of the same middleware type differ when they carry
    // different arguments, so identity spans the argument pointer as well.
    [[nodiscard]] friend bool operator==(const controller_middleware_descriptor& left,
        const controller_middleware_descriptor& right) noexcept {
        return left.invoke_ == right.invoke_ && left.create_ == right.create_ &&
               left.destroy_ == right.destroy_ && left.args_ == right.args_ &&
               left.prefix_ == right.prefix_;
    }

    [[nodiscard]] const void* validated_model_type_key() const noexcept {
        return validated_model_type_key_;
    }

    // Empty means app-wide. Otherwise the middleware runs only on routes whose
    // path is under this prefix, decided once when the route table is built --
    // there is no per-request pattern matching. The text is registration-owned
    // and outlives the table.
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

    // 0 when this middleware declares no deadline.
    [[nodiscard]] std::int64_t deadline_ms() const noexcept {
        return deadline_ms_;
    }

    [[nodiscard]] bool uses_route_rate_limit() const noexcept {
        return uses_route_rate_limit_;
    }

    [[nodiscard]] bool replay_safe() const noexcept {
        return replay_safe_;
    }

private:
    template <typename middleware_t_type, typename... args_type>
    friend controller_middleware_descriptor make_middleware_descriptor(args_type&&... args);

    constexpr controller_middleware_descriptor() noexcept = default;
    constexpr controller_middleware_descriptor(invoke_type invoke, create_type create, destroy_type destroy,
        const void* args, const void* validated_model_type_key, bool uses_route_rate_limit,
        bool runs_on_unmatched_requests = false, std::size_t request_body_limit = 0,
        std::int64_t deadline_ms = 0, bool replay_safe = false) noexcept
        : invoke_(invoke),
          create_(create),
          destroy_(destroy),
          args_(args),
          validated_model_type_key_(validated_model_type_key),
          uses_route_rate_limit_(uses_route_rate_limit),
          request_body_limit_(request_body_limit),
          deadline_ms_(deadline_ms),
          runs_on_unmatched_requests_(runs_on_unmatched_requests),
          replay_safe_(replay_safe) {}

    invoke_type invoke_{nullptr};
    create_type create_{nullptr};
    destroy_type destroy_{nullptr};
    const void* args_{nullptr};
    const void* validated_model_type_key_{nullptr};
    std::string_view prefix_{};
    bool uses_route_rate_limit_{false};
    std::size_t request_body_limit_{0};
    std::int64_t deadline_ms_{0};
    bool runs_on_unmatched_requests_{false};
    bool replay_safe_{false};
};

}  // namespace detail

}  // namespace ruvia
