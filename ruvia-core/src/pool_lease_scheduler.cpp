#include "ruvia/core/pool_lease_scheduler.h"

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/worker_timer.h"

#include "pool_waiter_queue.h"

namespace ruvia {

class pool_lease_scheduler::impl_type final {
public:
    using acquire_result_type = worker_wait_result<std::size_t>;

    impl_type(std::size_t pool_size, const worker_handle& worker_value, std::pmr::memory_resource* resource)
        : worker_(worker_value),
          free_slots_(pmr_resource_or_default(resource)),
          busy_(pmr_resource_or_default(resource)),
          waiter_state_(std::allocate_shared<waiter_state_type>(
              std::pmr::polymorphic_allocator<waiter_state_type>(process_resource()))) {
        free_slots_.reserve(pool_size);
        busy_.resize(pool_size, 0);
        for (std::size_t i = 0; i < pool_size; ++i) {
            free_slots_.push_back(i);
        }
    }

    ~impl_type() {
        if (reserved_acquires_ != 0) {
            std::terminate();
        }
    }

    struct waiter_state_type final : std::enable_shared_from_this<waiter_state_type> {
        detail::pool_waiter_queue queue_;

        void request_cancellation(const worker_handle& worker_value, std::uint64_t waiter_id) {
            if (worker_value.is_current()) {
                std::coroutine_handle<> continuation;
                if (!queue_.commit_cancellation(waiter_id, continuation) || !continuation) {
                    return;
                }
                detail::worker_handle_access::defer_or_terminate(
                    worker_value, [continuation] { continuation.resume(); });
                return;
            }
            auto retained = shared_from_this();
            // A detached worker has already retired the owners of every
            // suspended acquire, so there is nothing left to cancel.
            (void)detail::worker_handle_access::defer_if_attached(
                worker_value, [retained = std::move(retained), waiter_id] {
                    (void)retained->queue_.cancel(waiter_id);
                });
        }
    };

    [[nodiscard]] std::optional<acquire_result_type> try_acquire(const stop_token& stop_token_value) {
        if (!worker_.is_current()) {
            throw std::logic_error("pool lease acquire must run on its bound worker");
        }
        if (closing_) {
            return detail::worker_wait_result_access::outcome<std::size_t>(worker_wait_status::closed);
        }
        if (stop_token_value.stop_requested()) {
            return detail::worker_wait_result_access::outcome<std::size_t>(worker_wait_status::cancelled);
        }
        if (free_slots_.empty()) {
            return std::nullopt;
        }
        const auto slot = free_slots_.back();
        free_slots_.pop_back();
        busy_[slot] = 1;
        return detail::worker_wait_result_access::value(slot);
    }

    // The waiting path only: the caller holds the reservation that keeps this
    // scheduler alive, and try_acquire() has just found the pool saturated.
    [[nodiscard]] static task<acquire_result_type> acquire_waiting(impl_type& owner_value,
        std::optional<std::chrono::milliseconds> timeout, stop_token stop_token_value) {
        const auto& worker_value = owner_value.worker_;
        auto waiter_id = ++owner_value.next_waiter_id_;
        if (waiter_id == 0) {
            waiter_id = ++owner_value.next_waiter_id_;
        }
        detail::pool_waiter waiter(waiter_id);
        auto* const waiter_state = owner_value.waiter_state_.get();
        waiter_state->queue_.enqueue(waiter);
        struct waiter_guard final {
            detail::pool_waiter_queue& queue_;
            detail::pool_waiter& waiter_;
            ~waiter_guard() {
                queue_.remove(waiter_);
            }
        } guard_value{waiter_state->queue_, waiter};

        ::ruvia::worker_timer_registration deadline_timer;
        // A stopped timer queue arms nothing, exactly as stop_timers() cancels a
        // pending deadline: the waiter still completes on release, close(), or
        // its stop token.
        if (timeout.has_value() && worker_value.valid()) {
            worker_value.schedule_timer(deadline_timer, ::ruvia::worker_timer_deadline_after(*timeout), [waiter_state, waiter_id](::ruvia::worker_timer_outcome outcome) noexcept {
                if (outcome == ::ruvia::worker_timer_outcome::expired) {
                    (void)waiter_state->queue_.expire(waiter_id);
                }
            });
        }

        auto stop_registration_value = stop_token_value.register_callback([&worker_value, waiter_state, waiter_id] {
            waiter_state->request_cancellation(worker_value, waiter_id);
        });
        if (stop_token_value.stop_requested()) {
            (void)waiter_state->queue_.cancel(waiter_id);
        }

        auto result_value = co_await waiter;
        deadline_timer.cancel();
        stop_registration_value.reset();
        co_return result_value;
    }

    [[nodiscard]] pool_lease_release_status release(std::size_t slot) noexcept {
        if (slot >= busy_.size()) {
            return pool_lease_release_status::invalid_slot;
        }
        if (busy_[slot] == 0) {
            return pool_lease_release_status::already_released;
        }
        if (!closing_ && waiter_state_->queue_.resume_next(slot)) {
            return pool_lease_release_status::transferred_to_waiter;
        }
        busy_[slot] = 0;
        free_slots_.push_back(slot);
        return pool_lease_release_status::released;
    }

    [[nodiscard]] bool close() noexcept {
        if (closing_) {
            return false;
        }
        closing_ = true;
        waiter_state_->queue_.close_all();
        return true;
    }

    worker_handle worker_;
    std::pmr::vector<std::size_t> free_slots_;
    std::pmr::vector<std::uint8_t> busy_;
    std::shared_ptr<waiter_state_type> waiter_state_;
    std::size_t reserved_acquires_{0};
    std::uint64_t next_waiter_id_{0};
    bool closing_{false};
};

pool_lease_scheduler::pool_lease_scheduler(std::size_t pool_size, const worker_handle& worker_value,
    std::pmr::memory_resource* resource) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("pool lease scheduler requires a valid worker");
    }
    impl_ = std::make_unique<impl_type>(pool_size, worker_value, resource);
}

pool_lease_scheduler::~pool_lease_scheduler() = default;

pool_lease_acquire pool_lease_scheduler::acquire(const std::optional<std::chrono::milliseconds>& timeout) {
    return pool_lease_acquire(*impl_, timeout, {});
}

pool_lease_acquire pool_lease_scheduler::acquire(
    const std::optional<std::chrono::milliseconds>& timeout, stop_token stop_token_value) {
    return pool_lease_acquire(*impl_, timeout, std::move(stop_token_value));
}

pool_lease_acquire::pool_lease_acquire(pool_lease_scheduler::impl_type& owner_value,
    std::optional<std::chrono::milliseconds> timeout, stop_token stop_token_value) noexcept
    : owner_(&owner_value),
      timeout_(timeout),
      stop_token_(std::move(stop_token_value)) {
    ++owner_->reserved_acquires_;
}

pool_lease_acquire::pool_lease_acquire(pool_lease_acquire&& other) noexcept
    : owner_(other.awaited_ ? nullptr : std::exchange(other.owner_, nullptr)),
      timeout_(other.timeout_),
      stop_token_(std::move(other.stop_token_)) {
    if (other.awaited_) {
        std::terminate();
    }
}

pool_lease_acquire::~pool_lease_acquire() {
    if (owner_ != nullptr) {
        --owner_->reserved_acquires_;
    }
}

bool pool_lease_acquire::await_ready() {
    if (owner_ == nullptr || awaited_) {
        throw std::logic_error("pool lease acquire cannot be awaited");
    }
    awaited_ = true;
    result_ = owner_->try_acquire(stop_token_);
    return result_.has_value();
}

std::coroutine_handle<> pool_lease_acquire::await_suspend(std::coroutine_handle<> continuation) {
    // Nothing ran between await_ready and here, so the pool is still saturated.
    auto& awaiter = waiting_.emplace(
        pool_lease_scheduler::impl_type::acquire_waiting(*owner_, timeout_, std::move(stop_token_)));
    return awaiter.await_suspend(continuation);
}

worker_wait_result<std::size_t> pool_lease_acquire::await_resume() {
    if (waiting_.has_value()) {
        return waiting_->await_resume();
    }
    return *result_;
}

pool_lease_release_status pool_lease_scheduler::release(std::size_t slot) noexcept {
    return impl_->release(slot);
}

bool pool_lease_scheduler::close() noexcept {
    return impl_->close();
}

bool pool_lease_scheduler::closing() const noexcept {
    return impl_->closing_;
}

}  // namespace ruvia
