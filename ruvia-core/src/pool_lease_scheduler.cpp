#include "ruvia/core/pool_lease_scheduler.h"

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/worker_timer.h"

#include "pool_waiter_queue.h"

namespace ruvia {

class pool_lease_scheduler::impl_type final {
public:
    impl_type(std::size_t pool_size, const worker_handle* worker_value, std::pmr::memory_resource* resource)
        : worker_(worker_value != nullptr ? *worker_value : worker_handle{}),
          free_slots_(detail::pmr_resource_or_default(resource)),
          busy_(detail::pmr_resource_or_default(resource)),
          waiter_state_(std::allocate_shared<waiter_state_type>(
              std::pmr::polymorphic_allocator<waiter_state_type>(detail::process_resource()))) {
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
            detail::worker_handle_access::defer_or_terminate(
                worker_value, [retained = std::move(retained), waiter_id] {
                    (void)retained->queue_.cancel(waiter_id);
                });
        }
    };

    class acquire_reservation_type final {
    public:
        explicit acquire_reservation_type(impl_type& owner_value) noexcept
            : owner_(&owner_value) {
            ++owner_->reserved_acquires_;
        }
        ~acquire_reservation_type() {
            if (owner_ != nullptr) {
                --owner_->reserved_acquires_;
            }
        }
        acquire_reservation_type(const acquire_reservation_type&) = delete;
        acquire_reservation_type& operator=(const acquire_reservation_type&) = delete;
        acquire_reservation_type(acquire_reservation_type&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)) {}
        acquire_reservation_type& operator=(acquire_reservation_type&&) = delete;

        [[nodiscard]] impl_type& owner() const noexcept {
            return *owner_;
        }

    private:
        impl_type* owner_;
    };

    [[nodiscard]] static task<pool_waiter_result> acquire_reserved(acquire_reservation_type reservation,
        std::optional<std::chrono::milliseconds> timeout, stop_token stop_token_value,
        const worker_handle* worker_value) {
        auto& owner_value = reservation.owner();
        if (stop_token_value.stoppable() && (worker_value == nullptr || !worker_value->valid())) {
            throw std::logic_error("cancellable pool acquire requires a valid worker");
        }
        if (owner_value.closing_) {
            co_return pool_waiter_result::make_closed();
        }
        if (stop_token_value.stop_requested()) {
            co_return pool_waiter_result::make_cancelled();
        }
        if (!owner_value.free_slots_.empty()) {
            const auto slot = owner_value.free_slots_.back();
            owner_value.free_slots_.pop_back();
            owner_value.busy_[slot] = 1;
            co_return pool_waiter_result::make_acquired(slot);
        }

        const auto deadline_value = timeout.has_value() ? ::ruvia::worker_timer_deadline_after(*timeout)
                                                        : std::chrono::steady_clock::time_point::max();
        auto waiter_id = ++owner_value.next_waiter_id_;
        if (waiter_id == 0) {
            waiter_id = ++owner_value.next_waiter_id_;
        }
        detail::pool_waiter waiter(deadline_value, waiter_id);
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
        if (timeout.has_value() && worker_value != nullptr && worker_value->valid()) {
            (*worker_value).schedule_timer(deadline_timer, deadline_value, [waiter_state, waiter_id](::ruvia::worker_timer_outcome outcome) noexcept {
                if (outcome == ::ruvia::worker_timer_outcome::expired) {
                    (void)waiter_state->queue_.expire(waiter_id);
                }
            });
        }

        auto stop_registration_value = stop_token_value.register_callback([worker_value, waiter_state, waiter_id] {
            waiter_state->request_cancellation(*worker_value, waiter_id);
        });
        if (stop_token_value.stoppable() && stop_token_value.stop_requested()) {
            (void)waiter_state->queue_.cancel(waiter_id);
        }

        const auto& result_value = co_await waiter;
        deadline_timer.cancel();
        stop_registration_value.reset();
        if (result_value.acquired() != nullptr) {
            co_return pool_waiter_result::make_acquired(result_value.acquired()->index());
        }
        if (result_value.timed_out() != nullptr) {
            co_return pool_waiter_result::make_timed_out();
        }
        if (result_value.closed() != nullptr) {
            co_return pool_waiter_result::make_closed();
        }
        co_return pool_waiter_result::make_cancelled();
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

    void scan_deadlines(std::chrono::steady_clock::time_point now) noexcept {
        waiter_state_->queue_.expire_deadlines(now);
    }

    worker_handle worker_;
    std::pmr::vector<std::size_t> free_slots_;
    std::pmr::vector<std::uint8_t> busy_;
    std::shared_ptr<waiter_state_type> waiter_state_;
    std::size_t reserved_acquires_{0};
    std::uint64_t next_waiter_id_{0};
    bool closing_{false};
};

pool_lease_scheduler::pool_lease_scheduler(
    std::size_t pool_size, std::pmr::memory_resource* resource)
    : impl_(std::make_unique<impl_type>(pool_size, nullptr, resource)) {}

pool_lease_scheduler::pool_lease_scheduler(std::size_t pool_size, const worker_handle& worker_value,
    std::pmr::memory_resource* resource) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("pool lease scheduler requires a valid worker");
    }
    impl_ = std::make_unique<impl_type>(pool_size, &worker_value, resource);
}

pool_lease_scheduler::~pool_lease_scheduler() = default;

task<pool_waiter_result> pool_lease_scheduler::acquire(
    const std::optional<std::chrono::milliseconds>& timeout) {
    return impl_type::acquire_reserved(impl_type::acquire_reservation_type(*impl_), timeout, {}, &impl_->worker_);
}

task<pool_waiter_result> pool_lease_scheduler::acquire(
    const std::optional<std::chrono::milliseconds>& timeout, stop_token stop_token_value) {
    return impl_type::acquire_reserved(
        impl_type::acquire_reservation_type(*impl_), timeout, std::move(stop_token_value), &impl_->worker_);
}

pool_lease_release_status pool_lease_scheduler::release(std::size_t slot) noexcept {
    return impl_->release(slot);
}

bool pool_lease_scheduler::close() noexcept {
    return impl_->close();
}

void pool_lease_scheduler::scan_deadlines(std::chrono::steady_clock::time_point now) noexcept {
    impl_->scan_deadlines(now);
}

bool pool_lease_scheduler::closing() const noexcept {
    return impl_->closing_;
}

}  // namespace ruvia
