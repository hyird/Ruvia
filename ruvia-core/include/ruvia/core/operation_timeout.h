#pragma once

#include <chrono>
#include <concepts>
#include <optional>
#include <stdexcept>
#include <variant>

#include "ruvia/core/duration.h"

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
