#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "ruvia/core/detail/suspend_race_state.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait_result.h"

namespace ruvia::detail {

// The worker wait completion race is the suspend_race_state pattern with a
// worker_wait_result payload.
template <typename t_type>
using worker_wait_await_state_type = suspend_race_state<worker_wait_result<t_type>>;

template <typename state_type>
void complete_worker_single_wait(state_type& state_value, worker_wait_status status) noexcept;

template <typename state_type>
void cancel_worker_single_wait(const std::shared_ptr<state_type>& state_value, std::uint64_t generation) noexcept;

// Shared storage and transition machinery for one worker-bound waiter. Domain
// awaiters own this component by value and publish its typed address directly,
// without an outer-owner pointer, allocation, virtual dispatch, or type erasure.
// State must expose worker, mutex, waiter, waiter_generation_, and
// next_waiter_generation_.
template <typename t_type, typename state_type>
class worker_single_wait_awaiter final {
public:
    worker_single_wait_awaiter(std::shared_ptr<state_type> state_value,
        std::optional<std::chrono::steady_clock::duration> timeout, stop_token stop_token_value)
        : state_(std::move(state_value)),
          timeout_(timeout),
          stop_token_(std::move(stop_token_value)),
          generation_(stop_token_.stoppable() ? reserve_generation(*state_) : 0),
          stop_registration_(register_cancellation(stop_token_, state_, generation_)) {}

    worker_single_wait_awaiter(const worker_single_wait_awaiter&) = delete;
    worker_single_wait_awaiter& operator=(const worker_single_wait_awaiter&) = delete;
    worker_single_wait_awaiter(worker_single_wait_awaiter&&) = delete;
    worker_single_wait_awaiter& operator=(worker_single_wait_awaiter&&) = delete;

    [[nodiscard]] state_type& state() const noexcept {
        return *state_;
    }

    [[nodiscard]] const stop_token& get_stop_token() const noexcept {
        return stop_token_;
    }

    [[nodiscard]] const std::optional<std::chrono::steady_clock::duration>& timeout()
        const noexcept {
        return timeout_;
    }

    [[nodiscard]] bool complete_result(worker_wait_result<t_type>&& result_value) {
        return completion_.complete(std::move(result_value));
    }

    [[nodiscard]] bool complete_status(worker_wait_status status) noexcept {
        return completion_.complete(worker_wait_result_access::outcome<t_type>(status));
    }

    void publish() noexcept {
        auto& owner_value = state();
        owner_value.waiter_ = this;
        owner_value.waiter_generation_ = generation_;
    }

    [[nodiscard]] bool suspend(std::coroutine_handle<> handle) {
        auto& owner_value = state();
        auto* waiter = this;
        std::lock_guard lock(owner_value.mutex_);
        if (!completion_.suspend(handle)) {
            return false;
        }
        if (timeout_) {
            try {
                owner_value.worker_.schedule_timer(timer_,
                    worker_timer_deadline_after(*timeout_),
                    [&owner_value, waiter, completion = &completion_](worker_timer_outcome outcome) {
                        if (outcome == worker_timer_outcome::expired) {
                            std::lock_guard state_lock(owner_value.mutex_);
                            if (owner_value.waiter_ == waiter) {
                                owner_value.waiter_ = nullptr;
                                owner_value.waiter_generation_ = 0;
                                (void)completion->complete(worker_wait_result_access::outcome<t_type>(
                                    worker_wait_status::timed_out));
                            }
                        }
                        completion->continuation().resume();
                    });
            } catch (...) {
                if (owner_value.waiter_ == waiter) {
                    owner_value.waiter_ = nullptr;
                    owner_value.waiter_generation_ = 0;
                }
                throw;
            }
        }
        return true;
    }

    [[nodiscard]] worker_wait_result<t_type> take_result() {
        return completion_.take_value();
    }

    // Completion has already detached the intrusive waiter before wake() runs.
    // A failed continuation dispatch is therefore unrecoverable and shares the
    // same explicit terminate boundary as worker_signal and timer cancellation.
    void wake() noexcept {
        if (timer_.registered()) {
            timer_.cancel();
            return;
        }
        auto* waiter = this;
        worker_handle_access::defer_or_terminate(
            state().worker_, [waiter] { waiter->resume_continuation(); });
    }

    void resume_continuation() noexcept {
        completion_.continuation().resume();
    }

private:
    [[nodiscard]] static std::uint64_t reserve_generation(state_type& state_value) noexcept {
        std::lock_guard lock(state_value.mutex_);
        if (++state_value.next_waiter_generation_ == 0) {
            ++state_value.next_waiter_generation_;
        }
        return state_value.next_waiter_generation_;
    }

    [[nodiscard]] static stop_registration register_cancellation(
        const stop_token& token, const std::shared_ptr<state_type>& state_value, std::uint64_t generation) {
        if (!token.stoppable() || token.stop_requested()) {
            return {};
        }
        return token.register_callback([state_value, generation] {
            (void)worker_handle_access::defer_if_attached(
                state_value->worker_, [state_value, generation] { cancel_worker_single_wait(state_value, generation); });
        });
    }

    std::shared_ptr<state_type> state_;
    std::optional<std::chrono::steady_clock::duration> timeout_;
    stop_token stop_token_;
    std::uint64_t generation_{0};
    worker_timer_registration timer_;
    worker_wait_await_state_type<t_type> completion_;
    // Last so callback unregistration completes before the state, timer, and
    // borrowed dispatcher begin destruction.
    stop_registration stop_registration_;
};

// Called with the state's mutex_ held. Every terminal source uses this one transition,
// so detachment, generation invalidation, result publication, and wake ordering
// cannot drift between channel and one_shot.
template <typename state_type>
void complete_worker_single_wait(state_type& state_value, worker_wait_status status) noexcept {
    auto* waiter = std::exchange(state_value.waiter_, nullptr);
    state_value.waiter_generation_ = 0;
    if (waiter != nullptr && waiter->complete_status(status)) {
        waiter->wake();
    }
}

template <typename state_type>
void cancel_worker_single_wait(
    const std::shared_ptr<state_type>& state_value, std::uint64_t generation) noexcept {
    std::lock_guard lock(state_value->mutex_);
    if (state_value->waiter_ == nullptr || state_value->waiter_generation_ != generation) {
        return;
    }
    complete_worker_single_wait(*state_value, worker_wait_status::cancelled);
}

}  // namespace ruvia::detail
