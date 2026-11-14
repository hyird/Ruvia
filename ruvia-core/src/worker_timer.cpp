#include "worker_timer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "failure_report.h"
#include "move_only_function.h"
#include "stop_token.h"
#include "timeout_duration.h"
#include "worker_dispatcher.h"
#include "worker_ref.h"

namespace ruvia {

// The dispatcher's timer heap: registering a deadline, cancelling one from any
// thread, re-arming the single asio timer, and firing everything due on the
// worker.
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
    if (dispatcher == nullptr || slot_state == nullptr || slot_state->active_generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    if (!notify && detail::current_worker_dispatcher() != dispatcher) {
        auto expected = generation;
        if (!const_cast<detail::timer_slot*>(slot_state)->active_generation_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
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

void worker_timer_registration::bind(detail::worker_dispatcher& dispatcher, std::size_t slot, std::uint64_t generation, const detail::timer_slot& slot_state) noexcept {
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
    return slot_state_ != nullptr && generation_ != 0 && slot_state_->active_generation_.load(std::memory_order_acquire) == generation_;
}

// Only a cold operation may move; a started one is referenced by its timer
// callback and must stay address-stable.
sleep_operation::sleep_operation(sleep_operation&& other) noexcept :
    worker_(std::exchange(other.worker_, nullptr)),
    target_(other.target_),
    stop_token_(std::move(other.stop_token_)) {
    if (other.started_) {
        std::terminate();
    }
}

bool sleep_operation::await_ready() {
    if (worker_ == nullptr || started_) {
        throw std::logic_error("sleep operation cannot be awaited");
    }
    if (!worker_->is_current()) {
        throw std::logic_error("sleep must run on its bound worker");
    }
    started_ = true;
    if (stop_token_.stop_requested()) {
        return true;
    }
    if (const auto* duration = std::get_if<timeout_duration>(&target_)) {
        return duration->value() <= std::chrono::milliseconds::zero();
    }
    return std::get<std::chrono::steady_clock::time_point>(target_) <= std::chrono::steady_clock::now();
}

// A stopped timer queue resolves immediately with the outcome stop_timers()
// would have delivered to a pending sleep.
bool sleep_operation::await_suspend(std::coroutine_handle<> continuation) {
    continuation_ = continuation;
    const auto deadline = [&] {
        if (const auto* duration = std::get_if<timeout_duration>(&target_)) {
            return deadline_after(duration->value());
        }
        return std::get<std::chrono::steady_clock::time_point>(target_);
    }();
    const auto status = worker_->schedule_timer(registration_, deadline, [this](worker_timer_outcome value) {
        outcome_ = value;
        continuation_.resume();
    });
    if (status == worker_timer_schedule_status::worker_stopping) {
        outcome_ = worker_timer_outcome::cancelled;
        return false;
    }
    if (stop_token_.stoppable()) {
        stop_token_.register_callback(stop_registration_, [cancellation = registration_.cancellation()] {
            cancellation.cancel();
        });
    }
    return true;
}

// A zero/negative duration never suspends and keeps the default expired
// outcome, so it reports elapsed unless stop was already requested.
timer_sleep_result sleep_operation::await_resume() const noexcept {
    return outcome_ == worker_timer_outcome::expired && !stop_token_.stop_requested() ? timer_sleep_result::elapsed : timer_sleep_result::stop_requested;
}

sleep_operation sleep_for(const worker_ref& worker_value, timeout_duration duration, stop_token stop_token_value) {
    return sleep_operation(worker_value, duration, std::move(stop_token_value));
}

sleep_operation sleep_until(const worker_ref& worker_value, std::chrono::steady_clock::time_point deadline, stop_token stop_token_value) {
    return sleep_operation(worker_value, deadline, std::move(stop_token_value));
}
}  // namespace ruvia

namespace ruvia::detail {

worker_timer_schedule_status worker_dispatcher::schedule_timer(worker_timer_registration& registration, std::chrono::steady_clock::time_point deadline_value, move_only_function<void(worker_timer_outcome)> completion) {
    if (!is_current()) {
        throw std::logic_error("worker timers must be scheduled on their worker");
    }
    if (registration.scheduling_) {
        throw std::logic_error("worker timer registration is already active");
    }
    registration.scheduling_ = true;
    struct scheduling_guard final {
        bool& scheduling_;
        ~scheduling_guard() {
            scheduling_ = false;
        }
    } guard{registration.scheduling_};
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
    slot.cancel_requested_ = false;
    // The target's move may stop timers reentrantly. Keep this reserved slot
    // unpublished until the move completes so stop_timers() cannot retire it
    // while its callback is still being constructed.
    slot.completion_ = std::move(completion);
    if (timers_stopping_.load(std::memory_order_acquire)) {
        static_cast<void>(release_timer_slot(slot_index));
        return worker_timer_schedule_status::worker_stopping;
    }
    slot.occupied_ = true;
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

void worker_dispatcher::request_timer_cancellation(std::size_t slot, std::uint64_t generation, bool notify) noexcept {
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

void worker_dispatcher::pop_stale_timer_top() noexcept {
    while (!timers_.empty() && !has_live_timer_entry(timers_.front().slot_, timers_.front().generation_)) {
        std::ranges::pop_heap(timers_, timer_entry_later{});
        timers_.pop_back();
        if (stale_timer_count_ != 0) {
            --stale_timer_count_;
        }
    }
}

void worker_dispatcher::note_stale_timer() noexcept {
    ++stale_timer_count_;
    if (stale_timer_count_ == timers_.size()) {
        // Nothing live remains: drop the heap and the armed wait so an idle
        // worker holds no timer work.
        timers_.clear();
        release_drained_timer_heap();
        stale_timer_count_ = 0;
        if (timer_armed_ && timer_) {
            ++timer_generation_;
            timer_armed_ = false;
            timer_->cancel();
        }
    } else {
        pop_stale_timer_top();
        if (stale_timer_count_ >= 64 && stale_timer_count_ * 2 >= timers_.size()) {
            std::erase_if(timers_, [this](const timer_entry& entry_value) {
                return !has_live_timer_entry(entry_value.slot_, entry_value.generation_);
            });
            std::ranges::make_heap(timers_, timer_entry_later{});
            stale_timer_count_ = 0;
        }
    }
}

void worker_dispatcher::release_drained_timer_heap() noexcept {
    // Slots stay: registrations keep their addresses and (slot, generation)
    // pairs across threads, so only the heap, which nothing references, shrinks.
    // The heap regrows geometrically on the next burst.
    if (timers_.empty() && timers_.capacity() > capacity_) {
        std::vector<timer_entry>().swap(timers_);
    }
}

move_only_function<void(worker_timer_outcome)> worker_dispatcher::take_timer(std::size_t slot_index, std::uint64_t generation) noexcept {
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
        // worker_ref may keep the dispatcher endpoint alive after shutdown,
        // so stopping timers must release the Asio object while the context is
        // still known to be alive rather than leaving that work to the
        // dispatcher's eventual destructor.
        timer_.reset();
    }

    timers_.clear();
    release_drained_timer_heap();
    stale_timer_count_ = 0;
    for (std::size_t index = 0; index < timer_slots_.size(); ++index) {
        if (!timer_slots_[index].occupied_) {
            continue;
        }
        const bool active = timer_slots_[index].active_generation_.load(std::memory_order_acquire) != 0;
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
    pop_stale_timer_top();
    if (timers_stopping_.load(std::memory_order_acquire) || timers_.empty()) {
        release_drained_timer_heap();
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
        for (;;) {
            pop_stale_timer_top();
            if (timers_.empty() || timers_.front().deadline_ > now || timers_.front().sequence_ >= sequence_limit) {
                break;
            }
            std::ranges::pop_heap(timers_, timer_entry_later{});
            auto entry_value = std::move(timers_.back());
            timers_.pop_back();
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
    return generation != 0 && slot < timer_slots_.size() && timer_slots_[slot].active_generation_.load(std::memory_order_acquire) == generation;
}

bool worker_dispatcher::has_occupied_timer(std::size_t slot, std::uint64_t generation) const noexcept {
    return generation != 0 && slot < timer_slots_.size() && timer_slots_[slot].occupied_ && timer_slots_[slot].generation_ == generation;
}

bool worker_dispatcher::has_live_timer_entry(std::size_t slot, std::uint64_t generation) const noexcept {
    return has_occupied_timer(slot, generation) && !timer_slots_[slot].cancel_requested_;
}

move_only_function<void(worker_timer_outcome)> worker_dispatcher::release_timer_slot(std::size_t slot_index) noexcept {
    auto& slot = timer_slots_[slot_index];
    slot.occupied_ = false;
    slot.cancel_requested_ = false;
    auto completion = std::move(slot.completion_);
    slot.next_free_ = free_timer_slot_;
    free_timer_slot_ = slot_index;
    slot.active_generation_.store(0, std::memory_order_release);
    return completion;
}

}  // namespace ruvia::detail
