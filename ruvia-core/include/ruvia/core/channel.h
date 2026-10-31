#pragma once

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/detail/worker/worker_wait_awaiter.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
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
    // Bounds buffered and reserved sends. One extra slot permits a value
    // already assigned to a receiver while the buffer fills again.
    // A send may report full while every slot is temporarily in use.
    std::size_t capacity_{0};
    std::pmr::memory_resource* resource_{nullptr};
};

template <typename t_type>
using channel_send_result =
    worker_delivery_result<t_type, channel_send_status, channel_send_status::sent>;

namespace detail {

struct channel_open final {};
struct channel_closed final {};
struct channel_worker_stopping final {};

template <typename t_type>
struct channel_state final {
    struct node final {
        std::optional<t_type> value_;
        node* next_{nullptr};
    };

    static std::size_t slot_count(std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("channel capacity must be nonzero");
        }
        if (capacity == std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("channel capacity is too large");
        }
        return capacity + 1;
    }

    channel_state(
        worker_handle target, std::size_t requested_capacity, std::pmr::memory_resource* resource)
        : worker_(std::move(target)),
          slots_(slot_count(requested_capacity), resource),
          capacity_(requested_capacity) {
        if (!worker_.valid()) {
            throw std::invalid_argument("channel requires a valid worker");
        }
        for (auto& slot : slots_) {
            slot.next_ = free_;
            free_ = &slot;
        }
    }

    [[nodiscard]] std::unique_lock<std::mutex> lock_state() {
        return std::unique_lock(mutex_);
    }

    // Called under mutex_. Reservations hold capacity until publication.
    [[nodiscard]] node* reserve_node() noexcept {
        if (queued_ + reserved_ == capacity_ || free_ == nullptr) {
            return nullptr;
        }
        auto* slot = free_;
        free_ = slot->next_;
        slot->next_ = nullptr;
        ++reserved_;
        return slot;
    }

    void enqueue(node* slot) noexcept {
        slot->next_ = nullptr;
        if (tail_ != nullptr) {
            tail_->next_ = slot;
        } else {
            head_ = slot;
        }
        tail_ = slot;
        ++queued_;
    }

    [[nodiscard]] node* dequeue() noexcept {
        auto* slot = head_;
        if (slot == nullptr) {
            return nullptr;
        }
        head_ = slot->next_;
        if (head_ == nullptr) {
            tail_ = nullptr;
        }
        slot->next_ = nullptr;
        --queued_;
        return slot;
    }

    void recycle(node* slot) noexcept {
        slot->next_ = free_;
        free_ = slot;
    }

    // Owns one reserved or claimed slot; user destruction runs without mutex_.
    class node_guard final {
    public:
        node_guard(channel_state& state, node* slot) noexcept
            : state_(state),
              slot_(slot) {}
        node_guard(const node_guard&) = delete;
        node_guard& operator=(const node_guard&) = delete;
        ~node_guard() {
            if (slot_ != nullptr) {
                slot_->value_.reset();
                auto lock = state_.lock_state();
                state_.recycle(slot_);
            }
        }
        void release() noexcept {
            slot_ = nullptr;
        }

    private:
        channel_state& state_;
        node* slot_;
    };

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_hook shutdown_hook_;
    worker_handle worker_;
    std::mutex mutex_;
    std::pmr::vector<node> slots_;
    const std::size_t capacity_;
    node* free_{nullptr};
    node* head_{nullptr};
    node* tail_{nullptr};
    std::size_t queued_{0};
    std::size_t reserved_{0};
    using lifecycle_type = std::variant<channel_open, channel_closed, channel_worker_stopping>;
    lifecycle_type lifecycle_;
    bool receiver_closed_{false};
    worker_wait_slot<node*, channel_state<t_type>> wait_slot_;

    void close(bool retire_receiver);
    void worker_stopping() noexcept;

private:
    void discard_buffered();
};

// The receive operation: single-use, movable only before it is awaited, and
// started on the receiver's worker by co_await. Dropping a cold one has no
// effect on the channel.
template <typename t_type>
struct channel_receive_awaiter final {
    using state_type = channel_state<t_type>;
    using node = typename state_type::node;
    using wait_type = worker_single_wait_awaiter<node*, state_type>;

    channel_receive_awaiter(std::shared_ptr<channel_state<t_type>> value,
        std::optional<std::chrono::steady_clock::duration> timeout_value, stop_token stop_token_value) noexcept
        : wait_(std::move(value), timeout_value, std::move(stop_token_value)) {}

    [[nodiscard]] bool await_ready() {
        wait_.start("channel receive must run on its bound worker");
        auto& owner_value = wait_.state();
        auto lock = owner_value.lock_state();
        if (owner_value.receiver_closed_) {
            (void)wait_.complete_status(std::holds_alternative<channel_worker_stopping>(owner_value.lifecycle_)
                                            ? worker_wait_status::worker_stopping
                                            : worker_wait_status::closed);
            return true;
        }
        if (auto* item = owner_value.dequeue()) {
            (void)wait_.complete_result(worker_wait_result_access::value(item));
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
        auto result = wait_.take_result();
        if (!result.has_value()) {
            return worker_wait_result_access::outcome<t_type>(result.status());
        }
        auto* item = result.value();
        typename state_type::node_guard guard(wait_.state(), item);
        return worker_wait_result_access::value(std::move(*item->value_));
    }

private:
    wait_type wait_;
};

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
        node* retired;
        {
            auto lock = lock_state();
            retired = dequeue();
        }
        if (retired == nullptr) {
            return;
        }
        node_guard guard(*this, retired);
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

    // The sender must remain alive throughout the call, including payload
    // moves and destruction. Concurrent sends are delivered in publication
    // order; closure during a payload move returns that value as rejected.
    [[nodiscard]] channel_send_result<t_type> send(t_type value) const {
        auto* const state = state_.get();
        if (state == nullptr) {
            return channel_send_result<t_type>::reject(channel_send_status::closed, std::move(value));
        }
        using state_type = detail::channel_state<t_type>;
        typename state_type::node* slot = nullptr;
        std::optional<channel_send_status> rejection;
        {
            auto lock = state->lock_state();
            if (std::holds_alternative<detail::channel_closed>(state->lifecycle_)) {
                rejection = channel_send_status::closed;
            } else if (std::holds_alternative<detail::channel_worker_stopping>(state->lifecycle_) ||
                       !state->worker_.accepting()) {
                rejection = channel_send_status::worker_stopping;
            } else if ((slot = state->reserve_node()) == nullptr) {
                rejection = channel_send_status::full;
            }
        }
        if (rejection) {
            return channel_send_result<t_type>::reject(*rejection, std::move(value));
        }

        typename state_type::node_guard guard(*state, slot);
        try {
            slot->value_.emplace(std::move(value));
        } catch (...) {
            auto lock = state->lock_state();
            --state->reserved_;
            throw;
        }
        {
            auto lock = state->lock_state();
            --state->reserved_;
            if (std::holds_alternative<detail::channel_closed>(state->lifecycle_)) {
                rejection = channel_send_status::closed;
            } else if (std::holds_alternative<detail::channel_worker_stopping>(state->lifecycle_) ||
                       !state->worker_.accepting()) {
                rejection = channel_send_status::worker_stopping;
            } else if (state->wait_slot_.occupied()) {
                state->wait_slot_.complete(detail::worker_wait_result_access::value(slot));
            } else {
                state->enqueue(slot);
            }
            if (!rejection) {
                guard.release();
            }
        }
        if (rejection) {
            return channel_send_result<t_type>::reject(*rejection, std::move(*slot->value_));
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

    // The receiver owns the receive side: destroying it prevents cold receives
    // from consuming buffered values and closes pending receives. Require a
    // named owner so storing an operation cannot silently turn a temporary
    // receiver into an already-closed operation.
    using receive_operation = detail::channel_receive_awaiter<t_type>;

    [[nodiscard]] receive_operation receive(stop_token stop_token_value = {}) const& {
        return receive_operation(state_, std::nullopt, std::move(stop_token_value));
    }
    receive_operation receive(stop_token = {}) const&& = delete;

    template <typename rep_type, typename period_type>
    [[nodiscard]] receive_operation receive_for(
        std::chrono::duration<rep_type, period_type> duration, stop_token stop_token_value = {}) const& {
        return receive_operation(
            state_, ::ruvia::worker_timer_saturating_duration_cast(duration), std::move(stop_token_value));
    }
    template <typename rep_type, typename period_type>
    receive_operation receive_for(std::chrono::duration<rep_type, period_type>, stop_token = {}) const&& = delete;

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
