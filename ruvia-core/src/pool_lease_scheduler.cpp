#include "ruvia/core/pool_lease_scheduler.h"

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/worker_timer.h"

namespace ruvia {

class pool_lease_scheduler::impl_type final {
public:
    using acquire_result_type = worker_wait_result<std::size_t>;

    static constexpr std::size_t no_free_slot = static_cast<std::size_t>(-1);
    static constexpr std::size_t slot_leased = static_cast<std::size_t>(-2);

    // Intrusive FIFO of one pool_lease_scheduler. The scheduler is only ever
    // touched from its worker thread, so the queue needs no synchronization.
    class waiter_queue_type final {
    public:
        void enqueue(pool_lease_acquire& waiter) noexcept {
            waiter.previous_ = tail_;
            waiter.next_ = nullptr;
            waiter.queued_ = true;
            if (tail_ != nullptr) {
                tail_->next_ = std::addressof(waiter);
            } else {
                head_ = std::addressof(waiter);
            }
            tail_ = std::addressof(waiter);
        }

        void remove(pool_lease_acquire& waiter) noexcept {
            if (!waiter.queued_) {
                return;
            }
            if (waiter.previous_ != nullptr) {
                waiter.previous_->next_ = waiter.next_;
            } else {
                head_ = waiter.next_;
            }
            if (waiter.next_ != nullptr) {
                waiter.next_->previous_ = waiter.previous_;
            } else {
                tail_ = waiter.previous_;
            }
            waiter.previous_ = nullptr;
            waiter.next_ = nullptr;
            waiter.queued_ = false;
        }

        // Commit the freed slot to the next waiter before deferring its continuation.
        // Cancellation and closure cannot replace this result after the handoff.
        [[nodiscard]] bool commit_handoff(
            std::size_t index, std::coroutine_handle<>& continuation) noexcept {
            continuation = {};
            if (head_ == nullptr) {
                return false;
            }
            continuation = head_->resolve(worker_wait_result<std::size_t>::with_value(index));
            return true;
        }

        [[nodiscard]] bool cancel(std::uint64_t id) noexcept {
            auto* waiter = find(id);
            std::coroutine_handle<> continuation;
            if (waiter == nullptr || !commit_cancellation(*waiter, continuation)) {
                return false;
            }
            if (continuation) {
                continuation.resume();
            }
            return true;
        }

        // Commit cancellation while still inside a stop callback, but let the
        // caller resume the coroutine only after that callback has returned.
        [[nodiscard]] bool commit_cancellation(
            pool_lease_acquire& waiter, std::coroutine_handle<>& continuation) noexcept {
            continuation = {};
            if (!waiter.queued_) {
                return false;
            }
            continuation = waiter.resolve(worker_wait_status::cancelled);
            return true;
        }

        [[nodiscard]] bool expire(pool_lease_acquire& waiter, worker_wait_status status) noexcept {
            if (!waiter.queued_) {
                return false;
            }
            if (auto continuation = waiter.resolve(status)) {
                continuation.resume();
            }
            return true;
        }

        // Commit closure or shutdown for the entire current queue and resume
        // every waiter in a later worker turn, as release() does: closure runs
        // inside the owner's own teardown, which must finish before any waiter
        // observes it.
        void complete_all(const worker_ref& worker, worker_wait_status status) noexcept {
            while (head_ != nullptr) {
                if (auto continuation = head_->resolve(status)) {
                    worker.defer_resume(continuation);
                }
            }
        }

    private:
        [[nodiscard]] pool_lease_acquire* find(std::uint64_t id) const noexcept {
            auto* waiter = head_;
            while (waiter != nullptr) {
                if (waiter->waiter_id_ == id) {
                    return waiter;
                }
                waiter = waiter->next_;
            }
            return nullptr;
        }

        pool_lease_acquire* head_{nullptr};
        pool_lease_acquire* tail_{nullptr};
    };

    impl_type(std::size_t pool_size, const worker_ref& worker_value)
        : worker_(worker_value),
          next_free_(pool_size, no_free_slot),
          waiter_state_(std::make_shared<waiter_state_type>(worker_value)) {
        for (std::size_t i = 0; i < pool_size; ++i) {
            next_free_[i] = free_head_;
            free_head_ = i;
        }
        if (worker_.register_shutdown(waiter_state_, &waiter_state_type::shutdown_registration_) !=
            worker_registration_status::registered) {
            waiter_state_->stopping_ = true;
        }
    }

    ~impl_type() {
        if (reserved_acquires_ != 0) {
            std::terminate();
        }
    }

    struct waiter_state_type final : std::enable_shared_from_this<waiter_state_type> {
        explicit waiter_state_type(worker_ref worker_value) noexcept
            : worker_(std::move(worker_value)) {}

        // First member: unlinks from the worker registry after every other member is gone.
        worker_shutdown_registration shutdown_registration_;
        worker_ref worker_;
        waiter_queue_type queue_;
        // Worker-affine: set once the shutdown notification reaches the worker.
        bool stopping_{false};

        void stop_waiters() noexcept {
            stopping_ = true;
            queue_.complete_all(worker_, worker_wait_status::worker_stopping);
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
                (void)worker_.defer_completion([retained = shared_from_this()] { retained->stop_waiters(); });
            } catch (...) {
                std::terminate();
            }
        }

        void request_cancellation(pool_lease_acquire& waiter, std::uint64_t waiter_id) {
            if (worker_.is_current()) {
                std::coroutine_handle<> continuation;
                if (!queue_.commit_cancellation(waiter, continuation) || !continuation) {
                    return;
                }
                worker_.defer_resume(continuation);
                return;
            }
            auto retained = shared_from_this();
            // A detached worker has already retired the owners of every
            // suspended acquire, so there is nothing left to cancel.
            (void)worker_.defer_completion([retained = std::move(retained), waiter_id] {
                (void)retained->queue_.cancel(waiter_id);
            });
        }
    };

    [[nodiscard]] std::optional<acquire_result_type> try_acquire(
        const std::optional<std::chrono::steady_clock::time_point>& deadline, const stop_token& stop_token_value) {
        if (!worker_.is_current()) {
            throw std::logic_error("pool lease acquire must run on its bound worker");
        }
        if (closing_) {
            return worker_wait_result<std::size_t>::with_status(worker_wait_status::closed);
        }
        if (stop_token_value.stop_requested()) {
            return worker_wait_result<std::size_t>::with_status(worker_wait_status::cancelled);
        }
        if (free_head_ == no_free_slot) {
            if (waiter_state_->stopping_ || !worker_.accepting() ||
                detail::worker_ref_access::timers_stopping(worker_)) {
                return worker_wait_result<std::size_t>::with_status(worker_wait_status::worker_stopping);
            }
            if (deadline.has_value() && *deadline <= std::chrono::steady_clock::now()) {
                return worker_wait_result<std::size_t>::with_status(worker_wait_status::timed_out);
            }
            return std::nullopt;
        }
        const auto slot = free_head_;
        free_head_ = std::exchange(next_free_[slot], slot_leased);
        return worker_wait_result<std::size_t>::with_value(slot);
    }

    [[nodiscard]] std::uint64_t next_waiter_id() noexcept {
        auto waiter_id = ++next_waiter_id_;
        if (waiter_id == 0) {
            waiter_id = ++next_waiter_id_;
        }
        return waiter_id;
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
        waiter_state_->queue_.complete_all(worker_, worker_wait_status::closed);
        return true;
    }

    worker_ref worker_;
    std::vector<std::size_t> next_free_;
    std::size_t free_head_{no_free_slot};
    std::shared_ptr<waiter_state_type> waiter_state_;
    std::size_t reserved_acquires_{0};
    std::uint64_t next_waiter_id_{0};
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
    return pool_lease_acquire(*impl_, options.timeout_, std::move(options.stop_token_));
}

pool_lease_acquire::pool_lease_acquire(pool_lease_scheduler::impl_type& owner_value,
    std::optional<operation_timeout> timeout, stop_token stop_token_value) noexcept
    : owner_(&owner_value),
      timeout_(timeout),
      stop_token_(std::move(stop_token_value)) {
    ++owner_->reserved_acquires_;
}

pool_lease_acquire::pool_lease_acquire(pool_lease_acquire&& other) noexcept
    : owner_(other.awaited_ ? nullptr : std::exchange(other.owner_, nullptr)),
      timeout_(other.timeout_),
      deadline_(other.deadline_),
      stop_token_(std::move(other.stop_token_)) {
    if (other.awaited_) {
        std::terminate();
    }
}

pool_lease_acquire::~pool_lease_acquire() {
    if (owner_ != nullptr) {
        if (queued_) {
            cancel_registrations();
        }
        --owner_->reserved_acquires_;
    }
}

void pool_lease_acquire::cancel_registrations() noexcept {
    if (queued_ && owner_ != nullptr) {
        owner_->waiter_state_->queue_.remove(*this);
    }
    deadline_timer_.cancel();
    stop_registration_.reset();
}

void pool_lease_acquire::complete(worker_wait_result<std::size_t> result_value) noexcept {
    if (result_.has_value()) {
        std::terminate();
    }
    result_.emplace(result_value);
}

std::coroutine_handle<> pool_lease_acquire::resolve(worker_wait_result<std::size_t> result_value) noexcept {
    cancel_registrations();
    complete(result_value);
    return std::exchange(continuation_, {});
}

std::coroutine_handle<> pool_lease_acquire::resolve(worker_wait_status status) noexcept {
    return resolve(worker_wait_result<std::size_t>::with_status(status));
}

bool pool_lease_acquire::await_ready() {
    if (owner_ == nullptr || awaited_) {
        throw std::logic_error("pool lease acquire cannot be awaited");
    }
    awaited_ = true;
    if (timeout_.has_value()) {
        deadline_ = timeout_->deadline_from(std::chrono::steady_clock::now());
    }
    if (auto acquired = owner_->try_acquire(deadline_, stop_token_)) {
        result_.emplace(std::move(*acquired));
    }
    return result_.has_value();
}

bool pool_lease_acquire::await_suspend(std::coroutine_handle<> continuation) {
    // Nothing ran between await_ready and here, so the pool is still saturated.
    waiter_id_ = owner_->next_waiter_id();
    const auto waiter_id = waiter_id_;
    auto* const waiter_state = owner_->waiter_state_.get();
    waiter_state->queue_.enqueue(*this);

    // A deadline arms a worker-local timer owned by this acquire and cancelled
    // before it can be destroyed. Its expiry can unlink the waiter directly;
    // cross-thread stop notifications still use waiter IDs. A timer queue that
    // stops, before or after arming, ends the wait as worker_stopping.
    if (deadline_.has_value()) {
        auto status = worker_timer_schedule_status::worker_stopping;
        try {
            status = owner_->worker_.schedule_timer(deadline_timer_, *deadline_,
                [waiter_state, waiter = this](worker_timer_outcome outcome) noexcept {
                    (void)waiter_state->queue_.expire(*waiter, outcome == worker_timer_outcome::expired
                                                                   ? worker_wait_status::timed_out
                                                                   : worker_wait_status::worker_stopping);
                });
        } catch (...) {
            cancel_registrations();
            throw;
        }
        if (status == worker_timer_schedule_status::worker_stopping) {
            (void)resolve(worker_wait_status::worker_stopping);
            return false;
        }
    }

    if (stop_token_.stoppable()) {
        stop_token_.register_callback(stop_registration_,
            [waiter_state, waiter = this, waiter_id] { waiter_state->request_cancellation(*waiter, waiter_id); });
    }
    if (result_.has_value()) {
        return false;
    }
    continuation_ = continuation;
    return true;
}

worker_wait_result<std::size_t> pool_lease_acquire::await_resume() {
    if (!result_.has_value()) {
        std::terminate();
    }
    return *result_;
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
