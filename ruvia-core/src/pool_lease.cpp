#include "pool_lease.h"

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "operation_options.h"
#include "stop_token.h"
#include "wait_result.h"
#include "waiter_list.h"
#include "worker_cancellation.h"
#include "worker_dispatcher.h"
#include "worker_ref.h"
#include "worker_timer.h"

namespace ruvia {

class pool_lease_scheduler::impl_type final {
public:
    using acquire_result_type = wait_result<std::size_t>;

    static constexpr std::size_t no_free_slot = static_cast<std::size_t>(-1);
    static constexpr std::size_t slot_leased = static_cast<std::size_t>(-2);

    // FIFO of the suspended acquires of one pool_lease_scheduler. The
    // scheduler is only ever touched from its worker thread, so the queue
    // needs no synchronization.
    class waiter_queue_type final {
    public:
        void enqueue(pool_lease_acquire& waiter) noexcept {
            waiters_.push_back(waiter);
        }

        void remove(pool_lease_acquire& waiter) noexcept {
            (void)waiters_.remove(waiter);
        }

        // Commit the freed slot to the next waiter before deferring its continuation.
        // Cancellation and closure cannot replace this result after the handoff.
        [[nodiscard]] bool commit_handoff(std::size_t index, std::coroutine_handle<>& continuation) noexcept {
            continuation = {};
            auto* waiter = waiters_.front();
            if (waiter == nullptr) {
                return false;
            }
            continuation = waiter->resolve(acquire_result_type::with_value(index));
            return true;
        }

        // Commits the cancellation of a still-queued acquire and resumes it in
        // a later worker turn.
        void cancel(const worker_ref& worker, std::uint64_t id) noexcept {
            auto* waiter = waiters_.find_if([id](const pool_lease_acquire& queued) noexcept {
                return queued.waiter_id_ == id;
            });
            if (waiter == nullptr) {
                return;
            }
            if (auto continuation = waiter->resolve(acquire_result_type::with_status(wait_status::cancelled))) {
                worker.defer_resume(continuation);
            }
        }

        void expire(pool_lease_acquire& waiter, wait_status status) noexcept {
            if (!waiters_.remove(waiter)) {
                return;
            }
            if (auto continuation = waiter.resolve(acquire_result_type::with_status(status))) {
                continuation.resume();
            }
        }

        // Commit closure or shutdown for the entire current queue and resume
        // every waiter in a later worker turn, as release() does: closure runs
        // inside the owner's own teardown, which must finish before any waiter
        // observes it.
        void complete_all(const worker_ref& worker, wait_status status) noexcept {
            while (auto* waiter = waiters_.front()) {
                if (auto continuation = waiter->resolve(acquire_result_type::with_status(status))) {
                    worker.defer_resume(continuation);
                }
            }
        }

    private:
        detail::waiter_list<pool_lease_acquire> waiters_;
    };

    impl_type(std::size_t pool_size, const worker_ref& worker_value) :
        worker_(worker_value),
        next_free_(pool_size, no_free_slot),
        waiter_state_(std::make_shared<waiter_state_type>(worker_value)) {
        for (std::size_t i = 0; i < pool_size; ++i) {
            next_free_[i] = free_head_;
            free_head_ = i;
        }
        if (!detail::worker_ref_access::register_shutdown(worker_, waiter_state_, &waiter_state_type::shutdown_registration_)) {
            waiter_state_->stopping_ = true;
        }
    }

    ~impl_type() {
        if (reserved_acquires_ != 0) {
            std::terminate();
        }
    }

    struct waiter_state_type final : std::enable_shared_from_this<waiter_state_type> {
        explicit waiter_state_type(worker_ref worker_value) :
            worker_(std::move(worker_value)),
            cancellation_target_(make_worker_cancellation_target(*this, worker_)) {}
        waiter_state_type(const waiter_state_type&) = delete;
        waiter_state_type& operator=(const waiter_state_type&) = delete;
        ~waiter_state_type() {
            cancellation_target_->detach(*this);
        }

        // First member: unlinks from the worker registry after every other member is gone.
        detail::worker_shutdown_registration shutdown_registration_;
        worker_ref worker_;
        waiter_queue_type queue_;
        // Hops a stop requested on any thread to the worker by waiter id.
        std::shared_ptr<worker_cancellation_target<waiter_state_type>> cancellation_target_;
        // Worker-affine: set once the shutdown notification reaches the worker.
        bool stopping_{false};

        void stop_waiters() noexcept {
            stopping_ = true;
            queue_.complete_all(worker_, wait_status::worker_stopping);
        }

        // The notification may arrive on the stopping thread; the queue is
        // touched only on the worker.
        void worker_stopping() noexcept {
            if (worker_.is_current()) {
                stop_waiters();
                return;
            }
            try {
                // A detached worker has already retired every suspended acquire.
                (void)worker_.defer_completion([retained = shared_from_this()] {
                    retained->stop_waiters();
                });
            } catch (...) {
                std::terminate();
            }
        }

        void cancel_operation_by_id(std::uint64_t waiter_id) noexcept {
            queue_.cancel(worker_, waiter_id);
        }
    };

    using cancellation_target_type = worker_cancellation_target<waiter_state_type>;

    // Checks in order: closed, cancelled, a free slot, worker stopping, deadline.
    [[nodiscard]] std::optional<acquire_result_type> try_acquire(const wait_bounds& bounds) {
        if (closing_) {
            return acquire_result_type::with_status(wait_status::closed);
        }
        if (bounds.stop_requested()) {
            return acquire_result_type::with_status(wait_status::cancelled);
        }
        if (free_head_ == no_free_slot) {
            const auto* const dispatcher = detail::worker_dispatcher::of(worker_);
            if (waiter_state_->stopping_ || !worker_.accepting() || dispatcher == nullptr || dispatcher->timers_stopping()) {
                return acquire_result_type::with_status(wait_status::worker_stopping);
            }
            if (bounds.deadline_passed()) {
                return acquire_result_type::with_status(wait_status::timed_out);
            }
            return std::nullopt;
        }
        const auto slot = free_head_;
        free_head_ = std::exchange(next_free_[slot], slot_leased);
        return acquire_result_type::with_value(slot);
    }

    void release(std::size_t slot) noexcept {
        if (slot >= next_free_.size() || next_free_[slot] != slot_leased) {
            std::terminate();
        }
        std::coroutine_handle<> continuation;
        if (!closing_ && waiter_state_->queue_.commit_handoff(slot, continuation)) {
            if (continuation) {
                worker_.defer_resume(continuation);
            }
            return;
        }
        next_free_[slot] = free_head_;
        free_head_ = slot;
    }

    [[nodiscard]] bool close() noexcept {
        if (closing_) {
            return false;
        }
        closing_ = true;
        waiter_state_->queue_.complete_all(worker_, wait_status::closed);
        return true;
    }

    worker_ref worker_;
    std::vector<std::size_t> next_free_;
    std::size_t free_head_{no_free_slot};
    std::shared_ptr<waiter_state_type> waiter_state_;
    std::size_t reserved_acquires_{0};
    bool closing_{false};
};

pool_lease_scheduler::pool_lease_scheduler(std::size_t pool_size, const worker_ref& worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("pool lease scheduler requires a valid worker");
    }
    impl_ = std::make_unique<impl_type>(pool_size, worker_value);
}

pool_lease_scheduler::~pool_lease_scheduler() = default;

pool_lease_acquire pool_lease_scheduler::acquire(operation_options options) & {
    return pool_lease_acquire(*impl_, std::move(options));
}

pool_lease_acquire::pool_lease_acquire(pool_lease_scheduler::impl_type& owner_value, operation_options options) noexcept :
    owner_(&owner_value),
    bounds_(std::move(options)) {
    ++owner_->reserved_acquires_;
}

pool_lease_acquire::pool_lease_acquire(pool_lease_acquire&& other) noexcept :
    owner_(other.awaited_ ? nullptr : std::exchange(other.owner_, nullptr)),
    bounds_(std::move(other.bounds_)) {
    if (other.awaited_) {
        std::terminate();
    }
}

pool_lease_acquire::~pool_lease_acquire() {
    if (owner_ != nullptr) {
        if (queued()) {
            cancel_registrations();
        }
        --owner_->reserved_acquires_;
    }
}

void pool_lease_acquire::cancel_registrations() noexcept {
    if (owner_ != nullptr) {
        owner_->waiter_state_->queue_.remove(*this);
    }
    bounds_.cancel_deadline();
    stop_registration_.reset();
}

std::coroutine_handle<> pool_lease_acquire::resolve(wait_result<std::size_t> result_value) noexcept {
    cancel_registrations();
    if (completion_.complete(std::move(result_value))) {
        return completion_.continuation();
    }
    return {};
}

void pool_lease_acquire::on_end(wait_status status) noexcept {
    owner_->waiter_state_->queue_.expire(*this, status);
}

bool pool_lease_acquire::await_ready() {
    if (owner_ == nullptr || awaited_) {
        throw std::logic_error("pool lease acquire cannot be awaited");
    }
    awaited_ = true;
    bounds_.start(owner_->worker_, "pool lease acquire must run on its bound worker");
    auto acquired = owner_->try_acquire(bounds_);
    if (!acquired) {
        return false;
    }
    (void)completion_.complete(std::move(*acquired));
    return true;
}

bool pool_lease_acquire::await_suspend(std::coroutine_handle<> continuation) {
    // Nothing ran between await_ready and here, so the pool is still saturated.
    auto& waiter_state = *owner_->waiter_state_;
    waiter_state.queue_.enqueue(*this);

    // The deadline timer can unlink the waiter directly on the worker. A timer
    // queue that stops, before or after arming, ends the wait as worker_stopping.
    bool armed = false;
    try {
        armed = bounds_.arm_deadline<pool_lease_acquire, &pool_lease_acquire::on_end>(*this);
    } catch (...) {
        cancel_registrations();
        throw;
    }
    if (!armed) {
        (void)resolve(wait_result<std::size_t>::with_status(wait_status::worker_stopping));
    } else if (bounds_.token().stoppable()) {
        // A stop on any thread hops to the worker by waiter id; one already
        // requested, or requested on the worker, commits before returning.
        using target_type = pool_lease_scheduler::impl_type::cancellation_target_type;
        static_assert(worker_cancellation_post_is_inline<target_type>);
        waiter_id_ = waiter_state.cancellation_target_->next_operation_id();
        bounds_.token().register_callback(stop_registration_, worker_cancellation_post<target_type>(waiter_state.cancellation_target_.get(), waiter_id_));
    }
    return completion_.suspend(continuation);
}

wait_result<std::size_t> pool_lease_acquire::await_resume() {
    return completion_.take_value();
}

void pool_lease_scheduler::release(std::size_t slot) noexcept {
    impl_->release(slot);
}

bool pool_lease_scheduler::close() noexcept {
    return impl_->close();
}

bool pool_lease_scheduler::closing() const noexcept {
    return impl_->closing_;
}

}  // namespace ruvia
