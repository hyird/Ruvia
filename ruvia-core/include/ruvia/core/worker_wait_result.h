#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia {
template <typename signature_type>
class move_only_function;
}  // namespace ruvia

namespace ruvia::detail {
struct worker_wait_result_access;

template <typename t_type>
class worker_wait_value final {
public:
    template <typename u_type>
        requires std::constructible_from<t_type, u_type&&>
    explicit worker_wait_value(u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>)
        : value_(std::forward<u_type>(value)) {}

    t_type value_;
};

template <typename t_type>
inline constexpr bool is_move_only_function = false;
template <typename signature_type>
inline constexpr bool is_move_only_function<::ruvia::move_only_function<signature_type>> = true;
}  // namespace ruvia::detail

namespace ruvia {

enum class worker_wait_status : std::uint8_t {
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
template <typename t_type>
class worker_wait_result final {
public:
    [[nodiscard]] worker_wait_status status() const noexcept {
        return std::holds_alternative<detail::worker_wait_value<t_type>>(result_)
                   ? worker_wait_status::value
                   : std::get<worker_wait_status>(result_);
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<detail::worker_wait_value<t_type>>(result_);
    }

    [[nodiscard]] const t_type& value() const& {
        const auto* result_value = std::get_if<detail::worker_wait_value<t_type>>(&result_);
        if (result_value == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return result_value->value_;
    }
    const t_type& value() const&& = delete;

    [[nodiscard]] t_type& value() & {
        auto* result_value = std::get_if<detail::worker_wait_value<t_type>>(&result_);
        if (result_value == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return result_value->value_;
    }
    t_type& value() && = delete;

    [[nodiscard]] t_type take_value() && {
        auto* result_value = std::get_if<detail::worker_wait_value<t_type>>(&result_);
        if (result_value == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return std::move(result_value->value_);
    }

private:
    friend struct detail::worker_wait_result_access;

    using result_type = std::variant<detail::worker_wait_value<t_type>, worker_wait_status>;

    template <typename u_type>
    explicit worker_wait_result(
        std::in_place_type_t<detail::worker_wait_value<t_type>>, u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>)
        : result_(std::in_place_type<detail::worker_wait_value<t_type>>, std::forward<u_type>(value)) {}

    explicit worker_wait_result(worker_wait_status status) noexcept
        : result_(status) {
        if (status == worker_wait_status::value) {
            std::terminate();
        }
    }

    result_type result_;
};

template <typename t_type, typename status_type, status_type accepted_status>
class worker_delivery_result final {
public:
    worker_delivery_result(const worker_delivery_result&) = delete;
    worker_delivery_result& operator=(const worker_delivery_result&) = delete;
    worker_delivery_result(worker_delivery_result&&) noexcept(
        std::is_nothrow_move_constructible_v<t_type>) = default;
    worker_delivery_result& operator=(worker_delivery_result&&) noexcept(
        std::is_nothrow_move_constructible_v<t_type> && std::is_nothrow_move_assignable_v<t_type>) = default;

    [[nodiscard]] status_type status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return status_ == accepted_status;
    }

    [[nodiscard]] t_type* rejected() & noexcept {
        return rejected_ ? &*rejected_ : nullptr;
    }

    [[nodiscard]] const t_type* rejected() const& noexcept {
        return rejected_ ? &*rejected_ : nullptr;
    }

    t_type* rejected() && = delete;
    const t_type* rejected() const&& = delete;

    [[nodiscard]] t_type take_rejected() && {
        if (status_ == accepted_status || !rejected_) {
            throw std::logic_error("accepted delivery result has no rejected value");
        }
        struct clear_rejected final {
            std::optional<t_type>& rejected_;
            ~clear_rejected() {
                rejected_.reset();
            }
        } clear{rejected_};
        return std::move(*rejected_);
    }

    [[nodiscard]] static worker_delivery_result accept() noexcept {
        return worker_delivery_result(accepted_status);
    }

    [[nodiscard]] static worker_delivery_result reject(status_type status, t_type&& value) {
        if (status == accepted_status) {
            throw std::invalid_argument("rejected delivery result requires a rejection status");
        }
        if constexpr (detail::is_move_only_function<t_type>) {
            if (!value) {
                throw std::invalid_argument("rejected delivery result requires a callable task");
            }
        }
        return worker_delivery_result(status, std::move(value));
    }

    friend bool operator==(const worker_delivery_result& outcome, status_type status) noexcept {
        return outcome.status_ == status;
    }
    friend bool operator==(status_type status, const worker_delivery_result& outcome) noexcept {
        return outcome == status;
    }

private:
    explicit worker_delivery_result(status_type status) noexcept
        : status_(status) {}

    worker_delivery_result(status_type status, t_type&& rejected) noexcept(
        std::is_nothrow_move_constructible_v<t_type>)
        : status_(status),
          rejected_(std::move(rejected)) {}

    status_type status_;
    std::optional<t_type> rejected_;
};

}  // namespace ruvia

namespace ruvia::detail {

struct worker_wait_result_access final {
    template <typename t_type>
    [[nodiscard]] static worker_wait_result<std::remove_cvref_t<t_type>> value(t_type&& value) noexcept(
        std::is_nothrow_constructible_v<std::remove_cvref_t<t_type>, t_type&&>) {
        using value_type = std::remove_cvref_t<t_type>;
        return worker_wait_result<value_type>(
            std::in_place_type<worker_wait_value<value_type>>, std::forward<t_type>(value));
    }

    template <typename t_type>
    [[nodiscard]] static worker_wait_result<t_type> outcome(worker_wait_status status) noexcept {
        return worker_wait_result<t_type>(status);
    }
};

}  // namespace ruvia::detail
