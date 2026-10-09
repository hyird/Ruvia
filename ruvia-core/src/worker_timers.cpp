#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

#include <asio/post.hpp>

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
    cancel(false);
}

void worker_timer_registration::cancel() noexcept {
    cancel(true);
}

void worker_timer_registration::cancel_quietly() noexcept {
    cancel(false);
}

void worker_timer_registration::cancel(bool notify) noexcept {
    auto* dispatcher = std::exchange(dispatcher_, nullptr);
    const auto slot = std::exchange(slot_, 0);
    const auto generation = std::exchange(generation_, 0);
    if (dispatcher == nullptr || generation == 0) {
        return;
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

void worker_timer_registration::bind(
    detail::worker_dispatcher& dispatcher, std::size_t slot, std::uint64_t generation) noexcept {
    dispatcher_ = &dispatcher;
    slot_ = slot;
    generation_ = generation;
}

void worker_timer_registration::release() noexcept {
    dispatcher_ = nullptr;
    slot_ = 0;
    generation_ = 0;
}

}  // namespace ruvia

namespace ruvia::detail {

void worker_dispatcher::schedule_timer(worker_timer_registration& registration,
    std::chrono::steady_clock::time_point deadline_value,
    move_only_function<void(worker_timer_outcome)> completion) {
    if (!is_current()) {
        throw std::logic_error("worker timers must be scheduled on their worker");
    }
    if (!attached()) {
        throw std::runtime_error("worker execution context is detached");
    }
    if (impl_->timers_stopping_.load(std::memory_order_acquire)) {
        throw std::runtime_error("worker timer queue is stopping");
    }

    if (registration.dispatcher_ != nullptr) {
        if (registration.dispatcher_ != this ||
            has_timer(registration.slot_, registration.generation_)) {
            throw std::logic_error("worker timer registration is already active");
        }
        registration.release();
    }

    std::size_t slot_index = impl_->free_timer_slot_;
    if (slot_index == no_timer_slot) {
        slot_index = impl_->timer_slots_.size();
        impl_->timer_slots_.emplace_back();
    } else {
        impl_->free_timer_slot_ = impl_->timer_slots_[slot_index].next_free_;
    }
    auto& slot = impl_->timer_slots_[slot_index];
    if (++slot.generation_ == 0) {
        ++slot.generation_;
    }
    slot.active_ = true;
    slot.next_free_ = no_timer_slot;
    slot.completion_ = std::move(completion);
    try {
        impl_->timers_.push_back(timer_entry{
            .deadline_ = deadline_value,
            .sequence_ = impl_->next_timer_sequence_++,
            .slot_ = slot_index,
            .generation_ = slot.generation_,
        });
    } catch (...) {
        slot.active_ = false;
        slot.completion_ = nullptr;
        slot.next_free_ = impl_->free_timer_slot_;
        impl_->free_timer_slot_ = slot_index;
        throw;
    }
    std::ranges::push_heap(impl_->timers_, timer_entry_later{});
    registration.bind(*this, slot_index, slot.generation_);
    if (!impl_->dispatching_timers_) {
        arm_timer();
    }
}

void worker_dispatcher::request_timer_cancellation(
    std::size_t slot, std::uint64_t generation, bool notify) noexcept {
    if (generation == 0) {
        return;
    }
    if (current_worker_dispatcher() == this) {
        cancel_timer(slot, generation, notify);
        return;
    }
    bool current = false;
    {
        std::lock_guard lock(impl_->mutex_);
        if (impl_->timers_stopping_.load(std::memory_order_acquire) || !impl_->context_attached_) {
            return;
        }
        current = impl_->io_context_.get_executor().running_in_this_thread();
        if (!current) {
            try {
                // A stopped io_context can be restarted; queue the cancellation
                // so a destroyed registration cannot leave a live slot that
                // expires after that restart.
                asio::post(impl_->io_context_, [self = shared_from_this(), slot, generation, notify] {
                    self->cancel_timer(slot, generation, notify);
                });
            } catch (...) {
                std::terminate();
            }
            return;
        }
    }
    cancel_timer(slot, generation, notify);
}

void worker_dispatcher::cancel_timer(
    std::size_t slot_index, std::uint64_t generation, bool notify) noexcept {
    if (slot_index >= impl_->timer_slots_.size()) {
        return;
    }
    auto& slot = impl_->timer_slots_[slot_index];
    if (!slot.active_ || slot.generation_ != generation) {
        return;
    }
    slot.active_ = false;
    auto completion = std::move(slot.completion_);
    slot.next_free_ = impl_->free_timer_slot_;
    impl_->free_timer_slot_ = slot_index;
    ++impl_->stale_timer_count_;
    if (impl_->stale_timer_count_ >= 64 && impl_->stale_timer_count_ * 2 >= impl_->timers_.size()) {
        std::erase_if(impl_->timers_,
            [this](const timer_entry& entry_value) { return !has_timer(entry_value.slot_, entry_value.generation_); });
        std::ranges::make_heap(impl_->timers_, timer_entry_later{});
        impl_->stale_timer_count_ = 0;
    }
    if (!impl_->dispatching_timers_) {
        arm_timer();
    }
    if (notify) {
        try {
            if (completion) {
                defer([completion = std::move(completion)]() mutable {
                    completion(worker_timer_outcome::cancelled);
                });
            }
        } catch (...) {
            std::terminate();
        }
    }
}

void worker_dispatcher::stop_timers() noexcept {
    if (impl_->timers_stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    ++impl_->timer_generation_;
    impl_->timer_armed_ = false;
    if (impl_->timer_) {
        impl_->timer_->cancel();
        // The timer object is bound to the worker io_context service.  A
        // worker_handle may keep the dispatcher endpoint alive after shutdown,
        // so stopping timers must release the Asio object while the context is
        // still known to be alive rather than leaving that work to the
        // dispatcher's eventual destructor.
        impl_->timer_.reset();
    }

    impl_->timers_.clear();
    impl_->stale_timer_count_ = 0;
    for (std::size_t index = 0; index < impl_->timer_slots_.size(); ++index) {
        auto& slot = impl_->timer_slots_[index];
        if (!slot.active_) {
            continue;
        }
        slot.active_ = false;
        auto completion = std::move(slot.completion_);
        slot.next_free_ = impl_->free_timer_slot_;
        impl_->free_timer_slot_ = index;
        try {
            if (completion) {
                completion(worker_timer_outcome::cancelled);
            }
        } catch (...) {
            std::terminate();
        }
    }
}

void worker_dispatcher::arm_timer() {
    ++impl_->timer_generation_;
    const auto generation = impl_->timer_generation_;
    if (!impl_->timer_) {
        return;
    }
    impl_->timer_->cancel();
    impl_->timer_armed_ = false;
    while (!impl_->timers_.empty() &&
           !has_timer(impl_->timers_.front().slot_, impl_->timers_.front().generation_)) {
        std::ranges::pop_heap(impl_->timers_, timer_entry_later{});
        impl_->timers_.pop_back();
        if (impl_->stale_timer_count_ != 0) {
            --impl_->stale_timer_count_;
        }
    }
    if (impl_->timers_stopping_.load(std::memory_order_acquire) || impl_->timers_.empty()) {
        return;
    }
    impl_->timer_->expires_at(impl_->timers_.front().deadline_);
    impl_->timer_armed_ = true;
    impl_->timer_->async_wait([self = shared_from_this(), generation](const std::error_code& error) {
        if (error || generation != self->impl_->timer_generation_) {
            return;
        }
        self->impl_->timer_armed_ = false;
        self->fire_timers();
    });
}

void worker_dispatcher::fire_timers() {
    impl_->dispatching_timers_ = true;
    while (!impl_->timers_.empty() &&
           impl_->timers_.front().deadline_ <= std::chrono::steady_clock::now()) {
        std::ranges::pop_heap(impl_->timers_, timer_entry_later{});
        auto entry_value = std::move(impl_->timers_.back());
        impl_->timers_.pop_back();
        if (!has_timer(entry_value.slot_, entry_value.generation_)) {
            if (impl_->stale_timer_count_ != 0) {
                --impl_->stale_timer_count_;
            }
            continue;
        }
        auto& slot = impl_->timer_slots_[entry_value.slot_];
        slot.active_ = false;
        auto completion = std::move(slot.completion_);
        slot.next_free_ = impl_->free_timer_slot_;
        impl_->free_timer_slot_ = entry_value.slot_;
        if (completion) {
            completion(worker_timer_outcome::expired);
        }
    }
    impl_->dispatching_timers_ = false;
    arm_timer();
}

bool worker_dispatcher::has_timer(std::size_t slot, std::uint64_t generation) const noexcept {
    return slot < impl_->timer_slots_.size() && impl_->timer_slots_[slot].active_ &&
           impl_->timer_slots_[slot].generation_ == generation;
}

}  // namespace ruvia::detail
