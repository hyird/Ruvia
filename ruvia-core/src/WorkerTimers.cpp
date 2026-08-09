#include <ruvia/core/detail/worker/WorkerDispatcherImpl.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

#include <asio/post.hpp>

// The dispatcher's timer heap: registering a deadline, cancelling one from any
// thread, re-arming the single asio timer, and firing everything due on the
// worker.

namespace ruvia::detail {

WorkerTimerRegistration::~WorkerTimerRegistration() {
    cancel();
}

void WorkerTimerRegistration::cancel() noexcept {
    auto* dispatcher = std::exchange(dispatcher_, nullptr);
    const auto slot = std::exchange(slot_, 0);
    const auto generation = std::exchange(generation_, 0);
    if (dispatcher == nullptr || generation == 0) {
        return;
    }
    dispatcher->requestTimerCancellation(slot, generation);
}

bool WorkerTimerRegistration::registered() const noexcept {
    return dispatcher_ != nullptr;
}

void WorkerTimerRegistration::bind(WorkerDispatcher& dispatcher, std::size_t slot, std::uint64_t generation) noexcept {
    dispatcher_ = &dispatcher;
    slot_ = slot;
    generation_ = generation;
}

void WorkerTimerRegistration::release() noexcept {
    dispatcher_ = nullptr;
    slot_ = 0;
    generation_ = 0;
}

void WorkerDispatcher::scheduleTimer(WorkerTimerRegistration& registration, std::chrono::steady_clock::time_point deadline, MoveOnlyFunction<void(WorkerTimerOutcome)> completion) {
    if (!isCurrent()) {
        throw std::logic_error("worker timers must be scheduled on their worker");
    }
    if (!attached()) {
        throw std::runtime_error("worker execution context is detached");
    }
    if (impl_->timersStopping) {
        throw std::runtime_error("worker timer queue is stopping");
    }

    if (registration.dispatcher_ != nullptr) {
        if (registration.dispatcher_ != this || hasTimer(registration.slot_, registration.generation_)) {
            throw std::logic_error("worker timer registration is already active");
        }
        registration.release();
    }

    std::size_t slotIndex = impl_->freeTimerSlot;
    if (slotIndex == kNoTimerSlot) {
        slotIndex = impl_->timerSlots.size();
        impl_->timerSlots.emplace_back();
    } else {
        impl_->freeTimerSlot = impl_->timerSlots[slotIndex].nextFree;
    }
    auto& slot = impl_->timerSlots[slotIndex];
    if (++slot.generation == 0) {
        ++slot.generation;
    }
    slot.active = true;
    slot.nextFree = kNoTimerSlot;
    slot.completion = std::move(completion);
    try {
        impl_->timers.push_back(TimerEntry{
            .deadline = deadline,
            .sequence = impl_->nextTimerSequence++,
            .slot = slotIndex,
            .generation = slot.generation,
        });
    } catch (...) {
        slot.active = false;
        slot.completion = nullptr;
        slot.nextFree = impl_->freeTimerSlot;
        impl_->freeTimerSlot = slotIndex;
        throw;
    }
    std::ranges::push_heap(impl_->timers, TimerEntryLater{});
    registration.bind(*this, slotIndex, slot.generation);
    if (!impl_->dispatchingTimers) {
        armTimer();
    }
}

void WorkerDispatcher::requestTimerCancellation(std::size_t slot, std::uint64_t generation) noexcept {
    if (generation == 0) {
        return;
    }
    if (currentWorkerDispatcher() == this) {
        cancelTimer(slot, generation);
        return;
    }
    bool current = false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->timersStopping || !impl_->contextAttached) {
            return;
        }
        current = impl_->ioContext.get_executor().running_in_this_thread();
        if (!current) {
            try {
                // A stopped io_context can be restarted; queue the cancellation
                // so a destroyed registration cannot leave a live slot that
                // expires after that restart.
                asio::post(impl_->ioContext, [self = shared_from_this(), slot, generation] { self->cancelTimer(slot, generation); });
            } catch (...) {
                std::terminate();
            }
            return;
        }
    }
    cancelTimer(slot, generation);
}

void WorkerDispatcher::cancelTimer(std::size_t slotIndex, std::uint64_t generation) noexcept {
    if (slotIndex >= impl_->timerSlots.size()) {
        return;
    }
    auto& slot = impl_->timerSlots[slotIndex];
    if (!slot.active || slot.generation != generation) {
        return;
    }
    slot.active = false;
    auto completion = std::move(slot.completion);
    slot.nextFree = impl_->freeTimerSlot;
    impl_->freeTimerSlot = slotIndex;
    ++impl_->staleTimerCount;
    if (impl_->staleTimerCount >= 64 && impl_->staleTimerCount * 2 >= impl_->timers.size()) {
        std::erase_if(impl_->timers, [this](const TimerEntry& entry) { return !hasTimer(entry.slot, entry.generation); });
        std::ranges::make_heap(impl_->timers, TimerEntryLater{});
        impl_->staleTimerCount = 0;
    }
    if (!impl_->dispatchingTimers) {
        armTimer();
    }
    try {
        if (completion) {
            defer([completion = std::move(completion)]() mutable { completion(WorkerTimerOutcome::kCancelled); });
        }
    } catch (...) {
        std::terminate();
    }
}

void WorkerDispatcher::stopTimers() noexcept {
    if (impl_->timersStopping) {
        return;
    }
    impl_->timersStopping = true;
    ++impl_->timerGeneration;
    impl_->timerArmed = false;
    std::error_code ignored;
    if (impl_->timer) {
        impl_->timer->cancel(ignored);
        // The timer object is bound to the worker io_context service.  A
        // WorkerHandle may keep the dispatcher endpoint alive after shutdown,
        // so stopping timers must release the Asio object while the context is
        // still known to be alive rather than leaving that work to the
        // dispatcher's eventual destructor.
        impl_->timer.reset();
    }

    impl_->timers.clear();
    impl_->staleTimerCount = 0;
    for (std::size_t index = 0; index < impl_->timerSlots.size(); ++index) {
        auto& slot = impl_->timerSlots[index];
        if (!slot.active) {
            continue;
        }
        slot.active = false;
        auto completion = std::move(slot.completion);
        slot.nextFree = impl_->freeTimerSlot;
        impl_->freeTimerSlot = index;
        try {
            if (completion) {
                completion(WorkerTimerOutcome::kCancelled);
            }
        } catch (...) {
            std::terminate();
        }
    }
}

void WorkerDispatcher::armTimer() {
    ++impl_->timerGeneration;
    const auto generation = impl_->timerGeneration;
    std::error_code ignored;
    if (!impl_->timer) {
        return;
    }
    impl_->timer->cancel(ignored);
    impl_->timerArmed = false;
    while (!impl_->timers.empty() && !hasTimer(impl_->timers.front().slot, impl_->timers.front().generation)) {
        std::ranges::pop_heap(impl_->timers, TimerEntryLater{});
        impl_->timers.pop_back();
        if (impl_->staleTimerCount != 0) {
            --impl_->staleTimerCount;
        }
    }
    if (impl_->timersStopping || impl_->timers.empty()) {
        return;
    }
    impl_->timer->expires_at(impl_->timers.front().deadline);
    impl_->timerArmed = true;
    impl_->timer->async_wait([self = shared_from_this(), generation](const std::error_code& error) {
        if (error || generation != self->impl_->timerGeneration) {
            return;
        }
        self->impl_->timerArmed = false;
        self->fireTimers();
    });
}

void WorkerDispatcher::fireTimers() {
    impl_->dispatchingTimers = true;
    while (!impl_->timers.empty() && impl_->timers.front().deadline <= std::chrono::steady_clock::now()) {
        std::ranges::pop_heap(impl_->timers, TimerEntryLater{});
        auto entry = std::move(impl_->timers.back());
        impl_->timers.pop_back();
        if (!hasTimer(entry.slot, entry.generation)) {
            if (impl_->staleTimerCount != 0) {
                --impl_->staleTimerCount;
            }
            continue;
        }
        auto& slot = impl_->timerSlots[entry.slot];
        slot.active = false;
        auto completion = std::move(slot.completion);
        slot.nextFree = impl_->freeTimerSlot;
        impl_->freeTimerSlot = entry.slot;
        if (completion) {
            completion(WorkerTimerOutcome::kExpired);
        }
    }
    impl_->dispatchingTimers = false;
    armTimer();
}

bool WorkerDispatcher::hasTimer(std::size_t slot, std::uint64_t generation) const noexcept {
    return slot < impl_->timerSlots.size() && impl_->timerSlots[slot].active && impl_->timerSlots[slot].generation == generation;
}

}  // namespace ruvia::detail
