#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "stop_token.h"
#include "worker_ref.h"
#include "worker_wait.h"

namespace ruvia::detail {

// The connection-slot bookkeeping every pooled client shares: lease scheduling,
// stop-token cancellation addressed by operation id, and one deadline timer per
// slot. Only the error type and what an expiry or cancellation does to the
// driver's connection differ, so those are the policy.

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

    slot_pool(owner_type& owner_value, std::size_t slot_count, const worker_ref& worker_value) :
        owner_(owner_value),
        worker_(worker_value),
        slots_(slot_count),
        scheduler_(slot_count, worker_),
        cancellation_target_(make_worker_cancellation_target(*this, worker_)) {}

    slot_pool(owner_type&, std::size_t, worker_ref&&) = delete;

    ~slot_pool() {
        cancellation_target_->detach(*this);
    }

    slot_pool(const slot_pool&) = delete;
    slot_pool& operator=(const slot_pool&) = delete;
    slot_pool(slot_pool&&) = delete;
    slot_pool& operator=(slot_pool&&) = delete;

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    class acquire_awaiter final {
    public:
        acquire_awaiter(owner_type& owner, pool_lease_acquire acquire) noexcept :
            owner_(owner),
            acquire_(std::move(acquire)) {}
        acquire_awaiter(const acquire_awaiter&) = delete;
        acquire_awaiter& operator=(const acquire_awaiter&) = delete;
        acquire_awaiter(acquire_awaiter&&) noexcept = default;
        acquire_awaiter& operator=(acquire_awaiter&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready() {
            return acquire_.await_ready();
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
            return acquire_.await_suspend(continuation);
        }
        [[nodiscard]] std::size_t await_resume() {
            const auto result_value = acquire_.await_resume();
            if (result_value.has_value()) {
                return result_value.value();
            }
            policy_type::throw_acquire_failure(owner_, result_value.status());
        }

    private:
        owner_type& owner_;
        pool_lease_acquire acquire_;
    };

    // The common free-slot path completes directly in the caller's frame.
    // Saturated pools use the same scheduler, timeout and cancellation path.
    [[nodiscard]] acquire_awaiter acquire(const operation_deadline& timeout, std::optional<timeout_duration> acquire_timeout, stop_token stop_token_value) {
        return acquire_awaiter(owner_, scheduler_.acquire({.timeout_ = timeout.constrained_by(acquire_timeout).timeout(), .stop_token_ = std::move(stop_token_value)}));
    }

    // Leases a free slot without waiting; nullopt while every slot is leased
    // or the pool is closing. A free slot is exactly what acquire() leases
    // without suspending.
    [[nodiscard]] std::optional<std::size_t> try_acquire() {
        auto acquire = scheduler_.acquire();
        if (!acquire.await_ready()) {
            return std::nullopt;
        }
        const auto result_value = acquire.await_resume();
        if (!result_value.has_value()) {
            return std::nullopt;
        }
        return result_value.value();
    }

    // A release that names no live lease is a caller bug and cannot be
    // reported through a noexcept path.
    void release(std::size_t slot) noexcept {
        scheduler_.release(slot);
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

    // Starts an operation on a leased slot: clears a cancellation its previous
    // operation left, while closing stays permanent.
    void reset_abort(std::size_t slot) noexcept {
        slots_[slot].abort_reason_ = closing() ? pool_slot_abort_reason::closing : pool_slot_abort_reason::none;
    }

    // Releases a lease on scope exit, however the operation between acquire
    // and release ends.
    class lease_guard final {
    public:
        lease_guard(slot_pool& pool, std::size_t slot) noexcept :
            pool_(pool),
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
        cancellation_guard(slot_pool& pool, std::size_t slot, const stop_token& stop_token_value) :
            registration_(pool.cancellation_target_, pool.slots_[slot].cancellation_id_) {
            pool.reset_abort(slot);
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

    // Replaces the slot's absolute deadline. An absent deadline leaves the
    // slot unarmed; an expired one returns false so the caller reports its own
    // timeout. A scheduling failure rolls the slot back before propagating.
    [[nodiscard]] bool arm_deadline(std::size_t slot, std::optional<std::chrono::steady_clock::time_point> deadline, deadline_kind_type kind) {
        auto& state = slots_[slot];
        (void)clear_deadline(slot);
        if (!deadline.has_value()) {
            return true;
        }
        if (*deadline <= std::chrono::steady_clock::now()) {
            return false;
        }
        const auto deadline_value = *deadline;
        state.deadline_.arm(deadline_value, kind);
        try {
            (void)worker_.schedule_timer(state.deadline_timer_, deadline_value, [this, slot](worker_timer_outcome outcome) noexcept {
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
        deadline_lifecycle<deadline_kind_type> deadline_;
        worker_timer_registration deadline_timer_;
    };

    // Fixed-size, never relocated: timer registrations are neither movable
    // nor copyable, and pending timers address their slot by index.
    class slot_array final {
    public:
        explicit slot_array(std::size_t slot_count) :
            data_(std::make_unique<slot_state[]>(slot_count)),
            size_(slot_count) {}
        slot_array(const slot_array&) = delete;
        slot_array& operator=(const slot_array&) = delete;

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
            return data_.get();
        }
        [[nodiscard]] slot_state* end() noexcept {
            return data_.get() + size_;
        }

    private:
        std::unique_ptr<slot_state[]> data_;
        std::size_t size_;
    };

    owner_type& owner_;
    const worker_ref& worker_;
    slot_array slots_;
    pool_lease_scheduler scheduler_;
    std::shared_ptr<cancellation_target_type> cancellation_target_;
};

}  // namespace ruvia::detail
