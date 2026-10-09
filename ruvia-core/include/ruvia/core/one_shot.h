#pragma once

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/detail/worker/worker_wait_awaiter.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait_result.h"

namespace ruvia {

enum class one_shot_complete_status : std::uint8_t {
    completed,
    already_completed,
    receiver_closed,
    worker_stopping,
};

template <typename t_type>
class one_shot_completion;

struct one_shot_options final {
    std::pmr::memory_resource* resource_{nullptr};
};

template <typename t_type>
class one_shot_complete_result final {
public:
    one_shot_complete_result(const one_shot_complete_result&) = delete;
    one_shot_complete_result& operator=(const one_shot_complete_result&) = delete;
    one_shot_complete_result(one_shot_complete_result&&) noexcept(
        std::is_nothrow_move_constructible_v<t_type>) = default;
    one_shot_complete_result& operator=(one_shot_complete_result&&) noexcept(
        std::is_nothrow_move_constructible_v<t_type> && std::is_nothrow_move_assignable_v<t_type>) = default;

    [[nodiscard]] one_shot_complete_status status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return status_ == one_shot_complete_status::completed;
    }

    [[nodiscard]] t_type* rejected() & noexcept {
        return rejected_ ? &*rejected_ : nullptr;
    }

    [[nodiscard]] const t_type* rejected() const& noexcept {
        return rejected_ ? &*rejected_ : nullptr;
    }

    t_type* rejected() && = delete;
    const t_type* rejected() const&& = delete;

    [[nodiscard]] std::optional<t_type> take_rejected() && noexcept(
        std::is_nothrow_move_constructible_v<t_type>) {
        return std::move(rejected_);
    }

private:
    friend class one_shot_completion<t_type>;

    explicit one_shot_complete_result(one_shot_complete_status status) noexcept
        : status_(status) {}

    one_shot_complete_result(one_shot_complete_status status, t_type&& rejected) noexcept(
        std::is_nothrow_move_constructible_v<t_type>)
        : status_(status),
          rejected_(std::move(rejected)) {}

    [[nodiscard]] static one_shot_complete_result accept() noexcept {
        return one_shot_complete_result(one_shot_complete_status::completed);
    }

    [[nodiscard]] static one_shot_complete_result reject(
        one_shot_complete_status status, t_type&& value) noexcept(std::is_nothrow_move_constructible_v<t_type>) {
        return one_shot_complete_result(status, std::move(value));
    }

    one_shot_complete_status status_;
    std::optional<t_type> rejected_;
};

template <typename t_type>
class one_shot_receiver;

namespace detail {

struct one_shot_pending final {};
struct one_shot_consumed final {};
struct one_shot_receiver_closed final {};
struct one_shot_worker_stopping final {};

template <typename t_type>
class one_shot_ready final {
public:
    explicit one_shot_ready(t_type&& value) noexcept(std::is_nothrow_move_constructible_v<t_type>)
        : value_(std::move(value)) {}

    [[nodiscard]] t_type take_value() && noexcept(std::is_nothrow_move_constructible_v<t_type>) {
        return std::move(value_);
    }

private:
    t_type value_;
};

template <typename t_type>
struct one_shot_state final : worker_shutdown_listener {
    explicit one_shot_state(worker_handle target)
        : worker_(std::move(target)) {
        if (!worker_.valid()) {
            throw std::invalid_argument("one-shot requires a valid worker");
        }
    }

    void worker_stopping() noexcept override;

    worker_handle worker_;
    std::mutex mutex_;
    using lifecycle_type = std::variant<one_shot_pending, one_shot_ready<t_type>, one_shot_consumed,
        one_shot_receiver_closed, one_shot_worker_stopping>;
    lifecycle_type lifecycle_;
    worker_single_wait_awaiter<t_type, one_shot_state<t_type>>* waiter_{nullptr};
    std::uint64_t waiter_generation_{0};
    std::uint64_t next_waiter_generation_{0};
};

template <typename t_type>
struct one_shot_awaiter final {
    using wait_type = worker_single_wait_awaiter<t_type, one_shot_state<t_type>>;

    one_shot_awaiter(std::shared_ptr<one_shot_state<t_type>> value,
        std::optional<std::chrono::steady_clock::duration> timeout_value, stop_token stop_token_value)
        : wait_(std::move(value), timeout_value, std::move(stop_token_value)) {}

    [[nodiscard]] bool await_ready() {
        auto& owner_value = wait_.state();
        std::lock_guard lock(owner_value.mutex_);
        if (auto* ready = std::get_if<one_shot_ready<t_type>>(&owner_value.lifecycle_)) {
            (void)wait_.complete_result(
                worker_wait_result_access::value(std::move(*ready).take_value()));
            owner_value.lifecycle_.template emplace<one_shot_consumed>();
            return true;
        }
        if (std::holds_alternative<one_shot_worker_stopping>(owner_value.lifecycle_)) {
            (void)wait_.complete_status(worker_wait_status::worker_stopping);
            return true;
        }
        if (std::holds_alternative<one_shot_receiver_closed>(owner_value.lifecycle_) ||
            std::holds_alternative<one_shot_consumed>(owner_value.lifecycle_)) {
            (void)wait_.complete_status(worker_wait_status::closed);
            return true;
        }
        assert(std::holds_alternative<one_shot_pending>(owner_value.lifecycle_));
        if (wait_.get_stop_token().stop_requested()) {
            (void)wait_.complete_status(worker_wait_status::cancelled);
            return true;
        }
        const auto& timeout = wait_.timeout();
        if (timeout.has_value() && timeout.value() <= std::chrono::steady_clock::duration::zero()) {
            (void)wait_.complete_status(worker_wait_status::timed_out);
            return true;
        }
        if (owner_value.waiter_ != nullptr) {
            throw std::logic_error("one-shot supports one pending receiver");
        }
        wait_.publish();
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        return wait_.suspend(handle);
    }

    [[nodiscard]] worker_wait_result<t_type> await_resume() {
        return wait_.take_result();
    }

private:
    wait_type wait_;
};

template <typename t_type>
[[nodiscard]] task<worker_wait_result<t_type>> wait_one_shot_state(std::shared_ptr<one_shot_state<t_type>> state_value,
    std::optional<std::chrono::steady_clock::duration> timeout, stop_token stop_token_value) {
    if (!state_value || !state_value->worker_.is_current()) {
        throw std::logic_error("one-shot wait must run on its bound worker");
    }
    co_return co_await one_shot_awaiter<t_type>(std::move(state_value), timeout, std::move(stop_token_value));
}

template <typename t_type>
void one_shot_state<t_type>::worker_stopping() noexcept {
    std::lock_guard lock(mutex_);
    if (!std::holds_alternative<one_shot_ready<t_type>>(lifecycle_)) {
        lifecycle_.template emplace<one_shot_worker_stopping>();
    }
    complete_worker_single_wait(*this, worker_wait_status::worker_stopping);
}

}  // namespace detail

template <typename t_type>
class one_shot_completion final {
public:
    one_shot_completion() noexcept = default;

    [[nodiscard]] one_shot_complete_result<t_type> complete(t_type value) const {
        if (!state_) {
            return one_shot_complete_result<t_type>::reject(
                one_shot_complete_status::receiver_closed, std::move(value));
        }
        {
            std::lock_guard lock(state_->mutex_);
            if (std::holds_alternative<detail::one_shot_ready<t_type>>(state_->lifecycle_) ||
                std::holds_alternative<detail::one_shot_consumed>(state_->lifecycle_)) {
                return one_shot_complete_result<t_type>::reject(
                    one_shot_complete_status::already_completed, std::move(value));
            }
            if (std::holds_alternative<detail::one_shot_worker_stopping>(state_->lifecycle_)) {
                return one_shot_complete_result<t_type>::reject(
                    one_shot_complete_status::worker_stopping, std::move(value));
            }
            if (std::holds_alternative<detail::one_shot_receiver_closed>(state_->lifecycle_)) {
                return one_shot_complete_result<t_type>::reject(
                    one_shot_complete_status::receiver_closed, std::move(value));
            }
            assert(std::holds_alternative<detail::one_shot_pending>(state_->lifecycle_));
            if (!state_->worker_.accepting()) {
                return one_shot_complete_result<t_type>::reject(
                    one_shot_complete_status::worker_stopping, std::move(value));
            }
            if (auto* waiter = state_->waiter_) {
                const bool wake =
                    waiter->complete_result(detail::worker_wait_result_access::value(std::move(value)));
                state_->lifecycle_.template emplace<detail::one_shot_consumed>();
                state_->waiter_ = nullptr;
                state_->waiter_generation_ = 0;
                // Wake the receiver while still holding the mutex. Once it is
                // released, an already-in-flight timer expiry can resume the
                // receiver on its worker and destroy this awaiter, so reading
                // its timer registration afterward would be a use-after-free.
                if (wake) {
                    waiter->wake();
                }
            } else {
                try {
                    state_->lifecycle_.template emplace<detail::one_shot_ready<t_type>>(std::move(value));
                } catch (...) {
                    state_->lifecycle_.template emplace<detail::one_shot_pending>();
                    throw;
                }
            }
        }
        return one_shot_complete_result<t_type>::accept();
    }

private:
    explicit one_shot_completion(std::shared_ptr<detail::one_shot_state<t_type>> state_value)
        : state_(std::move(state_value)) {}
    std::shared_ptr<detail::one_shot_state<t_type>> state_;
    template <typename u_type>
    friend auto make_one_shot(worker_handle, one_shot_options);
};

template <typename t_type>
class one_shot_receiver final {
public:
    one_shot_receiver() = delete;
    one_shot_receiver(const one_shot_receiver&) = delete;
    one_shot_receiver& operator=(const one_shot_receiver&) = delete;
    one_shot_receiver(one_shot_receiver&&) noexcept = default;
    one_shot_receiver& operator=(one_shot_receiver&&) = delete;
    ~one_shot_receiver() {
        close();
    }

    // The task owns the shared state, but this receiver owns the receive side:
    // destroying it closes every cold or running wait. Require a named owner
    // so storing a task cannot silently turn a temporary receiver into an
    // already-closed operation.
    [[nodiscard]] task<worker_wait_result<t_type>> wait() const& {
        return detail::wait_one_shot_state<t_type>(state_, std::nullopt, {});
    }
    task<worker_wait_result<t_type>> wait() const&& = delete;

    [[nodiscard]] task<worker_wait_result<t_type>> wait(stop_token stop_token_value) const& {
        return detail::wait_one_shot_state<t_type>(state_, std::nullopt, std::move(stop_token_value));
    }
    task<worker_wait_result<t_type>> wait(stop_token) const&& = delete;

    template <typename rep_type, typename period_type>
    [[nodiscard]] task<worker_wait_result<t_type>> wait_for(
        std::chrono::duration<rep_type, period_type> duration) const& {
        return detail::wait_one_shot_state<t_type>(
            state_, ::ruvia::worker_timer_saturating_duration_cast(duration), {});
    }
    template <typename rep_type, typename period_type>
    task<worker_wait_result<t_type>> wait_for(std::chrono::duration<rep_type, period_type>) const&& = delete;

    template <typename rep_type, typename period_type>
    [[nodiscard]] task<worker_wait_result<t_type>> wait_for(
        std::chrono::duration<rep_type, period_type> duration, stop_token stop_token_value) const& {
        return detail::wait_one_shot_state<t_type>(
            state_, ::ruvia::worker_timer_saturating_duration_cast(duration), std::move(stop_token_value));
    }
    template <typename rep_type, typename period_type>
    task<worker_wait_result<t_type>> wait_for(
        std::chrono::duration<rep_type, period_type>, stop_token) const&& = delete;

    // The worker every wait must run on. Makes the receive-side affinity
    // contract queryable instead of only failing at await time. A moved-from
    // receiver has no bound worker and cannot answer this query.
    [[nodiscard]] const worker_handle& worker() const& noexcept {
        if (!state_) {
            std::terminate();
        }
        return state_->worker_;
    }
    const worker_handle& worker() const&& = delete;

    void close() const {
        if (!state_) {
            return;
        }
        std::lock_guard lock(state_->mutex_);
        if (!std::holds_alternative<detail::one_shot_pending>(state_->lifecycle_)) {
            return;
        }
        state_->lifecycle_.template emplace<detail::one_shot_receiver_closed>();
        detail::complete_worker_single_wait(*state_, worker_wait_status::closed);
    }

private:
    explicit one_shot_receiver(std::shared_ptr<detail::one_shot_state<t_type>> state_value)
        : state_(std::move(state_value)) {}
    std::shared_ptr<detail::one_shot_state<t_type>> state_;
    template <typename u_type>
    friend auto make_one_shot(worker_handle, one_shot_options);
};

template <typename t_type>
// The value parameter accepts both lvalue handles and rvalue handles before
// transferring the stable dispatcher endpoint into the one-shot state.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
[[nodiscard]] auto make_one_shot(worker_handle worker_value, one_shot_options options = {}) {
    auto* resolved = detail::pmr_resource_or_default(options.resource_);
    std::pmr::polymorphic_allocator<detail::one_shot_state<t_type>> allocator(resolved);
    auto state_value = std::allocate_shared<detail::one_shot_state<t_type>>(allocator, std::move(worker_value));
    detail::worker_handle_access::register_shutdown_listener(state_value->worker_, state_value);
    return std::pair(one_shot_completion<t_type>(state_value), one_shot_receiver<t_type>(state_value));
}

}  // namespace ruvia
