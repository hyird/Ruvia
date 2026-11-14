#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "move_only_function.h"

namespace ruvia::detail {
template <typename t_type>
class worker_wait_value final {
public:
    template <typename u_type>
        requires std::constructible_from<t_type, u_type&&>
    explicit worker_wait_value(u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>) :
        value_(std::forward<u_type>(value)) {}

    t_type value_;
};

template <typename t_type>
inline constexpr bool is_move_only_function = false;
template <typename signature_type>
inline constexpr bool is_move_only_function<::ruvia::move_only_function<signature_type>> = true;

struct worker_delivery_access;
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
// The outcome category is fixed at construction. Construction and extraction
// follow the payload's copy/move semantics; a failed payload move never leaves
// an existing outcome without a category. Rebuild an owning optional to replace
// an outcome instead of assigning it. with_value() and with_status() let a
// composed worker-bound wait produce the same outcomes as the built-in ones.
template <typename t_type>
class worker_wait_result final {
public:
    worker_wait_result(const worker_wait_result&) = default;
    worker_wait_result(worker_wait_result&&) = default;
    worker_wait_result& operator=(const worker_wait_result&) = delete;
    worker_wait_result& operator=(worker_wait_result&&) = delete;

    [[nodiscard]] worker_wait_status status() const noexcept {
        return std::holds_alternative<detail::worker_wait_value<t_type>>(result_) ? worker_wait_status::value : std::get<worker_wait_status>(result_);
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

    template <typename u_type>
        requires std::constructible_from<t_type, u_type&&>
    [[nodiscard]] static worker_wait_result with_value(u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>) {
        return worker_wait_result(std::in_place_type<detail::worker_wait_value<t_type>>, std::forward<u_type>(value));
    }

    // Every status except value; value outcomes come only from with_value().
    [[nodiscard]] static worker_wait_result with_status(worker_wait_status status) {
        if (status == worker_wait_status::value) {
            throw std::invalid_argument("worker wait status result requires a non-value status");
        }
        return worker_wait_result(status);
    }

private:
    using result_type = std::variant<detail::worker_wait_value<t_type>, worker_wait_status>;

    template <typename u_type>
    explicit worker_wait_result(std::in_place_type_t<detail::worker_wait_value<t_type>>, u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>) :
        result_(std::in_place_type<detail::worker_wait_value<t_type>>, std::forward<u_type>(value)) {}

    explicit worker_wait_result(worker_wait_status status) noexcept :
        result_(status) {}

    result_type result_;
};

// The outcome of a bounded delivery: post, submit, send, complete, or start.
// The category is fixed at construction. A rejection owns the unconsumed
// payload; when accepted_type is not void, acceptance owns that value (the
// started operation). take_rejected() and take_value() both consume their
// payload. Only the delivering primitive produces results, so an accepted
// result always reflects a real admission.
template <typename t_type, typename status_type, status_type accepted_status, typename accepted_type = void>
class worker_delivery_result final {
    using accepted_storage_type = std::conditional_t<std::is_void_v<accepted_type>, std::monostate, accepted_type>;

public:
    worker_delivery_result(const worker_delivery_result&) = delete;
    worker_delivery_result& operator=(const worker_delivery_result&) = delete;
    worker_delivery_result(worker_delivery_result&&) noexcept(std::is_nothrow_move_constructible_v<t_type> && std::is_nothrow_move_constructible_v<accepted_storage_type>) = default;
    worker_delivery_result& operator=(worker_delivery_result&&) = delete;

    [[nodiscard]] status_type status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return status_ == accepted_status;
    }

    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] self_type& value() & {
        return require_accepted();
    }
    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] const self_type& value() const& {
        return const_cast<worker_delivery_result&>(*this).require_accepted();
    }
    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    self_type& value() && = delete;
    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    const self_type& value() const&& = delete;

    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] self_type take_value() && {
        auto& accepted_value = require_accepted();
        struct clear_accepted final {
            std::optional<accepted_storage_type>& accepted_;
            ~clear_accepted() {
                accepted_.reset();
            }
        } clear{accepted_};
        return std::move(accepted_value);
    }

    [[nodiscard]] t_type* rejected() & noexcept {
        return rejected_ ? std::addressof(*rejected_) : nullptr;
    }

    [[nodiscard]] const t_type* rejected() const& noexcept {
        return rejected_ ? std::addressof(*rejected_) : nullptr;
    }

    t_type* rejected() && = delete;
    const t_type* rejected() const&& = delete;

    // A failed extraction retains the payload, in the state its move left it.
    // Only successful return-value construction consumes the rejected value.
    [[nodiscard]] t_type take_rejected() && {
        if (status_ == accepted_status || !rejected_) {
            throw std::logic_error("delivery result has no rejected value");
        }
        struct clear_rejected final {
            std::optional<t_type>& rejected_;
            const int exceptions_{std::is_nothrow_move_constructible_v<t_type> ? 0 : std::uncaught_exceptions()};
            ~clear_rejected() {
                if constexpr (std::is_nothrow_move_constructible_v<t_type>) {
                    rejected_.reset();
                } else if (std::uncaught_exceptions() == exceptions_) {
                    rejected_.reset();
                }
            }
        } clear{rejected_};
        return std::move(*rejected_);
    }

    friend bool operator==(const worker_delivery_result& outcome, status_type status) noexcept {
        return outcome.status_ == status;
    }
    friend bool operator==(status_type status, const worker_delivery_result& outcome) noexcept {
        return outcome == status;
    }

private:
    friend struct detail::worker_delivery_access;

    explicit worker_delivery_result(status_type status) noexcept :
        status_(status) {}

    worker_delivery_result(status_type status, std::in_place_t, accepted_storage_type&& accepted_value) noexcept(std::is_nothrow_move_constructible_v<accepted_storage_type>) :
        status_(status),
        accepted_(std::move(accepted_value)) {}

    worker_delivery_result(status_type status, t_type&& rejected) noexcept(std::is_nothrow_move_constructible_v<t_type>) :
        status_(status),
        rejected_(std::move(rejected)) {}

    [[nodiscard]] accepted_storage_type& require_accepted() {
        if (status_ != accepted_status || !accepted_) {
            throw std::logic_error("delivery result has no accepted value");
        }
        return *accepted_;
    }

    status_type status_;
    std::optional<t_type> rejected_;
    [[no_unique_address]] std::conditional_t<std::is_void_v<accepted_type>, std::monostate, std::optional<accepted_storage_type>> accepted_;
};

namespace detail {

// The producer side of worker_delivery_result, reserved to the delivering
// primitives so callers cannot fabricate an accepted outcome.
struct worker_delivery_access final {
    template <typename result_type>
    [[nodiscard]] static result_type accept() noexcept {
        return result_type(accepted_status_of<result_type>());
    }

    template <typename result_type, typename accepted_value_type>
    [[nodiscard]] static result_type accept(accepted_value_type&& accepted_value) noexcept(std::is_nothrow_move_constructible_v<std::remove_cvref_t<accepted_value_type>>) {
        return result_type(accepted_status_of<result_type>(), std::in_place, std::forward<accepted_value_type>(accepted_value));
    }

    template <typename result_type, typename status_type, typename rejected_type>
    [[nodiscard]] static result_type reject(status_type status, rejected_type&& value) {
        if (status == accepted_status_of<result_type>()) {
            throw std::invalid_argument("rejected delivery result requires a rejection status");
        }
        if constexpr (is_move_only_function<std::remove_cvref_t<rejected_type>>) {
            if (!value) {
                throw std::invalid_argument("rejected delivery result requires a callable task");
            }
        }
        return result_type(status, std::forward<rejected_type>(value));
    }

private:
    template <typename result_type>
    struct accepted_status_traits;

    template <typename t_type, typename status_type, status_type accepted_status, typename accepted_type>
    struct accepted_status_traits<worker_delivery_result<t_type, status_type, accepted_status, accepted_type>> final {
        static constexpr status_type value = accepted_status;
    };

    template <typename result_type>
    [[nodiscard]] static constexpr auto accepted_status_of() noexcept {
        return accepted_status_traits<result_type>::value;
    }
};

}  // namespace detail

}  // namespace ruvia
