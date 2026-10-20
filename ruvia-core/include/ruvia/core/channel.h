#pragma once

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
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
using channel_send_result =
    detail::worker_delivery_result<t_type, channel_send_status, channel_send_status::sent>;

namespace detail {

struct channel_open final {};
struct channel_closed final {};
struct channel_worker_stopping final {};

template <typename t_type>
struct channel_state final {
    channel_state(
        worker_handle target, std::size_t requested_capacity, std::pmr::memory_resource* resource)
        : worker_(std::move(target)),
          queue_(requested_capacity, resource) {
        if (!worker_.valid()) {
            throw std::invalid_argument("channel requires a valid worker");
        }
    }

    // The queue lock also guards the lifecycle, receiver, and wait slot, so checking
    // admission and publishing a value stay one critical section.
    [[nodiscard]] typename mpsc_ring_queue<t_type>::locked_access lock_state() {
        return queue_.lock();
    }

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_hook shutdown_hook_;
    worker_handle worker_;
    mpsc_ring_queue<t_type> queue_;
    using lifecycle_type = std::variant<channel_open, channel_closed, channel_worker_stopping>;
    lifecycle_type lifecycle_;
    bool receiver_closed_{false};
    worker_wait_slot<t_type, channel_state<t_type>> wait_slot_;

    void close(bool retire_receiver);
    void worker_stopping() noexcept;

private:
    void discard_buffered();
};

template <typename t_type>
struct channel_receive_awaiter final {
    using wait_type = worker_single_wait_awaiter<t_type, channel_state<t_type>>;

    channel_receive_awaiter(std::shared_ptr<channel_state<t_type>> value,
        std::optional<std::chrono::steady_clock::duration> timeout_value, stop_token stop_token_value)
        : wait_(std::move(value), timeout_value, std::move(stop_token_value)) {}

    [[nodiscard]] bool await_ready() {
        auto& owner_value = wait_.state();
        auto queue = owner_value.lock_state();
        if (owner_value.receiver_closed_) {
            (void)wait_.complete_status(std::holds_alternative<channel_worker_stopping>(owner_value.lifecycle_)
                                            ? worker_wait_status::worker_stopping
                                            : worker_wait_status::closed);
            return true;
        }
        if (auto* item = queue.front()) {
            // Building the result moves the item several times; a later move may
            // throw after an earlier one emptied it, so the front is consumed on
            // every exit rather than left for the next receive.
            struct front_consumption final {
                typename mpsc_ring_queue<t_type>::locked_access& queue_;
                ~front_consumption() {
                    queue_.pop();
                }
            } consumption{queue};
            (void)wait_.complete_result(
                worker_wait_result_access::value(std::move(*item)));
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
        return wait_.complete_or_publish("channel supports one pending receiver");
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
void channel_state<t_type>::close(bool retire_receiver) {
    {
        auto lock = lock_state();
        receiver_closed_ = receiver_closed_ || retire_receiver;
        if (std::holds_alternative<channel_open>(lifecycle_)) {
            lifecycle_.template emplace<channel_closed>();
            wait_slot_.complete(worker_wait_status::closed);
        }
    }
    if (retire_receiver) {
        discard_buffered();
    }
}

// Retirement leaves the lifecycle closed or stopping, so no sender can publish
// again and the queue only shrinks here. Buffered values die now, on the
// retiring thread, rather than with the last sender wherever it drops. Each
// value is unpublished under the lock but destroyed in its slot after the lock
// is released, so its destructor may close this channel or drop its endpoints;
// the caller keeps the state alive until the drain returns.
template <typename t_type>
void channel_state<t_type>::discard_buffered() {
    for (;;) {
        std::optional<t_type>* retired = nullptr;
        {
            auto queue = lock_state();
            if (queue.front() == nullptr) {
                return;
            }
            retired = &queue.detach_front();
        }
        retired->reset();
    }
}

template <typename t_type>
void channel_state<t_type>::worker_stopping() noexcept {
    auto lock = lock_state();
    lifecycle_.template emplace<channel_worker_stopping>();
    wait_slot_.complete(worker_wait_status::worker_stopping);
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
        std::optional<channel_send_status> rejection;
        {
            auto queue = state_->lock_state();
            if (std::holds_alternative<detail::channel_closed>(state_->lifecycle_)) {
                rejection = channel_send_status::closed;
            } else if (std::holds_alternative<detail::channel_worker_stopping>(state_->lifecycle_)) {
                rejection = channel_send_status::worker_stopping;
            } else if (!state_->worker_.accepting()) {
                rejection = channel_send_status::worker_stopping;
            } else if (state_->wait_slot_.occupied()) {
                state_->wait_slot_.complete(detail::worker_wait_result_access::value(std::move(value)));
            } else {
                assert(std::holds_alternative<detail::channel_open>(state_->lifecycle_));
                if (!queue.try_push(std::move(value))) {
                    rejection = channel_send_status::full;
                }
            }
        }
        if (rejection) {
            // The status is decided; owning the returned payload needs no queue
            // lock, and its user-defined move may close the channel again.
            return channel_send_result<t_type>::reject(*rejection, std::move(value));
        }
        return channel_send_result<t_type>::accept();
    }

    // Stop sending while allowing the receiver to drain buffered values.
    void close() const {
        if (state_) {
            state_->close(false);
        }
    }

private:
    explicit channel_sender(std::shared_ptr<detail::channel_state<t_type>> state_value)
        : state_(std::move(state_value)) {}

    std::shared_ptr<detail::channel_state<t_type>> state_;
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
    // destroying it prevents cold receives from consuming buffered values and
    // closes pending receives. Require a named owner so storing a task cannot
    // silently turn a temporary receiver into an already-closed operation.
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

    // Retire the receive side: buffered values are destroyed on the calling
    // thread and later receives report closed. Worker shutdown keeps its
    // worker_stopping outcome. The shared state itself lives until all
    // endpoints and tasks owning it retire.
    void close() const {
        if (state_) {
            // A buffered payload destructor may destroy this receiver and the
            // last sender, so the drain runs on its own owner of the state.
            const auto pinned = state_;
            pinned->close(true);
        }
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
    auto* resolved = pmr_resource_or_default(options.resource_);
    auto state_value = detail::make_worker_bound_state<detail::channel_state<t_type>>(
        resolved, std::move(worker_value), options.capacity_, resolved);
    return std::pair(channel_sender<t_type>(state_value), channel_receiver<t_type>(state_value));
}

}  // namespace ruvia
