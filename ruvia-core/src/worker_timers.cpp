#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <utility>

#include <asio/post.hpp>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_timer.h"

#include "worker_dispatcher_impl.h"

// The dispatcher's timer heap: registering a deadline, cancelling one from any
// thread, re-arming the single asio timer, and firing everything due on the
// worker.

namespace ruvia {

void worker_timer_cancellation::cancel() const noexcept {
    if (dispatcher_ != nullptr && generation_ != 0) {
        dispatcher_->request_timer_cancellation(slot_, generation_, true);
    }
}

worker_timer_registration::~worker_timer_registration() {
    unregister(false);
}

void worker_timer_registration::cancel() noexcept {
    unregister(false);
}

void worker_timer_registration::cancel_and_notify() noexcept {
    unregister(true);
}

void worker_timer_registration::unregister(bool notify) noexcept {
    auto* dispatcher = std::exchange(dispatcher_, nullptr);
    const auto* slot_state = std::exchange(slot_state_, nullptr);
    const auto slot = std::exchange(slot_, 0);
    const auto generation = std::exchange(generation_, 0);
    if (dispatcher == nullptr) {
        return;
    }
    if (!notify && slot_state != nullptr) {
        auto expected = generation;
        (void)const_cast<detail::timer_slot*>(slot_state)->active_generation_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel, std::memory_order_relaxed);
    }
    dispatcher->request_timer_cancellation(slot, generation, notify);
}

bool worker_timer_registration::registered() const noexcept {
    return dispatcher_ != nullptr;
}

worker_timer_cancellation worker_timer_registration::cancellation() const& {
    if (dispatcher_ == nullptr || generation_ == 0) {
        throw std::logic_error("worker timer registration is not active");
    }
    return worker_timer_cancellation(*dispatcher_, slot_, generation_);
}

void worker_timer_registration::bind(detail::worker_dispatcher& dispatcher, std::size_t slot,
    std::uint64_t generation, const detail::timer_slot& slot_state) noexcept {
    dispatcher_ = &dispatcher;
    slot_state_ = &slot_state;
    slot_ = slot;
    generation_ = generation;
}

void worker_timer_registration::release() noexcept {
    dispatcher_ = nullptr;
    slot_state_ = nullptr;
    slot_ = 0;
    generation_ = 0;
}

bool worker_timer_registration::pending() const noexcept {
    // Pairs with the release store that deactivates the slot on its own worker
    // before expiry or cancellation delivers a completion.
    return slot_state_ != nullptr && generation_ != 0 &&
           slot_state_->active_generation_.load(std::memory_order_acquire) == generation_;
}

}  // namespace ruvia

namespace ruvia::detail {

worker_timer_schedule_status worker_dispatcher::schedule_timer(worker_timer_registration& registration,
    std::chrono::steady_clock::time_point deadline_value,
    move_only_function<void(worker_timer_outcome)> completion) {
    if (!is_current()) {
        throw std::logic_error("worker timers must be scheduled on their worker");
    }
    if (registration.dispatcher_ != nullptr) {
        // The previous binding may belong to another worker; its slot state is
        // the only part of that worker read here.
        if (registration.pending()) {
            throw std::logic_error("worker timer registration is already active");
        }
        registration.release();
    }
    // Detachment also stops the timer queue, so one check covers both.
    if (timers_stopping_.load(std::memory_order_acquire)) {
        return worker_timer_schedule_status::worker_stopping;
    }
    // New due entries belong to the next dispatch round. Place them after all
    // entries due at this round's snapshot, even if their requested deadline
    // is earlier, so the sequence gate cannot hide other pending expirations.
    if (dispatching_timers_) {
        deadline_value = std::max(deadline_value, timer_round_deadline_);
    }

    std::size_t slot_index = free_timer_slot_;
    if (slot_index == no_timer_slot) {
        slot_index = timer_slots_.size();
        timer_slots_.emplace_back();
    } else {
        free_timer_slot_ = timer_slots_[slot_index].next_free_;
    }
    auto& slot = timer_slots_[slot_index];
    if (++slot.generation_ == 0) {
        ++slot.generation_;
    }
    slot.next_free_ = no_timer_slot;
    slot.occupied_ = true;
    slot.cancel_requested_ = false;
    slot.completion_ = std::move(completion);
    slot.active_generation_.store(slot.generation_, std::memory_order_release);
    try {
        timers_.push_back(timer_entry{
            .deadline_ = deadline_value,
            .sequence_ = next_timer_sequence_++,
            .slot_ = slot_index,
            .generation_ = slot.generation_,
        });
    } catch (...) {
        // Roll the slot back; the moved-in completion is destroyed uninvoked.
        static_cast<void>(release_timer_slot(slot_index));
        throw;
    }
    std::ranges::push_heap(timers_, timer_entry_later{});
    const auto generation = slot.generation_;
    registration.bind(*this, slot_index, generation, slot);
    // The armed wait already covers every deadline at or after it.
    if (!dispatching_timers_ && (!timer_armed_ || deadline_value < armed_deadline_)) {
        try {
            arm_timer();
        } catch (...) {
            // Arming cancelled the previous wait before failing. Withdraw the
            // new entry so the caller sees a clean failure, then re-arm for the
            // entries that were pending before this call.
            registration.release();
            static_cast<void>(take_timer(slot_index, generation));
            try {
                arm_timer();
            } catch (...) {
                fail_timers("worker timer arm", std::current_exception());
            }
            throw;
        }
    }
    return worker_timer_schedule_status::scheduled;
}

void worker_dispatcher::request_timer_cancellation(
    std::size_t slot, std::uint64_t generation, bool notify) noexcept {
    if (current_worker_dispatcher() != this) {
        std::lock_guard lock(mutex_);
        if (timers_stopping_.load(std::memory_order_acquire) || !context_attached_) {
            return;
        }
        if (!io_context_.get_executor().running_in_this_thread()) {
            try {
                // A stopped io_context can be restarted; queue the cancellation
                // so a destroyed registration cannot leave a live slot that
                // expires after that restart. The handler already runs fresh on
                // the worker, exactly like fire_timers(), so it delivers the
                // completion inline instead of posting a second hop.
                asio::post(io_context_, [self = shared_from_this(), slot, generation, notify] {
                    auto completion = self->take_timer(slot, generation);
                    if (notify && completion) {
                        completion(worker_timer_outcome::cancelled);
                    }
                });
            } catch (...) {
                std::terminate();
            }
            return;
        }
    }
    if (!notify) {
        static_cast<void>(take_timer(slot, generation));
        return;
    }
    if (!has_occupied_timer(slot, generation)) {
        return;
    }
    auto& slot_state = timer_slots_[slot];
    if (slot_state.cancel_requested_) {
        return;
    }
    slot_state.cancel_requested_ = true;
    note_stale_timer();
    // The caller is running synchronously on the worker; delivering its own
    // completion here would re-enter it, so the completion is posted. Deferring
    // slot extraction lets a subsequent registration.cancel() or destruction on
    // this worker withdraw the callback before the posted turn runs.
    defer_or_terminate([self = shared_from_this(), slot, generation] {
        auto completion = self->take_timer(slot, generation);
        if (completion) {
            completion(worker_timer_outcome::cancelled);
        }
    });
}

void worker_dispatcher::note_stale_timer() noexcept {
    ++stale_timer_count_;
    if (stale_timer_count_ == timers_.size()) {
        // Nothing live remains: drop the heap and the armed wait so an idle
        // worker holds no timer work.
        timers_.clear();
        stale_timer_count_ = 0;
        if (timer_armed_ && timer_) {
            ++timer_generation_;
            timer_armed_ = false;
            timer_->cancel();
        }
    } else if (stale_timer_count_ >= 64 && stale_timer_count_ * 2 >= timers_.size()) {
        std::erase_if(timers_, [this](const timer_entry& entry_value) {
            return !has_live_timer_entry(entry_value.slot_, entry_value.generation_);
        });
        std::ranges::make_heap(timers_, timer_entry_later{});
        stale_timer_count_ = 0;
    }
}

move_only_function<void(worker_timer_outcome)> worker_dispatcher::take_timer(
    std::size_t slot_index, std::uint64_t generation) noexcept {
    if (!has_occupied_timer(slot_index, generation)) {
        return {};
    }
    const bool active = has_timer(slot_index, generation);
    const bool already_stale = timer_slots_[slot_index].cancel_requested_;
    auto completion = release_timer_slot(slot_index);
    if (!already_stale) {
        note_stale_timer();
    }
    return active ? std::move(completion) : move_only_function<void(worker_timer_outcome)>{};
}

void worker_dispatcher::stop_timers() noexcept {
    if (timers_stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    ++timer_generation_;
    timer_armed_ = false;
    if (timer_) {
        timer_->cancel();
        // The timer object is bound to the worker io_context service.  A
        // worker_handle may keep the dispatcher endpoint alive after shutdown,
        // so stopping timers must release the Asio object while the context is
        // still known to be alive rather than leaving that work to the
        // dispatcher's eventual destructor.
        timer_.reset();
    }

    timers_.clear();
    stale_timer_count_ = 0;
    for (std::size_t index = 0; index < timer_slots_.size(); ++index) {
        if (!timer_slots_[index].occupied_) {
            continue;
        }
        const bool active =
            timer_slots_[index].active_generation_.load(std::memory_order_acquire) != 0;
        auto completion = release_timer_slot(index);
        try {
            if (active && completion) {
                completion(worker_timer_outcome::cancelled);
            }
        } catch (...) {
            std::terminate();
        }
    }
}

void worker_dispatcher::arm_timer() {
    ++timer_generation_;
    const auto generation = timer_generation_;
    if (!timer_) {
        return;
    }
    if (timer_armed_) {
        timer_->cancel();
        timer_armed_ = false;
    }
    while (!timers_.empty() &&
           !has_live_timer_entry(timers_.front().slot_, timers_.front().generation_)) {
        std::ranges::pop_heap(timers_, timer_entry_later{});
        timers_.pop_back();
        if (stale_timer_count_ != 0) {
            --stale_timer_count_;
        }
    }
    if (timers_stopping_.load(std::memory_order_acquire) || timers_.empty()) {
        return;
    }
    armed_deadline_ = timers_.front().deadline_;
    timer_->expires_at(armed_deadline_);
    timer_->async_wait([self = shared_from_this(), generation](const std::error_code& error) {
        if (error || generation != self->timer_generation_) {
            return;
        }
        self->timer_armed_ = false;
        self->fire_timers();
    });
    // Initiation may allocate and throw; only a successful wait is armed.
    timer_armed_ = true;
}

void worker_dispatcher::fire_timers() {
    std::exception_ptr failure;
    dispatching_timers_ = true;
    // One pass fires only what was due when it began. A completion that
    // schedules a deadline already in the past (a periodic timer whose work
    // outlasts its interval) waits for the re-armed wait, so I/O, deferred work,
    // and stop controls run between rounds instead of starving behind them.
    const auto now = std::chrono::steady_clock::now();
    timer_round_deadline_ = now;
    const auto sequence_limit = next_timer_sequence_;
    try {
        while (!timers_.empty() && timers_.front().deadline_ <= now &&
               timers_.front().sequence_ < sequence_limit) {
            std::ranges::pop_heap(timers_, timer_entry_later{});
            auto entry_value = std::move(timers_.back());
            timers_.pop_back();
            if (!has_live_timer_entry(entry_value.slot_, entry_value.generation_)) {
                if (stale_timer_count_ != 0) {
                    --stale_timer_count_;
                }
                continue;
            }
            const bool active = has_timer(entry_value.slot_, entry_value.generation_);
            auto completion = release_timer_slot(entry_value.slot_);
            if (active && completion) {
                completion(worker_timer_outcome::expired);
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
    // Restore the queue before propagating a user callback failure. An external
    // io_context owner may catch it and continue running the remaining timers.
    dispatching_timers_ = false;
    try {
        arm_timer();
    } catch (...) {
        fail_timers("worker timer re-arm", std::current_exception());
        if (!failure) {
            throw;
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// Without an armed wait nothing pending could ever fire. Stopping the queue
// delivers every pending completion as cancelled instead of stalling silently.
void worker_dispatcher::fail_timers(std::string_view context_value, std::exception_ptr failure) noexcept {
    report_unhandled_failure(context_value, std::move(failure));
    stop_timers();
}

bool worker_dispatcher::has_timer(std::size_t slot, std::uint64_t generation) const noexcept {
    return generation != 0 && slot < timer_slots_.size() &&
           timer_slots_[slot].active_generation_.load(std::memory_order_acquire) == generation;
}

bool worker_dispatcher::has_occupied_timer(std::size_t slot, std::uint64_t generation) const noexcept {
    return generation != 0 && slot < timer_slots_.size() && timer_slots_[slot].occupied_ &&
           timer_slots_[slot].generation_ == generation;
}

bool worker_dispatcher::has_live_timer_entry(std::size_t slot, std::uint64_t generation) const noexcept {
    return has_occupied_timer(slot, generation) && !timer_slots_[slot].cancel_requested_;
}

move_only_function<void(worker_timer_outcome)> worker_dispatcher::release_timer_slot(
    std::size_t slot_index) noexcept {
    auto& slot = timer_slots_[slot_index];
    slot.occupied_ = false;
    slot.cancel_requested_ = false;
    slot.active_generation_.store(0, std::memory_order_release);
    auto completion = std::move(slot.completion_);
    slot.next_free_ = free_timer_slot_;
    free_timer_slot_ = slot_index;
    return completion;
}

}  // namespace ruvia::detail
