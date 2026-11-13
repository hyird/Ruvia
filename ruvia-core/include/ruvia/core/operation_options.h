#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia {

// The bound of one operation: either a positive relative limit that starts
// when the operation starts, or an absolute steady-clock deadline, which may
// already have passed. A value is valid by construction: the relative
// constructors throw std::invalid_argument for a non-positive limit, and
// try_create() is the tolerant form for values that may legitimately be
// non-positive (configuration, remaining budgets).
class operation_timeout final {
public:
    using clock_type = std::chrono::steady_clock;

    // Implicit so `{.timeout_ = 5s}` stays writable. Rounds up to whole
    // milliseconds and saturates, so a positive input is at least 1 ms.
    template <std::integral rep_type, typename period_type>
    operation_timeout(std::chrono::duration<rep_type, period_type> value)
        : value_(require_positive(saturating_duration_cast<std::chrono::milliseconds>(value), value.count() > 0)) {}

    operation_timeout(timeout_duration value)
        : value_(require_positive(value.value(), value.value().count() > 0)) {}

    explicit operation_timeout(clock_type::time_point deadline) noexcept
        : value_(deadline) {}

    template <std::integral rep_type, typename period_type>
    [[nodiscard]] static std::optional<operation_timeout> try_create(
        std::chrono::duration<rep_type, period_type> value) noexcept {
        if (value.count() <= 0) {
            return std::nullopt;
        }
        return operation_timeout(relative_tag{}, saturating_duration_cast<std::chrono::milliseconds>(value));
    }

    [[nodiscard]] static std::optional<operation_timeout> try_create(timeout_duration value) noexcept {
        if (value.value().count() <= 0) {
            return std::nullopt;
        }
        return operation_timeout(relative_tag{}, value.value());
    }

    // The relative limit; empty for an absolute deadline.
    [[nodiscard]] std::optional<std::chrono::milliseconds> duration() const noexcept {
        if (const auto* relative = std::get_if<std::chrono::milliseconds>(&value_)) {
            return *relative;
        }
        return std::nullopt;
    }

    // The absolute deadline; empty for a relative limit.
    [[nodiscard]] std::optional<clock_type::time_point> deadline() const noexcept {
        if (const auto* absolute = std::get_if<clock_type::time_point>(&value_)) {
            return *absolute;
        }
        return std::nullopt;
    }

    // Resolves the bound for an operation that starts at `start`.
    [[nodiscard]] clock_type::time_point deadline_from(clock_type::time_point start) const noexcept {
        if (const auto* relative = std::get_if<std::chrono::milliseconds>(&value_)) {
            return saturating_deadline(start, *relative);
        }
        return std::get<clock_type::time_point>(value_);
    }

    friend bool operator==(const operation_timeout&, const operation_timeout&) noexcept = default;

private:
    struct relative_tag final {};

    operation_timeout(relative_tag, std::chrono::milliseconds value) noexcept
        : value_(value) {}

    [[nodiscard]] static std::chrono::milliseconds require_positive(std::chrono::milliseconds value, bool positive) {
        if (!positive) {
            throw std::invalid_argument("operation timeout must be greater than zero");
        }
        return value;
    }

    std::variant<std::chrono::milliseconds, clock_type::time_point> value_;
};

}  // namespace ruvia

namespace ruvia {

// The single per-operation input of every cold wait and outbound operation.
// timeout_ bounds the operation end to end: a relative limit starts when the
// operation starts, an absolute deadline is used as given.
struct operation_options final {
    std::optional<operation_timeout> timeout_{};
    stop_token stop_token_{};
};

// Unspecified fields inherit. A supplied token replaces the user cancellation
// state; callers needing accumulation own a stop_group until their work joins.
// A supplied timeout may tighten but never relax the inherited bound: two
// relative limits keep the shorter, two deadlines keep the earlier, and a
// mixed pair resolves the relative limit against the merge time and keeps the
// earlier deadline.
[[nodiscard]] inline operation_options override_operation_options(
    const operation_options& base, operation_options overrides) {
    operation_options merged = base;
    if (overrides.timeout_.has_value()) {
        if (!merged.timeout_.has_value()) {
            merged.timeout_ = overrides.timeout_;
        } else {
            const auto& current = *merged.timeout_;
            const auto& candidate = *overrides.timeout_;
            const auto current_duration = current.duration();
            const auto candidate_duration = candidate.duration();
            if (current_duration.has_value() && candidate_duration.has_value()) {
                if (*candidate_duration < *current_duration) {
                    merged.timeout_ = candidate;
                }
            } else {
                const auto now = operation_timeout::clock_type::now();
                const auto current_deadline = current.deadline_from(now);
                const auto candidate_deadline = candidate.deadline_from(now);
                merged.timeout_ = operation_timeout(std::min(current_deadline, candidate_deadline));
            }
        }
    }
    if (overrides.stop_token_.has_state()) {
        merged.stop_token_ = std::move(overrides.stop_token_);
    }
    return merged;
}

}  // namespace ruvia

namespace ruvia {

// The resolved deadline of an operation that has started, shared across its
// asynchronous phases. Phases that hand the remaining budget to a nested
// operation pass timeout(), the same absolute deadline, rather than a fresh
// relative limit.
class operation_deadline final {
public:
    using clock_type = std::chrono::steady_clock;

    // Unbounded.
    operation_deadline() noexcept = default;

    // Resolves `timeout` against the current time; empty stays unbounded.
    explicit operation_deadline(const std::optional<operation_timeout>& timeout) noexcept {
        if (timeout.has_value()) {
            deadline_ = timeout->deadline_from(clock_type::now());
        }
    }

    explicit operation_deadline(clock_type::time_point deadline) noexcept
        : deadline_(deadline) {}

    [[nodiscard]] std::optional<clock_type::time_point> deadline() const noexcept {
        return deadline_;
    }

    // Zero once expired; positive remainders round up to whole milliseconds.
    [[nodiscard]] std::optional<std::chrono::milliseconds> remaining() const noexcept {
        if (!deadline_.has_value()) {
            return std::nullopt;
        }
        return ceil_milliseconds(*deadline_ - clock_type::now());
    }

    [[nodiscard]] bool expired() const noexcept {
        return deadline_.has_value() && clock_type::now() >= *deadline_;
    }

    // The earlier of this deadline and `timeout` resolved now.
    [[nodiscard]] operation_deadline constrained_by(const std::optional<operation_timeout>& timeout) const noexcept {
        operation_deadline constrained(timeout);
        if (deadline_.has_value() && (!constrained.deadline_.has_value() || *deadline_ < *constrained.deadline_)) {
            constrained.deadline_ = deadline_;
        }
        return constrained;
    }

    // The same deadline as an absolute operation_timeout for a nested
    // operation; empty when unbounded.
    [[nodiscard]] std::optional<operation_timeout> timeout() const noexcept {
        if (!deadline_.has_value()) {
            return std::nullopt;
        }
        return operation_timeout(*deadline_);
    }

private:
    std::optional<clock_type::time_point> deadline_;
};

// One allocation-free deadline lifecycle. A deadline is either absent, armed
// with its cancellation kind and exact time point, or expired with that same
// kind retained until the awaiting operation consumes the outcome.
template <typename kind_type = std::monostate>
    requires(std::is_enum_v<kind_type> || std::is_same_v<kind_type, std::monostate>)
class deadline_lifecycle final {
public:
    using clock_type = std::chrono::steady_clock;

    void arm(clock_type::time_point deadline_value, kind_type kind) noexcept {
        state_.template emplace<active>(deadline_value, std::move(kind));
    }

    void arm(clock_type::time_point deadline_value) noexcept
        requires std::is_same_v<kind_type, std::monostate>
    {
        arm(deadline_value, std::monostate{});
    }

    void reset() noexcept {
        state_.template emplace<inactive>();
    }

    [[nodiscard]] bool expired() const noexcept {
        return std::holds_alternative<expired_state>(state_);
    }

    [[nodiscard]] const kind_type* kind() const& noexcept {
        if (const auto* armed = std::get_if<active>(&state_)) {
            return &armed->kind_;
        }
        if (const auto* expired_value = std::get_if<expired_state>(&state_)) {
            return &expired_value->kind_;
        }
        return nullptr;
    }
    const kind_type* kind() const&& = delete;

    [[nodiscard]] std::optional<kind_type> expire(clock_type::time_point now) noexcept {
        const auto* armed = std::get_if<active>(&state_);
        if (armed == nullptr || armed->deadline_ > now) {
            return std::nullopt;
        }
        auto kind = armed->kind_;
        state_.template emplace<expired_state>(kind);
        return kind;
    }

    [[nodiscard]] bool clear() noexcept {
        const bool was_expired = expired();
        reset();
        return was_expired;
    }

private:
    struct inactive final {};

    struct active final {
        active(clock_type::time_point armed_deadline, kind_type armed_kind) noexcept
            : deadline_(armed_deadline),
              kind_(std::move(armed_kind)) {}

        clock_type::time_point deadline_;
        kind_type kind_;
    };

    struct expired_state final {
        explicit expired_state(kind_type expired_kind) noexcept
            : kind_(std::move(expired_kind)) {}

        kind_type kind_;
    };

    using state_type = std::variant<inactive, active, expired_state>;

    state_type state_;
};

}  // namespace ruvia
