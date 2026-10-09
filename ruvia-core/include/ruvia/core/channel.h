#pragma once

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
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
#include "ruvia/core/mpsc_ring_queue.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait_result.h"

namespace ruvia {

enum class channel_send_status : std::uint8_t {
    sent,
    full,
    closed,
    worker_stopping,
};

template <typename t_type>
class channel_sender;

struct channel_options final {
    std::size_t capacity_{0};
    std::pmr::memory_resource* resource_{nullptr};
};

template <typename t_type>
class channel_send_result final {
public:
    channel_send_result(const channel_send_result&) = delete;
    channel_send_result& operator=(const channel_send_result&) = delete;
    channel_send_result(channel_send_result&&) noexcept(
        std::is_nothrow_move_constructible_v<t_type>) = default;
    channel_send_result& operator=(channel_send_result&&) noexcept(
        std::is_nothrow_move_constructible_v<t_type> && std::is_nothrow_move_assignable_v<t_type>) = default;

    [[nodiscard]] channel_send_status status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return status_ == channel_send_status::sent;
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
    friend class channel_sender<t_type>;

    explicit channel_send_result(channel_send_status status) noexcept
        : status_(status) {}

    channel_send_result(channel_send_status status, t_type&& rejected) noexcept(
        std::is_nothrow_move_constructible_v<t_type>)
        : status_(status),
          rejected_(std::move(rejected)) {}

    [[nodiscard]] static channel_send_result accept() noexcept {
        return channel_send_result(channel_send_status::sent);
    }

    [[nodiscard]] static channel_send_result reject(channel_send_status status, t_type&& value) noexcept(
        std::is_nothrow_move_constructible_v<t_type>) {
        return channel_send_result(status, std::move(value));
    }

    channel_send_status status_;
    std::optional<t_type> rejected_;
};

template <typename t_type>
class channel_receiver;

namespace detail {

struct channel_open final {};
struct channel_closed final {};
struct channel_worker_stopping final {};

template <typename t_type>
struct channel_state final : worker_shutdown_listener {
    channel_state(
        worker_handle target, std::size_t requested_capacity, std::pmr::memory_resource* resource)
        : worker_(std::move(target)),
          queue_(requested_capacity, resource),
          mutex_(queue_.synchronization_mutex()) {
        if (!worker_.valid()) {
            throw std::invalid_argument("channel requires a valid worker");
        }
    }

    worker_handle worker_;
    mpsc_ring_queue<t_type> queue_;
    std::mutex& mutex_;
    using lifecycle_type = std::variant<channel_open, channel_closed, channel_worker_stopping>;
    lifecycle_type lifecycle_;
    worker_single_wait_awaiter<t_type, channel_state<t_type>>* waiter_{nullptr};
    std::uint64_t waiter_generation_{0};
    std::uint64_t next_waiter_generation_{0};

    void worker_stopping() noexcept override;
};

template <typename t_type>
struct channel_receive_awaiter final {
    using wait_type = worker_single_wait_awaiter<t_type, channel_state<t_type>>;

    channel_receive_awaiter(std::shared_ptr<channel_state<t_type>> value,
        std::optional<std::chrono::steady_clock::duration> timeout_value, stop_token stop_token_value)
        : wait_(std::move(value), timeout_value, std::move(stop_token_value)) {}

    [[nodiscard]] bool await_ready() {
        auto& owner_value = wait_.state();
        auto queue = owner_value.queue_.lock();
        if (auto* item = queue.front()) {
            (void)wait_.complete_result(
                worker_wait_result_access::value(std::move(*item)));
            queue.pop();
            return true;
        }
        if (std::holds_alternative<channel_worker_stopping>(owner_value.lifecycle_)) {
            (void)wait_.complete_status(worker_wait_status::worker_stopping);
            return true;
        }
        if (std::holds_alternative<channel_closed>(owner_value.lifecycle_)) {
            (void)wait_.complete_status(worker_wait_status::closed);
            return true;
        }
        assert(std::holds_alternative<channel_open>(owner_value.lifecycle_));
        if (wait_.get_stop_token().stop_requested()) {
            (void)wait_.complete_status(worker_wait_status::cancelled);
            return true;
        }
        if (wait_.timeout() && *wait_.timeout() <= std::chrono::steady_clock::duration::zero()) {
            (void)wait_.complete_status(worker_wait_status::timed_out);
            return true;
        }
        if (owner_value.waiter_ != nullptr) {
            throw std::logic_error("channel supports one pending receiver");
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
[[nodiscard]] task<worker_wait_result<t_type>> receive_channel_state(std::shared_ptr<channel_state<t_type>> state_value,
    std::optional<std::chrono::steady_clock::duration> timeout, stop_token stop_token_value) {
    if (!state_value || !state_value->worker_.is_current()) {
        throw std::logic_error("channel receive must run on its bound worker");
    }
    co_return co_await channel_receive_awaiter<t_type>(std::move(state_value), timeout, std::move(stop_token_value));
}

template <typename t_type>
void channel_state<t_type>::worker_stopping() noexcept {
    std::lock_guard lock(mutex_);
    lifecycle_.template emplace<channel_worker_stopping>();
    complete_worker_single_wait(*this, worker_wait_status::worker_stopping);
}

}  // namespace detail

template <typename t_type>
class channel_sender final {
public:
    channel_sender() noexcept = default;

    [[nodiscard]] channel_send_result<t_type> send(t_type value) const {
        if (!state_) {
            return channel_send_result<t_type>::reject(channel_send_status::closed, std::move(value));
        }
        {
            auto queue = state_->queue_.lock();
            if (std::holds_alternative<detail::channel_closed>(state_->lifecycle_)) {
                return channel_send_result<t_type>::reject(channel_send_status::closed, std::move(value));
            }
            if (std::holds_alternative<detail::channel_worker_stopping>(state_->lifecycle_)) {
                return channel_send_result<t_type>::reject(
                    channel_send_status::worker_stopping, std::move(value));
            }
            assert(std::holds_alternative<detail::channel_open>(state_->lifecycle_));
            if (!state_->worker_.accepting()) {
                return channel_send_result<t_type>::reject(
                    channel_send_status::worker_stopping, std::move(value));
            }
            if (auto* waiter = state_->waiter_) {
                const bool wake =
                    waiter->complete_result(detail::worker_wait_result_access::value(std::move(value)));
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
                if (!queue.try_push(std::move(value))) {
                    return channel_send_result<t_type>::reject(channel_send_status::full, std::move(value));
                }
            }
        }
        return channel_send_result<t_type>::accept();
    }

    void close() const {
        if (!state_) {
            return;
        }
        std::lock_guard lock(state_->mutex_);
        if (!std::holds_alternative<detail::channel_open>(state_->lifecycle_)) {
            return;
        }
        state_->lifecycle_.template emplace<detail::channel_closed>();
        detail::complete_worker_single_wait(*state_, worker_wait_status::closed);
    }

private:
    explicit channel_sender(std::shared_ptr<detail::channel_state<t_type>> state_value)
        : state_(std::move(state_value)) {}

    std::shared_ptr<detail::channel_state<t_type>> state_;
    friend class channel_receiver<t_type>;
    template <typename u_type>
    friend auto make_channel(worker_handle, channel_options);
};

template <typename t_type>
class channel_receiver final {
public:
    channel_receiver() = delete;
    channel_receiver(const channel_receiver&) = delete;
    channel_receiver& operator=(const channel_receiver&) = delete;
    channel_receiver(channel_receiver&&) noexcept = default;
    channel_receiver& operator=(channel_receiver&&) = delete;

    ~channel_receiver() {
        close();
    }

    // The task owns the shared state, but this receiver owns the receive side:
    // destroying it closes every cold or running receive. Require a named
    // owner so storing a task cannot silently turn a temporary receiver into
    // an already-closed operation.
    [[nodiscard]] task<worker_wait_result<t_type>> receive() const& {
        return detail::receive_channel_state<t_type>(state_, std::nullopt, {});
    }
    task<worker_wait_result<t_type>> receive() const&& = delete;

    [[nodiscard]] task<worker_wait_result<t_type>> receive(stop_token stop_token_value) const& {
        return detail::receive_channel_state<t_type>(state_, std::nullopt, std::move(stop_token_value));
    }
    task<worker_wait_result<t_type>> receive(stop_token) const&& = delete;

    template <typename rep_type, typename period_type>
    [[nodiscard]] task<worker_wait_result<t_type>> receive_for(
        std::chrono::duration<rep_type, period_type> duration) const& {
        return detail::receive_channel_state<t_type>(
            state_, ::ruvia::worker_timer_saturating_duration_cast(duration), {});
    }
    template <typename rep_type, typename period_type>
    task<worker_wait_result<t_type>> receive_for(std::chrono::duration<rep_type, period_type>) const&& = delete;

    template <typename rep_type, typename period_type>
    [[nodiscard]] task<worker_wait_result<t_type>> receive_for(
        std::chrono::duration<rep_type, period_type> duration, stop_token stop_token_value) const& {
        return detail::receive_channel_state<t_type>(
            state_, ::ruvia::worker_timer_saturating_duration_cast(duration), std::move(stop_token_value));
    }
    template <typename rep_type, typename period_type>
    task<worker_wait_result<t_type>> receive_for(
        std::chrono::duration<rep_type, period_type>, stop_token) const&& = delete;

    // The worker every receive must run on. Makes the receive-side affinity
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
        channel_sender<t_type>(state_).close();
    }

private:
    explicit channel_receiver(std::shared_ptr<detail::channel_state<t_type>> state_value)
        : state_(std::move(state_value)) {}

    std::shared_ptr<detail::channel_state<t_type>> state_;
    template <typename u_type>
    friend auto make_channel(worker_handle, channel_options);
};

template <typename t_type>
// The value parameter accepts both lvalue handles and rvalue handles before
// transferring the stable dispatcher endpoint into the channel state.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
[[nodiscard]] auto make_channel(worker_handle worker_value, channel_options options) {
    auto* resolved = detail::pmr_resource_or_default(options.resource_);
    std::pmr::polymorphic_allocator<detail::channel_state<t_type>> allocator(resolved);
    auto state_value = std::allocate_shared<detail::channel_state<t_type>>(
        allocator, std::move(worker_value), options.capacity_, resolved);
    detail::worker_handle_access::register_shutdown_listener(state_value->worker_, state_value);
    return std::pair(channel_sender<t_type>(state_value), channel_receiver<t_type>(state_value));
}

}  // namespace ruvia
