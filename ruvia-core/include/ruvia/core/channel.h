#pragma once

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_wait.h"

namespace ruvia {

enum class channel_send_status : std::uint8_t {
    sent,
    // send() only: every slot is in use.
    full,
    closed,
    worker_stopping,
    // send_wait() only: its operation_options ended the wait for a slot.
    timed_out,
    cancelled,
};

template <typename t_type>
class channel_sender;
template <typename t_type>
class channel_send_operation;

struct channel_options final {
    // Bounds buffered and reserved sends. One extra slot permits a value
    // already assigned to a receiver while the buffer fills again.
    // A send may report full while every slot is temporarily in use.
    std::size_t capacity_{0};
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

    channel_state(worker_ref target, std::size_t requested_capacity)
        : worker_(std::move(target)),
          slots_(slot_count(requested_capacity)),
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

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] worker_wait_slot<node*, channel_state>& wait_slot() noexcept {
        return wait_slot_;
    }

    [[nodiscard]] std::optional<channel_send_status> check_open_locked() const noexcept {
        if (std::holds_alternative<channel_closed>(lifecycle_)) {
            return channel_send_status::closed;
        }
        if (std::holds_alternative<channel_worker_stopping>(lifecycle_) || !worker_.accepting()) {
            return channel_send_status::worker_stopping;
        }
        return std::nullopt;
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

    // Called under mutex_ whenever capacity may have been released: hands
    // each free slot, in FIFO order, to a waiting send_wait() as a
    // reservation it fills once it resumes on its own worker.
    void grant_send_waiters() noexcept;
    // Called under mutex_ by close and worker shutdown.
    void complete_send_waiters(channel_send_status status) noexcept;
    void enqueue_send_waiter(channel_send_operation<t_type>& waiter) noexcept;
    // Returns false when the waiter is no longer queued.
    [[nodiscard]] bool remove_send_waiter(channel_send_operation<t_type>& waiter) noexcept;

    // Called under mutex_: publishes a filled reservation to the pending
    // receiver or the queue. Returns the rejection when the channel closed or
    // stopped since the reservation, leaving the value in the slot.
    [[nodiscard]] std::optional<channel_send_status> publish_reserved(node* slot) {
        --reserved_;
        auto rejection = check_open_locked();
        if (!rejection) {
            if (wait_slot_.occupied()) {
                wait_slot_.complete(worker_wait_result<node*>::with_value(slot));
                // A direct handoff never occupied the queue.
                grant_send_waiters();
            } else {
                enqueue(slot);
            }
        }
        return rejection;
    }

    // Called under mutex_ when a filled reservation is abandoned.
    void release_reserved(node* slot) noexcept {
        --reserved_;
        recycle(slot);
        grant_send_waiters();
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
                state_.grant_send_waiters();
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
    worker_shutdown_registration shutdown_registration_;
    worker_ref worker_;
    std::mutex mutex_;
    std::vector<node> slots_;
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
    channel_send_operation<t_type>* send_head_{nullptr};
    channel_send_operation<t_type>* send_tail_{nullptr};

    void close(bool retire_receiver);
    void worker_stopping() noexcept;

private:
    void discard_buffered();
};

template <typename t_type>
void channel_state<t_type>::close(bool retire_receiver) {
    {
        auto lock = lock_state();
        receiver_closed_ = receiver_closed_ || retire_receiver;
        if (std::holds_alternative<channel_open>(lifecycle_)) {
            lifecycle_.template emplace<channel_closed>();
            wait_slot_.complete(worker_wait_status::closed);
            complete_send_waiters(channel_send_status::closed);
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
    complete_send_waiters(channel_send_status::worker_stopping);
}

}  // namespace detail

// One send_wait(): single-use, movable only before it is awaited. It owns the
// value until delivery and returns it in the result otherwise. A slot granted
// while it waits is reserved for it and filled when it resumes on the worker
// that awaited it.
template <typename t_type>
class channel_send_operation final {
public:
    channel_send_operation(const channel_send_operation&) = delete;
    channel_send_operation& operator=(const channel_send_operation&) = delete;
    channel_send_operation(channel_send_operation&& other) noexcept(std::is_nothrow_move_constructible_v<t_type>)
        : state_(std::move(other.state_)),
          value_(std::move(other.value_)),
          timeout_(other.timeout_),
          stop_token_(std::move(other.stop_token_)) {
        if (other.started_) {
            std::terminate();
        }
    }
    channel_send_operation& operator=(channel_send_operation&&) = delete;
    void operator&() const = delete;

    ~channel_send_operation() {
        // A frame destroyed while suspended must not leave a dangling waiter.
        if (queued_) {
            auto lock = state_->lock_state();
            (void)state_->remove_send_waiter(*this);
        }
    }

    [[nodiscard]] bool await_ready() {
        if (started_ || !value_.has_value()) {
            throw std::logic_error("channel send cannot be awaited");
        }
        if (!state_) {
            started_ = true;
            complete_ready({channel_send_status::closed, nullptr});
            return true;
        }
        auto worker_value = detail::worker_ref_access::current();
        if (!worker_value.valid()) {
            throw std::logic_error("channel send_wait must be awaited on a worker");
        }
        started_ = true;
        worker_ = std::move(worker_value);
        const auto now = std::chrono::steady_clock::now();
        if (timeout_.has_value()) {
            deadline_ = timeout_->deadline_from(now);
        }
        auto& state = *state_;
        auto lock = state.lock_state();
        if (const auto rejection = state.check_open_locked()) {
            complete_ready({*rejection, nullptr});
            return true;
        }
        if (auto* slot = state.reserve_node()) {
            complete_ready({channel_send_status::sent, slot});
            return true;
        }
        if (stop_token_.stop_requested()) {
            complete_ready({channel_send_status::cancelled, nullptr});
            return true;
        }
        if (deadline_.has_value() && *deadline_ <= now) {
            complete_ready({channel_send_status::timed_out, nullptr});
            return true;
        }
        state.enqueue_send_waiter(*this);
        return false;
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
        auto& state = *state_;
        bool suspended = false;
        {
            auto lock = state.lock_state();
            // A timer is always armed on the awaiting worker, at the deadline or
            // never, so that worker's timer shutdown also ends the wait.
            if (queued_) {
                auto status = worker_timer_schedule_status::worker_stopping;
                try {
                    status = worker_.schedule_timer(timer_,
                        deadline_.value_or(std::chrono::steady_clock::time_point::max()),
                        [this](worker_timer_outcome outcome) noexcept { on_timer_completed(outcome); });
                } catch (...) {
                    (void)state.remove_send_waiter(*this);
                    throw;
                }
                if (status == worker_timer_schedule_status::worker_stopping) {
                    (void)state.remove_send_waiter(*this);
                    (void)completion_.complete({channel_send_status::worker_stopping, nullptr});
                }
            }
            suspended = completion_.suspend(continuation);
        }
        // Registered only once the wait really suspends; a stop already
        // requested runs the callback inline and completes the wait.
        if (suspended && stop_token_.stoppable()) {
            stop_token_.register_callback(stop_registration_, [this] { request_cancellation(); });
        }
        return suspended;
    }

    [[nodiscard]] channel_send_result<t_type> await_resume() {
        using result_type = channel_send_result<t_type>;
        const auto outcome = completion_.take_value();
        if (outcome.status_ != channel_send_status::sent) {
            return detail::worker_delivery_access::reject<result_type>(outcome.status_, std::move(*value_));
        }
        auto& state = *state_;
        typename state_type::node_guard guard(state, outcome.slot_);
        try {
            outcome.slot_->value_.emplace(std::move(*value_));
        } catch (...) {
            auto lock = state.lock_state();
            state.release_reserved(outcome.slot_);
            guard.release();
            throw;
        }
        std::optional<channel_send_status> rejection;
        {
            auto lock = state.lock_state();
            rejection = state.publish_reserved(outcome.slot_);
            if (!rejection) {
                guard.release();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<result_type>(*rejection, std::move(*outcome.slot_->value_));
        }
        return detail::worker_delivery_access::accept<result_type>();
    }

private:
    using state_type = detail::channel_state<t_type>;
    using node = typename state_type::node;

    friend class channel_sender<t_type>;
    friend struct detail::channel_state<t_type>;

    struct outcome_type final {
        channel_send_status status_;
        // The reserved slot when status_ is sent.
        node* slot_;
    };

    channel_send_operation(std::shared_ptr<state_type> state_value, t_type&& value, operation_options options) noexcept(
        std::is_nothrow_move_constructible_v<t_type>)
        : state_(std::move(state_value)),
          value_(std::move(value)),
          timeout_(options.timeout_),
          stop_token_(std::move(options.stop_token_)) {}

    void complete_ready(outcome_type outcome) noexcept {
        (void)completion_.complete(std::move(outcome));
    }

    // Under the state lock, once removed from the queue. The wake runs before
    // the lock is released, so a racing timer expiry cannot resume and destroy
    // this operation first.
    void complete_locked(outcome_type outcome) noexcept {
        if (completion_.complete(std::move(outcome))) {
            if (timer_.registered()) {
                timer_.cancel();
            }
            worker_.defer_resume(completion_.continuation());
        }
    }

    void on_timer_completed(worker_timer_outcome outcome) noexcept {
        std::coroutine_handle<> continuation;
        {
            auto lock = state_->lock_state();
            if (!state_->remove_send_waiter(*this)) {
                return;
            }
            if (!completion_.complete({outcome == worker_timer_outcome::expired ? channel_send_status::timed_out
                                                                                : channel_send_status::worker_stopping,
                    nullptr})) {
                return;
            }
            continuation = completion_.continuation();
        }
        continuation.resume();
    }

    // Unregistration waits for a running callback, so this operation and its
    // state outlive it.
    void request_cancellation() noexcept {
        auto lock = state_->lock_state();
        if (state_->remove_send_waiter(*this)) {
            complete_locked({channel_send_status::cancelled, nullptr});
        }
    }

    std::shared_ptr<state_type> state_;
    std::optional<t_type> value_;
    std::optional<operation_timeout> timeout_;
    stop_token stop_token_;
    worker_ref worker_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    // Queue links, guarded by the state lock.
    channel_send_operation* previous_{nullptr};
    channel_send_operation* next_{nullptr};
    bool queued_{false};
    bool started_{false};
    worker_timer_registration timer_;
    detail::suspend_race_state<outcome_type> completion_;
    // Last so callback unregistration completes before anything it touches
    // begins destruction.
    stop_registration stop_registration_;
};

namespace detail {

template <typename t_type>
void channel_state<t_type>::enqueue_send_waiter(channel_send_operation<t_type>& waiter) noexcept {
    waiter.previous_ = send_tail_;
    waiter.next_ = nullptr;
    waiter.queued_ = true;
    if (send_tail_ != nullptr) {
        send_tail_->next_ = std::addressof(waiter);
    } else {
        send_head_ = std::addressof(waiter);
    }
    send_tail_ = std::addressof(waiter);
}

template <typename t_type>
bool channel_state<t_type>::remove_send_waiter(channel_send_operation<t_type>& waiter) noexcept {
    if (!waiter.queued_) {
        return false;
    }
    if (waiter.previous_ != nullptr) {
        waiter.previous_->next_ = waiter.next_;
    } else {
        send_head_ = waiter.next_;
    }
    if (waiter.next_ != nullptr) {
        waiter.next_->previous_ = waiter.previous_;
    } else {
        send_tail_ = waiter.previous_;
    }
    waiter.previous_ = nullptr;
    waiter.next_ = nullptr;
    waiter.queued_ = false;
    return true;
}

template <typename t_type>
void channel_state<t_type>::grant_send_waiters() noexcept {
    while (send_head_ != nullptr && std::holds_alternative<channel_open>(lifecycle_)) {
        auto* slot = reserve_node();
        if (slot == nullptr) {
            return;
        }
        auto& waiter = *send_head_;
        (void)remove_send_waiter(waiter);
        waiter.complete_locked({channel_send_status::sent, slot});
    }
}

template <typename t_type>
void channel_state<t_type>::complete_send_waiters(channel_send_status status) noexcept {
    while (send_head_ != nullptr) {
        auto& waiter = *send_head_;
        (void)remove_send_waiter(waiter);
        waiter.complete_locked({status, nullptr});
    }
}

}  // namespace detail

template <typename t_type>
class channel_sender final {
public:
    channel_sender() noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    // The sender must remain alive throughout the call, including payload
    // moves and destruction. Concurrent sends are delivered in publication
    // order; closure during a payload move returns that value as rejected.
    // Never reports timed_out or cancelled.
    [[nodiscard]] channel_send_result<t_type> send(t_type value) const {
        // Payload code may synchronously rebind this handle while the call is active.
        const auto state = state_;
        if (state == nullptr) {
            return detail::worker_delivery_access::reject<channel_send_result<t_type>>(channel_send_status::closed, std::move(value));
        }
        using state_type = detail::channel_state<t_type>;
        typename state_type::node* slot = nullptr;
        std::optional<channel_send_status> rejection;
        {
            auto lock = state->lock_state();
            rejection = state->check_open_locked();
            if (!rejection && (slot = state->reserve_node()) == nullptr) {
                rejection = channel_send_status::full;
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<channel_send_result<t_type>>(*rejection, std::move(value));
        }

        typename state_type::node_guard guard(*state, slot);
        try {
            slot->value_.emplace(std::move(value));
        } catch (...) {
            auto lock = state->lock_state();
            state->release_reserved(slot);
            guard.release();
            throw;
        }
        {
            auto lock = state->lock_state();
            rejection = state->publish_reserved(slot);
            if (!rejection) {
                guard.release();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<channel_send_result<t_type>>(*rejection, std::move(*slot->value_));
        }
        return detail::worker_delivery_access::accept<channel_send_result<t_type>>();
    }

    // The waiting form of send(): a cold operation that, once awaited, takes a
    // free slot at once or suspends until the receiver releases one. Waiting
    // senders are served in FIFO order. It must be awaited on a worker thread,
    // which is where it resumes (std::logic_error elsewhere); the channel's
    // worker need not be that worker. A value that is not delivered -- the
    // channel closed or its worker stopped, the awaiting worker stopped
    // (worker_stopping), options.timeout_ elapsed (timed_out), or
    // options.stop_token_ fired (cancelled) -- returns in the result. Without
    // a timeout or stop token, the wait lasts until a slot frees, the channel
    // closes, or either worker stops.
    [[nodiscard]] channel_send_operation<t_type> send_wait(t_type value, operation_options options = {}) const& {
        return channel_send_operation<t_type>(state_, std::move(value), std::move(options));
    }
    channel_send_operation<t_type> send_wait(t_type, operation_options = {}) const&& = delete;

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
    friend auto make_channel(worker_ref, channel_options);
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

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }
    // The receive operation: single-use, movable only before it is awaited,
    // and started on the receiver's worker by co_await. Dropping a cold one
    // has no effect on the channel.
    class receive_operation final {
    public:
        receive_operation(receive_operation&&) noexcept = default;
        receive_operation(const receive_operation&) = delete;
        receive_operation& operator=(const receive_operation&) = delete;
        receive_operation& operator=(receive_operation&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready() {
            wait_.start("channel receive must run on its bound worker");
            auto& owner_value = wait_.state();
            auto lock = owner_value.lock_state();
            if (!owner_value.receiver_closed_) {
                if (auto* item = owner_value.dequeue()) {
                    owner_value.grant_send_waiters();
                    wait_.complete_ready(worker_wait_result<node*>::with_value(item));
                    return true;
                }
            }
            if (std::holds_alternative<detail::channel_worker_stopping>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::worker_stopping);
                return true;
            }
            if (std::holds_alternative<detail::channel_closed>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::closed);
                return true;
            }
            assert(std::holds_alternative<detail::channel_open>(owner_value.lifecycle_));
            return wait_.complete_or_publish("channel supports one pending receiver");
        }

        bool await_suspend(std::coroutine_handle<> continuation) {
            return wait_.suspend(continuation);
        }

        [[nodiscard]] worker_wait_result<t_type> await_resume() {
            auto result = wait_.take_result();
            if (!result.has_value()) {
                return worker_wait_result<t_type>::with_status(result.status());
            }
            auto* item = result.value();
            typename state_type::node_guard guard(wait_.state(), item);
            return worker_wait_result<t_type>::with_value(std::move(*item->value_));
        }

    private:
        friend class channel_receiver;
        using state_type = detail::channel_state<t_type>;
        using node = typename state_type::node;

        receive_operation(std::shared_ptr<state_type> state_value, operation_options options) noexcept
            : wait_(std::move(state_value), std::move(options)) {}

        worker_single_wait_awaiter<node*, state_type> wait_;
    };

    // The receiver owns the receive side: destroying it prevents cold receives
    // from consuming buffered values and closes pending receives. Require a
    // named owner so storing an operation cannot silently turn a temporary
    // receiver into an already-closed operation. A relative options.timeout_
    // bounds the wait from its start; a deadline that has passed reports
    // timed_out once nothing is buffered.
    [[nodiscard]] receive_operation receive(operation_options options = {}) const& {
        return receive_operation(state_, std::move(options));
    }
    receive_operation receive(operation_options = {}) const&& = delete;

    // The worker every receive must run on. Makes the receive-side affinity
    // contract queryable instead of only failing at await time. A moved-from
    // receiver has no bound worker and cannot answer this query.
    [[nodiscard]] const worker_ref& worker() const& noexcept {
        if (!state_) {
            std::terminate();
        }
        return state_->worker_;
    }
    const worker_ref& worker() const&& = delete;

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
    friend auto make_channel(worker_ref, channel_options);
};

template <typename t_type>
// The value parameter accepts both lvalue and rvalue worker refs before
// transferring the stable dispatcher endpoint into the channel state.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
[[nodiscard]] auto make_channel(worker_ref worker_value, channel_options options) {
    auto state_value = make_worker_wait_state<detail::channel_state<t_type>>(
        std::move(worker_value), options.capacity_);
    channel_sender<t_type> sender(state_value);
    return std::pair(std::move(sender), channel_receiver<t_type>(std::move(state_value)));
}

}  // namespace ruvia
