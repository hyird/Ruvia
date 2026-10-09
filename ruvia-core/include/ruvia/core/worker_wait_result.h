#pragma once

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia::detail {
struct worker_wait_result_access;

template <typename t_type>
class worker_wait_value final {
public:
    explicit worker_wait_value(t_type value) noexcept(std::is_nothrow_move_constructible_v<t_type>)
        : value_(std::move(value)) {}

    t_type value_;
};
}  // namespace ruvia::detail

namespace ruvia {

enum class worker_wait_status : std::uint8_t {
    value,
    closed,
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

    explicit worker_wait_result(detail::worker_wait_value<t_type> value) noexcept(
        std::is_nothrow_move_constructible_v<t_type>)
        : result_(std::move(value)) {}

    explicit worker_wait_result(worker_wait_status status) noexcept
        : result_(status) {
        if (status == worker_wait_status::value) {
            std::terminate();
        }
    }

    result_type result_;
};

}  // namespace ruvia

namespace ruvia::detail {

struct worker_wait_result_access final {
    template <typename t_type>
    [[nodiscard]] static worker_wait_result<t_type> value(t_type value) noexcept(
        std::is_nothrow_move_constructible_v<t_type>) {
        return worker_wait_result<t_type>(worker_wait_value<t_type>(std::move(value)));
    }

    template <typename t_type>
    [[nodiscard]] static worker_wait_result<t_type> outcome(worker_wait_status status) noexcept {
        return worker_wait_result<t_type>(status);
    }
};

}  // namespace ruvia::detail
