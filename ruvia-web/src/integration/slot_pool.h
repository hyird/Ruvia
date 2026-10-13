#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/pool_lease_scheduler.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait_result.h"

// The connection-slot bookkeeping every pooled client shares: lease scheduling,
// stop-token cancellation addressed by operation id, and one deadline timer per
// slot. Only the error type and what an expiry or cancellation does to the
// driver's connection differ, so those are the policy.

namespace ruvia::detail {

enum class pool_slot_abort_reason : std::uint8_t {
    none,
    cancelled,
    closing,
};

// policy_type supplies:
//   using owner_type = ...;          // the driver pool that owns the connections
//   using deadline_kind_type = ...;  // scoped enum naming what a deadline guards
//   [[noreturn]] static void throw_acquire_failure(const owner_type&, worker_wait_status);
//   [[noreturn]] static void throw_aborted(const owner_type&, pool_slot_abort_reason);
//   static void expire_slot(owner_type&, std::size_t slot, deadline_kind_type) noexcept;
//   static void cancel_slot(owner_type&, std::size_t slot) noexcept;
//
// The pool is worker-affine and address-stable: slot state lives in one array
// sized at construction, and pending timers and cancellation registrations
// borrow it by address.
template <typename policy_type>
class slot_pool final {
public:
    using owner_type = typename policy_type::owner_type;
    using deadline_kind_type = typename policy_type::deadline_kind_type;
    using cancellation_target_type = worker_cancellation_target<slot_pool>;

    slot_pool(owner_type& owner_value, std::size_t slot_count, const worker_handle& worker_value,
        std::pmr::memory_resource* resource)
        : owner_(owner_value),
          worker_(worker_value),
          resource_(pmr_resource_or_default(resource)),
          slots_(slot_count, resource_),
          scheduler_(slot_count, worker_, resource_),
          cancellation_target_(make_worker_cancellation_target(*this, worker_)) {
        static_assert(worker_cancellation_post_is_inline<cancellation_target_type>);
    }

    slot_pool(owner_type&, std::size_t, worker_handle&&, std::pmr::memory_resource*) = delete;

    ~slot_pool() {
        cancellation_target_->detach(*this);
    }

    slot_pool(const slot_pool&) = delete;
    slot_pool& operator=(const slot_pool&) = delete;
    slot_pool(slot_pool&&) = delete;
    slot_pool& operator=(slot_pool&&) = delete;

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

    // Waits for a free slot under the tighter of the operation deadline and
    // the pool's own acquire timeout. Every non-acquired outcome is reported
    // through the policy's error type.
    [[nodiscard]] task<std::size_t> acquire(const operation_timeout& timeout,
        std::optional<std::chrono::milliseconds> acquire_timeout, stop_token stop_token_value) {
        const auto result_value = co_await scheduler_.acquire(
            timeout.constrained_by(acquire_timeout).remaining(), std::move(stop_token_value));
        if (result_value.has_value()) {
            co_return result_value.value();
        }
        policy_type::throw_acquire_failure(owner_, result_value.status());
    }

    // A release that names no live lease is a caller bug and cannot be
    // reported through a noexcept path.
    void release(std::size_t slot) noexcept {
        const auto status = scheduler_.release(slot);
        if (status == pool_lease_release_status::invalid_slot ||
            status == pool_lease_release_status::already_released) {
            std::terminate();
        }
    }

    // Refuses new leases and marks every slot without an abort reason as
    // closing; on true the owner must abort every connection, since a later
    // stop no longer reaches cancel_slot(). Returns false when the pool was
    // already closed; the owner then has nothing left to tear down.
    [[nodiscard]] bool close() noexcept {
        if (!scheduler_.close()) {
            return false;
        }
        for (auto& slot : slots_) {
            if (slot.abort_reason_ == pool_slot_abort_reason::none) {
                slot.abort_reason_ = pool_slot_abort_reason::closing;
            }
        }
        return true;
    }

    [[nodiscard]] bool closing() const noexcept {
        return scheduler_.closing();
    }

    // Releases a lease on scope exit, however the operation between acquire
    // and release ends.
    class lease_guard final {
    public:
        lease_guard(slot_pool& pool, std::size_t slot) noexcept
            : pool_(pool),
              slot_(slot) {}
        lease_guard(const lease_guard&) = delete;
        lease_guard& operator=(const lease_guard&) = delete;
        ~lease_guard() {
            pool_.release(slot_);
        }

    private:
        slot_pool& pool_;
        std::size_t slot_;
    };

    // Binds the operation's stop token to one slot for the duration of the
    // operation. A stop delivered on (or posted to) the worker reaches
    // cancel_operation_by_id(), which marks the slot cancelled and lets the
    // policy abort the driver connection. A cancellation belongs to the
    // operation it ended; closing is permanent, so an operation still holding
    // a lease after close() (an open stream or transaction) observes it, even
    // when its stop arrives later.
    class cancellation_guard final {
    public:
        cancellation_guard(slot_pool& pool, std::size_t slot, const stop_token& stop_token_value)
            : registration_(pool.cancellation_target_, pool.slots_[slot].cancellation_id_) {
            pool.slots_[slot].abort_reason_ =
                pool.closing() ? pool_slot_abort_reason::closing : pool_slot_abort_reason::none;
            registration_.arm(stop_token_value);
        }
        cancellation_guard(const cancellation_guard&) = delete;
        cancellation_guard& operator=(const cancellation_guard&) = delete;
        ~cancellation_guard() {
            finish();
        }

        void finish() noexcept {
            registration_.reset();
        }

        // The operation's id; cancel_operation_by_id() with it acts as a stop.
        [[nodiscard]] std::uint64_t id() const noexcept {
            return registration_.id();
        }

    private:
        worker_cancellation_registration<cancellation_target_type> registration_;
    };

    // worker_cancellation_target hook; runs on the worker. A closing slot
    // keeps reporting closing: its owner aborts every connection right after
    // close(), so a later stop has nothing left to cancel.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
        if (cancellation_id == 0) {
            return;
        }
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            auto& slot = slots_[index];
            if (slot.cancellation_id_ != cancellation_id) {
                continue;
            }
            slot.cancellation_id_ = 0;
            if (slot.abort_reason_ != pool_slot_abort_reason::closing) {
                slot.abort_reason_ = pool_slot_abort_reason::cancelled;
                policy_type::cancel_slot(owner_, index);
            }
            return;
        }
    }

    [[nodiscard]] bool aborted(std::size_t slot) const noexcept {
        return slots_[slot].abort_reason_ != pool_slot_abort_reason::none;
    }

    void throw_if_aborted(std::size_t slot) const {
        const auto reason = slots_[slot].abort_reason_;
        if (reason != pool_slot_abort_reason::none) {
            policy_type::throw_aborted(owner_, reason);
        }
    }

    // Replaces the slot's deadline. An absent remaining time leaves the slot
    // without a deadline; an exhausted one leaves it without a deadline and
    // returns false so the caller reports its own timeout. A failure to
    // schedule rolls the slot back to no deadline before propagating.
    [[nodiscard]] bool arm_deadline(
        std::size_t slot, std::optional<std::chrono::milliseconds> remaining, deadline_kind_type kind) {
        auto& state = slots_[slot];
        (void)clear_deadline(slot);
        if (!remaining.has_value()) {
            return true;
        }
        if (remaining->count() <= 0) {
            return false;
        }
        const auto deadline_value = worker_timer_deadline_after(*remaining);
        state.deadline_.arm(deadline_value, kind);
        try {
            (void)worker_.schedule_timer(state.deadline_timer_, deadline_value,
                [this, slot](worker_timer_outcome outcome) noexcept {
                    if (outcome != worker_timer_outcome::expired) {
                        return;
                    }
                    const auto expired = slots_[slot].deadline_.expire(std::chrono::steady_clock::now());
                    if (expired.has_value()) {
                        policy_type::expire_slot(owner_, slot, *expired);
                    }
                });
        } catch (...) {
            state.deadline_timer_.cancel();
            state.deadline_.reset();
            throw;
        }
        return true;
    }

    // Disarms the slot's deadline; true when it had already expired.
    [[nodiscard]] bool clear_deadline(std::size_t slot) noexcept {
        auto& state = slots_[slot];
        state.deadline_timer_.cancel();
        return state.deadline_.clear();
    }

    // Expires an armed deadline whose time has passed without waiting for the
    // timer; true when the deadline is (now) expired.
    [[nodiscard]] bool poll_deadline(std::size_t slot) noexcept {
        auto& deadline = slots_[slot].deadline_;
        return deadline.expire(std::chrono::steady_clock::now()).has_value() || deadline.expired();
    }

    [[nodiscard]] const deadline_kind_type* deadline_kind(std::size_t slot) const noexcept {
        return slots_[slot].deadline_.kind();
    }

private:
    struct slot_state final {
        std::uint64_t cancellation_id_{0};
        pool_slot_abort_reason abort_reason_{pool_slot_abort_reason::none};
        operation_deadline<deadline_kind_type> deadline_;
        worker_timer_registration deadline_timer_;
    };

    // Fixed-size, never relocated: timer registrations are neither movable
    // nor copyable, and pending timers address their slot by index.
    class slot_array final {
    public:
        slot_array(std::size_t slot_count, std::pmr::memory_resource* resource)
            : resource_(resource),
              data_(std::pmr::polymorphic_allocator<slot_state>(resource_).allocate(slot_count)),
              size_(slot_count) {
            for (std::size_t index = 0; index < size_; ++index) {
                std::construct_at(data_ + index);
            }
        }
        slot_array(const slot_array&) = delete;
        slot_array& operator=(const slot_array&) = delete;
        ~slot_array() {
            std::destroy(data_, data_ + size_);
            std::pmr::polymorphic_allocator<slot_state>(resource_).deallocate(data_, size_);
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return size_;
        }
        [[nodiscard]] slot_state& operator[](std::size_t index) noexcept {
            return data_[index];
        }
        [[nodiscard]] const slot_state& operator[](std::size_t index) const noexcept {
            return data_[index];
        }
        [[nodiscard]] slot_state* begin() noexcept {
            return data_;
        }
        [[nodiscard]] slot_state* end() noexcept {
            return data_ + size_;
        }

    private:
        std::pmr::memory_resource* resource_;
        slot_state* data_;
        std::size_t size_;
    };

    owner_type& owner_;
    const worker_handle& worker_;
    std::pmr::memory_resource* resource_;
    slot_array slots_;
    pool_lease_scheduler scheduler_;
    std::shared_ptr<cancellation_target_type> cancellation_target_;
};

}  // namespace ruvia::detail
