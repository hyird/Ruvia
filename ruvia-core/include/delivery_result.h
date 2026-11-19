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

#include "byte_view.h"
#include "move_only_function.h"

namespace ruvia::detail {
template <typename t_type>
inline constexpr bool is_move_only_function = false;
template <typename signature_type>
inline constexpr bool is_move_only_function<::ruvia::move_only_function<signature_type>> = true;

struct delivery_access;
}  // namespace ruvia::detail

namespace ruvia {

// The outcome of a bounded delivery: post, send, complete, or start.
// The category is fixed at construction. A rejection owns the unconsumed
// payload; when accepted_type is not void, acceptance owns that value (the
// started operation). take_rejected() and take_value() both consume their
// payload. Only the delivering primitive produces results, so an accepted
// result always reflects a real admission.
template <typename t_type, typename status_type, status_type accepted_status, typename accepted_type = void>
class delivery_result final {
    using accepted_storage_type = std::conditional_t<std::is_void_v<accepted_type>, std::monostate, accepted_type>;

public:
    delivery_result(const delivery_result&) = delete;
    delivery_result& operator=(const delivery_result&) = delete;
    delivery_result(delivery_result&&) noexcept(std::is_nothrow_move_constructible_v<t_type> && std::is_nothrow_move_constructible_v<accepted_storage_type>) = default;
    delivery_result& operator=(delivery_result&&) = delete;

    [[nodiscard]] status_type status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return status_ == accepted_status;
    }

    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] const self_type& value() const& RUVIA_LIFETIMEBOUND {
        return require_accepted();
    }
    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    const self_type& value() const&& = delete;

    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] self_type take_value() && {
        auto& accepted_value = require_accepted();
        struct clear_accepted final {
            std::optional<accepted_storage_type>& accepted_;
            const int exceptions_{std::is_nothrow_move_constructible_v<accepted_storage_type> ? 0 : std::uncaught_exceptions()};
            ~clear_accepted() {
                if constexpr (std::is_nothrow_move_constructible_v<accepted_storage_type>) {
                    accepted_.reset();
                } else if (std::uncaught_exceptions() == exceptions_) {
                    accepted_.reset();
                }
            }
        } clear{accepted_};
        return std::move(accepted_value);
    }

    [[nodiscard]] const t_type* rejected() const& noexcept RUVIA_LIFETIMEBOUND {
        return rejected_ ? std::addressof(*rejected_) : nullptr;
    }

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

    friend bool operator==(const delivery_result& outcome, status_type status) noexcept {
        return outcome.status_ == status;
    }
    friend bool operator==(status_type status, const delivery_result& outcome) noexcept {
        return outcome == status;
    }

private:
    friend struct detail::delivery_access;

    explicit delivery_result(status_type status) noexcept :
        status_(status) {}

    delivery_result(status_type status, std::in_place_t, accepted_storage_type&& accepted_value) noexcept(std::is_nothrow_move_constructible_v<accepted_storage_type>) :
        status_(status),
        accepted_(std::move(accepted_value)) {}

    template <typename rejected_value_type>
        requires std::constructible_from<t_type, rejected_value_type&&>
    delivery_result(status_type status, rejected_value_type&& rejected) noexcept(std::is_nothrow_constructible_v<t_type, rejected_value_type&&>) :
        status_(status),
        rejected_(std::in_place, std::forward<rejected_value_type>(rejected)) {}

    [[nodiscard]] const accepted_storage_type& require_accepted() const {
        if (status_ != accepted_status || !accepted_) {
            throw std::logic_error("delivery result has no accepted value");
        }
        return *accepted_;
    }

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

// Admission of one callable into a worker queue or the blocking pool.
enum class post_status : std::uint8_t {
    accepted,
    queue_full,
    // The worker or pool no longer admits work.
    stopping,
};

template <typename signature_type = void()>
using post_result = delivery_result<move_only_function<signature_type>, post_status, post_status::accepted>;

namespace detail {

// The producer side of delivery_result, reserved to the delivering
// primitives so callers cannot fabricate an accepted outcome.
struct delivery_access final {
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
    struct accepted_status_traits<delivery_result<t_type, status_type, accepted_status, accepted_type>> final {
        static constexpr status_type value = accepted_status;
    };

    template <typename result_type>
    [[nodiscard]] static constexpr auto accepted_status_of() noexcept {
        return accepted_status_traits<result_type>::value;
    }
};

}  // namespace detail

}  // namespace ruvia
