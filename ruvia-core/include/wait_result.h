#pragma once

#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "byte_view.h"

namespace ruvia::detail {
template <typename t_type>
class wait_value final {
public:
    template <typename u_type>
        requires std::constructible_from<t_type, u_type&&>
    explicit wait_value(u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>) :
        value_(std::forward<u_type>(value)) {}

    t_type value_;
};
}  // namespace ruvia::detail

namespace ruvia {

enum class wait_status : std::uint8_t {
    value,
    closed,
    // The bound worker is shutting down: its stopping notification arrived, or
    // a timed wait found (or was pending when) the worker's timer queue stopped.
    worker_stopping,
    timed_out,
    cancelled,
};

// Worker-bound waits have five mutually exclusive outcomes. A status is always
// available for exhaustive handling; only value owns a payload.
// The outcome category is fixed at construction. Construction and extraction
// follow the payload's copy/move semantics; a failed payload move never leaves
// an existing outcome without a category. Rebuild an owning optional to replace
// an outcome instead of assigning it. with_value() and with_status() let a
// composed worker-bound wait produce the same outcomes as the built-in ones.
template <typename t_type>
class wait_result final {
public:
    wait_result(const wait_result&) = default;
    wait_result(wait_result&&) = default;
    wait_result& operator=(const wait_result&) = delete;
    wait_result& operator=(wait_result&&) = delete;

    [[nodiscard]] wait_status status() const noexcept {
        return std::holds_alternative<detail::wait_value<t_type>>(result_) ? wait_status::value : std::get<wait_status>(result_);
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<detail::wait_value<t_type>>(result_);
    }

    [[nodiscard]] const t_type& value() const& RUVIA_LIFETIMEBOUND {
        const auto* result_value = std::get_if<detail::wait_value<t_type>>(&result_);
        if (result_value == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return result_value->value_;
    }
    const t_type& value() const&& = delete;

    [[nodiscard]] t_type take_value() && {
        auto* result_value = std::get_if<detail::wait_value<t_type>>(&result_);
        if (result_value == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return std::move(result_value->value_);
    }

    template <typename u_type>
        requires std::constructible_from<t_type, u_type&&>
    [[nodiscard]] static wait_result with_value(u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>) {
        return wait_result(std::in_place_type<detail::wait_value<t_type>>, std::forward<u_type>(value));
    }

    // Every status except value; value outcomes come only from with_value().
    [[nodiscard]] static wait_result with_status(wait_status status) {
        if (status == wait_status::value) {
            throw std::invalid_argument("worker wait status result requires a non-value status");
        }
        return wait_result(status);
    }

private:
    using result_type = std::variant<detail::wait_value<t_type>, wait_status>;

    template <typename u_type>
    explicit wait_result(std::in_place_type_t<detail::wait_value<t_type>>, u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>) :
        result_(std::in_place_type<detail::wait_value<t_type>>, std::forward<u_type>(value)) {}

    explicit wait_result(wait_status status) noexcept :
        result_(status) {}

    result_type result_;
};

}  // namespace ruvia
